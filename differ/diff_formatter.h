#ifndef DIFFER_DIFF_FORMATTER_H_
#define DIFFER_DIFF_FORMATTER_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

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

  // Formats a single instruction side into a column string (unpadded).
  std::string FormatInstructionSide(const NormalizedInstruction& norm) const;

 private:
  std::string PadColumn(std::string_view text) const;

  DiffFormatterOptions options_;
  const SymbolIndex* symbols_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_DIFF_FORMATTER_H_
