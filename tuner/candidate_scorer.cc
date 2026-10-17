#include "tuner/candidate_scorer.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "core/candidate_compiler.h"
#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom {

bool CandidateScore::IsBetterThan(const CandidateScore& other) const {
  if (raw_bit_exact != other.raw_bit_exact) {
    return raw_bit_exact;
  }
  if (normalized_exact != other.normalized_exact) {
    return normalized_exact;
  }
  if (bit_exact_instructions != other.bit_exact_instructions) {
    return bit_exact_instructions > other.bit_exact_instructions;
  }
  if (matching_instructions != other.matching_instructions) {
    return matching_instructions > other.matching_instructions;
  }
  if (SizeDelta() != other.SizeDelta()) {
    return SizeDelta() < other.SizeDelta();
  }
  if (differing_instructions != other.differing_instructions) {
    return differing_instructions < other.differing_instructions;
  }
  return false;
}

CandidateScorer::CandidateScorer(const TargetExtractor* extractor, const SymbolIndex* symbols)
    : extractor_(extractor),
      symbols_(symbols != nullptr ? symbols
                                  : (extractor != nullptr ? extractor->Symbols() : nullptr)) {}

CandidateScore CandidateScorer::ScoreInstructions(
    const TargetFunction& target, absl::Span<const Instruction> compiled_instructions,
    absl::Span<const uint32_t> patched_words) const {
  const size_t target_len = target.instructions.size();
  if (target_len >= 2 && compiled_instructions.size() > target_len &&
      compiled_instructions.size() - target_len <= 3 && (compiled_instructions.size() % 4) == 0) {
    bool all_trailing_nops = true;
    for (size_t i = target_len; i < compiled_instructions.size(); ++i) {
      if (!compiled_instructions[i].IsNop()) {
        all_trailing_nops = false;
        break;
      }
    }
    if (all_trailing_nops) {
      compiled_instructions = compiled_instructions.subspan(0, target_len);
      if (patched_words.size() >= target_len) {
        patched_words = patched_words.subspan(0, target_len);
      }
    }
  }

  FunctionDiffContext diff_ctx;
  diff_ctx.target_vram = target.vram;
  diff_ctx.target_size =
      target.size > 0 ? target.size : static_cast<uint32_t>(target.instructions.size() * 4);
  diff_ctx.compiled_vram = target.vram;
  diff_ctx.compiled_size = static_cast<uint32_t>(compiled_instructions.size() * 4);

  Normalizer normalizer(diff_ctx);

  std::vector<NormalizedInstruction> norm_target;
  norm_target.reserve(target.instructions.size());
  for (const auto& inst : target.instructions) {
    norm_target.push_back(normalizer.NormalizeTarget(inst));
  }

  std::vector<NormalizedInstruction> norm_compiled;
  norm_compiled.reserve(compiled_instructions.size());
  for (const auto& inst : compiled_instructions) {
    norm_compiled.push_back(normalizer.NormalizeCompiled(inst));
  }

  SequenceAligner aligner(&normalizer);
  AlignmentResult alignment = aligner.Align(norm_target, norm_compiled);

  CandidateScore score;
  score.bit_exact_instructions = alignment.stats.bit_exact_instructions;
  score.matching_instructions = alignment.stats.matching_instructions;
  score.differing_instructions = alignment.stats.differing_instructions;
  score.target_instruction_count = alignment.stats.target_instruction_count;
  score.compiled_instruction_count = alignment.stats.compiled_instruction_count;
  score.normalized_exact = alignment.stats.IsBitExactMatch();
  score.patched_words.assign(patched_words.begin(), patched_words.end());

  if (!target.raw_words.empty() && patched_words.size() == target.raw_words.size()) {
    bool all_equal = true;
    for (size_t i = 0; i < target.raw_words.size(); ++i) {
      if (patched_words[i] != target.raw_words[i]) {
        all_equal = false;
        break;
      }
    }
    score.raw_bit_exact = all_equal;
  } else if (score.normalized_exact &&
             score.bit_exact_instructions == score.target_instruction_count) {
    score.raw_bit_exact = true;
  }

  return score;
}

absl::StatusOr<CandidateScore> CandidateScorer::CompileAndScore(
    const TargetFunction& target, std::string_view c_code, const std::vector<std::string>& c_flags,
    const std::vector<std::filesystem::path>& extra_include_dirs) const {
  if (extractor_ == nullptr) {
    return absl::FailedPreconditionError("CandidateScorer: TargetExtractor is required.");
  }

  static std::atomic<uint64_t> temp_counter{0};
  uint64_t id = temp_counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() / absl::StrCat("tuner_cand_", id);
  std::error_code ec;
  std::filesystem::create_directories(temp_dir, ec);
  std::filesystem::path src_file = temp_dir / absl::StrCat(target.name, ".c");

  {
    std::ofstream out(src_file);
    if (!out.is_open()) {
      std::filesystem::remove_all(temp_dir, ec);
      return absl::InternalError("CandidateScorer: Failed to write temporary C file.");
    }
    out << c_code;
  }

  CandidateCompiler compiler({
      .repo_root = extractor_->Options().repo_root,
      .game_name = extractor_->Options().game_name,
      .config = extractor_->Config(),
      .symbols = symbols_,
  });
  auto compiled_or =
      compiler.CompileFunction(src_file, target.name, target.vram, c_flags, extra_include_dirs);
  std::filesystem::remove_all(temp_dir, ec);
  if (!compiled_or.ok()) {
    return compiled_or.status();
  }

  return ScoreInstructions(target, compiled_or->instructions, compiled_or->patched_words);
}

bool CandidateScorer::VerifyEquivalence(const TargetFunction& target,
                                        absl::Span<const uint32_t> candidate_words,
                                        size_t num_vectors) const {
  if (target.raw_words.empty() || candidate_words.empty()) {
    return false;
  }
  fuzzer::DifferentialFuzzer diff_fuzzer;
  auto cases = fuzzer::DifferentialFuzzer::GenerateBoundaryCases();
  auto random_cases = fuzzer::DifferentialFuzzer::GenerateRandomCases(num_vectors, /*seed=*/42);
  cases.insert(cases.end(), random_cases.begin(), random_cases.end());

  std::vector<uint32_t> cand_vec(candidate_words.begin(), candidate_words.end());
  for (const auto& tc : cases) {
    auto comp = diff_fuzzer.Compare(target.raw_words, target.vram, cand_vec, target.vram, tc);
    if (comp.result != fuzzer::EquivalenceResult::kEquivalent) {
      return false;
    }
  }
  return true;
}

}  // namespace rom_nom_nom
