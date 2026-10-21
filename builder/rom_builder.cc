#include "builder/rom_builder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/elf_reader.h"
#include "core/game_config.h"
#include "core/game_config.pb.h"
#include "core/sha1.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ModuleFunctionFlagsMap =
    absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, std::vector<std::string>>>;

struct LoadedConfigAndFlags {
  GameConfig config;
  ModuleFunctionFlagsMap function_flags_by_module;
};

absl::StatusOr<LoadedConfigAndFlags> LoadGameConfigAndTuningFlags(
    const RomBuilderOptions& options, const std::filesystem::path& repo_root) {
  LoadedConfigAndFlags loaded;
  std::error_code ec;
  if (!options.config_path.empty() && std::filesystem::exists(options.config_path, ec)) {
    auto config_or = LoadGameConfig(options.config_path);
    if (!config_or.ok()) {
      return config_or.status();
    }
    loaded.config = std::move(*config_or);
  }

  absl::flat_hash_set<std::string> explicit_o0_modules;
  for (const auto& [module_name, flag_list] : loaded.config.c_flags()) {
    for (const auto& f : flag_list.flags()) {
      if (f == "-O0") {
        explicit_o0_modules.insert(module_name);
        break;
      }
    }
  }

  auto index_tuning_cache = [&](const TuningCache& cache) {
    for (const auto& [module_name, flags] : cache.module_c_flags()) {
      if (explicit_o0_modules.contains(module_name)) {
        continue;
      }
      (*loaded.config.mutable_c_flags())[module_name] = flags;
    }
    for (const auto& entry : cache.entries()) {
      if (explicit_o0_modules.contains(entry.module_name())) {
        continue;
      }
      if (entry.c_flags_size() > 0 && !entry.function_name().empty()) {
        std::vector<std::string> flags(entry.c_flags().begin(), entry.c_flags().end());
        loaded.function_flags_by_module[std::string(entry.module_name())]
                                       [std::string(entry.function_name())] = std::move(flags);
      }
    }
  };

  for (const auto& entry : loaded.config.tuning_entries()) {
    if (entry.c_flags_size() > 0 && !entry.function_name().empty()) {
      std::vector<std::string> flags(entry.c_flags().begin(), entry.c_flags().end());
      loaded.function_flags_by_module[std::string(entry.module_name())]
                                     [std::string(entry.function_name())] = std::move(flags);
    }
  }

  std::filesystem::path cache_file;
  if (!loaded.config.tuning_cache_file().empty()) {
    cache_file = loaded.config.tuning_cache_file();
    if (cache_file.is_relative()) {
      cache_file = repo_root / cache_file;
    }
  } else if (!loaded.config.basename().empty()) {
    std::filesystem::path default_cache =
        repo_root / "config" / absl::StrCat(loaded.config.basename(), ".tuning.textproto");
    if (std::filesystem::exists(default_cache, ec)) {
      cache_file = default_cache;
    }
  }
  if (!cache_file.empty() && std::filesystem::exists(cache_file, ec)) {
    auto cache_or = LoadTuningCache(cache_file);
    if (cache_or.ok()) {
      index_tuning_cache(*cache_or);
    }
  }

  if (!options.src_dir.empty()) {
    std::filesystem::path tuned_flags_path = options.src_dir / "tuned_c_flags.textproto";
    if (std::filesystem::exists(tuned_flags_path, ec)) {
      auto cache_or = LoadTuningCache(tuned_flags_path);
      if (cache_or.ok()) {
        index_tuning_cache(*cache_or);
      }
    }
  }

  return loaded;
}

