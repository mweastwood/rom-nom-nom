#ifndef LIFTER_PASS05_LREG_LOCAL_ALLOC_H_
#define LIFTER_PASS05_LREG_LOCAL_ALLOC_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Whole-stage right-inverse $g_{\text{lreg}}$ of Stage 5
// (`local-alloc.c:local_alloc`), composing in reverse compiler execution order:
//   1. `InvertBlockAlloc` (`local-alloc.c:block_alloc`)
//   2. `InvertUpdateEquivRegs` (`local-alloc.c:update_equiv_regs`)
//
// Given a post-`local_alloc` `.08.lreg` `RtlControlFlowGraph` `lreg_cfg_x`
// (`RtlPassKind::kLreg`) and `CompilationProfile` `profile`, recovers a
// pre-`local_alloc` `.07.sched` (`RtlPassKind::kSched`) `RtlControlFlowGraph`
// such that re-running `local_alloc` (when `optimize > 0`, or `-O0` passthrough
// when `optimize == 0`) reproduces `lreg_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` (`rpo_blocks.empty()`) if
// `lreg_cfg_x` is empty or contains invalid / malformed RTL patterns, sideband,
// or CFG edges.
RtlControlFlowGraph InvertPass05LregLocalAlloc(const RtlControlFlowGraph& lreg_cfg_x,
                                               const CompilationProfile& profile = {
                                                   .optimize_level = 2, .c_flags = {"-O2"}});

inline RtlControlFlowGraph InvertLregLocalAlloc(const RtlControlFlowGraph& lreg_cfg_x,
                                                const CompilationProfile& profile = {
                                                    .optimize_level = 2, .c_flags = {"-O2"}}) {
  return InvertPass05LregLocalAlloc(lreg_cfg_x, profile);
}

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertLregLocalAlloc;
using lifter::InvertPass05LregLocalAlloc;
}  // namespace rom_nom_nom

#endif  // LIFTER_PASS05_LREG_LOCAL_ALLOC_H_
