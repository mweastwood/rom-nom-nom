#ifndef LIFTER_INVERSE_CSE2_POST_LOOP_STAGE_H_
#define LIFTER_INVERSE_CSE2_POST_LOOP_STAGE_H_

#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Stage 9 of the inverse compiler pipeline (`InverseCse2PostLoopStage`).
//
// Inverts GCC 2.7.2 Stage 4 (`toplev.c:rest_of_compilation` post-loop `reg_scan` +
// `jump.c:jump_optimize(..., after_regscan=1)` + `cse.c:cse_main(..., after_loop=1)`) to transform
// a `.cse2` `RtlControlFlowGraph` into a canonical `.loop` `RtlControlFlowGraph`.
//
// In forward GCC 2.7.2 compilation (`-O2` / `-frerun-cse-after-loop`), `loop.c:strength_reduce`
// replaces memory and arithmetic uses of each general induction variable (GIV) address/multiply
// pseudo (`(set R_addr (plus:SI R_shift R_base))`, plus intermediate shift/add pseudos for
// compound non-power-of-two strides) with a strength-reduced pointer/accumulator GIV, leaving the
// original `(set R_addr ...)` instruction in `.loop` as a single-definition unused pseudo
// (`regno_first_uid[r] == regno_last_note_uid[r]`). Immediately after `.loop` is dumped,
// `jump_optimize(..., after_regscan=1)` (`0x080f6817`) deletes every such single-def unused pseudo
// before `.cse2` is dumped. This stage restores those strength-reduced GIV address/multiply
// instructions per basic block.
class InverseCse2PostLoopLifter {
 public:
  static void InvertCse2ToLoopRtlCfg(RtlControlFlowGraph* cfg);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_CSE2_POST_LOOP_STAGE_H_
