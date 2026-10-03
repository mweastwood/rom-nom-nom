#include "differ/differ_pipeline.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/compiler.h"
#include "core/mips.h"
#include "core/toolchain.h"
#include "differ/diff_formatter.h"
#include "differ/elf_reader.h"
#include "differ/normalizer.h"
#include "differ/relocation_applier.h"
#include "differ/sequence_aligner.h"
#include "differ/target_extractor.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

namespace {

// Default compiler flags if unspecified in SplitConfig.
const std::vector<std::string>& DefaultCompilerFlags() {
  static const auto* k_flags =
      new std::vector<std::string>({"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  return *k_flags;
}

std::filesystem::path FindRepoRoot() {
  const char* ws_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  if (ws_dir != nullptr && *ws_dir != '\0') {
    return ws_dir;
  }
  std::filesystem::path cur = std::filesystem::current_path();
  while (!cur.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(cur / "MODULE.bazel", ec) ||
        std::filesystem::exists(cur / "WORKSPACE", ec)) {
      return cur;
    }
    auto parent = cur.parent_path();
    if (parent == cur) {
      break;
    }
    cur = parent;
  }
  return std::filesystem::current_path();
}

}  // namespace

DifferPipeline::DifferPipeline(DifferPipelineOptions options) : options_(std::move(options)) {
  if (!options_.repo_root.empty()) {
    repo_root_ = options_.repo_root;
  } else {
    repo_root_ = FindRepoRoot();
  }
}

absl::StatusOr<DifferResult> DifferPipeline::Diff(std::string_view func_name) const {
  // 1. Initialize TargetExtractor with game configuration and symbol index.
  auto extractor_or = TargetExtractor::Create({
      .repo_root = repo_root_,
      .game_name = options_.game_name,
      .config_path = options_.config_path,
      .symbols_path = options_.symbols_path,
      .rom_path = options_.rom_path,
  });
  if (!extractor_or.ok()) {
    return extractor_or.status();
  }
  auto extractor = std::move(*extractor_or);

  // 2. Extract target instructions from retail ROM binary (or nonmatching ASM fallback).
  auto target_or = extractor->ExtractTarget(func_name);
  if (!target_or.ok()) {
    return target_or.status();
  }
  const TargetFunction& target_func = *target_or;
  std::string canonical_name = target_func.name;

  // 3. Locate the C source file if an explicit path was supplied.
  std::optional<std::filesystem::path> c_file;
  if (!options_.source_file.empty()) {
    std::filesystem::path resolved_source = options_.source_file;
    if (resolved_source.is_relative()) {
      resolved_source = repo_root_ / resolved_source;
    }
    std::error_code ec;
    if (!std::filesystem::exists(resolved_source, ec)) {
      return absl::NotFoundError(absl::StrFormat("Specified source file does not exist: %s",
                                                 options_.source_file.string()));
    }
    c_file = resolved_source;
  }

  std::vector<Instruction> compiled_instructions;
  uint32_t compiled_vram = 0;
  uint32_t compiled_size = 0;

  // 4. If C source exists, compile it with GCC 2.7.2, read object, and apply relocations.
  if (c_file.has_value()) {
    auto tc_or = Toolchain::Discover({
        .repo_root = repo_root_,
        .mode = ToolchainMode::kOriginal,
    });
    if (!tc_or.ok()) {
      return tc_or.status();
    }
    Compiler compiler(*tc_or);

    // Determine module compiler flags from SplitConfig.
    std::vector<std::string> flags = DefaultCompilerFlags();
    if (extractor->Config() != nullptr) {
      const auto& c_flags_map = extractor->Config()->c_flags();
      auto it = c_flags_map.find(c_file->stem().string());
      if (it != c_flags_map.end()) {
        flags.assign(it->second.flags().begin(), it->second.flags().end());
      } else {
        auto def_it = c_flags_map.find("default");
        if (def_it != c_flags_map.end()) {
          flags.assign(def_it->second.flags().begin(), def_it->second.flags().end());
        }
      }
    }

    std::filesystem::path asm_dir = repo_root_ / "bazel-bin" / "asm" / options_.game_name;
    std::error_code ec;
    if (!std::filesystem::exists(asm_dir, ec)) {
      asm_dir = repo_root_ / "asm" / options_.game_name;
    }

    std::vector<std::filesystem::path> includes = {
        repo_root_,
        repo_root_ / "src" / options_.game_name,
        repo_root_ / "src" / "c" / options_.game_name,
        repo_root_ / "src" / "cc" / options_.game_name,
        asm_dir,
        repo_root_ / "bazel-bin",
    };
    if (asm_dir.has_parent_path()) {
      includes.push_back(asm_dir.parent_path());
    }

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_obj =
        temp_dir / absl::StrCat("diff_", c_file->stem().string(), "_", func_name, ".o");

    auto compile_status = compiler.CompileC({
        .src_file = *c_file,
        .out_obj = temp_obj,
        .opt_flags = flags,
        .include_dirs = includes,
        .macro_inc = FindMacroInc(repo_root_, options_.game_name),
    });
    if (!compile_status.ok()) {
      return absl::InternalError(absl::StrCat(
          "Compilation failed for ", c_file->filename().string(), ":\n", compile_status.message()));
    }

    auto reader_or = ElfReader::LoadFromFile(temp_obj);
    std::filesystem::remove(temp_obj, ec);
    if (!reader_or.ok()) {
      return reader_or.status();
    }

    auto func_or = reader_or->ExtractFunction(canonical_name);
    if (!func_or.ok()) {
      func_or = reader_or->ExtractFunction(func_name);
    }
    if (!func_or.ok()) {
      return absl::NotFoundError(
          absl::StrFormat("Function '%s' not found in compiled object file %s: %s", canonical_name,
                          c_file->filename().string(), func_or.status().message()));
    }
    const ElfFunction& matched_elf_func = *func_or;

    compiled_vram = matched_elf_func.section_offset;
    compiled_size = matched_elf_func.size;

    // Apply relocations to compiled object code using symbol registry and section base.
    uint32_t text_base_vram = target_func.vram - matched_elf_func.section_offset;
    auto symbol_lookup = [&](std::string_view name) -> std::optional<uint32_t> {
      if (name == ".text") {
        return text_base_vram;
      }
      if (extractor->Symbols() != nullptr) {
        const auto* entry = extractor->Symbols()->FindByName(name);
        if (entry != nullptr) {
          return entry->address();
        }
      }
      size_t last_underscore = name.rfind('_');
      if (last_underscore != std::string_view::npos && last_underscore + 1 < name.size() &&
          name.size() - (last_underscore + 1) == 8) {
        uint32_t addr = 0;
        if (absl::SimpleHexAtoi(name.substr(last_underscore + 1), &addr)) {
          return addr;
        }
      }
      return std::nullopt;
    };
    RelocationApplyResult applied = RelocationApplier::Apply(matched_elf_func, symbol_lookup);

    for (size_t i = 0; i < applied.patched_words.size(); ++i) {
      auto inst_or =
          DecodeInstruction(applied.patched_words[i], matched_elf_func.section_offset + i * 4);
      if (inst_or.ok()) {
        compiled_instructions.push_back(*inst_or);
      }
    }
  }

  // 5. Normalization: map branch/jump targets to function-relative offsets.
  FunctionDiffContext diff_ctx;
  diff_ctx.target_vram = target_func.vram;
  diff_ctx.target_size = target_func.size;
  diff_ctx.compiled_vram = compiled_vram;
  diff_ctx.compiled_size = compiled_size;

  Normalizer normalizer(diff_ctx);

  std::vector<NormalizedInstruction> target_norm;
  target_norm.reserve(target_func.instructions.size());
  for (const auto& inst : target_func.instructions) {
    target_norm.push_back(normalizer.NormalizeTarget(inst));
  }

  std::vector<NormalizedInstruction> compiled_norm;
  compiled_norm.reserve(compiled_instructions.size());
  for (const auto& inst : compiled_instructions) {
    compiled_norm.push_back(normalizer.NormalizeCompiled(inst));
  }

  // 6. Sequence Alignment: compute globally optimal aligned rows.
  SequenceAligner aligner(&normalizer);
  AlignmentResult alignment = aligner.Align(target_norm, compiled_norm);

  // 7. Visual Formatting: side-by-side terminal columns with colors and summary.
  DiffFormatter formatter(options_.format_options, extractor->Symbols());
  std::string source_display =
      c_file.has_value() ? c_file->lexically_relative(repo_root_).string() : "Not yet in C";
  std::string formatted_output = formatter.FormatDiff(func_name, alignment, source_display);

  DifferResult result;
  result.func_name = std::string(func_name);
  result.canonical_name = canonical_name;
  result.vram = target_func.vram;
  result.target_instructions_count = target_func.instructions.size();
  result.compiled_instructions_count = compiled_instructions.size();
  result.source_file = c_file;
  result.alignment = std::move(alignment);
  result.formatted_output = std::move(formatted_output);
  result.is_bit_exact = result.alignment.stats.IsBitExactMatch();

  return result;
}

}  // namespace rom_nom_nom
