#ifndef LIFTER_STACK_FRAME_ANALYZER_H_
#define LIFTER_STACK_FRAME_ANALYZER_H_

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"

namespace rom_nom_nom {

// Callee-saved register stack offsets discovered during function prologue analysis.
struct PrologueAnalysis {
  absl::flat_hash_map<Register, int32_t> saved_reg_offsets;
  absl::flat_hash_map<FpRegister, int32_t> saved_fpr_offsets;
  uint32_t min_saved_reg_offset = 0xFFFFFFFF;
};

// Complete stack frame analysis results for a function CFG.
struct StackFrameAnalysis {
  int32_t frame_size = 0;
  int32_t max_sp_access = 0;
  int32_t sp_buffer_size = 0;
  int32_t outgoing_args_size = 0;
  bool has_frame_pointer = false;
  bool has_calls = false;
  bool sp_address_taken = false;
  PrologueAnalysis prologue;
  absl::flat_hash_map<int32_t, std::string> stack_stores;
  absl::flat_hash_set<int32_t> stack_store_offsets;
};

// Analyzes stack frame layout, callee-saved register spills, and stack buffer usage across a CFG.
class StackFrameAnalyzer {
 public:
  // Formats a stack offset into its canonical local variable name (`var_sp_<offset>` or
  // `var_sp_neg_<offset>`).
  static std::string StackVarName(int32_t offset);

  // Computes the maximum allocated stack frame size across all blocks in a CFG.
  static uint32_t ComputeFrameSize(const ControlFlowGraph& cfg);

  // Returns true if `imm` is a 4-byte-aligned caller-homed register argument slot (`arg0`..`arg3`
  // at `[frame_size, frame_size + 16)`) that is stored to in the function.
  static bool IsCallerHomedScalarAddr(int32_t frame_size, int32_t imm,
                                      const absl::flat_hash_set<int32_t>& stack_store_offsets);

  // Scans a sequence of instructions for frame size, frame pointer setup, calls, and stack stores.
  static void ScanInstructionsPass1(absl::Span<const Instruction> instructions,
                                    StackFrameAnalysis* analysis);

  // Scans a sequence of instructions for stack-address-taken uses using results from Pass 1.
  static void ScanInstructionsAddressTaken(
      absl::Span<const Instruction> instructions, StackFrameAnalysis* analysis,
      absl::flat_hash_set<int32_t>* addr_taken_offsets = nullptr);

  // Computes all stack frame properties in a unified analysis pass over the CFG.
  static StackFrameAnalysis Analyze(const ControlFlowGraph& cfg);

  // Analyzes function prologue blocks to identify callee-saved GPR and FPR spill offsets.
  static PrologueAnalysis AnalyzePrologue(const ControlFlowGraph& cfg, bool has_frame_pointer);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_STACK_FRAME_ANALYZER_H_
