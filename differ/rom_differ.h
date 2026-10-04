#ifndef DIFFER_ROM_DIFFER_H_
#define DIFFER_ROM_DIFFER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

// Options configuring the ROM difference analysis.
struct RomDiffOptions {
  std::filesystem::path base_rom_path;
  std::filesystem::path candidate_rom_path;
  std::filesystem::path config_path;
  bool use_color = true;
};

// Structural integrity and cartridge header validation results.
struct StructuralValidationResult {
  bool is_valid = false;
  std::string target_sha1;
  std::string built_sha1;
  uint64_t target_size = 0;
  uint64_t built_size = 0;
  bool size_matches = false;
  bool header_magic_matches = false;
  bool entrypoint_matches = false;
  std::string failure_reason;
};

// Word-level LCS comparison results for an executable code segment.
struct CodeSegmentDiffResult {
  std::string segment_name;
  uint64_t retail_rom_start = 0;
  uint64_t retail_rom_end = 0;
  uint64_t built_rom_start = 0;
  uint64_t built_rom_end = 0;

  size_t retail_words_count = 0;
  size_t built_words_count = 0;
  size_t matched_words_count = 0;
  size_t target_only_words_count = 0;
  size_t built_only_words_count = 0;

  double MatchPercentageRetail() const {
    if (retail_words_count == 0) {
      return 0.0;
    }
    return (static_cast<double>(matched_words_count) / static_cast<double>(retail_words_count)) *
           100.0;
  }

  double MatchPercentageBuilt() const {
    if (built_words_count == 0) {
      return 0.0;
    }
    return (static_cast<double>(matched_words_count) / static_cast<double>(built_words_count)) *
           100.0;
  }
};

// Block-level comparison results for an asset/data segment.
struct AssetSegmentDiffResult {
  std::string segment_name;
  uint64_t retail_rom_start = 0;
  uint64_t retail_rom_end = 0;
  uint64_t built_rom_start = 0;
  int64_t shift_offset = 0;
  size_t total_bytes = 0;
  size_t matching_bytes = 0;

  double MatchPercentage() const {
    if (total_bytes == 0) {
      return 0.0;
    }
    return (static_cast<double>(matching_bytes) / static_cast<double>(total_bytes)) * 100.0;
  }
};

// Complete result of ROM difference analysis.
struct RomDiffResult {
  StructuralValidationResult structural;
  size_t header_matching_bytes = 0;
  size_t header_total_bytes = 0;
  std::vector<CodeSegmentDiffResult> code_segments;
  std::vector<AssetSegmentDiffResult> asset_segments;
  size_t total_logical_matching_bytes = 0;
  size_t total_rom_bytes = 0;
  double overall_match_percentage = 0.0;
  std::string formatted_report;
  bool success = false;
};

// RomDiffer coordinates structural verification, segment-aware partitioning,
// 32-bit word LCS sequence alignment on code segments, and shift-aligned
// block verification on data/asset segments.
class RomDiffer {
 public:
  explicit RomDiffer(RomDiffOptions options);

  // Executes difference analysis comparing the target ROM and built ROM.
  absl::StatusOr<RomDiffResult> Diff() const;

  // Computes the size of the Longest Common Subsequence (LCS) between two
  // sequences of 32-bit words using prefix/suffix trimming and Hunt-Szymanski.
  static size_t ComputeWordLcs(absl::Span<const uint32_t> target_words,
                               absl::Span<const uint32_t> built_words);

 private:
  RomDiffOptions options_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_ROM_DIFFER_H_
