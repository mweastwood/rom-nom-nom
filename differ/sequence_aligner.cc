#include "differ/sequence_aligner.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "differ/normalizer.h"

namespace rom_nom_nom {

namespace {

// Needleman-Wunsch alignment reward and penalty weights.
constexpr int kGapPenalty = -20;

// Resolves the synchronized anchor row index in `rows` corresponding to an intra-function
// byte offset `target_offset` on either the target side (`is_target = true`) or the compiled
// side (`is_target = false`). If the instruction at `target_offset` sits on a gap row
// (`kTargetOnly` or `kCompiledOnly`), advances forward to the next synchronized row
// (`kMatch` or `kDifference`) so that labels immediately preceding a deleted/inserted
// instruction resolve to the same synchronized block boundary.
std::optional<size_t> ResolveSynchronizedTargetRow(const std::vector<AlignedDiffRow>& rows,
                                                   uint32_t target_offset, bool is_target) {
  std::optional<size_t> exact_row;
  for (size_t idx = 0; idx < rows.size(); ++idx) {
    const auto& side = is_target ? rows[idx].target : rows[idx].compiled;
    if (side.has_value() && side->func_offset == target_offset) {
      exact_row = idx;
      break;
    }
  }
  if (!exact_row.has_value()) {
    return std::nullopt;
  }
  for (size_t idx = *exact_row; idx < rows.size(); ++idx) {
    if (rows[idx].kind == DiffRowKind::kMatch || rows[idx].kind == DiffRowKind::kDifference) {
      return idx;
    }
  }
  return rows.size();
}

}  // namespace

SequenceAligner::SequenceAligner(const Normalizer* normalizer) : normalizer_(normalizer) {}

AlignmentResult SequenceAligner::Align(absl::Span<const NormalizedInstruction> target,
                                       absl::Span<const NormalizedInstruction> compiled) const {
  const size_t n = target.size();
  const size_t m = compiled.size();

  // Edge case: if both sequences are empty, return empty result.
  if (n == 0 && m == 0) {
    return AlignmentResult{};
  }

  // Flat 2D dynamic programming grid of dimensions (n + 1) x (m + 1).
  constexpr int kUnreachableScore = -1000000000;
  const bool use_band = (n * m > 250000);
  const size_t band_half_width = (n > m ? (n - m) : (m - n)) + 256;
  std::vector<int> dp((n + 1) * (m + 1), use_band ? kUnreachableScore : 0);
  auto at = [&](size_t i, size_t j) -> int& { return dp[i * (m + 1) + j]; };
  at(0, 0) = 0;

  // Base cases: costs for aligning against leading gaps.
  const size_t max_init_i = use_band ? std::min(n, band_half_width) : n;
  for (size_t i = 1; i <= max_init_i; ++i) {
    at(i, 0) = at(i - 1, 0) + kGapPenalty;
  }
  const size_t max_init_j = use_band ? std::min(m, band_half_width) : m;
  for (size_t j = 1; j <= max_init_j; ++j) {
    at(0, j) = at(0, j - 1) + kGapPenalty;
  }

  // Populate DP matrix using Needleman-Wunsch recurrence with operand-graded similarity.
  for (size_t i = 1; i <= n; ++i) {
    const size_t j_min = (use_band && i > band_half_width) ? (i - band_half_width) : 1;
    const size_t j_max = use_band ? std::min(m, i + band_half_width) : m;
    for (size_t j = j_min; j <= j_max; ++j) {
      int match_score = normalizer_->SimilarityScore(target[i - 1], compiled[j - 1]);

      int score_diag = at(i - 1, j - 1) + match_score;
      int score_up = at(i - 1, j) + kGapPenalty;
      int score_left = at(i, j - 1) + kGapPenalty;

      at(i, j) = std::max({score_diag, score_up, score_left});
    }
  }

  // Backtrack from (n, m) to (0, 0) to reconstruct the optimal alignment path.
  size_t i = n;
  size_t j = m;
  std::vector<AlignedDiffRow> rows;

  while (i > 0 || j > 0) {
    if (i > 0 && j > 0) {
      MatchQuality quality = normalizer_->Compare(target[i - 1], compiled[j - 1]);
      int match_score = normalizer_->SimilarityScore(target[i - 1], compiled[j - 1]);

      // Check if diagonal transition was taken.
      if (at(i, j) == at(i - 1, j - 1) + match_score) {
        AlignedDiffRow row;
        if (quality == MatchQuality::kBitExact || quality == MatchQuality::kNormalizedExact) {
          row.kind = DiffRowKind::kMatch;
        } else {
          row.kind = DiffRowKind::kDifference;
        }
        row.quality = quality;
        row.target = target[i - 1];
        row.compiled = compiled[j - 1];
        rows.push_back(std::move(row));
        --i;
        --j;
        continue;
      }
    }

    // Check if vertical transition (target deletion) was taken.
    if (i > 0 && at(i, j) == at(i - 1, j) + kGapPenalty) {
      AlignedDiffRow row;
      row.kind = DiffRowKind::kTargetOnly;
      row.quality = MatchQuality::kDifferent;
      row.target = target[i - 1];
      row.compiled = std::nullopt;
      rows.push_back(std::move(row));
      --i;
    }
    // Check if horizontal transition (compiled insertion) was taken.
    else if (j > 0 && at(i, j) == at(i, j - 1) + kGapPenalty) {
      AlignedDiffRow row;
      row.kind = DiffRowKind::kCompiledOnly;
      row.quality = MatchQuality::kDifferent;
      row.target = std::nullopt;
      row.compiled = compiled[j - 1];
      rows.push_back(std::move(row));
      --j;
    } else {
      break;
    }
  }

  // Reverse backtracked rows into sequential forward order.
  std::reverse(rows.begin(), rows.end());

  // Post-alignment pass: upgrade intra-function branch and jump instructions from
  // kSimilarOpcode (kDifference) to kNormalizedExact (kMatch) when their opcode and
  // register operands are identical and their intra-function branch targets resolve
  // to the exact same synchronized destination row in `rows`.
  for (auto& row : rows) {
    if (row.kind != DiffRowKind::kDifference || row.quality != MatchQuality::kSimilarOpcode ||
        !row.target.has_value() || !row.compiled.has_value()) {
      continue;
    }
    const NormalizedInstruction& t_norm = *row.target;
    const NormalizedInstruction& c_norm = *row.compiled;
    if (!t_norm.intra_function_target_offset.has_value() ||
        !c_norm.intra_function_target_offset.has_value()) {
      continue;
    }
    const Instruction& t_inst = t_norm.original;
    const Instruction& c_inst = c_norm.original;
    bool same_branch_opcode_and_regs = false;
    if (t_inst.IsBranch() && c_inst.IsBranch()) {
      // Upper 16 bits encode opcode, rs, and rt (or regimm/cop1 condition code).
      same_branch_opcode_and_regs =
          (t_inst.raw_word & 0xFFFF0000u) == (c_inst.raw_word & 0xFFFF0000u);
    } else if (t_inst.opcode == Opcode::kJ && c_inst.opcode == Opcode::kJ) {
      same_branch_opcode_and_regs = true;
    }
    if (!same_branch_opcode_and_regs) {
      continue;
    }
    auto t_dest_row = ResolveSynchronizedTargetRow(rows, *t_norm.intra_function_target_offset,
                                                   /*is_target=*/true);
    auto c_dest_row = ResolveSynchronizedTargetRow(rows, *c_norm.intra_function_target_offset,
                                                   /*is_target=*/false);
    if (t_dest_row.has_value() && c_dest_row.has_value() && *t_dest_row == *c_dest_row) {
      row.kind = DiffRowKind::kMatch;
      row.quality = MatchQuality::kNormalizedExact;
    }
  }

  // Aggregate summary metrics and stats.
  AlignmentStats stats;
  stats.total_rows = rows.size();
  stats.target_instruction_count = n;
  stats.compiled_instruction_count = m;

  for (const auto& row : rows) {
    switch (row.kind) {
      case DiffRowKind::kMatch:
        ++stats.matching_instructions;
        if (row.quality == MatchQuality::kBitExact) {
          ++stats.bit_exact_instructions;
        }
        break;
      case DiffRowKind::kDifference:
        ++stats.differing_instructions;
        break;
      case DiffRowKind::kTargetOnly:
        ++stats.target_only_instructions;
        break;
      case DiffRowKind::kCompiledOnly:
        ++stats.compiled_only_instructions;
        break;
    }
  }

  AlignmentResult result;
  result.rows = std::move(rows);
  result.stats = stats;
  return result;
}

}  // namespace rom_nom_nom
