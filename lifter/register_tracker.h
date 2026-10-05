#ifndef LIFTER_REGISTER_TRACKER_H_
#define LIFTER_REGISTER_TRACKER_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/mips.h"
#include "lifter/control_flow_graph.h"

namespace rom_nom_nom {

// Classification of a tracked register's value.
enum class ValueKind {
  kUnknown,      // Value is indeterminate or external
  kConstant,     // Known 32-bit integer constant
  kStackOffset,  // Value is an offset relative to initial stack pointer ($sp)
  kSymbolHi,     // Result of lui for a high 16-bit address
};

// Represents a tracked register value.
struct TrackedValue {
  ValueKind kind = ValueKind::kUnknown;
  uint32_t constant_value = 0;
  int32_t stack_offset = 0;
  uint32_t symbol_hi = 0;

  static TrackedValue Constant(uint32_t val) {
    TrackedValue v;
    v.kind = ValueKind::kConstant;
    v.constant_value = val;
    return v;
  }

  static TrackedValue StackOffset(int32_t offset) {
    TrackedValue v;
    v.kind = ValueKind::kStackOffset;
    v.stack_offset = offset;
    return v;
  }

  static TrackedValue SymbolHi(uint32_t hi_val) {
    TrackedValue v;
    v.kind = ValueKind::kSymbolHi;
    v.symbol_hi = hi_val;
    return v;
  }
};

// Specifies which registers are read (used) and written (defined) by an instruction.
struct RegisterUseDef {
  std::vector<Register> gpr_uses;
  std::vector<Register> gpr_defs;
  std::vector<FpRegister> fpr_uses;
  std::vector<FpRegister> fpr_defs;
  bool uses_hi = false;
  bool uses_lo = false;
  bool defs_hi = false;
  bool defs_lo = false;
};

// Extracts the use/def register sets for any decoded MIPS instruction.
RegisterUseDef GetInstructionUseDef(const Instruction& inst);

// Represents a register definition site.
struct Definition {
  uint32_t vram = 0;
  size_t instruction_index = 0;
  Register reg = Register::kZero;
  const Instruction* inst = nullptr;
};

// Information discovered about a function's stack frame.
struct StackFrameInfo {
  uint32_t frame_size = 0;
  std::optional<int32_t> saved_ra_offset;
  absl::flat_hash_map<Register, int32_t> saved_gpr_offsets;
  bool is_leaf = true;
};

// Tracks register state, constant values, and def-use chains across instructions.
class RegisterTracker {
 public:
  RegisterTracker();

  // Analyzes a sequence of instructions within a basic block or function.
  void Analyze(absl::Span<const Instruction> instructions, size_t start_instruction_index = 0);

  // Queries register state at a given instruction index.
  TrackedValue GetRegisterValue(Register reg) const;
  bool IsConstant(Register reg) const;
  std::optional<uint32_t> GetConstant(Register reg) const;

  // Def-use queries
  std::optional<Definition> GetReachingDefinition(Register reg) const;
  const std::vector<size_t>& GetUses(size_t def_instruction_index) const;

  // Stack frame information discovered during analysis
  const StackFrameInfo& FrameInfo() const { return frame_info_; }

  // Resets all tracked registers to initial state.
  void Reset();

 private:
  // Register state table (32 GPRs)
  std::vector<TrackedValue> gpr_values_;

  // Current reaching definition for each GPR
  std::vector<std::optional<Definition>> reaching_defs_;

  // Mapping from definition instruction index -> list of instruction indices that use it
  absl::flat_hash_map<size_t, std::vector<size_t>> def_to_uses_;

  StackFrameInfo frame_info_;

  static const std::vector<size_t>& EmptyVector();
};

}  // namespace rom_nom_nom

#endif  // LIFTER_REGISTER_TRACKER_H_
