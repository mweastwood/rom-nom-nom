#ifndef DIFFER_DIFF_FORMATTER_H_
#define DIFFER_DIFF_FORMATTER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/symbol_registry.h"
#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"

namespace rom_nom_nom {

// Structural integrity and cartridge header validation results.
struct StructuralValidationResult {
  bool is_valid = false;
  bool exact_sha1_match = false;
  std::string target_sha1;
  std::string built_sha1;
  uint64_t target_size = 0;
  uint64_t built_size = 0;
  bool size_matches = false;
  bool code_shift_matches = false;
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

// Per-function structural readiness metrics across the code segment.
struct StructuralReadinessStats {
  size_t total_functions = 0;
  size_t frame_size_matches = 0;
  size_t saved_registers_matches = 0;
  size_t cfg_topology_matches = 0;
  size_t structurally_isomorphic_functions = 0;
  size_t total_target_instructions = 0;
  size_t total_normalized_lcs_instructions = 0;

  double FrameSizeMatchPercentage() const {
    if (total_functions == 0) {
      return 0.0;
    }
    return (static_cast<double>(frame_size_matches) / static_cast<double>(total_functions)) * 100.0;
  }

  double SavedRegistersMatchPercentage() const {
    if (total_functions == 0) {
      return 0.0;
    }
    return (static_cast<double>(saved_registers_matches) / static_cast<double>(total_functions)) *
           100.0;
  }

  double CfgTopologyMatchPercentage() const {
    if (total_functions == 0) {
      return 0.0;
    }
    return (static_cast<double>(cfg_topology_matches) / static_cast<double>(total_functions)) *
           100.0;
  }

  double StructurallyIsomorphicPercentage() const {
    if (total_functions == 0) {
      return 0.0;
    }
    return (static_cast<double>(structurally_isomorphic_functions) /
            static_cast<double>(total_functions)) *
           100.0;
  }

  double NormalizedLcsPercentage() const {
    if (total_target_instructions == 0) {
      return 0.0;
    }
    return (static_cast<double>(total_normalized_lcs_instructions) /
            static_cast<double>(total_target_instructions)) *
           100.0;
  }
};

// Complete result of ROM difference analysis.
struct RomDiffResult {
  StructuralValidationResult structural;
  size_t header_matching_bytes = 0;
  size_t header_total_bytes = 0;
  std::vector<CodeSegmentDiffResult> code_segments;
  std::vector<AssetSegmentDiffResult> asset_segments;
  StructuralReadinessStats readiness;
  size_t total_logical_matching_bytes = 0;
  size_t total_rom_bytes = 0;
  double overall_match_percentage = 0.0;
  std::string formatted_report;
  bool success = false;
};

// Visual rendering and layout options for side-by-side assembly diffs.
struct DiffFormatterOptions {
  bool use_color = false;    // Emit ANSI color escape codes
  bool show_offsets = true;  // Prefix lines with function-relative offsets (+0x000: )
  bool show_hex = true;      // Include raw 32-bit machine word hex (27BDFFE0  )
  size_t column_width = 48;  // Width of each side-by-side column in characters
};

// Formats aligned assembly difference rows into side-by-side terminal columns,
// rendering ANSI colors, instruction offsets, raw hex words, disassembly,
// and match summaries.
class DiffFormatter {
 public:
  explicit DiffFormatter(DiffFormatterOptions options = {}, const SymbolIndex* symbols = nullptr);

  // Formats a single aligned diff row into a side-by-side terminal line.
  std::string FormatRow(const AlignedDiffRow& row) const;

  // Formats header banner and column headers.
  std::string FormatHeader(std::string_view func_name, std::string_view source_path,
                           const AlignmentStats& stats) const;

  // Formats summary banner and match statistics.
  std::string FormatSummary(const AlignmentStats& stats) const;

  // Formats the complete diff report: header, all aligned rows, and summary.
  std::string FormatDiff(std::string_view func_name, const AlignmentResult& result,
                         std::string_view source_path = "") const;

  // Formats a full-ROM structural validation and segment-aware diff report.
  std::string FormatRomDiff(const RomDiffResult& result) const;
  static std::string FormatRomDiff(const RomDiffResult& result, bool use_color);

  // Formats a single instruction side into a column string (unpadded).
  std::string FormatInstructionSide(const NormalizedInstruction& norm) const;

 private:
  std::string PadColumn(std::string_view text) const;

  DiffFormatterOptions options_;
  const SymbolIndex* symbols_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_DIFF_FORMATTER_H_
