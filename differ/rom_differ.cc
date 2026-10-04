#include "differ/rom_differ.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/sha1.h"
#include "splitter/config.h"
#include "splitter/config.pb.h"

namespace rom_nom_nom {

namespace {

constexpr const char* kAnsiReset = "\033[0m";
constexpr const char* kAnsiBold = "\033[1m";
constexpr const char* kAnsiGreen = "\033[32m";
constexpr const char* kAnsiRed = "\033[31m";
constexpr const char* kAnsiYellow = "\033[33m";
constexpr const char* kAnsiCyan = "\033[36m";

absl::StatusOr<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& file_path) {
  std::ifstream file_stream(file_path, std::ios::binary);
  if (!file_stream.is_open()) {
    return absl::NotFoundError(absl::StrFormat("Could not open file '%s'", file_path.string()));
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file_stream)),
                              std::istreambuf_iterator<char>());
}

std::string ComputeBytesSha1(absl::Span<const uint8_t> bytes) {
  Sha1 sha1_calculator;
  sha1_calculator.Update(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  return sha1_calculator.FinalizeHex();
}

std::vector<uint32_t> ExtractBigEndianWords(absl::Span<const uint8_t> bytes) {
  std::vector<uint32_t> words;
  words.reserve(bytes.size() / 4);
  for (size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
    uint32_t word = (static_cast<uint32_t>(bytes[offset]) << 24) |
                    (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                    (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
                    static_cast<uint32_t>(bytes[offset + 3]);
    words.push_back(word);
  }
  return words;
}

}  // namespace

RomDiffer::RomDiffer(RomDiffOptions options) : options_(std::move(options)) {}

size_t RomDiffer::ComputeWordLcs(absl::Span<const uint32_t> target_words,
                                 absl::Span<const uint32_t> built_words) {
  const size_t target_length = target_words.size();
  const size_t built_length = built_words.size();
  if (target_length == 0 || built_length == 0) {
    return 0;
  }

  // 1. Trim common prefix
  size_t prefix_length = 0;
  const size_t minimum_length = std::min(target_length, built_length);
  while (prefix_length < minimum_length &&
         target_words[prefix_length] == built_words[prefix_length]) {
    ++prefix_length;
  }
  if (prefix_length == target_length || prefix_length == built_length) {
    return prefix_length;
  }

  // 2. Trim common suffix
  size_t suffix_length = 0;
  while (suffix_length < (target_length - prefix_length) &&
         suffix_length < (built_length - prefix_length) &&
         target_words[target_length - 1 - suffix_length] ==
             built_words[built_length - 1 - suffix_length]) {
    ++suffix_length;
  }

  absl::Span<const uint32_t> middle_target =
      target_words.subspan(prefix_length, target_length - prefix_length - suffix_length);
  absl::Span<const uint32_t> middle_built =
      built_words.subspan(prefix_length, built_length - prefix_length - suffix_length);

  // 3. Hunt-Szymanski algorithm on trimmed middle sequences
  std::unordered_map<uint32_t, std::vector<int>> target_word_positions;
  target_word_positions.reserve(middle_target.size());
  for (int index = 0; index < static_cast<int>(middle_target.size()); ++index) {
    target_word_positions[middle_target[index]].push_back(index);
  }

  std::vector<int> longest_increasing_subsequence;
  longest_increasing_subsequence.reserve(std::min(middle_target.size(), middle_built.size()));

  for (int built_index = 0; built_index < static_cast<int>(middle_built.size()); ++built_index) {
    auto positions_iterator = target_word_positions.find(middle_built[built_index]);
    if (positions_iterator == target_word_positions.end()) {
      continue;
    }
    const std::vector<int>& positions = positions_iterator->second;
    // Iterate in reverse order so each position in middle_target is consumed at most once
    for (auto reverse_position_iterator = positions.rbegin();
         reverse_position_iterator != positions.rend(); ++reverse_position_iterator) {
      int position = *reverse_position_iterator;
      auto lower_bound_iterator = std::lower_bound(longest_increasing_subsequence.begin(),
                                                   longest_increasing_subsequence.end(), position);
      if (lower_bound_iterator == longest_increasing_subsequence.end()) {
        longest_increasing_subsequence.push_back(position);
      } else {
        *lower_bound_iterator = position;
      }
    }
  }

  return prefix_length + longest_increasing_subsequence.size() + suffix_length;
}

absl::StatusOr<RomDiffResult> RomDiffer::Diff() const {
  auto target_bytes_or = ReadBinaryFile(options_.base_rom_path);
  if (!target_bytes_or.ok()) {
    return target_bytes_or.status();
  }
  const std::vector<uint8_t>& target_bytes = *target_bytes_or;

  auto built_bytes_or = ReadBinaryFile(options_.candidate_rom_path);
  if (!built_bytes_or.ok()) {
    return built_bytes_or.status();
  }
  const std::vector<uint8_t>& built_bytes = *built_bytes_or;

  RomDiffResult result;
  result.total_rom_bytes = target_bytes.size();

  // 1. Structural Validation
  result.structural.target_size = target_bytes.size();
  result.structural.built_size = built_bytes.size();
  result.structural.target_sha1 = ComputeBytesSha1(target_bytes);
  result.structural.built_sha1 = ComputeBytesSha1(built_bytes);
  result.structural.size_matches = (result.structural.target_size == result.structural.built_size);

  if (target_bytes.size() >= 4 && built_bytes.size() >= 4) {
    result.structural.header_magic_matches =
        (target_bytes[0] == built_bytes[0] && target_bytes[1] == built_bytes[1] &&
         target_bytes[2] == built_bytes[2] && target_bytes[3] == built_bytes[3]);
  }
  if (target_bytes.size() >= 12 && built_bytes.size() >= 12) {
    result.structural.entrypoint_matches =
        (target_bytes[8] == built_bytes[8] && target_bytes[9] == built_bytes[9] &&
         target_bytes[10] == built_bytes[10] && target_bytes[11] == built_bytes[11]);
  }

  result.structural.is_valid =
      (result.structural.size_matches && result.structural.header_magic_matches &&
       result.structural.entrypoint_matches);

  if (!result.structural.size_matches) {
    result.structural.failure_reason =
        absl::StrFormat("Size mismatch: target has %d bytes, built has %d bytes",
                        result.structural.target_size, result.structural.built_size);
  } else if (!result.structural.header_magic_matches) {
    result.structural.failure_reason = "N64 header magic mismatch";
  } else if (!result.structural.entrypoint_matches) {
    result.structural.failure_reason = "N64 entrypoint address mismatch";
  }

  // 2. Header and IPL3 Verification (0x0 - 0x1050)
  const size_t header_end_offset = std::min<size_t>(0x1050, target_bytes.size());
  result.header_total_bytes = header_end_offset;
  size_t header_matches = 0;
  for (size_t index = 0; index < header_end_offset && index < built_bytes.size(); ++index) {
    if (target_bytes[index] == built_bytes[index]) {
      ++header_matches;
    }
  }
  result.header_matching_bytes = header_matches;

  // 3. Segment Resolution
  uint64_t retail_code_start = 0x1050;
  uint64_t retail_code_end = target_bytes.size();
  std::string code_segment_name = "code";
  struct AssetSegmentRange {
    std::string name;
    uint64_t rom_start = 0;
    uint64_t rom_end = 0;
  };
  std::vector<AssetSegmentRange> asset_segment_ranges;

  std::error_code error_code;
  if (!options_.config_path.empty() && std::filesystem::exists(options_.config_path, error_code)) {
    auto config_or = LoadSplitConfig(options_.config_path);
    if (config_or.ok()) {
      bool found_code_segment = false;
      for (const auto& segment : config_or->segments()) {
        bool has_code_subsegments = false;
        for (const auto& subsegment : segment.subsegments()) {
          if (subsegment.type() == SUBSEGMENT_C || subsegment.type() == SUBSEGMENT_ASM) {
            has_code_subsegments = true;
            break;
          }
        }
        if (!found_code_segment &&
            (has_code_subsegments || segment.name() == "main" || segment.name() == "code")) {
          found_code_segment = true;
          code_segment_name = segment.name();
          retail_code_start = segment.rom_start();
          retail_code_end = segment.rom_end();
        } else if (found_code_segment) {
          asset_segment_ranges.push_back(AssetSegmentRange{.name = std::string(segment.name()),
                                                           .rom_start = segment.rom_start(),
                                                           .rom_end = segment.rom_end()});
        }
      }
    }
  }

  // 4. Locate start of Asset Segments in built ROM
  uint64_t built_code_end = built_bytes.size();
  uint64_t first_asset_built_offset = built_bytes.size();

  if (!asset_segment_ranges.empty()) {
    const uint64_t first_retail_asset_start = asset_segment_ranges.front().rom_start;
    // Extract a 32-byte non-zero signature from the start of the first asset segment
    size_t signature_length = std::min<size_t>(32, target_bytes.size() - first_retail_asset_start);
    std::string_view asset_signature(
        reinterpret_cast<const char*>(target_bytes.data() + first_retail_asset_start),
        signature_length);

    std::string_view built_rom_view(reinterpret_cast<const char*>(built_bytes.data()),
                                    built_bytes.size());
    size_t found_position = built_rom_view.find(asset_signature, retail_code_start);
    if (found_position != std::string_view::npos) {
      first_asset_built_offset = found_position;
      built_code_end = found_position;
    }
  }

  // 5. Code Segment Word-Level LCS
  CodeSegmentDiffResult code_diff;
  code_diff.segment_name = code_segment_name;
  code_diff.retail_rom_start = retail_code_start;
  code_diff.retail_rom_end = retail_code_end;
  code_diff.built_rom_start = retail_code_start;
  code_diff.built_rom_end = built_code_end;

  if (retail_code_end > retail_code_start && target_bytes.size() >= retail_code_end &&
      built_code_end > retail_code_start && built_bytes.size() >= built_code_end) {
    absl::Span<const uint8_t> target_code_bytes(target_bytes.data() + retail_code_start,
                                                retail_code_end - retail_code_start);
    absl::Span<const uint8_t> built_code_bytes(built_bytes.data() + retail_code_start,
                                               built_code_end - retail_code_start);

    std::vector<uint32_t> target_words = ExtractBigEndianWords(target_code_bytes);
    std::vector<uint32_t> built_words = ExtractBigEndianWords(built_code_bytes);

    code_diff.retail_words_count = target_words.size();
    code_diff.built_words_count = built_words.size();
    code_diff.matched_words_count = ComputeWordLcs(target_words, built_words);
    code_diff.target_only_words_count =
        code_diff.retail_words_count - code_diff.matched_words_count;
    code_diff.built_only_words_count = code_diff.built_words_count - code_diff.matched_words_count;
  }
  result.code_segments.push_back(code_diff);

  // 6. Asset Segments Shift-Aligned Matching
  size_t total_asset_matches = 0;
  for (const auto& asset_range : asset_segment_ranges) {
    uint64_t retail_segment_start = asset_range.rom_start;
    uint64_t retail_segment_end = asset_range.rom_end;
    uint64_t segment_length = retail_segment_end - retail_segment_start;

    int64_t shift_offset = 0;
    if (first_asset_built_offset < built_bytes.size() && !asset_segment_ranges.empty()) {
      shift_offset = static_cast<int64_t>(first_asset_built_offset) -
                     static_cast<int64_t>(asset_segment_ranges.front().rom_start);
    }
    int64_t built_segment_start = static_cast<int64_t>(retail_segment_start) + shift_offset;

    AssetSegmentDiffResult asset_diff;
    asset_diff.segment_name = asset_range.name;
    asset_diff.retail_rom_start = retail_segment_start;
    asset_diff.retail_rom_end = retail_segment_end;
    asset_diff.built_rom_start = (built_segment_start >= 0) ? built_segment_start : 0;
    asset_diff.shift_offset = shift_offset;
    asset_diff.total_bytes = segment_length;

    size_t matching_bytes = 0;
    if (built_segment_start >= 0 &&
        static_cast<size_t>(built_segment_start + segment_length) <= built_bytes.size() &&
        retail_segment_end <= target_bytes.size()) {
      for (size_t offset = 0; offset < segment_length; ++offset) {
        if (target_bytes[retail_segment_start + offset] ==
            built_bytes[built_segment_start + offset]) {
          ++matching_bytes;
        }
      }
    }
    asset_diff.matching_bytes = matching_bytes;
    total_asset_matches += matching_bytes;
    result.asset_segments.push_back(asset_diff);
  }

  // 7. Aggregate Total Logical Matches
  size_t total_code_matched_bytes = code_diff.matched_words_count * 4;
  result.total_logical_matching_bytes =
      result.header_matching_bytes + total_code_matched_bytes + total_asset_matches;
  if (result.total_rom_bytes > 0) {
    result.overall_match_percentage = (static_cast<double>(result.total_logical_matching_bytes) /
                                       static_cast<double>(result.total_rom_bytes)) *
                                      100.0;
  }
  result.success = result.structural.is_valid;

  // 8. Generate Formatted Output Report
  const bool color = options_.use_color;
  std::string bold = color ? kAnsiBold : "";
  std::string green = color ? kAnsiGreen : "";
  std::string red = color ? kAnsiRed : "";
  std::string yellow = color ? kAnsiYellow : "";
  std::string cyan = color ? kAnsiCyan : "";
  std::string reset = color ? kAnsiReset : "";

  auto format_commas = [](uint64_t value) -> std::string {
    std::string text = std::to_string(value);
    int insert_position = static_cast<int>(text.length()) - 3;
    while (insert_position > 0) {
      text.insert(insert_position, ",");
      insert_position -= 3;
    }
    return text;
  };

  auto format_signed_commas = [&](int64_t value) -> std::string {
    if (value < 0) {
      return absl::StrCat("-", format_commas(static_cast<uint64_t>(-value)));
    }
    if (value > 0) {
      return absl::StrCat("+", format_commas(static_cast<uint64_t>(value)));
    }
    return "0";
  };

  std::string report;
  absl::StrAppend(
      &report,
      "================================================================================\n");
  absl::StrAppend(&report, bold, "[LIFTED C ROM STRUCTURAL VALIDATION]", reset, "\n");
  absl::StrAppendFormat(&report, "Target ROM SHA-1: %s\n", result.structural.target_sha1);
  absl::StrAppendFormat(&report, "Built  ROM SHA-1: %s\n", result.structural.built_sha1);
  absl::StrAppendFormat(&report, "Cartridge Size:   %s bytes (%s)\n",
                        format_commas(result.structural.built_size),
                        result.structural.size_matches ? "Exact match" : "MISMATCH");
  absl::StrAppendFormat(
      &report, "N64 Header & IPL3: %s / %s bytes (%.2f%% bit-exact)\n",
      format_commas(result.header_matching_bytes), format_commas(result.header_total_bytes),
      (result.header_total_bytes > 0)
          ? (static_cast<double>(result.header_matching_bytes) / result.header_total_bytes * 100.0)
          : 0.0);

  if (result.structural.is_valid) {
    absl::StrAppend(&report, green, "[SUCCESS] ROM structural integrity verified!", reset, "\n");
  } else {
    absl::StrAppendFormat(&report, "%s[FAIL] Structural validation failed: %s%s\n", red,
                          result.structural.failure_reason, reset);
  }

  absl::StrAppend(
      &report,
      "--------------------------------------------------------------------------------\n");
  absl::StrAppend(&report, bold, "[SEGMENT-AWARE INSERTION/DELETION RESILIENT DIFF]", reset, "\n");

  absl::StrAppendFormat(&report, "1. Code Segment (.%s):\n", code_diff.segment_name);
  absl::StrAppendFormat(&report, "   - Retail Words:     %s words (%s bytes)\n",
                        format_commas(code_diff.retail_words_count),
                        format_commas(code_diff.retail_words_count * 4));
  int64_t code_size_delta = static_cast<int64_t>(code_diff.built_words_count * 4) -
                            static_cast<int64_t>(code_diff.retail_words_count * 4);
  absl::StrAppendFormat(&report, "   - Built Words:      %s words (%s bytes) [%s bytes]\n",
                        format_commas(code_diff.built_words_count),
                        format_commas(code_diff.built_words_count * 4),
                        format_signed_commas(code_size_delta));
  absl::StrAppendFormat(&report, "   - Myers Match (LCS): %s words (%s bytes)\n",
                        format_commas(code_diff.matched_words_count),
                        format_commas(code_diff.matched_words_count * 4));
  absl::StrAppendFormat(&report, "   - Match vs Retail:   %.2f%%\n",
                        code_diff.MatchPercentageRetail());
  absl::StrAppendFormat(&report, "   - Match vs Built:    %.2f%%\n",
                        code_diff.MatchPercentageBuilt());
  absl::StrAppendFormat(&report, "   - Word Insertions:  %s words | Word Deletions: %s words\n",
                        format_commas(code_diff.built_only_words_count),
                        format_commas(code_diff.target_only_words_count));

  for (size_t index = 0; index < result.asset_segments.size(); ++index) {
    const auto& asset = result.asset_segments[index];
    absl::StrAppendFormat(&report, "\n%d. Asset Segment (.%s):\n", index + 2, asset.segment_name);
    absl::StrAppendFormat(&report, "   - Shift Offset:     %s bytes (0x%06X vs retail 0x%06X)\n",
                          format_signed_commas(asset.shift_offset), asset.built_rom_start,
                          asset.retail_rom_start);
    absl::StrAppendFormat(&report, "   - Byte Match:       %s / %s bytes (%.2f%% bit-exact)\n",
                          format_commas(asset.matching_bytes), format_commas(asset.total_bytes),
                          asset.MatchPercentage());
  }

  absl::StrAppend(
      &report,
      "--------------------------------------------------------------------------------\n");
  absl::StrAppendFormat(&report, "%sOverall Logical ROM Content Match: %s / %s bytes (%.2f%%)%s\n",
                        bold, format_commas(result.total_logical_matching_bytes),
                        format_commas(result.total_rom_bytes), result.overall_match_percentage,
                        reset);
  absl::StrAppend(
      &report,
      "================================================================================\n");

  result.formatted_report = report;
  return result;
}

}  // namespace rom_nom_nom
