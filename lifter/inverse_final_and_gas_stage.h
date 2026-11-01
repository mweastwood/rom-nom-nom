#ifndef LIFTER_INVERSE_FINAL_AND_GAS_STAGE_H_
#define LIFTER_INVERSE_FINAL_AND_GAS_STAGE_H_

#include <string>
#include <string_view>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "lifter/rtl_cfg.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

// Stage 0 of the inverse compiler pipeline (`InverseFinalAndGasStage`).
//
// Inverts GCC 2.7.2 Stage 13 (`final.c:final_scan_insn`, `config/mips/mips.c:function_epilogue`,
// and GNU `as` macro expansion) to lift a machine `ControlFlowGraph` into a canonical
// `RtlControlFlowGraph` matching GCC's `.dbr` RTL pass representation:
//   1. Coalesces linear fallthrough blocks split only by `jal`/`jalr` (`flow.c:find_basic_blocks`).
//   2. Strips `mips.c:function_epilogue` instructions and `.dbr` `current_function_epilogue_delay`
//      slots, and tags `expand_prologue` instructions (`is_prologue_insn = true`).
//   3. Strips `gas` division-by-zero trap sequences (`bnez` + `break 7`).
//   4. Fuses `mips.md` multi-instruction RTL patterns (`mulsi3_internal`, `floatsisf2`,
//      `floatsidf2`, `fix_truncsfsi2`, `fix_truncdfsi2`, `mips_move_2words`, `movstrsi_internal`)
//      and `gas` `%hi`/`%lo` macro expansions into atomic `RtlInsn` entries.
class InverseFinalAndGasLifter {
 public:
  // Lifts a machine `ControlFlowGraph` into a canonical `RtlControlFlowGraph`.
  static RtlControlFlowGraph LiftToRtlCfg(const ControlFlowGraph& input_cfg,
                                          const StackFrameAnalysis* frame_analysis = nullptr);

  // Lifts a Stage 2 machine `ControlFlowGraph` into a canonical `.sched2` `RtlControlFlowGraph` by
  // inverting GCC 2.7.2 Stage 11 (`jump.c:jump_optimize` cross-jumping, post-reload jump diamond
  // simplifications, and `delete_noop_moves`).
  static RtlControlFlowGraph LiftToSched2RtlCfg(const ControlFlowGraph& input_cfg,
                                                const StackFrameAnalysis* frame_analysis = nullptr);

  // Inverts GCC 2.7.2 Stage 10 (`function.c:thread_prologue_and_epilogue_insns` and
  // `sched.c:schedule_insns` post-reload scheduling) to transform a `.sched2` `RtlControlFlowGraph`
  // into a canonical `.greg` `RtlControlFlowGraph`.
  static void InvertSched2ToGregRtlCfg(RtlControlFlowGraph* cfg,
                                       const StackFrameAnalysis* frame_analysis = nullptr,
                                       const SymbolIndex* symbols = nullptr,
                                       std::string_view function_name = "");

  // Formats a single MIPS instruction into its canonical GCC 2.7.2 RTL pattern string.
  static std::string FormatMipsInstAsCanonicalRtl(const Instruction& inst);

  // Returns true if `inst` is a candidate `mips.c:function_epilogue` instruction (`jr $ra`,
  // positive `$sp` deallocation, `$sp = $fp` move, or callee-saved `$s0`..`$s7`/`$fp`/`$ra`/
  // `$f20`..`$f31` stack restore).
  static bool IsEpilogueCandidateInsn(const Instruction& inst);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_FINAL_AND_GAS_STAGE_H_
