#ifndef LIFTER_INVERT_DO_CROSS_JUMP_H_
#define LIFTER_INVERT_DO_CROSS_JUMP_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{do\_cross\_jump}}$ of GCC 2.7.2's post-reload `jump2`
// subfunction boundary (`SubfunctionId::kDoCrossJump`,
// `jump.c:jump_optimize(insns, /*cross_jump=*/1, /*noop_moves=*/1,
// /*after_regscan=*/0)`).
//
// Given a post-`jump2` control-flow graph `jump2_cfg_x` (`RtlPassKind::kJump2`),
// reconstructs a pre-`jump2` (`.10.sched2`, `RtlPassKind::kSched2`)
// control-flow graph by:
//   1. Un-merging cross-jumped tail instructions (`find_cross_jump` /
//      `do_cross_jump`) from join blocks back into incoming predecessors,
//   2. Restoring `noop_moves` death notes and deleted call-arg / HI-LO moves,
//   3. Un-expanding post-reload `HAVE_return` trailing `return_internal`
//      insertions (`jump.c:289-306, 2065-2081`) on zero-frame
//      `simple_epilogue_p()` leaf functions,
// such that executing `SubfunctionId::kDoCrossJump` reproduces `jump2_cfg_x`
// bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` if `jump2_cfg_x` has no basic blocks
// or contains malformed RTL instruction patterns or out-of-range edges.
RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x);
RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                      const CompilationProfile& profile);

// Restores post-reload noop self-moves `(set r r)` deleted at the start of
// `sched2` (`sched.c:4806-4818`: `insn != basic_block_end[b]`) prior to
// `schedule_block` when `flags.RunsSched2()` is active.
void RestoreSched2DeletedNoopMoves(RtlControlFlowGraph* cfg, const CompilationProfile& profile);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertDoCrossJump;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_DO_CROSS_JUMP_H_
