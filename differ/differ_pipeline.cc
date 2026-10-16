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
#include "core/elf_reader.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/relocation_applier.h"
#include "core/symbol_registry.h"
#include "core/target_extractor.h"
#include "core/toolchain.h"
#include "differ/diff_formatter.h"
#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"

namespace rom_nom_nom {

namespace {

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
    auto cand_or = extractor->CompileCandidateFunction(*c_file, canonical_name, target_func.vram);
    if (absl::IsNotFound(cand_or.status()) && canonical_name != func_name) {
      cand_or = extractor->CompileCandidateFunction(*c_file, func_name, target_func.vram);
    }
    if (!cand_or.ok()) {
      return cand_or.status();
    }
    compiled_vram = target_func.vram;
    compiled_size = cand_or->size;
    compiled_instructions = std::move(cand_or->instructions);
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
