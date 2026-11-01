#ifndef LIFTER_INVERSE_JUMP2_CROSS_JUMP_STAGE_H_
#define LIFTER_INVERSE_JUMP2_CROSS_JUMP_STAGE_H_

#include "core/control_flow_graph.h"
#include "lifter/rtl_cfg.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

// Stage 2 of the inverse compiler pipeline (`InverseJump2CrossJumpStage`).
//
// Inverts GCC 2.7.2 Stage 11 (`jump.c:jump_optimize` post-reload cross-jumping, jump diamond
// simplifications, and `delete_noop_moves`) to transform a Stage 2 machine `ControlFlowGraph` into
// a canonical `.sched2` `RtlControlFlowGraph`.
class InverseJump2CrossJumpLifter {
 public:
  static RtlControlFlowGraph LiftToSched2RtlCfg(const ControlFlowGraph& input_cfg,
                                                const StackFrameAnalysis* frame_analysis = nullptr);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_JUMP2_CROSS_JUMP_STAGE_H_
