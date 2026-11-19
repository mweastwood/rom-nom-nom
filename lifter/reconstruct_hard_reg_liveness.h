#ifndef LIFTER_RECONSTRUCT_HARD_REG_LIVENESS_H_
#define LIFTER_RECONSTRUCT_HARD_REG_LIVENESS_H_

#include <cstddef>
#include <vector>

#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// Returns the byte width of `mode` under GCC 2.7.2 MIPS `machmode.def`.
int MipsModeByteSize(RtlMachineMode mode);

// Mirrors GCC 2.7.2 `HARD_REGNO_NREGS(regno, mode)` (`mips.h:1395`).
int MipsHardRegNregs(int regno, RtlMachineMode mode, bool gp64, bool fp64);

// Determines MIPS 64-bit GPR (`gp64`) and FPR (`fp64`) width flags from
// `profile` (when non-null) or by inspecting `cfg`'s structured RTL.
void DetectMipsRegisterWidthFlags(const RtlControlFlowGraph& cfg, const CompilationProfile* profile,
                                  bool* gp64, bool* fp64);

// Parses `profile.optimize_level` and `profile.c_flags` into `ParsedCompilerFlags`.
ParsedCompilerFlags ParseCompilationProfileFlags(const CompilationProfile& profile);

// Populates `RtlInsn::call_fusage` and `has_explicit_call_fusage = true` via
// inter-block MIPS O32/N32 argument dataflow for any `CALL_INSN` (or `SEQUENCE`
// call head) in `cfg` that lacks explicit `CALL_INSN_FUNCTION_USAGE`. Leaves
// calls that already carry `has_explicit_call_fusage == true` untouched.
void ReconstructMissingCallFusage(RtlControlFlowGraph* cfg, const CompilationProfile& profile);

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

// Recomputes `cfg->sideband.basic_block_live_at_start` for a post-reload
// (`cfg->pass > RtlPassKind::kGreg`, no pseudo-registers) CFG under `profile`
// without mutating any instruction patterns or `REG_NOTES`. When
// `profile.optimize_level == 0`, clears `basic_block_live_at_start`; leaves
// pre-`sched2` (`<= kGreg`) and pseudo-register CFGs untouched.
void RecomputePostReloadLiveAtStart(RtlControlFlowGraph* cfg, const CompilationProfile& profile);
void RecomputePostReloadLiveAtStart(RtlControlFlowGraph* cfg);

// Resolves any `reorg.c:918-920` `(expr_list:REG_DEAD (const_int 0) ...)`
// delay-slot markers on unpacked post-reload top-level instructions back into
// `(expr_list:REG_DEAD (reg:<mode> <regno>) ...)` notes (and restores notes
// shifted by `reorg.c:2283-2306` `update_reg_dead_notes`), returning
// immediately without mutating `cfg` when `profile.optimize_level == 0` or no
// `(REG_DEAD (const_int 0))` notes are present.
void ResolvePostReloadConstZeroRegDeadNotes(RtlControlFlowGraph* cfg,
                                            const CompilationProfile& profile);
void ResolvePostReloadConstZeroRegDeadNotes(RtlControlFlowGraph* cfg);

// Normalizes post-reload hard-register `REG_DEAD` / `REG_UNUSED` notes on an
// unpacked post-reload CFG (e.g. after `InvertFillSimpleDelaySlots` folds
// delay-slot-split multi-insn idioms at `-O1` where `sched2` does not run)
// back to `.09.greg` / pre-`dbr_schedule` invariants.
void NormalizePostReloadHardRegNotes(RtlControlFlowGraph* cfg, const CompilationProfile& profile);

// Refreshes `update_equiv_regs`-synthesized `(expr_list:REG_EQUIV (mem ...) ...)`
// notes on a post-reload CFG after final Stage 0 / Stage 1 `MEM` flag (`mem/u`,
// `mem/v`), `REG_LIBCALL`/`REG_RETVAL`, and outgoing-argument updates.
void SynthesizePostReloadMemRegEquivNotes(RtlControlFlowGraph* cfg,
                                          const CompilationProfile& profile);

// Computes post-reload hard-register live-in sets purely from `cfg`'s
// instructions, control-flow edges, and frame/epilogue metadata (ignoring
// `cfg.sideband.basic_block_live_at_start`), returning true if any hard
// register defined by `def_pat` (accounting for multi-word modes such as
// `DImode`/`DFmode`) is live at entry to `block_idx`.
bool IsAnyDefinedHardRegLiveAtBlockEntry(const RtlControlFlowGraph& cfg, const RtlExpr& def_pat,
                                         size_t block_idx);

// Computes post-reload hard-register live-in sets per RPO block purely from
// `cfg`'s instructions, control-flow edges, and frame/epilogue metadata
// (ignoring `cfg.sideband.basic_block_live_at_start`, e.g. at `-O0`).
std::vector<std::vector<int>> ComputePostReloadHardRegLiveIn(const RtlControlFlowGraph& cfg);

// Returns true when `cfg` contains a multi-block caller-saved register web in a
// strictly later `REG_ALLOC_ORDER` slot of the same register class as
// `(ret_hr, ret_mode)` that is neither entry-live at Block 0 nor call-crossing
// / call-argument-bound and whose live range never conflicts with `ret_hr`
// (`global.c:871, 1508` `regs_someone_prefers` / `find_free_reg` witness that
// a shared return pseudo `p_out` occupied or reserved `ret_hr` before `jump2`
// deleted `(set (reg/i:M ret_hr) (reg:M ret_hr))`).
bool HasDeletedSharedReturnMoveWitness(const RtlControlFlowGraph& cfg,
                                       const CompilationProfile& profile, int ret_hr,
                                       RtlMachineMode ret_mode);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_RECONSTRUCT_HARD_REG_LIVENESS_H_
