#include "lifter/pass01_dbr_schedule.h"

#include "core/rtl_cfg.h"
#include "lifter/invert_fill_eager_delay_slots.h"
#include "lifter/invert_fill_simple_delay_slots.h"
#include "lifter/invert_make_return_insns.h"
#include "lifter/invert_relax_delay_slots.h"

namespace rom_nom_nom::lifter {

// Stage 1 (`reorg.c:dbr_schedule`) right-inverse stitcher composing the four
// `reorg.c` subfunction inverses in reverse compilation order:
//   1. `InvertMakeReturnInsns`
//   2. `InvertRelaxDelaySlots`
//   3. `InvertFillEagerDelaySlots`
//   4. `InvertFillSimpleDelaySlots`
RtlControlFlowGraph InvertPass01DbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) {
  RtlControlFlowGraph cfg = InvertMakeReturnInsns(dbr_cfg_x);
  if (cfg.rpo_blocks.empty()) {
    return {};
  }
  cfg = InvertRelaxDelaySlots(cfg);
  if (cfg.rpo_blocks.empty()) {
    return {};
  }
  cfg = InvertFillEagerDelaySlots(cfg);
  if (cfg.rpo_blocks.empty()) {
    return {};
  }
  cfg = InvertFillSimpleDelaySlots(cfg);
  return cfg;
}

}  // namespace rom_nom_nom::lifter
