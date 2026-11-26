#ifndef ROM_NOM_NOM_LIFTER_INVERT_UPDATE_FLOW_INFO_H_
#define ROM_NOM_NOM_LIFTER_INVERT_UPDATE_FLOW_INFO_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Inverts `sched.c:update_flow_info` + `try_split` (`sched.c:4271-4660,
// 4825-4848`) for pre-allocation Stage 6 (`RtlPassKind::kSched`): re-merges
// split 32-bit `large_int` immediate moves (`config/mips/mips.md:3208-3221`:
// `(set OP0 (const_int HI))` followed by `(set OP0 (ior:SI OP0 (const_int LO)))`
// where `OP0` is `(reg:SI R)` or `(subreg:SI (reg ...) W)`, adjacent or
// separated only by instructions independent of `R`) back into a single
// `(set OP0 (const_int (HI | LO)))` instruction, restores `REG_NOTES` onto the
// merged instruction, preserves explicit `CALL_INSN_FUNCTION_USAGE`, and
// decrements `sideband.reg_n_sets[R]`.
//
// Returns an empty `RtlControlFlowGraph` if `scheduled_cfg` is empty or
// structurally invalid.
RtlControlFlowGraph InvertUpdateFlowInfo(const RtlControlFlowGraph& scheduled_cfg);

// Reconstructs pre-`sched1` `flow.c` liveness (`basic_block_live_at_start`,
// `REG_DEAD`/`REG_UNUSED` notes, and pre-sched register life statistics) on an
// unscheduled `.06.combine` CFG so GCC `sched.c:2715-2728, 3575-3583,
// 3967-3968` (`dead_notes` conservation) holds uniformly without branching on
// incoming sideband presence, while preserving `combine.c` `reg_n_refs`,
// negative `reg_live_length` sentinels (`sched.c:4946`), and `REG_BLOCK_GLOBAL`
// `reg_n_calls_crossed` (`sched.c:4962-4964`).
void RestorePreSched1FlowLiveness(RtlControlFlowGraph* cfg, const CompilationProfile& profile);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_INVERT_UPDATE_FLOW_INFO_H_
