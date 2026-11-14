#include "lifter/pass02_jump2_cross_jump.h"

#include "core/rtl_cfg.h"
#include "lifter/invert_do_cross_jump.h"

namespace rom_nom_nom::lifter {

// Stage 2 (`jump.c:jump_optimize(..., cross_jump=1, noop_moves=1,
// after_regscan=0)`) right-inverse stitcher delegating to `InvertDoCrossJump`
// (`jump.c:do_cross_jump` + `jump.c:find_cross_jump`):
//   1. `InvertDoCrossJump`
RtlControlFlowGraph InvertPass02Jump2CrossJump(const RtlControlFlowGraph& jump2_cfg_x) {
  return InvertPass02Jump2CrossJump(jump2_cfg_x, CompilationProfile{.optimize_level = 2});
}

RtlControlFlowGraph InvertPass02Jump2CrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                               const CompilationProfile& profile) {
  RtlControlFlowGraph cfg = InvertDoCrossJump(jump2_cfg_x, profile);
  if (cfg.rpo_blocks.empty()) {
    return {};
  }
  return cfg;
}

}  // namespace rom_nom_nom::lifter
