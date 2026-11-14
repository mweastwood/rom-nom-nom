#ifndef LIFTER_PASS01_DBR_SCHEDULE_H_
#define LIFTER_PASS01_DBR_SCHEDULE_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Whole-stage right-inverse $g_{\text{dbr\_schedule}}$ of Stage 1
// (`reorg.c:dbr_schedule`):
//   `reorg.c:make_return_insns` -> `reorg.c:relax_delay_slots` ->
//   `reorg.c:fill_eager_delay_slots` -> `reorg.c:fill_simple_delay_slots`.
//
// Given a post-`dbr_schedule` `RtlControlFlowGraph` `dbr_cfg_x`
// (`RtlPassKind::kDbr`), composes `InvertMakeReturnInsns`,
// `InvertRelaxDelaySlots`, `InvertFillEagerDelaySlots`, and
// `InvertFillSimpleDelaySlots` in reverse compilation order to recover a
// pre-`dbr_schedule` (`.11.jump2`) `RtlControlFlowGraph` such that re-running
// `dbr_schedule` reproduces `dbr_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` (`rpo_blocks.empty()`) if `dbr_cfg_x`
// is empty or contains invalid / malformed RTL patterns or CFG edges.
RtlControlFlowGraph InvertPass01DbrSchedule(const RtlControlFlowGraph& dbr_cfg_x,
                                            const CompilationProfile& profile);

RtlControlFlowGraph InvertPass01DbrSchedule(const RtlControlFlowGraph& dbr_cfg_x);

inline RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x,
                                             const CompilationProfile& profile) {
  return InvertPass01DbrSchedule(dbr_cfg_x, profile);
}

inline RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) {
  return InvertPass01DbrSchedule(dbr_cfg_x);
}

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertDbrSchedule;
using lifter::InvertPass01DbrSchedule;
}  // namespace rom_nom_nom

#endif  // LIFTER_PASS01_DBR_SCHEDULE_H_
