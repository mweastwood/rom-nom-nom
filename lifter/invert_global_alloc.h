#ifndef ROM_NOM_NOM_LIFTER_INVERT_GLOBAL_ALLOC_H_
#define ROM_NOM_NOM_LIFTER_INVERT_GLOBAL_ALLOC_H_

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Step 21: Reverses `global.c:global_alloc` (`SubfunctionId::kGlobalAlloc`) on
// a `.07.greg` (`SubfunctionId::kGlobalAlloc` post-state) `RtlControlFlowGraph`
// to recover the pre-global-alloc `.06.lreg` (`SubfunctionId::kBlockAlloc`
// post-state) `RtlControlFlowGraph`:
//   1. Validates CFG structure, basic-block edges, S-expression instructions,
//      and sideband register tables.
//   2. Partitions `sideband.reg_renumber` using structured `RtlExpr` def-use
//      and liveness analysis, removing hard-register assignments produced by
//      `global.c:global_alloc` (multi-block pseudos, pseudos live across basic
//      block boundaries, pseudos with non-unit death counts or upward-exposed
//      uses, and frame-pointer / multiply-divide register allocations) while
//      preserving single-block local allocations from `local-alloc.c:block_alloc`.
//   3. Transitions `pass` from `RtlPassKind::kGreg` back to `RtlPassKind::kLreg`
//      and updates `sideband.max_regno` so re-executing
//      `ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc)` reproduces
//      `greg_cfg` and `sideband.reg_renumber` bit-for-bit.
absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_INVERT_GLOBAL_ALLOC_H_
