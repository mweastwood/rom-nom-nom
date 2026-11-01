#ifndef LIFTER_INVERSE_SCHED1_PRE_ALLOC_STAGE_H_
#define LIFTER_INVERSE_SCHED1_PRE_ALLOC_STAGE_H_

#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Stage 6 of the inverse compiler pipeline (`InverseSched1PreAllocStage`).
//
// Inverts GCC 2.7.2 Stage 7 (`sched.c:schedule_insns` pre-allocation instruction splitting via
// `try_split` -> `insn-emit.c:gen_split_156`) to transform a `.sched` `RtlControlFlowGraph` into a
// canonical `.combine` `RtlControlFlowGraph`.
class InverseSched1PreAllocLifter {
 public:
  static void InvertSchedToCombineRtlCfg(RtlControlFlowGraph* cfg);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_SCHED1_PRE_ALLOC_STAGE_H_
