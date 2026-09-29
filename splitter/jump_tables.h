#ifndef SPLITTER_JUMP_TABLES_H_
#define SPLITTER_JUMP_TABLES_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

// Represents a switch statement jump table located in .rodata.
struct JumpTable {
  uint32_t vram_start = 0;              // VRAM address of the table start
  std::string name;                     // e.g. "jtbl_80025D80"
  std::vector<uint32_t> entry_targets;  // Target VRAM address for each case

  size_t SizeInBytes() const { return entry_targets.size() * sizeof(uint32_t); }
  bool Empty() const { return entry_targets.empty(); }
};

// Options for jump table detection and formatting.
struct JumpTableOptions {
  size_t min_entries = 3;     // Minimum consecutive valid branch targets to form a table
  bool emit_section = false;  // Emits ".section .rodata" header
  bool emit_globl = true;     // Emits ".globl <name>" directive
};

// Represents a function's code boundaries.
struct FunctionRange {
  uint32_t vram_start = 0;
  uint32_t vram_end = 0;
  std::string name;
};

// Detects and formats switch jump tables in .rodata.
class JumpTableResolver {
 public:
  explicit JumpTableResolver(const SymbolIndex* symbols = nullptr, JumpTableOptions options = {});

  // Detects a jump table starting at a specific offset within rodata.
  // Entries must fall strictly within [func.vram_start, func.vram_end).
  std::optional<JumpTable> DetectTableAt(absl::Span<const uint8_t> rodata, size_t offset_in_rodata,
                                         uint32_t rodata_vram_start,
                                         const FunctionRange& func) const;

  // Scans rodata for all jump tables associated with a collection of functions.
  std::vector<JumpTable> ScanRodata(absl::Span<const uint8_t> rodata, uint32_t rodata_vram_start,
                                    absl::Span<const FunctionRange> functions) const;

  // Formats a single JumpTable as GNU assembly text (.word .LXXXXXXXX).
  std::string FormatJumpTableAsm(const JumpTable& jtbl) const;

  // Extracts all unique local target labels (.LXXXXXXXX) from a set of jump tables
  // that belong to a specific function range.
  std::vector<uint32_t> ExtractLocalLabels(absl::Span<const JumpTable> tables,
                                           const FunctionRange& func) const;

 private:
  const SymbolIndex* symbols_ = nullptr;
  JumpTableOptions options_;
};

}  // namespace rom_nom_nom

#endif  // SPLITTER_JUMP_TABLES_H_
