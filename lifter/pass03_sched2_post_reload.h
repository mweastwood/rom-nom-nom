#ifndef LIFTER_PASS03_SCHED2_POST_RELOAD_H_
#define LIFTER_PASS03_SCHED2_POST_RELOAD_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Whole-stage right-inverse $g_{\text{sched2}}$ of Stage 3
// (`function.c:thread_prologue_and_epilogue_insns` +
//  `sched.c:schedule_insns(..., /*after_reload=*/1)`):
//   1. `ReconstructRtlLiveness` (post-reload liveness bootstrap on `.10.sched2`)
//   2. `InvertScheduleBlock` (`sched.c:schedule_block`, when `RunsSched2()`)
//   3. `InvertThreadPrologueAndEpilogueInsns` (`mips.c:mips_expand_prologue`)
//   4. `ReconstructRtlLiveness` (post-prologue-strip `.09.greg` live-in sets)
//
// Given a post-`sched2` `RtlControlFlowGraph` `sched2_cfg_x`
// (`RtlPassKind::kSched2`) and `CompilationProfile` `profile`, recovers a
// pre-`sched2` (`.09.greg`, `RtlPassKind::kGreg`) `RtlControlFlowGraph` such
// that re-running `thread_prologue_and_epilogue_insns` + post-reload
// `schedule_insns` reproduces `sched2_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` (`rpo_blocks.empty()`) if
// `sched2_cfg_x` is empty or contains invalid / malformed RTL patterns or CFG
// edges.
RtlControlFlowGraph InvertPass03Sched2PostReload(const RtlControlFlowGraph& sched2_cfg_x,
                                                 const CompilationProfile& profile = {
                                                     .optimize_level = 2, .c_flags = {"-O2"}});

inline RtlControlFlowGraph InvertSched2PostReload(const RtlControlFlowGraph& sched2_cfg_x,
                                                  const CompilationProfile& profile = {
                                                      .optimize_level = 2, .c_flags = {"-O2"}}) {
  return InvertPass03Sched2PostReload(sched2_cfg_x, profile);
}

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertPass03Sched2PostReload;
using lifter::InvertSched2PostReload;
}  // namespace rom_nom_nom

#endif  // LIFTER_PASS03_SCHED2_POST_RELOAD_H_
