#ifndef LIFTER_INVERT_RELAX_DELAY_SLOTS_H_
#define LIFTER_INVERT_RELAX_DELAY_SLOTS_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{relax\_delay\_slots}}$ of GCC 2.7.2's
// `reorg.c:relax_delay_slots` (and `reorg.c:try_merge_delay_insns`).
//
// Given a post-`relax_delay_slots` control-flow graph `dbr_relaxed_cfg_x`
// (`RtlPassKind::kDbr`), reconstructs a pre-`relax_delay_slots` control-flow
// graph such that executing `RunRelaxDelaySlots` on the returned CFG reproduces
// `dbr_relaxed_cfg_x` bit-for-bit, and restores instructions/edges merged or
// redirected by `relax_delay_slots` for downstream delay-slot unscheduling.
//
// Returns an empty `RtlControlFlowGraph` if `dbr_relaxed_cfg_x` has no basic
// blocks or contains malformed RTL instruction patterns or out-of-range
// successor edges.
RtlControlFlowGraph InvertRelaxDelaySlots(const RtlControlFlowGraph& dbr_relaxed_cfg_x);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertRelaxDelaySlots;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_RELAX_DELAY_SLOTS_H_
