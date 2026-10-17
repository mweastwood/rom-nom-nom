#ifndef LIFTER_STACK_FRAME_ANALYZER_H_
#define LIFTER_STACK_FRAME_ANALYZER_H_

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
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
  // Computes all stack frame properties in a unified analysis pass over the CFG.
  static StackFrameAnalysis Analyze(const ControlFlowGraph& cfg);

  // Analyzes function prologue blocks to identify callee-saved GPR and FPR spill offsets.
  static PrologueAnalysis AnalyzePrologue(const ControlFlowGraph& cfg, bool has_frame_pointer);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_STACK_FRAME_ANALYZER_H_
