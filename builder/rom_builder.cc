#include "builder/rom_builder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
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
#include "core/sha1.h"
#include "splitter/config.h"
#include "splitter/config.pb.h"

namespace rom_nom_nom {

absl::StatusOr<std::string> ComputeFileSha1(const std::filesystem::path& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return absl::NotFoundError(
        absl::StrFormat("Could not open file '%s' for SHA-1 calculation.", path.string()));
  }
  Sha1 sha1;
  std::array<char, 65536> buffer;
  while (ifs.read(buffer.data(), buffer.size()) || ifs.gcount() > 0) {
    sha1.Update(std::string_view(buffer.data(), ifs.gcount()));
  }
  return sha1.FinalizeHex();
}

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

  // 1. Load SplitConfig if provided
  SplitConfig config;
  if (!options_.config_path.empty() && std::filesystem::exists(options_.config_path, ec)) {
    auto config_or = LoadSplitConfig(options_.config_path);
    if (!config_or.ok()) {
      return config_or.status();
    }
    config = std::move(*config_or);
  }

  std::string expected_sha1 = std::string(config.sha1());
  std::vector<std::string> default_c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  auto default_it = config.c_flags().find("default");
  if (default_it != config.c_flags().end()) {
    default_c_flags.assign(default_it->second.flags().begin(), default_it->second.flags().end());
  }

  // 2. Prepare isolated object directory
  std::filesystem::path obj_dir =
      options_.out_elf.parent_path() / absl::StrCat("_", options_.game_name, "_objs");
  std::filesystem::remove_all(obj_dir, ec);
  std::filesystem::create_directories(obj_dir, ec);

  // 3. Set up symbols.ld
  std::filesystem::path target_symbols_ld = obj_dir / "symbols.ld";
  auto aux = LinkerScriptAdapter::DiscoverAuxiliaryScripts(options_.build_dir);
  if (!aux.symbols_ld.empty()) {
    std::filesystem::copy_file(aux.symbols_ld, target_symbols_ld,
                               std::filesystem::copy_options::overwrite_existing, ec);
  } else if (!options_.symbols_path.empty() && std::filesystem::exists(options_.symbols_path, ec)) {
    std::ifstream s_in(options_.symbols_path);
    std::ofstream s_out(target_symbols_ld);
    s_out << "/* Generated symbols */\n";
    std::string line;
    static const std::regex sym_re(R"(^([a-zA-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+)\s*;)");
    while (std::getline(s_in, line)) {
      size_t comment_pos = line.find("//");
      if (comment_pos != std::string::npos) {
        line = line.substr(0, comment_pos);
      }
      std::smatch match;
      if (std::regex_search(line, match, sym_re)) {
        s_out << match[1].str() << " = " << match[2].str() << ";\n";
      }
    }
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
  std::filesystem::path rewritten_ld_path = obj_dir / absl::StrCat(options_.game_name, ".ld");
  {
    std::ofstream ofs(rewritten_ld_path);
    ofs << rewritten_ld;
  }

  // 5. Resolve and compile all objects
  SourceResolver resolver({
      .src_dir = toolchain_.RepoRoot() / "src",
      .asm_dir = options_.asm_dir,
      .assets_dir = options_.assets_dir,
  });

  std::vector<std::filesystem::path> extra_includes = {
      toolchain_.RepoRoot() / "src" / options_.game_name,
      toolchain_.RepoRoot() / "src" / "c" / options_.game_name,
      toolchain_.RepoRoot() / "src" / "cc" / options_.game_name,
      toolchain_.RepoRoot(),
  };

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

  std::filesystem::path macro_inc = FindMacroInc(toolchain_.RepoRoot(), options_.game_name);

  for (const auto& obj_ref : obj_refs) {
    auto resolved_or = resolver.Resolve(obj_ref);
    if (!resolved_or.ok()) {
      return resolved_or.status();
    }
    std::filesystem::path target_obj = obj_dir / absl::StrCat(resolved_or->object_stem, ".o");

    switch (resolved_or->type) {
      case SourceType::kC: {
        std::vector<std::string> flags = default_c_flags;
        auto flag_it = config.c_flags().find(resolved_or->object_stem);
        if (flag_it != config.c_flags().end()) {
          flags.assign(flag_it->second.flags().begin(), flag_it->second.flags().end());
        }
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
  if (!aux.undefined_syms_auto.empty()) {
    script_paths.push_back(aux.undefined_syms_auto);
  }
  if (!aux.undefined_funcs_auto.empty()) {
    script_paths.push_back(aux.undefined_funcs_auto);
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
