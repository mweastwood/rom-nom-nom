#include "differ/diff_formatter.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/mips.h"
#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"
#include "splitter/symbol_registry.h"

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

}  // namespace rom_nom_nom
