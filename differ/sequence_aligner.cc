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
constexpr int kExactMatchReward = 4;
constexpr int kSimilarMatchReward = 2;
constexpr int kDifferentPenalty = -2;
constexpr int kGapPenalty = -2;

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
  std::vector<int> dp((n + 1) * (m + 1), 0);
  auto at = [&](size_t i, size_t j) -> int& { return dp[i * (m + 1) + j]; };

  // Base cases: costs for aligning against leading gaps.
  for (size_t i = 1; i <= n; ++i) {
    at(i, 0) = at(i - 1, 0) + kGapPenalty;
  }
  for (size_t j = 1; j <= m; ++j) {
    at(0, j) = at(0, j - 1) + kGapPenalty;
  }

  // Populate DP matrix using Needleman-Wunsch recurrence.
  for (size_t i = 1; i <= n; ++i) {
    for (size_t j = 1; j <= m; ++j) {
      MatchQuality quality = normalizer_->Compare(target[i - 1], compiled[j - 1]);
      int match_score = kDifferentPenalty;
      if (quality == MatchQuality::kBitExact || quality == MatchQuality::kNormalizedExact) {
        match_score = kExactMatchReward;
      } else if (quality == MatchQuality::kSimilarOpcode) {
        match_score = kSimilarMatchReward;
      }

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
      int match_score = kDifferentPenalty;
      if (quality == MatchQuality::kBitExact || quality == MatchQuality::kNormalizedExact) {
        match_score = kExactMatchReward;
      } else if (quality == MatchQuality::kSimilarOpcode) {
        match_score = kSimilarMatchReward;
      }

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

  // Aggregate summary metrics and stats.
  AlignmentStats stats;
  stats.total_rows = rows.size();
  stats.target_instruction_count = n;
  stats.compiled_instruction_count = m;

  for (const auto& row : rows) {
    switch (row.kind) {
      case DiffRowKind::kMatch:
        ++stats.matching_instructions;
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
