#ifndef LIFTER_PASS02_JUMP2_CROSS_JUMP_H_
#define LIFTER_PASS02_JUMP2_CROSS_JUMP_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Whole-stage right-inverse $g_{\text{jump2}}$ of Stage 2
// (`jump.c:jump_optimize(..., cross_jump=1, noop_moves=1, after_regscan=0)`):
//   `jump.c:do_cross_jump` + `jump.c:find_cross_jump`.
//
// Given a post-`jump2` `RtlControlFlowGraph` `jump2_cfg_x`
// (`RtlPassKind::kJump2`), delegates to `InvertDoCrossJump` to recover a
// pre-`jump2` (`.10.sched2`, `RtlPassKind::kSched2`) `RtlControlFlowGraph` such
// that re-running `jump2` reproduces `jump2_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` (`rpo_blocks.empty()`) if
// `jump2_cfg_x` is empty or contains invalid / malformed RTL patterns or CFG
// edges.
RtlControlFlowGraph InvertPass02Jump2CrossJump(const RtlControlFlowGraph& jump2_cfg_x);

inline RtlControlFlowGraph InvertJump2CrossJump(const RtlControlFlowGraph& jump2_cfg_x) {
  return InvertPass02Jump2CrossJump(jump2_cfg_x);
}

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertJump2CrossJump;
using lifter::InvertPass02Jump2CrossJump;
}  // namespace rom_nom_nom

#endif  // LIFTER_PASS02_JUMP2_CROSS_JUMP_H_
