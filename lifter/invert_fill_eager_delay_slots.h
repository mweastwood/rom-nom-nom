#ifndef LIFTER_INVERT_FILL_EAGER_DELAY_SLOTS_H_
#define LIFTER_INVERT_FILL_EAGER_DELAY_SLOTS_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{fill\_eager\_delay\_slots}}$ of GCC 2.7.2's
// `reorg.c:fill_eager_delay_slots` (and its helpers `fill_slots_from_thread`,
// `steal_delay_list_from_target`, and `steal_delay_list_from_fallthrough`).
//
// Given a post-`fill_eager_delay_slots` control-flow graph `dbr_eager_cfg_x`
// (`RtlPassKind::kDbr`), reconstructs a pre-`fill_eager_delay_slots`
// control-flow graph such that executing `RunFillEagerDelaySlots` on the
// returned CFG reproduces `dbr_eager_cfg_x` bit-for-bit, while preserving
// same-block/simple delay-slot `SEQUENCE`s for downstream
// `InvertFillSimpleDelaySlots`.
//
// Returns an empty `RtlControlFlowGraph` if `dbr_eager_cfg_x` has no basic
// blocks or contains malformed RTL instruction patterns or out-of-range
// successor edges.
RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& dbr_eager_cfg_x,
                                              const CompilationProfile& profile);
RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& dbr_eager_cfg_x);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertFillEagerDelaySlots;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_FILL_EAGER_DELAY_SLOTS_H_
