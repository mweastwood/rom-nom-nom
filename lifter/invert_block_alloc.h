#ifndef LIFTER_INVERT_BLOCK_ALLOC_H_
#define LIFTER_INVERT_BLOCK_ALLOC_H_

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse of `local-alloc.c:block_alloc` (`SubfunctionId::kBlockAlloc`).
//
// Given the post-`block_alloc` `RtlControlFlowGraph` (`RtlPassKind::kLreg`) and
// active `CompilationProfile`, reconstructs a pre-`block_alloc`
// `RtlControlFlowGraph` (`RtlPassKind::kLreg`) by clearing block-local register
// allocations in `sideband.reg_renumber`, restoring `(clobber (scratch:M))`
// operands (together with their shared `REG_UNUSED` notes) that `block_alloc`
// mutated in-place into hard-register clobbers when `optimize > 0`, and pruning
// non-explicit local-only callee-saved registers from `saved_gpr_regnos` /
// `saved_fpr_regnos`, such that replaying `SubfunctionId::kBlockAlloc`
// reproduces `lreg_cfg` and its `sideband.reg_renumber` table identically.
absl::StatusOr<RtlControlFlowGraph> InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg,
                                                     const CompilationProfile& profile);

absl::StatusOr<RtlControlFlowGraph> InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_INVERT_BLOCK_ALLOC_H_
