#ifndef LIFTER_INVERSE_LOOP_OPT_STAGE_H_
#define LIFTER_INVERSE_LOOP_OPT_STAGE_H_

#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Stage 10 of the inverse compiler pipeline (`InverseLoopOptStage`).
//
// Inverts GCC 2.7.2 Stage 3 (`toplev.c:rest_of_compilation` `0x08071dea` ->
// `loop.c:loop_optimize` `0x08111c4d`, `find_and_verify_loops` `0x08115857`,
// `scan_loop` `0x08112366`, `move_movables` `0x08114795`, `strength_reduce`
// `0x08117004`, `emit_iv_add_mult` `0x0811b2bf`, `check_dbra_loop` `0x0811b6ca`,
// `final_biv_value` `0x08121899`) to transform a `.loop` `RtlControlFlowGraph`
// into a canonical `.cse` `RtlControlFlowGraph`.
//
// Specifically, this stage:
// 1. Reverses `move_movables` (`0x08114795`) by moving loop-invariant constants,
//    symbol addresses, floating-point constants, and tablejump setup sequences
//    from loop preheaders back into their loop-body consumer basic blocks.
// 2. Reverses `strength_reduce` (`0x08117004`) / `emit_iv_add_mult` (`0x0811b2bf`)
//    / `check_dbra_loop` (`0x0811b6ca`) / `final_biv_value` (`0x08121899`) by
//    removing synthesized GIV preheader initializations, loop-body GIV updates,
//    countdown loop reversals, and post-loop final BIV assignments.
// 3. Contracts single-successor preheader basic blocks created solely by
//    `loop_optimize` before `NOTE_INSN_LOOP_BEG`, and reverses loop-exit block
//    relocations performed by `find_and_verify_loops` (`0x08115857`).
class InverseLoopOptLifter {
 public:
  static void InvertLoopToCseRtlCfg(RtlControlFlowGraph* cfg);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_LOOP_OPT_STAGE_H_