void RewriteCodeSegmentLinkerScript(const GameConfig& config, const std::filesystem::path& obj_dir,
                                    std::string* rewritten_ld) {
  // Align each subsegment (.text and .data) in SEGMENT_CODE segments to at least its retail
  // section-relative offset so shrinkage in an earlier module does not cascade-shift all
  // subsequent modules and .data/.bss. Also move <seg>_RODATA after <seg>_DATA so compiler-
  // generated .rodata sections (from jump tables and float constants in lifted C) do not push
  // the retail .data subsegments forward from their retail VRAM addresses.
  for (const auto& seg : config.segments()) {
    if (seg.type() != SEGMENT_CODE || seg.vram() == 0) {
      continue;
    }
    if (!seg.name().empty()) {
      std::string rodata_start_marker = absl::StrCat("        ", seg.name(), "_RODATA_START = .;");
      std::string data_start_marker = absl::StrCat("        ", seg.name(), "_DATA_START = .;");
      std::string rom_pos_inc_marker = absl::StrCat("    __romPos += SIZEOF(.", seg.name(), ");");
      size_t rodata_pos = rewritten_ld->find(rodata_start_marker);
      size_t data_pos = rewritten_ld->find(data_start_marker);
      if (rodata_pos != std::string::npos && data_pos != std::string::npos &&
          rodata_pos < data_pos) {
        std::string rodata_block = rewritten_ld->substr(rodata_pos, data_pos - rodata_pos);
        rewritten_ld->erase(rodata_pos, data_pos - rodata_pos);
        size_t rom_pos_inc = rewritten_ld->find(rom_pos_inc_marker);
        if (rom_pos_inc != std::string::npos) {
          size_t align_line_end = rewritten_ld->find('\n', rom_pos_inc);
          if (align_line_end != std::string::npos) {
            size_t next_line_end = rewritten_ld->find('\n', align_line_end + 1);
            if (next_line_end != std::string::npos) {
              align_line_end = next_line_end;
            }
            std::string rodata_section = absl::StrCat(
                "    .", seg.name(), "_rodata (ADDR(.", seg.name(), "_bss) + SIZEOF(.", seg.name(),
                "_bss)) : AT(__romPos) SUBALIGN(16)\n    {\n        FILL(0x00000000);\n",
                rodata_block, "    }\n    __romPos += SIZEOF(.", seg.name(),
                "_rodata);\n    __romPos = ALIGN(__romPos, 16);\n");
            rewritten_ld->insert(align_line_end + 1, rodata_section);
          }
        }
      }
    }
    for (const auto& sub : seg.subsegments()) {
      if (sub.name().empty() || sub.rom_start() <= seg.rom_start()) {
        continue;
      }
      uint32_t sub_offset = sub.rom_start() - seg.rom_start();
      uint32_t sub_vram = seg.vram() + sub_offset;
      std::string align_expr =
          absl::StrFormat(". = (ABSOLUTE(.) < 0x%08X) ? 0x%X : .;\n        ", sub_vram, sub_offset);
      if (sub.type() == SUBSEGMENT_DATA) {
        std::string target = (obj_dir / absl::StrCat(sub.name(), ".data.o")).string() + "(.data);";
        size_t pos = rewritten_ld->find(target);
        if (pos != std::string::npos) {
          rewritten_ld->insert(pos, align_expr);
        }
      } else if (sub.type() == SUBSEGMENT_C || sub.type() == SUBSEGMENT_ASM) {
        std::string obj_path = (obj_dir / absl::StrCat(sub.name(), ".o")).string();
        std::string target_exact = obj_path + "(.text);";
        size_t pos = rewritten_ld->find(target_exact);
        if (pos == std::string::npos) {
          std::string target_wild = obj_path + "(.text*);";
          pos = rewritten_ld->find(target_wild);
        }
        if (pos != std::string::npos) {
          rewritten_ld->insert(pos, align_expr);
        }
      }
    }
  }
}

absl::flat_hash_map<std::string, uint32_t> CollectFunctionTargetVrams(
    const SymbolIndex* symbol_index, const std::filesystem::path& src_dir) {
  absl::flat_hash_map<std::string, uint32_t> function_target_vrams;
  if (symbol_index != nullptr) {
    for (const SymbolRegistryEntry* entry : symbol_index->SortedEntries()) {
      if (entry->type() == SYMBOL_FUNC && !entry->name().empty() && entry->address() != 0) {
        function_target_vrams[std::string(entry->name())] = entry->address();
      }
    }
  }
  std::error_code ec;
  if (!src_dir.empty()) {
    std::filesystem::path lifted_symbols_path = src_dir / "lifted_symbols.ld";
    if (std::filesystem::exists(lifted_symbols_path, ec)) {
      std::ifstream ls_ifs(lifted_symbols_path);
      std::string line;
      while (std::getline(ls_ifs, line)) {
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
          continue;
        }
        std::string_view line_view = line;
        std::string_view lhs = absl::StripAsciiWhitespace(line_view.substr(0, eq_pos));
        if (absl::StartsWith(lhs, "PROVIDE(")) {
          lhs.remove_prefix(8);
          lhs = absl::StripAsciiWhitespace(lhs);
        }
        std::string_view rhs = absl::StripAsciiWhitespace(line_view.substr(eq_pos + 1));
        while (!rhs.empty() && (rhs.back() == ';' || rhs.back() == ')' ||
                                absl::ascii_isspace(static_cast<unsigned char>(rhs.back())))) {
          rhs.remove_suffix(1);
        }
        if (!lhs.empty() && (absl::StartsWith(rhs, "0x") || absl::StartsWith(rhs, "0X"))) {
          uint32_t addr = 0;
          if (absl::SimpleHexAtoi(rhs.substr(2), &addr) && addr != 0) {
            function_target_vrams.try_emplace(std::string(lhs), addr);
          }
        }
      }
    }
  }
  return function_target_vrams;
}

