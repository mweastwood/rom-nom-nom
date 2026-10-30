#ifndef TUNER_CANDIDATE_SCORER_H_
#define TUNER_CANDIDATE_SCORER_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "core/target_extractor.h"
#include "differ/sequence_aligner.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom {

struct CandidateScore {
  bool raw_bit_exact = false;
  bool normalized_exact = false;
  bool frame_size_matches = false;
  bool saved_registers_match = false;
  bool cfg_topology_matches = false;
  size_t normalized_lcs_instructions = 0;
  size_t bit_exact_instructions = 0;
  size_t matching_instructions = 0;
  size_t differing_instructions = 0;
  size_t target_instruction_count = 0;
  size_t compiled_instruction_count = 0;
  std::vector<uint32_t> patched_words;

  bool IsStructurallyIsomorphic() const {
    return frame_size_matches && saved_registers_match && cfg_topology_matches;
  }

  int StructuralMatchCount() const {
    return static_cast<int>(frame_size_matches) + static_cast<int>(saved_registers_match) +
           static_cast<int>(cfg_topology_matches);
  }

  size_t SizeDelta() const {
    return compiled_instruction_count > target_instruction_count
               ? compiled_instruction_count - target_instruction_count
               : target_instruction_count - compiled_instruction_count;
  }

  double MatchPercentage() const {
    if (target_instruction_count == 0) {
      return compiled_instruction_count == 0 ? 100.0 : 0.0;
    }
    return (static_cast<double>(matching_instructions) /
            static_cast<double>(target_instruction_count)) *
           100.0;
  }

  // Returns true if `*this` is strictly closer to bit-exactness than `other`.
  bool IsBetterThan(const CandidateScore& other) const;
};

// Compiles and scores candidate C implementations against retail target instruction sequences.
class CandidateScorer {
 public:
  explicit CandidateScorer(const TargetExtractor* extractor = nullptr,
                           const SymbolIndex* symbols = nullptr);

  // Scores already-decoded candidate instructions and relocated words against `target`.
  CandidateScore ScoreInstructions(const TargetFunction& target,
                                   absl::Span<const Instruction> compiled_instructions,
                                   absl::Span<const uint32_t> patched_words) const;

  // Writes `c_code` to a temporary file, compiles it with `c_flags`, and scores the resulting
  // machine code against `target`.
  absl::StatusOr<CandidateScore> CompileAndScore(
      const TargetFunction& target, std::string_view c_code,
      const std::vector<std::string>& c_flags = {},
      const std::vector<std::filesystem::path>& extra_include_dirs = {}) const;

  // Verifies behavioral equivalence between `target.raw_words` and `candidate_words` using the
  // differential fuzzer.
  bool VerifyEquivalence(const TargetFunction& target, absl::Span<const uint32_t> candidate_words,
                         size_t num_vectors = 100) const;

 private:
  const TargetExtractor* extractor_ = nullptr;
  const SymbolIndex* symbols_ = nullptr;
  std::vector<fuzzer::MemoryPayload> target_payloads_;
};

}  // namespace rom_nom_nom

#endif  // TUNER_CANDIDATE_SCORER_H_
