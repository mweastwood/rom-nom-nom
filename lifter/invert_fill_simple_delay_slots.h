#ifndef LIFTER_INVERT_FILL_SIMPLE_DELAY_SLOTS_H_
#define LIFTER_INVERT_FILL_SIMPLE_DELAY_SLOTS_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{fill\_simple\_delay\_slots}}$ of GCC 2.7.2's
// `reorg.c:fill_simple_delay_slots` (and its helper `optimize_skip`).
//
// Given a post-`fill_simple_delay_slots` control-flow graph `dbr_simple_cfg_x`
// (`RtlPassKind::kDbr`), reconstructs a pre-`dbr_schedule` (`.11.jump2`)
// control-flow graph such that executing `RunFillSimpleDelaySlots` on the
// returned CFG reproduces `dbr_simple_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` if `dbr_simple_cfg_x` has no basic
// blocks or contains malformed RTL instruction patterns or out-of-range
// successor edges.
RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x,
                                               const CompilationProfile& profile);

RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertFillSimpleDelaySlots;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_FILL_SIMPLE_DELAY_SLOTS_H_