struct GlobalJalCounts {
  absl::flat_hash_map<std::string, int32_t> total;
  absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, int32_t>> by_module;

  absl::flat_hash_map<std::string, int32_t> ExternalForModule(std::string_view module_stem) const {
    absl::flat_hash_map<std::string, int32_t> ext = total;
    if (auto it = by_module.find(module_stem); it != by_module.end()) {
      for (const auto& [sym, cnt] : it->second) {
        auto tot_it = ext.find(sym);
        if (tot_it != ext.end()) {
          tot_it->second = std::max(0, tot_it->second - cnt);
        }
      }
    }
    return ext;
  }
};

GlobalJalCounts CollectGlobalJalCounts(
    const std::filesystem::path& asm_dir,
    const absl::flat_hash_map<std::string, uint32_t>& function_target_vrams) {
  GlobalJalCounts result;
  std::error_code ec;
  if (asm_dir.empty() || !std::filesystem::exists(asm_dir, ec)) {
    return result;
  }

  absl::flat_hash_map<uint32_t, std::vector<std::string>> vram_to_names;
  for (const auto& [name, vram] : function_target_vrams) {
    vram_to_names[vram].push_back(name);
    vram_to_names[vram].push_back(absl::StrFormat("func_%08X", vram));
  }

  for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(asm_dir, ec)) {
    if (!dir_entry.is_regular_file() || dir_entry.path().extension() != ".s") {
      continue;
    }
    std::string stem = dir_entry.path().stem().string();
    std::ifstream ifs(dir_entry.path());
    std::string line;
    while (std::getline(ifs, line)) {
      std::string_view sv = line;
      if (size_t close_comment = sv.find("*/"); close_comment != std::string_view::npos) {
        sv.remove_prefix(close_comment + 2);
      }
      sv = absl::StripAsciiWhitespace(sv);
      if (absl::StartsWith(sv, "jal\t") || absl::StartsWith(sv, "jal ")) {
        std::string_view target = absl::StripAsciiWhitespace(sv.substr(3));
        if (target.empty()) {
          continue;
        }
        std::string target_str(target);
        result.total[target_str]++;
        result.by_module[stem][target_str]++;
        if (auto it_vram = function_target_vrams.find(target_str);
            it_vram != function_target_vrams.end()) {
          for (const auto& alias : vram_to_names[it_vram->second]) {
            if (alias != target_str) {
              result.total[alias]++;
              result.by_module[stem][alias]++;
            }
          }
        } else if (absl::StartsWith(target_str, "func_") && target_str.size() == 13) {
          uint32_t addr = 0;
          if (absl::SimpleHexAtoi(target.substr(5), &addr)) {
            if (auto it_aliases = vram_to_names.find(addr); it_aliases != vram_to_names.end()) {
              for (const auto& alias : it_aliases->second) {
                if (alias != target_str) {
                  result.total[alias]++;
                  result.by_module[stem][alias]++;
                }
              }
            }
          }
        }
      }
    }
  }
  return result;
}

