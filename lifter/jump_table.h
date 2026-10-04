#ifndef LIFTER_JUMP_TABLE_H_
#define LIFTER_JUMP_TABLE_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom {

// Represents a single entry in a jump table.
struct JumpTableEntry {
  uint32_t case_index = 0;   // 0, 1, 2, ...
  int64_t case_value = 0;    // Adjusted case value (case_index + lower_bound)
  uint32_t target_vram = 0;  // Target instruction VRAM address
};

// Represents a recovered MIPS jump table corresponding to a C switch statement.
struct JumpTable {
  uint32_t switch_vram = 0;                   // VRAM of the indirect `jr` instruction
  uint32_t bounds_branch_vram = 0;            // VRAM of the `beqz` bounds check branch
  uint32_t table_vram = 0;                    // Address of jump table in .rodata
  uint32_t num_cases = 0;                     // Number of entries in table
  int64_t lower_bound = 0;                    // Minimum case value (if adjusted via addiu)
  Register index_register = Register::kZero;  // Register holding the switch variable
  uint32_t default_target_vram = 0;           // Branch target when index >= num_cases
  std::vector<JumpTableEntry> entries;
};

// Reader callback to fetch 32-bit big-endian words from memory (e.g. .rodata / ROM).
using MemoryReader = std::function<std::optional<uint32_t>(uint32_t vram)>;

// Analyzes disassembled MIPS instructions to detect and resolve jump tables.
class JumpTableDetector {
 public:
  // Scans a function's instructions and detects all valid jump tables.
  static std::vector<JumpTable> DetectJumpTables(absl::Span<const Instruction> instructions,
                                                 const MemoryReader& memory_reader);

  // Attempts to detect a jump table for a specific `jr` instruction at `jr_index`.
  static std::optional<JumpTable> DetectAt(absl::Span<const Instruction> instructions,
                                           size_t jr_index, const MemoryReader& memory_reader);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_JUMP_TABLE_H_
