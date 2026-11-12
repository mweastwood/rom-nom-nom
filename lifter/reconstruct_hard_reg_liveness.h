#ifndef LIFTER_RECONSTRUCT_HARD_REG_LIVENESS_H_
#define LIFTER_RECONSTRUCT_HARD_REG_LIVENESS_H_

#include <cstddef>

#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// Reconstructs post-reload MIPS hard-register liveness and return-value uses on
// a Stage 0 (`kDbr`) CFG lifted from machine words / assembly:
//   1. Identifies non-void scalar return registers (`$2`/`$3` or `$f0`/`$f1`)
//      reaching function exit without an intervening read, re-def, or void-call
//      clobber, marks exit-reaching `SET` destinations with
//      `REG_FUNCTION_VALUE_P` (`reg/i`), and inserts the trailing
//      `(use (reg/i:<mode> <regno>))` pseudo-instruction when absent.
//   2. When `profile.optimize_level > 0`, populates
//      `cfg->sideband.basic_block_live_at_start` across all RPO blocks and
//      attaches `REG_DEAD` and `REG_UNUSED` notes to non-call instructions and
//      `SEQUENCE` sub-instructions; when `profile.optimize_level == 0`, leaves
//      `basic_block_live_at_start` empty and suppresses `REG_DEAD`/`REG_UNUSED`
//      notes.
void ReconstructPostDbrHardRegLivenessAndReturnUse(RtlControlFlowGraph* cfg,
                                                   const CompilationProfile& profile);

// Recomputes `cfg->sideband.basic_block_live_at_start` for an optimized
// post-reload (`cfg->sideband.reg_renumber.empty()`, no pseudo-registers) CFG
// without mutating any instruction patterns or `REG_NOTES`. Leaves `-O0` (empty
// `basic_block_live_at_start` with no post-flow/reorg indicators) and
// pre-reload CFGs untouched.
void RecomputePostReloadLiveAtStart(RtlControlFlowGraph* cfg);

// Computes post-reload hard-register live-in sets purely from `cfg`'s
// instructions, control-flow edges, and frame/epilogue metadata (ignoring
// `cfg.sideband.basic_block_live_at_start`), returning true if any hard
// register defined by `def_pat` (accounting for multi-word modes such as
// `DImode`/`DFmode`) is live at entry to `block_idx`.
bool IsAnyDefinedHardRegLiveAtBlockEntry(const RtlControlFlowGraph& cfg, const RtlExpr& def_pat,
                                         size_t block_idx);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_RECONSTRUCT_HARD_REG_LIVENESS_H_
