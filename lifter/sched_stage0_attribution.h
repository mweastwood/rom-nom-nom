#ifndef ROM_NOM_NOM_LIFTER_SCHED_STAGE0_ATTRIBUTION_H_
#define ROM_NOM_NOM_LIFTER_SCHED_STAGE0_ATTRIBUTION_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter::sched_model {

// Stage 0 `sched1`-unsplit `SImode` `(const_double:VOID ...)` constant-load
// recovery (`varasm.c:1857-1895`, `config/mips/mips.md:3208-3221`,
// `config/mips/mips.c:1047-1057`): `large_int` (`mips.md:3208`) splits
// `CONST_INT` during `sched1` (`reg_n_sets == 2`) but never splits
// `CONST_DOUBLE` (`reg_n_sets == 1`, `birthing_insn_p == true`), while
// `mips_move_1word` emits `li` macros that GAS expands into `lui; ori`
// machine words identical to split `CONST_INT`s. For any `-O2+`
// (`RunsSched1 && RunsSched2 && !TARGET_64BIT`) block not already
// `kSched1`-achievable, greedily collapses the minimal subset of eligible
// bit-31-set `SI` (`lui; ori`) constant-load spans into `(const_double:VOID ...)`
// iff the collapsed block is both `kSched2`- and `kSched1`-achievable.
void CollapseSched1UnsplitConstDoubleLoads(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile);

// Stage 0 erased `MEM_UNCHANGING_P` (`/u`) and `MEM_IN_STRUCT_P` (`/s`) alias
// flag attribution (`varasm.c:601-604`, `expr.c:4534, 4571-4586, 4889`,
// `sched.c:817-882`): `final.c` erases `MEM` alias qualifiers from emitted
// assembly, which can reintroduce spurious `true_dependence`/`anti_dependence`
// edges in `sched2` and `sched1` when `sched2` hoisted read-only or aggregate
// loads across non-aliasing stores. For any `-O2+` (`RunsSched1 && RunsSched2`)
// block not already `kSched1`-achievable, recovers the minimal subset of
// GCC-valid semantic alias groups (read-only static symbols, read-only varying
// pointer bases, non-zero-offset varying pointer aggregates, and contiguous
// local stack-array aggregates) iff the attributed block remains in
// `Im(F_sched2)` and admits a `kSched1`-achievable preimage.
// Returns true if any instruction's `MEM` alias flags were updated.
bool AttributeSched2ErasedMemAliasFlags(RtlControlFlowGraph* cfg,
                                        const CompilationProfile& profile);

// Stage 0 post-`sched2` stale `REG_DEAD` / `REG_EQUIV` note attribution
// (`sched.c:3415-3611, 3764-3830`, `local-alloc.c:540-665, 990-1000, 1049-1055`):
// post-reload `sched2` (`reload_completed == 1`) never updates `REG_DEAD` or
// `REG_EQUIV` notes when reordering pure readers or moving independent memory
// operations across a `MEM`-equivalent register's live range, and `final.c`
// erases all notes. For any `-O2+` (`RunsSched1 && RunsSched2`) block whose
// post-`sched2` order is not `kSched1`-achievable, transfers pure-reader
// `REG_DEAD` notes and synthesizes `sched2`-obstructed `REG_EQUIV (mem ...)`
// notes iff the attributed block passes exact forward `kSched2` replay,
// `PreservesPostReloadOrderSafety`, and `IsAchievable(kSched1)`.
void AttributeSched2PureReaderRegDeadNotes(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile);

}  // namespace rom_nom_nom::lifter::sched_model

#endif  // ROM_NOM_NOM_LIFTER_SCHED_STAGE0_ATTRIBUTION_H_
