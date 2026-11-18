#ifndef ROM_NOM_NOM_LIFTER_INVERT_GLOBAL_ALLOC_H_
#define ROM_NOM_NOM_LIFTER_INVERT_GLOBAL_ALLOC_H_

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Reverses `global.c:global_alloc` (`SubfunctionId::kGlobalAlloc`, or
// `stupid.c:stupid_life_analysis` at `-O0`) on a pre-reload `.07.greg`
// `RtlControlFlowGraph` to recover the pre-global-alloc `.06.lreg`
// (`SubfunctionId::kBlockAlloc` post-state) `RtlControlFlowGraph`:
//   1. Validates CFG structure, basic-block edges, S-expression instructions,
//      and sideband register tables.
//   2. Deterministically recomputes pseudo liveness (`basic_block_live_at_start`,
//      `reg_n_refs`, `reg_n_sets`, `reg_n_deaths`, `reg_live_length`,
//      `reg_n_calls_crossed`) from the RTL instructions and `profile` via
//      `ReconstructRtlLiveness` (preserving zero-occurrence phantom pseudos).
//   3. At `-O0` (`ParsedCompilerFlags::opt_level == 0`), clears
//      `sideband.reg_renumber` (`stupid_life_analysis` produced all hard-reg
//      assignments). At `-O1`/`-O2`/`-O3`, partitions `sideband.reg_renumber`
//      using structured `RtlExpr` def-use and recomputed liveness analysis,
//      removing assignments produced by `global.c:global_alloc` while
//      preserving single-block local allocations from `local-alloc.c:block_alloc`
//      (and restoring `MD_REGS` `DImode` widening-multiply local allocations
//      spilled during `global_alloc`).
//   4. Transitions `pass` from `RtlPassKind::kGreg` back to `RtlPassKind::kLreg`
//      so re-executing `ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc)`
//      reproduces `greg_cfg` and `sideband.reg_renumber` bit-for-bit.
absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg,
                                                      const CompilationProfile& profile);
absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_INVERT_GLOBAL_ALLOC_H_
