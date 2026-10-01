#ifndef DIFFER_SEQUENCE_ALIGNER_H_
#define DIFFER_SEQUENCE_ALIGNER_H_

#include <cstddef>
#include <optional>
#include <vector>

#include "absl/types/span.h"
#include "differ/normalizer.h"

namespace rom_nom_nom {

// Classification of an aligned diff row.
enum class DiffRowKind {
  // Both sides are present and match (MatchQuality::kBitExact or kNormalizedExact).
  kMatch,

  // Both sides are present at this aligned position but differ in operands or opcode
  // (MatchQuality::kSimilarOpcode or kDifferent).
  kDifference,

  // Instruction present in retail ROM target but omitted in recompiled object (deletion).
  kTargetOnly,

  // Instruction present in recompiled object but not in retail ROM target (insertion).
  kCompiledOnly,
};

// Represents a single row in an aligned side-by-side assembly diff.
struct AlignedDiffRow {
  DiffRowKind kind = DiffRowKind::kMatch;
  MatchQuality quality = MatchQuality::kBitExact;

  std::optional<NormalizedInstruction> target;
  std::optional<NormalizedInstruction> compiled;
};

// Summary statistics and match fidelity metrics across aligned instruction sequences.
struct AlignmentStats {
  size_t total_rows = 0;
  size_t matching_instructions = 0;       // kBitExact or kNormalizedExact
  size_t differing_instructions = 0;      // kSimilarOpcode or kDifferent
  size_t target_only_instructions = 0;    // Missing in compiled
  size_t compiled_only_instructions = 0;  // Extra in compiled

  size_t target_instruction_count = 0;
  size_t compiled_instruction_count = 0;

  // Returns true if the compiled instructions match the target retail ROM 100% bit-exact.
  bool IsBitExactMatch() const {
    return target_instruction_count > 0 && target_instruction_count == compiled_instruction_count &&
           matching_instructions == target_instruction_count && differing_instructions == 0 &&
           target_only_instructions == 0 && compiled_only_instructions == 0;
  }

  // Returns match percentage relative to target instructions [0.0, 100.0].
  double MatchPercentage() const {
    if (target_instruction_count == 0) {
      return (compiled_instruction_count == 0) ? 100.0 : 0.0;
    }
    return (static_cast<double>(matching_instructions) /
            static_cast<double>(target_instruction_count)) *
           100.0;
  }
};

// Complete result of sequence alignment.
struct AlignmentResult {
  std::vector<AlignedDiffRow> rows;
  AlignmentStats stats;
};

// SequenceAligner aligns target and compiled instruction sequences using
// Needleman-Wunsch global sequence alignment, pairing matching and similar
// instructions side-by-side and inserting gaps for deletions and additions.
class SequenceAligner {
 public:
  explicit SequenceAligner(const Normalizer* normalizer);

  // Aligns target and compiled instruction sequences and computes statistics.
  AlignmentResult Align(absl::Span<const NormalizedInstruction> target,
                        absl::Span<const NormalizedInstruction> compiled) const;

 private:
  const Normalizer* normalizer_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_SEQUENCE_ALIGNER_H_
