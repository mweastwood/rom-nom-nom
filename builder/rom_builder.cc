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

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/game_config.h"
#include "core/game_config.pb.h"
#include "core/sha1.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

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

  // 1. Load GameConfig if provided
  GameConfig config;
  if (!options_.config_path.empty() && std::filesystem::exists(options_.config_path, ec)) {
    auto config_or = LoadGameConfig(options_.config_path);
    if (!config_or.ok()) {
      return config_or.status();
    }
    config = std::move(*config_or);
  }
  if (!config.tuning_cache_file().empty()) {
    std::filesystem::path cache_file(config.tuning_cache_file());
    if (cache_file.is_relative()) {
      cache_file = toolchain_.RepoRoot() / cache_file;
    }
    if (std::filesystem::exists(cache_file, ec)) {
      auto cache_or = LoadTuningCache(cache_file);
      if (cache_or.ok()) {
        for (const auto& [module_name, flags] : cache_or->module_c_flags()) {
          (*config.mutable_c_flags())[module_name] = flags;
        }
      }
    }
  }
  if (!options_.src_dir.empty()) {
    std::filesystem::path tuned_flags_path = options_.src_dir / "tuned_c_flags.textproto";
    if (std::filesystem::exists(tuned_flags_path, ec)) {
      auto cache_or = LoadTuningCache(tuned_flags_path);
      if (cache_or.ok()) {
        for (const auto& [module_name, flags] : cache_or->module_c_flags()) {
          (*config.mutable_c_flags())[module_name] = flags;
        }
      }
    }
  }

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

  std::filesystem::path rewritten_ld_path = obj_dir / absl::StrCat(obj_prefix, ".ld");
  {
    std::ofstream ofs(rewritten_ld_path);
    ofs << rewritten_ld;
  }

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

  for (const auto& obj_ref : obj_refs) {
    auto resolved_or = resolver.Resolve(obj_ref);
    if (!resolved_or.ok()) {
      return resolved_or.status();
    }
    std::filesystem::path target_obj = obj_dir / std::filesystem::path(obj_ref).filename();

    switch (resolved_or->type) {
      case SourceType::kC: {
        std::vector<std::string> flags = ResolveCFlags(config, resolved_or->object_stem);
        auto status = compiler_.CompileC({
            .src_file = resolved_or->source_path,
            .out_obj = target_obj,
            .opt_flags = std::move(flags),
            .include_dirs = extra_includes,
            .macro_inc = macro_inc,
        });
        if (!status.ok()) {
          return status;
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
  RomBuildResult result;
  result.total_objects = obj_refs.size();

  auto sha1_or = ComputeFileSha1(options_.out_rom);
  if (!sha1_or.ok()) {
    return sha1_or.status();
  }
  result.built_sha1 = *sha1_or;
  result.expected_sha1 = expected_sha1;

  if (!expected_sha1.empty()) {
    result.sha1_matches =
        (absl::AsciiStrToLower(result.built_sha1) == absl::AsciiStrToLower(expected_sha1));
  } else {
    result.sha1_matches = true;
  }

  std::filesystem::path target_rom = options_.verify_rom;
  if (target_rom.empty()) {
    std::filesystem::path candidate =
        toolchain_.RepoRoot() / "roms" / absl::StrCat(options_.game_name, ".z64");
    if (std::filesystem::exists(candidate, ec)) {
      target_rom = candidate;
    }
  }

  result.byte_matches = true;
  if (!target_rom.empty() && std::filesystem::exists(target_rom, ec)) {
    std::ifstream f_target(target_rom, std::ios::binary);
    std::ifstream f_built(options_.out_rom, std::ios::binary);
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

  if (options_.toolchain_mode == ToolchainMode::kModern) {
    result.success = true;
  } else {
    result.success = result.sha1_matches && result.byte_matches;
  }

  if (options_.is_test && !result.success) {
    return absl::InternalError(absl::StrFormat(
        "RomBuilder: Verification failed for %s. Built SHA-1: %s, Expected SHA-1: %s, "
        "Differing bytes: %d",
        options_.game_name, result.built_sha1, result.expected_sha1, result.differing_bytes));
  }

  return result;
}

}  // namespace rom_nom_nom
