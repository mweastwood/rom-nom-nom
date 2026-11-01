#ifndef LIFTER_INVERSE_GREG_RELOAD_STAGE_H_
#define LIFTER_INVERSE_GREG_RELOAD_STAGE_H_

#include "lifter/rtl_cfg.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

// Stage 4 of the inverse compiler pipeline (`InverseGregReloadStage`).
//
// Inverts GCC 2.7.2 Stage 9 (`global.c:global_alloc` and `reload1.c:reload` / `emit_reload_insns`)
// to transform a `.greg` `RtlControlFlowGraph` into a canonical `.lreg` `RtlControlFlowGraph`.
class InverseGregReloadLifter {
 public:
  static void InvertGregToLregRtlCfg(RtlControlFlowGraph* cfg,
                                     const StackFrameAnalysis* frame_analysis = nullptr);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_GREG_RELOAD_STAGE_H_
