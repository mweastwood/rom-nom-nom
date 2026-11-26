#ifndef ROM_NOM_NOM_LIFTER_POST_RELOAD_NOOP_MOVES_H_
#define ROM_NOM_NOM_LIFTER_POST_RELOAD_NOOP_MOVES_H_

#include <vector>

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Restores post-reload noop self-moves `(set r r)` deleted by `jump2`
// (`jump.c:jump_optimize` with `noop_moves = 1`). When `sched2` ran prior to
// `jump2`, restores only block-end noop self-moves (`insn == basic_block_end[b]`)
// that survived `sched2`; when `sched2` did not run, restores all post-reload
// noop self-moves at the `jump2` boundary.
void RestoreJump2DeletedNoopMoves(RtlControlFlowGraph* cfg, const CompilationProfile& profile);

// Restores non-block-end post-reload noop self-moves `(set r r)` (and shared
// return-register moves) deleted by `sched2` (`sched.c:schedule_block` when
// `reload_completed != 0` and `prev_nonnote_insn` precedes `basic_block_end[b]`).
void RestoreSched2DeletedNoopMoves(RtlControlFlowGraph* cfg, const CompilationProfile& profile);

// Restores post-reload noop self-moves `(set r r)` deleted by `sched2` prior to
// `schedule_block` (`sched.c:schedule_insns`), including multi-cycle entry
// blocks and pre-call argument self-moves in non-entry blocks.
void RestoreSched2PreScheduleDeletedNoopMoves(RtlControlFlowGraph* cfg,
                                              const CompilationProfile& profile);

// Read-only query returning the hard-register numbers of entry-block (`B0`)
// incoming ABI parameter self-moves `(set (reg:M hr) (reg:M hr))` that
// `RestoreJump2DeletedNoopMoves` / `RestoreSched2DeletedNoopMoves` /
// `RestoreSched2PreScheduleDeletedNoopMoves` will restore for `cfg` under
// `profile`. Used by Stage 1 and Stage 3 `sched1` projection so `reg_n_sets`
// stays 100% aligned with Stage 4/5 `assign_parms` pseudo recovery.
std::vector<int> PredictRestoredEntryArgSelfMoveRegs(const RtlControlFlowGraph& cfg,
                                                     const CompilationProfile& profile);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_POST_RELOAD_NOOP_MOVES_H_
