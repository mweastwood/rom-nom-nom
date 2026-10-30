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
#include "core/register_tracker.h"
#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"
#include "fuzzer/candidate_symbol_map.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom {

bool CandidateScore::IsBetterThan(const CandidateScore& other) const {
  if (raw_bit_exact != other.raw_bit_exact) {
    return raw_bit_exact;
  }
  if (normalized_exact != other.normalized_exact) {
    return normalized_exact;
  }
  if (StructuralMatchCount() != other.StructuralMatchCount()) {
    return StructuralMatchCount() > other.StructuralMatchCount();
  }
  if (cfg_topology_matches != other.cfg_topology_matches) {
    return cfg_topology_matches;
  }
  if (saved_registers_match != other.saved_registers_match) {
    return saved_registers_match;
  }
  if (frame_size_matches != other.frame_size_matches) {
    return frame_size_matches;
  }
  const int64_t net_bit_exact =
      static_cast<int64_t>(bit_exact_instructions) * 2 - static_cast<int64_t>(SizeDelta());
  const int64_t other_net_bit_exact = static_cast<int64_t>(other.bit_exact_instructions) * 2 -
                                      static_cast<int64_t>(other.SizeDelta());
  if (net_bit_exact != other_net_bit_exact) {
    return net_bit_exact > other_net_bit_exact;
  }
  if (bit_exact_instructions != other.bit_exact_instructions) {
    return bit_exact_instructions > other.bit_exact_instructions;
  }
  if (IsOpcodeSequenceExact() != other.IsOpcodeSequenceExact()) {
    return IsOpcodeSequenceExact();
  }
  const int64_t net_matching =
      static_cast<int64_t>(matching_instructions) * 2 - static_cast<int64_t>(SizeDelta());
  const int64_t other_net_matching = static_cast<int64_t>(other.matching_instructions) * 2 -
                                     static_cast<int64_t>(other.SizeDelta());
  if (net_matching != other_net_matching) {
    return net_matching > other_net_matching;
  }
  if (matching_instructions != other.matching_instructions) {
    return matching_instructions > other.matching_instructions;
  }
  const int64_t net_lcs =
      static_cast<int64_t>(normalized_lcs_instructions) * 2 - static_cast<int64_t>(SizeDelta());
  const int64_t other_net_lcs = static_cast<int64_t>(other.normalized_lcs_instructions) * 2 -
                                static_cast<int64_t>(other.SizeDelta());
  if (net_lcs != other_net_lcs) {
    return net_lcs > other_net_lcs;
  }
  if (normalized_lcs_instructions != other.normalized_lcs_instructions) {
    return normalized_lcs_instructions > other.normalized_lcs_instructions;
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
                                  : (extractor != nullptr ? extractor->Symbols() : nullptr)) {
  if (extractor_ != nullptr) {
    target_payloads_ =
        fuzzer::ExtractTargetDataPayloads(extractor_->RomBytes(), extractor_->Config());
  }
}

namespace {

size_t CountTrailingSectionPaddingNops(uint32_t start_vram,
                                       absl::Span<const Instruction> instructions) {
  if (instructions.size() < 3 ||
      ((start_vram + static_cast<uint32_t>(instructions.size() * 4)) % 16) != 0) {
    return 0;
  }
  for (size_t k = 1; k <= 3 && instructions.size() >= k + 2; ++k) {
    if (!instructions[instructions.size() - k].IsNop()) {
      break;
    }
    const auto& terminator = instructions[instructions.size() - k - 2];
    bool is_unconditional_exit =
        terminator.IsReturn() || terminator.opcode == Opcode::kJ ||
        (terminator.opcode == Opcode::kBeq && terminator.rs == Register::kZero &&
         terminator.rt == Register::kZero);
    if (!is_unconditional_exit) {
      continue;
    }
    uint32_t padding_start_vram = instructions[instructions.size() - k].vram;
    bool branch_targets_padding = false;
    for (size_t i = 0; i <= instructions.size() - k - 2; ++i) {
      if (instructions[i].IsBranch() && instructions[i].BranchTarget() >= padding_start_vram) {
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
  absl::Span<const Instruction> target_instructions = target.instructions;
  absl::Span<const uint32_t> target_words = target.raw_words;

  const size_t target_padding = CountTrailingSectionPaddingNops(target.vram, target_instructions);
  const size_t compiled_padding =
      CountTrailingSectionPaddingNops(/*start_vram=*/0, compiled_instructions);

  std::vector<Instruction> adjusted_compiled_instructions;
  std::vector<uint32_t> adjusted_patched_words;
  if (compiled_padding > target_padding) {
    const size_t excess = compiled_padding - target_padding;
    if (compiled_instructions.size() > excess) {
      compiled_instructions =
          compiled_instructions.subspan(0, compiled_instructions.size() - excess);
      if (patched_words.size() >= compiled_instructions.size()) {
        patched_words = patched_words.subspan(0, compiled_instructions.size());
      }
    }
  } else if (compiled_padding < target_padding && !compiled_instructions.empty()) {
    const size_t deficit = target_padding - compiled_padding;
    adjusted_compiled_instructions.assign(compiled_instructions.begin(),
                                          compiled_instructions.end());
    adjusted_patched_words.assign(patched_words.begin(), patched_words.end());
    for (size_t i = 0; i < deficit; ++i) {
      Instruction nop_instruction = target_instructions.back();
      nop_instruction.vram = adjusted_compiled_instructions.back().vram + 4;
      adjusted_compiled_instructions.push_back(nop_instruction);
      if (!adjusted_patched_words.empty()) {
        adjusted_patched_words.push_back(0x00000000);
      }
    }
    compiled_instructions = adjusted_compiled_instructions;
    if (!adjusted_patched_words.empty()) {
      patched_words = adjusted_patched_words;
    }
  }

  FunctionDiffContext diff_context;
  diff_context.target_vram = target.vram;
  diff_context.target_size =
      target.size > 0 ? target.size : static_cast<uint32_t>(target_instructions.size() * 4);
  diff_context.compiled_vram = target.vram;
  diff_context.compiled_size = static_cast<uint32_t>(compiled_instructions.size() * 4);

  Normalizer normalizer(diff_context);

  std::vector<NormalizedInstruction> normalized_target;
  normalized_target.reserve(target_instructions.size());
  for (const auto& instruction : target_instructions) {
    normalized_target.push_back(normalizer.NormalizeTarget(instruction));
  }

  std::vector<NormalizedInstruction> normalized_compiled;
  normalized_compiled.reserve(compiled_instructions.size());
  for (const auto& instruction : compiled_instructions) {
    normalized_compiled.push_back(normalizer.NormalizeCompiled(instruction));
  }

  SequenceAligner aligner(&normalizer);
  AlignmentResult alignment = aligner.Align(normalized_target, normalized_compiled);

  CandidateScore score;
  score.bit_exact_instructions = alignment.stats.bit_exact_instructions;
  score.matching_instructions = alignment.stats.matching_instructions;
  score.differing_instructions = alignment.stats.differing_instructions;
  score.target_instruction_count = alignment.stats.target_instruction_count;
  score.compiled_instruction_count = alignment.stats.compiled_instruction_count;
  score.normalized_exact = alignment.stats.IsBitExactMatch();
  score.patched_words.assign(patched_words.begin(), patched_words.end());

  if (!target_words.empty() && !patched_words.empty()) {
    FunctionStructuralComparison structural =
        CompareFunctionStructure(target_words, target.vram, patched_words, target.vram);
    score.frame_size_matches = structural.frame_size_matches;
    score.saved_registers_match = structural.saved_registers_match;
    score.cfg_topology_matches = structural.cfg_topology_matches;
    score.normalized_lcs_instructions = structural.normalized_lcs_instructions;
  }

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

  static std::atomic<uint64_t> temporary_counter{0};
  uint64_t identifier = temporary_counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temporary_directory =
      std::filesystem::temp_directory_path() / absl::StrCat("tuner_candidate_", identifier);
  std::error_code error_code;
  std::filesystem::create_directories(temporary_directory, error_code);
  std::filesystem::path source_file = temporary_directory / absl::StrCat(target.name, ".c");

  {
    std::ofstream output_stream(source_file);
    if (!output_stream.is_open()) {
      std::filesystem::remove_all(temporary_directory, error_code);
      return absl::InternalError("CandidateScorer: Failed to write temporary C file.");
    }
    output_stream << c_code;
  }

  CandidateCompiler compiler({
      .repo_root = extractor_->Options().repo_root,
      .game_name = extractor_->Options().game_name,
      .config = extractor_->Config(),
      .symbols = symbols_,
  });
  auto compiled_or =
      compiler.CompileFunction(source_file, target.name, target.vram, c_flags, extra_include_dirs,
                               InferTargetRodataVram(target.instructions));
  std::filesystem::remove_all(temporary_directory, error_code);
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
  fuzzer::DifferentialFuzzer::Options fuzzer_options;
  fuzzer_options.target_initial_memory = target_payloads_;
  fuzzer::DifferentialFuzzer differential_fuzzer(fuzzer_options);
  auto cases = fuzzer::DifferentialFuzzer::GenerateBoundaryCases();
  auto random_cases = fuzzer::DifferentialFuzzer::GenerateRandomCases(num_vectors, /*seed=*/42);
  cases.insert(cases.end(), random_cases.begin(), random_cases.end());
  size_t pointer_vectors = num_vectors >= 20 ? 20 : num_vectors;
  auto pointer_cases = fuzzer::DifferentialFuzzer::GeneratePointerCases(
      pointer_vectors, /*buffer_vram=*/0x80500000, /*buffer_size=*/256, /*seed=*/42);
  cases.insert(cases.end(), pointer_cases.begin(), pointer_cases.end());

  std::vector<uint32_t> candidate_vector(candidate_words.begin(), candidate_words.end());
  for (const auto& test_case : cases) {
    auto comparison = differential_fuzzer.Compare(target.raw_words, target.vram, candidate_vector,
                                                  target.vram, test_case);
    if (comparison.result != fuzzer::EquivalenceResult::kEquivalent) {
      return false;
    }
  }
  return true;
}

}  // namespace rom_nom_nom
