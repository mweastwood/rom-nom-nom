#ifndef LIFTER_INVERT_DO_CROSS_JUMP_H_
#define LIFTER_INVERT_DO_CROSS_JUMP_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{do\_cross\_jump}}$ of GCC 2.7.2's
// `jump.c:do_cross_jump` + `find_cross_jump` (invoked during Stage 2
// `.10.sched2 -> .11.jump2` via `jump_optimize(insns, /*cross_jump=*/1, ...)`).
//
// Given a post-`jump2` control-flow graph `jump2_cfg_x` (`RtlPassKind::kJump2`),
// reconstructs a pre-`jump2` (`.10.sched2`, `RtlPassKind::kSched2`)
// control-flow graph by un-merging cross-jumped tail instructions from a join
// block back into its incoming predecessors such that executing
// `jump_optimize` with `cross_jump = 1` reproduces `jump2_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` if `jump2_cfg_x` has no basic blocks
// or contains malformed RTL instruction patterns or out-of-range edges.
RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x);
RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                      const CompilationProfile& profile);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertDoCrossJump;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_DO_CROSS_JUMP_H_
