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

namespace {

size_t CountTrailingSectionPaddingNops(uint32_t start_vram, absl::Span<const Instruction> insts) {
  if (insts.size() < 3 || ((start_vram + static_cast<uint32_t>(insts.size() * 4)) % 16) != 0) {
    return 0;
  }
  for (size_t k = 1; k <= 3 && insts.size() >= k + 2; ++k) {
    if (!insts[insts.size() - k].IsNop()) {
      break;
    }
    const auto& term = insts[insts.size() - k - 2];
    bool is_unconditional_exit =
        term.IsReturn() || term.opcode == Opcode::kJ ||
        (term.opcode == Opcode::kBeq && term.rs == Register::kZero && term.rt == Register::kZero);
    if (!is_unconditional_exit) {
      continue;
    }
    uint32_t pad_start_vram = insts[insts.size() - k].vram;
    bool branch_targets_padding = false;
    for (size_t i = 0; i <= insts.size() - k - 2; ++i) {
      if (insts[i].IsBranch() && insts[i].BranchTarget() >= pad_start_vram) {
        branch_targets_padding = true;
        break;
      }
    }
    if (!branch_targets_padding) {
      return k;
    }
  }
  return 0;
}

}  // namespace

CandidateScore CandidateScorer::ScoreInstructions(
    const TargetFunction& target, absl::Span<const Instruction> compiled_instructions,
    absl::Span<const uint32_t> patched_words) const {
  absl::Span<const Instruction> target_insts = target.instructions;
  absl::Span<const uint32_t> target_words = target.raw_words;

  const size_t target_pad = CountTrailingSectionPaddingNops(target.vram, target_insts);
  const size_t compiled_pad =
      CountTrailingSectionPaddingNops(/*start_vram=*/0, compiled_instructions);

  std::vector<Instruction> adjusted_compiled_insts;
  std::vector<uint32_t> adjusted_patched_words;
  if (compiled_pad > target_pad) {
    const size_t excess = compiled_pad - target_pad;
    if (compiled_instructions.size() > excess) {
      compiled_instructions =
          compiled_instructions.subspan(0, compiled_instructions.size() - excess);
      if (patched_words.size() >= compiled_instructions.size()) {
        patched_words = patched_words.subspan(0, compiled_instructions.size());
      }
    }
  } else if (compiled_pad < target_pad && !compiled_instructions.empty()) {
    const size_t deficit = target_pad - compiled_pad;
    adjusted_compiled_insts.assign(compiled_instructions.begin(), compiled_instructions.end());
    adjusted_patched_words.assign(patched_words.begin(), patched_words.end());
    for (size_t i = 0; i < deficit; ++i) {
      Instruction nop_inst = target_insts.back();
      nop_inst.vram = adjusted_compiled_insts.back().vram + 4;
      adjusted_compiled_insts.push_back(nop_inst);
      if (!adjusted_patched_words.empty()) {
        adjusted_patched_words.push_back(0x00000000);
      }
    }
    compiled_instructions = adjusted_compiled_insts;
    if (!adjusted_patched_words.empty()) {
      patched_words = adjusted_patched_words;
    }
  }

  FunctionDiffContext diff_ctx;
  diff_ctx.target_vram = target.vram;
  diff_ctx.target_size =
      target.size > 0 ? target.size : static_cast<uint32_t>(target_insts.size() * 4);
  diff_ctx.compiled_vram = target.vram;
  diff_ctx.compiled_size = static_cast<uint32_t>(compiled_instructions.size() * 4);

  Normalizer normalizer(diff_ctx);

  std::vector<NormalizedInstruction> norm_target;
  norm_target.reserve(target_insts.size());
  for (const auto& inst : target_insts) {
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

  if (!target_words.empty() && patched_words.size() == target_words.size()) {
    bool all_equal = true;
    for (size_t i = 0; i < target_words.size(); ++i) {
      if (patched_words[i] != target_words[i]) {
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
  if (candidate_words == absl::MakeConstSpan(target.raw_words)) {
    return true;
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
