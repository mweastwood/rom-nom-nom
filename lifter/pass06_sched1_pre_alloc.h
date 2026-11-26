#ifndef LIFTER_PASS06_SCHED1_PRE_ALLOC_H_
#define LIFTER_PASS06_SCHED1_PRE_ALLOC_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Whole-stage right-inverse $g_{\text{sched1}}$ of Stage 6
// (`sched.c:schedule_insns` when `reload_completed == 0`), inverting
// pre-allocation instruction scheduling (`schedule_block`) and pre-reload
// `define_split` instruction splitting (`try_split` + `update_flow_info`)
// in exact reverse order of `sched.c:4825-4854`.
//
// Given a post-`sched1` `.07.sched` `RtlControlFlowGraph` `sched1_cfg_x`
// (`RtlPassKind::kSched`) and `CompilationProfile` `profile`, recovers a
// pre-`sched1` `.06.combine` (`RtlPassKind::kCombine`) `RtlControlFlowGraph`
// such that re-running `schedule_insns` (when `optimize > 0 &&
// flag_schedule_insns != 0`, or passthrough when `sched1` is disabled)
// reproduces `sched1_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` (`rpo_blocks.empty()`) if
// `sched1_cfg_x` is empty or contains invalid / malformed RTL patterns,
// sideband, or CFG edges.
RtlControlFlowGraph InvertPass06Sched1PreAlloc(const RtlControlFlowGraph& sched1_cfg_x,
                                               const CompilationProfile& profile = {
                                                   .optimize_level = 2, .c_flags = {"-O2"}});

inline RtlControlFlowGraph InvertSched1PreAlloc(const RtlControlFlowGraph& sched1_cfg_x,
                                                const CompilationProfile& profile = {
                                                    .optimize_level = 2, .c_flags = {"-O2"}}) {
  return InvertPass06Sched1PreAlloc(sched1_cfg_x, profile);
}

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertPass06Sched1PreAlloc;
using lifter::InvertSched1PreAlloc;
}  // namespace rom_nom_nom

#endif  // LIFTER_PASS06_SCHED1_PRE_ALLOC_H_
