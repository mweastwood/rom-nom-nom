#ifndef LIFTER_INVERT_UPDATE_EQUIV_REGS_H_
#define LIFTER_INVERT_UPDATE_EQUIV_REGS_H_

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse of `local-alloc.c:update_equiv_regs`
// (`SubfunctionId::kUpdateEquivRegs`).
//
// Given the post-`update_equiv_regs` `RtlControlFlowGraph`
// (`RtlPassKind::kLreg`), reconstructs a pre-`update_equiv_regs`
// `RtlControlFlowGraph` (`RtlPassKind::kSched`) by:
//   1. Demoting `REG_EQUIV` notes with constant payloads (`CONSTANT_P`) on
//      single-set pseudo-register definitions back to `REG_EQUAL` notes and
//      halving their doubled `sideband.reg_live_length[regno]` entries.
//   2. Stripping block-local `REG_EQUIV (mem ...)` notes attached by
//      `update_equiv_regs` to single-set pseudo-register definitions (and
//      halving `sideband.reg_live_length[regno]` when the `REG_EQUIV` note was
//      present at line 1058 of `update_equiv_regs` rather than added by a
//      subsequent store instruction).
//   3. Reversing intra-block `optimize_reg_copy_1` register-copy propagations
//      and restoring `REG_DEAD` notes and sideband live-length / reference
//      counts, such that replaying `SubfunctionId::kUpdateEquivRegs` reproduces
//      `lreg_cfg` identically.
absl::StatusOr<RtlControlFlowGraph> InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_INVERT_UPDATE_EQUIV_REGS_H_