absl::StatusOr<RomBuildResult> VerifyBuiltRom(const RomBuilderOptions& options,
                                              const std::filesystem::path& repo_root,
                                              std::string_view expected_sha1,
                                              size_t total_objects) {
  RomBuildResult result;
  result.total_objects = total_objects;

  auto sha1_or = ComputeFileSha1(options.out_rom);
  if (!sha1_or.ok()) {
    return sha1_or.status();
  }
  result.built_sha1 = *sha1_or;
  result.expected_sha1 = std::string(expected_sha1);

  if (!expected_sha1.empty()) {
    result.sha1_matches =
        (absl::AsciiStrToLower(result.built_sha1) == absl::AsciiStrToLower(expected_sha1));
  } else {
    result.sha1_matches = true;
  }

  std::error_code ec;
  std::filesystem::path target_rom = options.verify_rom;
  if (target_rom.empty()) {
    std::filesystem::path candidate = repo_root / "roms" / absl::StrCat(options.game_name, ".z64");
    if (std::filesystem::exists(candidate, ec)) {
      target_rom = candidate;
    }
  }

  result.byte_matches = true;
  if (!target_rom.empty() && std::filesystem::exists(target_rom, ec)) {
    std::ifstream f_target(target_rom, std::ios::binary);
    std::ifstream f_built(options.out_rom, std::ios::binary);
    if (f_target && f_built) {
      std::vector<uint8_t> target_bytes((std::istreambuf_iterator<char>(f_target)),
                                        std::istreambuf_iterator<char>());
      std::vector<uint8_t> built_bytes((std::istreambuf_iterator<char>(f_built)),
                                       std::istreambuf_iterator<char>());
      if (target_bytes != built_bytes) {
        result.byte_matches = false;
        size_t min_len = std::min(target_bytes.size(), built_bytes.size());
        size_t diff_count = 0;
        for (size_t i = 0; i < min_len; ++i) {
          if (target_bytes[i] != built_bytes[i]) {
            diff_count++;
          }
        }
        diff_count += std::max(target_bytes.size(), built_bytes.size()) - min_len;
        result.differing_bytes = diff_count;
      }
    }
  }

  if (options.toolchain_mode == ToolchainMode::kModern) {
    result.success = true;
  } else {
    result.success = result.sha1_matches && result.byte_matches;
  }

  if (options.is_test && !result.success) {
    return absl::InternalError(absl::StrFormat(
        "RomBuilder: Verification failed for %s. Built SHA-1: %s, Expected SHA-1: %s, "
        "Differing bytes: %d",
        options.game_name, result.built_sha1, result.expected_sha1, result.differing_bytes));
  }

  return result;
}

}  // namespace

RomBuilder::RomBuilder(Toolchain toolchain, RomBuilderOptions options)
    : toolchain_(std::move(toolchain)), options_(std::move(options)), compiler_(toolchain_) {}

RomBuilder::RomBuilder(Toolchain toolchain, RomBuilderOptions options, Compiler compiler)
    : toolchain_(std::move(toolchain)),
      options_(std::move(options)),
      compiler_(std::move(compiler)) {}

