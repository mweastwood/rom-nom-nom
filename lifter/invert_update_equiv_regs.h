#ifndef LIFTER_INVERT_UPDATE_EQUIV_REGS_H_
#define LIFTER_INVERT_UPDATE_EQUIV_REGS_H_

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse of `local-alloc.c:update_equiv_regs`
// (`SubfunctionId::kUpdateEquivRegs`).
//
// Given the post-`update_equiv_regs` `RtlControlFlowGraph`
// (`RtlPassKind::kLreg`) and active `CompilationProfile`, reconstructs a
// pre-`update_equiv_regs` `RtlControlFlowGraph` (`RtlPassKind::kSched`) by:
//   1. Restoring cross-block single-set / single-use `reg_equiv_init_insn`
//      definitions deleted by `update_equiv_regs` (`local-alloc.c:1079-1116`),
//      substituting the pseudo register and its `REG_DEAD` note back into the
//      using instruction, and restoring `sideband` reference, death, and
//      pre-doubled live-length counts.
//   2. Reversing intra-block `optimize_reg_copy_2` and `optimize_reg_copy_1`
//      register-copy propagations (`local-alloc.c:700-936`) when
//      `flag_expensive_optimizations` is active.
//   3. Demoting `REG_EQUIV <const>` notes (`CONSTANT_P`) back to `REG_EQUAL`
//      notes, stripping block-local `REG_EQUIV (mem ...)` notes attached by
//      `update_equiv_regs`, and halving doubled `sideband.reg_live_length`
//      entries, such that replaying `SubfunctionId::kUpdateEquivRegs`
//      reproduces `lreg_cfg` identically.
absl::StatusOr<RtlControlFlowGraph> InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg,
                                                          const CompilationProfile& profile);

absl::StatusOr<RtlControlFlowGraph> InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_INVERT_UPDATE_EQUIV_REGS_H_
