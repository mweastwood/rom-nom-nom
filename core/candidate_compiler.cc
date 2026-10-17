#include "core/candidate_compiler.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/compiler.h"
#include "core/elf_reader.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/relocation_applier.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

CandidateCompiler::CandidateCompiler(CandidateCompileOptions options)
    : options_(std::move(options)) {
  options_.repo_root = FindRepoRoot(options_.repo_root);
}

absl::StatusOr<CompiledCandidateFunction> CandidateCompiler::CompileFunction(
    const std::filesystem::path& source_file, std::string_view func_name,
    uint32_t target_vram) const {
  std::filesystem::path resolved_source = source_file;
  if (resolved_source.is_relative()) {
    resolved_source = options_.repo_root / resolved_source;
  }
  std::error_code ec;
  if (!std::filesystem::exists(resolved_source, ec)) {
    return absl::NotFoundError(
        absl::StrFormat("Specified source file does not exist: %s", source_file.string()));
  }

  auto tc_or = Toolchain::Discover({
      .repo_root = options_.repo_root,
      .mode = ToolchainMode::kOriginal,
  });
  if (!tc_or.ok()) {
    return tc_or.status();
  }
  Compiler compiler(*tc_or);

  std::vector<std::string> flags;
  if (options_.config != nullptr) {
    flags = ResolveCFlags(*options_.config, resolved_source.stem().string());
  }
  if (flags.empty()) {
    flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  }

  std::filesystem::path asm_dir = options_.repo_root / "bazel-bin" / "asm" / options_.game_name;
  if (!std::filesystem::exists(asm_dir, ec)) {
    asm_dir = options_.repo_root / "asm" / options_.game_name;
  }

  std::vector<std::filesystem::path> includes;
  if (options_.config != nullptr && options_.config->include_dirs_size() > 0) {
    for (const auto& dir : options_.config->include_dirs()) {
      std::filesystem::path p(dir);
      includes.push_back(p.is_relative() ? (options_.repo_root / p) : p);
    }
  } else {
    includes = {
        options_.repo_root,
        options_.repo_root / "src" / options_.game_name,
        options_.repo_root / "src" / "c" / options_.game_name,
        options_.repo_root / "src" / "cc" / options_.game_name,
    };
  }
  includes.push_back(asm_dir);
  includes.push_back(options_.repo_root / "bazel-bin");
  if (asm_dir.has_parent_path()) {
    includes.push_back(asm_dir.parent_path());
  }

  std::filesystem::path macro_inc;
  if (options_.config != nullptr && !options_.config->macro_inc().empty()) {
    std::filesystem::path p(options_.config->macro_inc());
    macro_inc = p.is_relative() ? (options_.repo_root / p) : p;
  } else {
    macro_inc = FindMacroInc(options_.repo_root, options_.game_name);
  }

  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  std::filesystem::path temp_obj =
      temp_dir / absl::StrCat("diff_", resolved_source.stem().string(), "_", func_name, ".o");

  auto compile_status = compiler.CompileC({
      .src_file = resolved_source,
      .out_obj = temp_obj,
      .opt_flags = flags,
      .include_dirs = includes,
      .macro_inc = macro_inc,
  });
  if (!compile_status.ok()) {
    return absl::InternalError(absl::StrCat("Compilation failed for ",
                                            resolved_source.filename().string(), ":\n",
                                            compile_status.message()));
  }

  auto reader_or = ElfReader::LoadFromFile(temp_obj);
  std::filesystem::remove(temp_obj, ec);
  if (!reader_or.ok()) {
    return reader_or.status();
  }

  auto func_or = reader_or->ExtractFunction(func_name);
  if (!func_or.ok()) {
    return absl::NotFoundError(
        absl::StrFormat("Function '%s' not found in compiled object file %s: %s", func_name,
                        resolved_source.filename().string(), func_or.status().message()));
  }
  const ElfFunction& matched_elf_func = *func_or;

  CompiledCandidateFunction out;
  out.section_offset = matched_elf_func.section_offset;
  out.size = matched_elf_func.size;

  uint32_t text_base_vram = target_vram - matched_elf_func.section_offset;
  auto symbol_lookup = [&](std::string_view name) -> std::optional<uint32_t> {
    if (name == ".text") {
      return text_base_vram;
    }
    return ResolveSymbolOrSynthesizedAddress(name, options_.symbols);
  };
  RelocationApplyResult applied = RelocationApplier::Apply(matched_elf_func, symbol_lookup);
  out.patched_words = std::move(applied.patched_words);
  out.instructions.reserve(out.patched_words.size());
  for (size_t i = 0; i < out.patched_words.size(); ++i) {
    auto inst_or =
        DecodeInstruction(out.patched_words[i], target_vram + static_cast<uint32_t>(i * 4));
    if (inst_or.ok()) {
      out.instructions.push_back(*inst_or);
    }
  }
  return out;
}

}  // namespace rom_nom_nom