absl::StatusOr<RomBuildResult> RomBuilder::Build() const {
  if (options_.game_name.empty()) {
    return absl::InvalidArgumentError("RomBuilder: Game name must not be empty.");
  }
  if (options_.out_elf.empty()) {
    return absl::InvalidArgumentError("RomBuilder: Output ELF path must not be empty.");
  }
  if (options_.out_rom.empty()) {
    return absl::InvalidArgumentError("RomBuilder: Output ROM path must not be empty.");
  }

  std::error_code ec;
  if (options_.out_elf.has_parent_path()) {
    std::filesystem::create_directories(options_.out_elf.parent_path(), ec);
  }
  if (options_.out_rom.has_parent_path()) {
    std::filesystem::create_directories(options_.out_rom.parent_path(), ec);
  }

  // 1. Load GameConfig and tuning flags
  auto loaded_or = LoadGameConfigAndTuningFlags(options_, toolchain_.RepoRoot());
  if (!loaded_or.ok()) {
    return loaded_or.status();
  }
  GameConfig config = std::move(loaded_or->config);
  ModuleFunctionFlagsMap function_flags_by_module = std::move(loaded_or->function_flags_by_module);
  std::string expected_sha1 = std::string(config.sha1());

  // 2. Prepare isolated object directory
  std::string obj_prefix = options_.out_elf.stem().string();
  if (obj_prefix.empty()) {
    obj_prefix = options_.game_name;
  }
  std::filesystem::path obj_dir =
      options_.out_elf.parent_path() / absl::StrCat("_", obj_prefix, "_objs");
  std::filesystem::remove_all(obj_dir, ec);
  std::filesystem::create_directories(obj_dir, ec);

  // 3. Set up symbols.ld
  std::unique_ptr<SymbolIndex> symbol_index;
  SymbolLoadOptions sym_load_opts{
      .config = &config,
      .repo_root = toolchain_.RepoRoot(),
      .game_name = options_.game_name,
  };
  if (!options_.symbols_path.empty() && std::filesystem::exists(options_.symbols_path, ec)) {
    sym_load_opts.symbols_path = options_.symbols_path;
  }
  auto loaded_index_or = LoadSymbolIndex(sym_load_opts);
  if (loaded_index_or.ok() && !loaded_index_or->Empty()) {
    symbol_index = std::make_unique<SymbolIndex>(std::move(*loaded_index_or));
  }

  std::filesystem::path target_symbols_ld = obj_dir / "symbols.ld";
  auto aux = LinkerScriptAdapter::DiscoverAuxiliaryScripts(options_.build_dir);
  if (!aux.symbols_ld.empty()) {
    std::filesystem::copy_file(aux.symbols_ld, target_symbols_ld,
                               std::filesystem::copy_options::overwrite_existing, ec);
  } else if (symbol_index != nullptr) {
    std::ofstream s_out(target_symbols_ld);
    s_out << symbol_index->GenerateSymbolsScript();
  }

  // 4. Adapt and rewrite main linker script
  auto main_script_or = LinkerScriptAdapter::FindMainScript(options_.build_dir, options_.game_name);
  if (!main_script_or.ok()) {
    return main_script_or.status();
  }
  auto adapter_or = LinkerScriptAdapter::Load(*main_script_or);
  if (!adapter_or.ok()) {
    return adapter_or.status();
  }

  const auto& obj_refs = adapter_or->ObjectReferences();
  std::string rewritten_ld = adapter_or->RewriteObjectPaths(obj_dir);
  RewriteCodeSegmentLinkerScript(config, obj_dir, &rewritten_ld);

  std::filesystem::path rewritten_ld_path = obj_dir / absl::StrCat(obj_prefix, ".ld");
  {
    std::ofstream ofs(rewritten_ld_path);
    ofs << rewritten_ld;
  }

  absl::flat_hash_map<std::string, uint32_t> function_target_vrams =
      CollectFunctionTargetVrams(symbol_index.get(), options_.src_dir);
  GlobalJalCounts global_jal_counts =
      CollectGlobalJalCounts(options_.asm_dir, function_target_vrams);

  // 5. Resolve and compile all objects
  std::filesystem::path src_dir = options_.src_dir;

  SourceResolver resolver({
      .src_dir = src_dir,
      .asm_dir = options_.asm_dir,
      .assets_dir = options_.assets_dir,
      .prefer_c = options_.prefer_c,
  });

  std::vector<std::filesystem::path> extra_includes;
  for (const auto& inc_dir : config.include_dirs()) {
    std::filesystem::path p(inc_dir);
    extra_includes.push_back(p.is_relative() ? (toolchain_.RepoRoot() / p) : p);
  }
  if (!options_.src_dir.empty()) {
    extra_includes.push_back(options_.src_dir);
  }

  if (!options_.asm_dir.empty()) {
    extra_includes.push_back(options_.asm_dir);
    if (options_.asm_dir.has_parent_path()) {
      extra_includes.push_back(options_.asm_dir.parent_path());
      if (options_.asm_dir.parent_path().has_parent_path()) {
        extra_includes.push_back(options_.asm_dir.parent_path().parent_path());
      }
    }
  }
  if (!options_.build_dir.empty()) {
    extra_includes.push_back(options_.build_dir);
  }

  std::filesystem::path macro_inc;
  if (!config.macro_inc().empty()) {
    std::filesystem::path p(config.macro_inc());
    macro_inc = p.is_relative() ? (toolchain_.RepoRoot() / p) : p;
  }

  absl::flat_hash_map<std::string, uint32_t> module_target_sizes;
  absl::flat_hash_map<std::string, std::string> module_segment_names;
  absl::flat_hash_map<std::string, uint32_t> module_rom_starts;
  for (const auto& seg : config.segments()) {
    if (seg.type() != SEGMENT_CODE) {
      continue;
    }
    for (int i = 0; i < seg.subsegments_size(); ++i) {
      const auto& sub = seg.subsegments(i);
      uint32_t sub_end =
          (i + 1 < seg.subsegments_size()) ? seg.subsegments(i + 1).rom_start() : seg.rom_end();
      if (!sub.name().empty()) {
        module_rom_starts.try_emplace(std::string(sub.name()), sub.rom_start());
      }
      if ((sub.type() == SUBSEGMENT_C || sub.type() == SUBSEGMENT_ASM) && !sub.name().empty() &&
          sub_end > sub.rom_start()) {
        module_target_sizes[std::string(sub.name())] = sub_end - sub.rom_start();
        module_segment_names[std::string(sub.name())] = std::string(seg.name());
      }
    }
  }

  std::vector<std::string> ordered_obj_refs(obj_refs.begin(), obj_refs.end());
  std::stable_sort(
      ordered_obj_refs.begin(), ordered_obj_refs.end(),
      [&](const std::string& a, const std::string& b) {
        std::string stem_a = std::filesystem::path(a).stem().string();
        std::string stem_b = std::filesystem::path(b).stem().string();
        auto it_a = module_rom_starts.find(stem_a);
        auto it_b = module_rom_starts.find(stem_b);
        uint32_t rom_a = (it_a != module_rom_starts.end()) ? it_a->second : 0xFFFFFFFFu;
        uint32_t rom_b = (it_b != module_rom_starts.end()) ? it_b->second : 0xFFFFFFFFu;
        return rom_a < rom_b;
      });

  absl::flat_hash_map<std::string, uint32_t> segment_overflow_bytes;
  for (const auto& obj_ref : ordered_obj_refs) {
    auto resolved_or = resolver.Resolve(obj_ref);
    if (!resolved_or.ok()) {
      return resolved_or.status();
    }
    std::filesystem::path target_obj = obj_dir / std::filesystem::path(obj_ref).filename();

    switch (resolved_or->type) {
      case SourceType::kC: {
        std::vector<std::string> flags = ResolveCFlags(config, resolved_or->object_stem);
        const auto* mod_fn_flags = function_flags_by_module.contains(resolved_or->object_stem)
                                       ? &function_flags_by_module.at(resolved_or->object_stem)
                                       : nullptr;
        uint32_t mod_target_size = 0;
        if (auto it_sz = module_target_sizes.find(resolved_or->object_stem);
            it_sz != module_target_sizes.end()) {
          mod_target_size = it_sz->second;
        }
        std::string seg_name;
        uint32_t initial_overflow = 0;
        if (auto it_seg = module_segment_names.find(resolved_or->object_stem);
            it_seg != module_segment_names.end()) {
          seg_name = it_seg->second;
          initial_overflow = segment_overflow_bytes[seg_name];
        }

        bool has_distinct_flags = false;
        if (mod_fn_flags != nullptr && toolchain_.Mode() == ToolchainMode::kOriginal) {
          for (const auto& [fn_name, fn_flags] : *mod_fn_flags) {
            if (fn_flags != flags) {
              has_distinct_flags = true;
              break;
            }
          }
        }

        absl::flat_hash_map<std::string, int32_t> external_jal_counts =
            global_jal_counts.ExternalForModule(resolved_or->object_stem);
        absl::Status status;
        if (has_distinct_flags) {
          status = compiler_.CompileCWithPerFunctionFlags({
              .src_file = resolved_or->source_path,
              .out_obj = target_obj,
              .default_flags = flags,
              .function_flags = *mod_fn_flags,
              .include_dirs = extra_includes,
              .macro_inc = macro_inc,
              .function_target_vrams = function_target_vrams,
              .module_target_size = mod_target_size,
              .initial_vram_offset = initial_overflow,
              .external_jal_counts = std::move(external_jal_counts),
          });
        } else {
          status = compiler_.CompileC({
              .src_file = resolved_or->source_path,
              .out_obj = target_obj,
              .opt_flags = std::move(flags),
              .include_dirs = extra_includes,
              .macro_inc = macro_inc,
              .function_target_vrams = function_target_vrams,
              .module_target_size = mod_target_size,
              .initial_vram_offset = initial_overflow,
              .external_jal_counts = std::move(external_jal_counts),
          });
        }
        if (!status.ok()) {
          return status;
        }
        if (!seg_name.empty() && mod_target_size > 0) {
          auto elf_or = ElfReader::LoadFromFile(target_obj);
          if (elf_or.ok()) {
            const ElfSection* text_sec = elf_or->FindSection(".text");
            if (text_sec != nullptr) {
              uint32_t text_sz = static_cast<uint32_t>(text_sec->data.size());
              uint32_t aligned_sz = (text_sz + 15u) & ~15u;
              uint32_t total_end = initial_overflow + aligned_sz;
              segment_overflow_bytes[seg_name] =
                  (total_end > mod_target_size) ? (total_end - mod_target_size) : 0;
            }
          }
        }
        break;
      }
      case SourceType::kCpp: {
        auto status = compiler_.CompileCpp({
            .src_file = resolved_or->source_path,
            .out_obj = target_obj,
            .include_dirs = extra_includes,
        });
        if (!status.ok()) {
          return status;
        }
        break;
      }
      case SourceType::kAssembly: {
        auto status = compiler_.Assemble({
            .src_file = resolved_or->source_path,
            .out_obj = target_obj,
            .include_dirs = {options_.asm_dir, options_.build_dir},
        });
        if (!status.ok()) {
          return status;
        }
        break;
      }
      case SourceType::kBinary: {
        auto status = compiler_.ConvertBinary({
            .src_file = resolved_or->source_path,
            .out_obj = target_obj,
        });
        if (!status.ok()) {
          return status;
        }
        break;
      }
    }
  }

  // 6. Link final ELF
  std::vector<std::filesystem::path> script_paths;
  if (std::filesystem::exists(target_symbols_ld, ec)) {
    script_paths.push_back(target_symbols_ld);
  }
  if (!aux.hardware_regs_ld.empty()) {
    script_paths.push_back(aux.hardware_regs_ld);
  }
  bool has_lifted_symbols = false;
  if (!options_.src_dir.empty()) {
    std::filesystem::path lifted_symbols_ld_script_path = options_.src_dir / "lifted_symbols.ld";
    if (std::filesystem::exists(lifted_symbols_ld_script_path, ec)) {
      script_paths.push_back(lifted_symbols_ld_script_path);
      has_lifted_symbols = true;
    }
  }
  if (!has_lifted_symbols) {
    if (!aux.undefined_syms_auto.empty()) {
      script_paths.push_back(aux.undefined_syms_auto);
    }
    if (!aux.undefined_funcs_auto.empty()) {
      script_paths.push_back(aux.undefined_funcs_auto);
    }
  }

  auto link_status = compiler_.Link({
      .script_paths = script_paths,
      .main_ld_script = rewritten_ld_path,
      .out_elf = options_.out_elf,
  });
  if (!link_status.ok()) {
    return link_status;
  }

  // 7. Extract raw ROM binary
  auto extract_status = compiler_.ExtractRom({
      .in_elf = options_.out_elf,
      .out_rom = options_.out_rom,
  });
  if (!extract_status.ok()) {
    return extract_status;
  }

  // Pad ROM to expected cartridge size based on configuration segments
  uint64_t expected_rom_size = 0;
  for (const auto& segment : config.segments()) {
    expected_rom_size = std::max<uint64_t>(expected_rom_size, segment.rom_end());
  }
  uint64_t current_rom_size = std::filesystem::file_size(options_.out_rom, ec);
  if (expected_rom_size > current_rom_size) {
    std::ofstream rom_append_stream(options_.out_rom, std::ios::binary | std::ios::app);
    if (rom_append_stream.is_open()) {
      std::vector<char> padding_buffer(expected_rom_size - current_rom_size, 0);
      rom_append_stream.write(padding_buffer.data(), padding_buffer.size());
    }
  }

  // 8. Verification
  return VerifyBuiltRom(options_, toolchain_.RepoRoot(), expected_sha1, obj_refs.size());
}

absl::Status RomBuilder::CompileCWithPerFunctionFlags(
    const PerFunctionCCompileOptions& options) const {
  return compiler_.CompileCWithPerFunctionFlags(options);
}

}  // namespace rom_nom_nom
