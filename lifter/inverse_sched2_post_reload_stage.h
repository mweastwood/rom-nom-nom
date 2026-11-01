#ifndef LIFTER_INVERSE_SCHED2_POST_RELOAD_STAGE_H_
#define LIFTER_INVERSE_SCHED2_POST_RELOAD_STAGE_H_

#include <string_view>

#include "core/symbol_registry.h"
#include "lifter/rtl_cfg.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

// Stage 3 of the inverse compiler pipeline (`InverseSched2PostReloadStage`).
//
// Inverts GCC 2.7.2 Stage 10 (`function.c:thread_prologue_and_epilogue_insns` and
// `sched.c:schedule_insns` post-reload scheduling) to transform a `.sched2` `RtlControlFlowGraph`
// into a canonical `.greg` `RtlControlFlowGraph`.
class InverseSched2PostReloadLifter {
 public:
  static void InvertSched2ToGregRtlCfg(RtlControlFlowGraph* cfg,
                                       const StackFrameAnalysis* frame_analysis = nullptr,
                                       const SymbolIndex* symbols = nullptr,
                                       std::string_view function_name = "");
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_SCHED2_POST_RELOAD_STAGE_H_
