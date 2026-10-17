#include "differ/diff_formatter.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"

namespace rom_nom_nom {

namespace {

// ANSI escape sequences for terminal syntax highlighting.
constexpr std::string_view kColorReset = "\033[0m";
constexpr std::string_view kColorGreen = "\033[32m";
constexpr std::string_view kColorYellow = "\033[33m";
constexpr std::string_view kColorRed = "\033[31m";
constexpr std::string_view kColorCyan = "\033[36m";
constexpr std::string_view kColorDim = "\033[2m";
constexpr std::string_view kColorBold = "\033[1m";

}  // namespace

DiffFormatter::DiffFormatter(DiffFormatterOptions options, const SymbolIndex* symbols)
    : options_(options), symbols_(symbols) {}

std::string DiffFormatter::PadColumn(std::string_view text) const {
  if (text.size() >= options_.column_width) {
    return std::string(text);
  }
  return absl::StrCat(text, std::string(options_.column_width - text.size(), ' '));
}

std::string DiffFormatter::FormatInstructionSide(const NormalizedInstruction& norm) const {
  std::string text;
  if (options_.show_offsets) {
    absl::StrAppendFormat(&text, "+0x%03X: ", norm.func_offset);
  }
  if (options_.show_hex) {
    absl::StrAppendFormat(&text, "%08X  ", norm.original.raw_word);
  }

  // Format intra-function branches and jumps using relative labels.
  if (norm.intra_function_target_offset.has_value()) {
    std::string rel_label = absl::StrFormat(".L_%04X", *norm.intra_function_target_offset);
    if (norm.original.IsBranch()) {
      absl::StrAppend(&text, FormatBranch(norm.original, rel_label));
    } else if (norm.original.opcode == Opcode::kJ) {
      absl::StrAppendFormat(&text, "j      %s", rel_label);
    } else {
      absl::StrAppend(&text, norm.original.Disassemble());
    }
  }
  // Format function calls with registered semantic symbol names.
  else if (norm.original.opcode == Opcode::kJal) {
    uint32_t target = norm.original.JumpTarget();
    if (symbols_ != nullptr && symbols_->HasAddress(target)) {
      const auto* entry = symbols_->FindByAddress(target);
      if (entry != nullptr && !entry->name().empty()) {
        absl::StrAppendFormat(&text, "jal    %s", entry->name());
      } else {
        absl::StrAppendFormat(&text, "jal    func_%08X", target);
      }
    } else {
      absl::StrAppendFormat(&text, "jal    func_%08X", target);
    }
  } else {
    absl::StrAppend(&text, norm.original.Disassemble());
  }

  return text;
}

std::string DiffFormatter::FormatRow(const AlignedDiffRow& row) const {
  std::string target_side;
  if (row.target.has_value()) {
    target_side = PadColumn(FormatInstructionSide(*row.target));
  } else {
    target_side = std::string(options_.column_width, ' ');
  }

  std::string compiled_side;
  if (row.compiled.has_value()) {
    compiled_side = PadColumn(FormatInstructionSide(*row.compiled));
  } else {
    compiled_side = std::string(options_.column_width, ' ');
  }

  std::string line;
  switch (row.kind) {
    case DiffRowKind::kMatch:
      if (options_.use_color) {
        line = absl::StrCat(kColorGreen, target_side, " | ", compiled_side, " [OK]", kColorReset);
      } else {
        line = absl::StrCat(target_side, " | ", compiled_side, " [OK]");
      }
      break;

    case DiffRowKind::kDifference: {
      std::string_view color =
          (row.quality == MatchQuality::kSimilarOpcode) ? kColorYellow : kColorRed;
      if (options_.use_color) {
        line = absl::StrCat(color, target_side, " | ", compiled_side, " [DIFF]", kColorReset);
      } else {
        line = absl::StrCat(target_side, " | ", compiled_side, " [DIFF]");
      }
      break;
    }

    case DiffRowKind::kTargetOnly:
      if (options_.use_color) {
        line =
            absl::StrCat(kColorRed, target_side, " | ", compiled_side, " [MISSING]", kColorReset);
      } else {
        line = absl::StrCat(target_side, " | ", compiled_side, " [MISSING]");
      }
      break;

    case DiffRowKind::kCompiledOnly:
      if (options_.use_color) {
        line = absl::StrCat(kColorCyan, target_side, " | ", compiled_side, " [EXTRA]", kColorReset);
      } else {
        line = absl::StrCat(target_side, " | ", compiled_side, " [EXTRA]");
      }
      break;
  }

  return line;
}

std::string DiffFormatter::FormatHeader(std::string_view func_name, std::string_view source_path,
                                        const AlignmentStats& stats) const {
  std::string out;
  absl::StrAppendFormat(&out, "\nFunction: %s\n", func_name);
  if (!source_path.empty()) {
    absl::StrAppendFormat(&out, "Source:   %s\n", source_path);
  }
  absl::StrAppendFormat(&out, "Target:   %zu instructions\n", stats.target_instruction_count);
  absl::StrAppendFormat(&out, "Current:  %zu instructions\n\n", stats.compiled_instruction_count);

  std::string target_hdr = PadColumn("TARGET (Original ROM)");
  std::string curr_hdr = PadColumn("CURRENT (Compiled C)");
  std::string sep = absl::StrCat(std::string(options_.column_width, '='), " | ",
                                 std::string(options_.column_width, '='));

  if (options_.use_color) {
    absl::StrAppend(&out, kColorDim, target_hdr, " | ", curr_hdr, kColorReset, "\n");
    absl::StrAppend(&out, kColorDim, sep, kColorReset, "\n");
  } else {
    absl::StrAppend(&out, target_hdr, " | ", curr_hdr, "\n");
    absl::StrAppend(&out, sep, "\n");
  }

  return out;
}

std::string DiffFormatter::FormatSummary(const AlignmentStats& stats) const {
  std::string sep = absl::StrCat(std::string(options_.column_width, '='), " | ",
                                 std::string(options_.column_width, '='));
  std::string out;
  if (options_.use_color) {
    absl::StrAppend(&out, kColorDim, sep, kColorReset, "\n");
  } else {
    absl::StrAppend(&out, sep, "\n");
  }

  if (stats.IsBitExactMatch()) {
    if (options_.use_color) {
      absl::StrAppendFormat(
          &out, "\n%s%s*** [MATCH 100%%] %zu/%zu instructions match bit-exact! ***%s\n", kColorBold,
          kColorGreen, stats.matching_instructions, stats.target_instruction_count, kColorReset);
    } else {
      absl::StrAppendFormat(&out, "\n*** [MATCH 100%%] %zu/%zu instructions match bit-exact! ***\n",
                            stats.matching_instructions, stats.target_instruction_count);
    }
  } else {
    if (options_.use_color) {
      absl::StrAppendFormat(
          &out, "\n%sMatch: %zu/%zu instructions (%.1f%%) [%zu diff, %zu missing, %zu extra]%s\n",
          kColorYellow, stats.matching_instructions, stats.target_instruction_count,
          stats.MatchPercentage(), stats.differing_instructions, stats.target_only_instructions,
          stats.compiled_only_instructions, kColorReset);
    } else {
      absl::StrAppendFormat(
          &out, "\nMatch: %zu/%zu instructions (%.1f%%) [%zu diff, %zu missing, %zu extra]\n",
          stats.matching_instructions, stats.target_instruction_count, stats.MatchPercentage(),
          stats.differing_instructions, stats.target_only_instructions,
          stats.compiled_only_instructions);
    }
  }

  return out;
}

std::string DiffFormatter::FormatDiff(std::string_view func_name, const AlignmentResult& result,
                                      std::string_view source_path) const {
  std::string out = FormatHeader(func_name, source_path, result.stats);
  for (const auto& row : result.rows) {
    absl::StrAppend(&out, FormatRow(row), "\n");
  }
  absl::StrAppend(&out, FormatSummary(result.stats));
  return out;
}

std::string DiffFormatter::FormatRomDiff(const RomDiffResult& result) const {
  return FormatRomDiff(result, options_.use_color);
}

std::string DiffFormatter::FormatRomDiff(const RomDiffResult& result, bool use_color) {
  std::string_view bold = use_color ? kColorBold : "";
  std::string_view green = use_color ? kColorGreen : "";
  std::string_view red = use_color ? kColorRed : "";
  std::string_view reset = use_color ? kColorReset : "";

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

  int64_t total_shift_offset = 0;
  if (!result.asset_segments.empty()) {
    total_shift_offset = result.asset_segments.front().shift_offset;
  }

  std::string size_status;
  if (result.structural.size_matches) {
    size_status = "Exact match";
  } else if (result.structural.code_shift_matches) {
    size_status =
        absl::StrFormat("Shift match: %s bytes", format_signed_commas(total_shift_offset));
  } else {
    size_status = "MISMATCH";
  }

  std::string report;
  absl::StrAppend(
      &report,
      "================================================================================\n");
  absl::StrAppend(&report, bold, "[LIFTED C ROM STRUCTURAL VALIDATION]", reset, "\n");
  absl::StrAppendFormat(&report, "Target ROM SHA-1: %s\n", result.structural.target_sha1);
  absl::StrAppendFormat(&report, "Built  ROM SHA-1: %s\n", result.structural.built_sha1);
  absl::StrAppendFormat(&report, "Cartridge Size:   %s bytes (%s)\n",
                        format_commas(result.structural.built_size), size_status);
  absl::StrAppendFormat(
      &report, "N64 Header & IPL3: %s / %s bytes (%.2f%% bit-exact)\n",
      format_commas(result.header_matching_bytes), format_commas(result.header_total_bytes),
      (result.header_total_bytes > 0)
          ? (static_cast<double>(result.header_matching_bytes) / result.header_total_bytes * 100.0)
          : 0.0);

  if (result.structural.is_valid) {
    if (result.structural.code_shift_matches) {
      absl::StrAppend(&report, green,
                      "[SUCCESS] ROM structural integrity verified (code segment shifted with "
                      "bit-exact assets)!",
                      reset, "\n");
    } else {
      absl::StrAppend(&report, green, "[SUCCESS] ROM structural integrity verified!", reset, "\n");
    }
  } else {
    absl::StrAppendFormat(&report, "%s[FAIL] Structural validation failed: %s%s\n", red,
                          result.structural.failure_reason, reset);
  }

  absl::StrAppend(
      &report,
      "--------------------------------------------------------------------------------\n");
  absl::StrAppend(&report, bold, "[SEGMENT-AWARE INSERTION/DELETION RESILIENT DIFF]", reset, "\n");

  for (size_t i = 0; i < result.code_segments.size(); ++i) {
    const auto& code_diff = result.code_segments[i];
    absl::StrAppendFormat(&report, "%d. Code Segment (.%s):\n", i + 1, code_diff.segment_name);
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
  }

  size_t base_idx = result.code_segments.size() + 1;
  for (size_t index = 0; index < result.asset_segments.size(); ++index) {
    const auto& asset = result.asset_segments[index];
    absl::StrAppendFormat(&report, "\n%d. Asset Segment (.%s):\n", base_idx + index,
                          asset.segment_name);
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

  return report;
}

}  // namespace rom_nom_nom
