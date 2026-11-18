#ifndef LIFTER_PASS04_GREG_RELOAD_H_
#define LIFTER_PASS04_GREG_RELOAD_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Whole-stage right-inverse $g_{\text{greg}}$ of Stage 4
// (`global.c:global_alloc` / `stupid.c:stupid_life_analysis` +
//  `reload1.c:reload` + `reload1.c:alter_reg`), composing in reverse compiler
// execution order:
//   1. `InvertAlterReg` (`reload1.c:alter_reg`)
//   2. `InvertReload` (`reload1.c:reload`)
//   3. `InvertGlobalAlloc` (`global.c:global_alloc` / `stupid.c:stupid_life_analysis`)
//
// Given a post-reload `.09.greg` `RtlControlFlowGraph` `greg_cfg_x`
// (`RtlPassKind::kGreg`) and `CompilationProfile` `profile`, recovers a
// pre-`global_alloc` `.06.lreg` (`RtlPassKind::kLreg`) `RtlControlFlowGraph`
// such that re-running `global_alloc` (or `stupid_life_analysis` + `reload` at
// `-O0`) reproduces `greg_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` (`rpo_blocks.empty()`) if
// `greg_cfg_x` is empty or contains invalid / malformed RTL patterns or CFG
// edges.
RtlControlFlowGraph InvertPass04GregReload(const RtlControlFlowGraph& greg_cfg_x,
                                           const CompilationProfile& profile = {
                                               .optimize_level = 2, .c_flags = {"-O2"}});

inline RtlControlFlowGraph InvertGregReload(const RtlControlFlowGraph& greg_cfg_x,
                                            const CompilationProfile& profile = {
                                                .optimize_level = 2, .c_flags = {"-O2"}}) {
  return InvertPass04GregReload(greg_cfg_x, profile);
}

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertGregReload;
using lifter::InvertPass04GregReload;
}  // namespace rom_nom_nom

#endif  // LIFTER_PASS04_GREG_RELOAD_H_
