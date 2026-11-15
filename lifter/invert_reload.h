#ifndef ROM_NOM_NOM_LIFTER_INVERT_RELOAD_H_
#define ROM_NOM_NOM_LIFTER_INVERT_RELOAD_H_

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Step 34: Reverses `reload1.c:reload` (`SubfunctionId::kReload`) on a
// `.09.greg` (`SubfunctionId::kReload` post-state or `InvertAlterReg` output)
// `RtlControlFlowGraph` to recover the pre-reload `.08.lreg` /
// `SubfunctionId::kGlobalAlloc` post-state `RtlControlFlowGraph`:
//   1. Reverses frame-pointer and argument-pointer register elimination
//      (`eliminate_regs` / `eliminate_regs_in_insn`) across the 3-region MIPS
//      stack layout (`[0, outgoing_args_size)` outgoing args at `$sp`,
//      `[outgoing_args_size, post_reload_tsize)` local frame at `$fp`, and
//      `[post_reload_tsize, ...)` incoming/homed args at `(reg:SI 0 argp)`),
//      including large-frame (`>= 32768`) address reloads and `REG_NOTES`.
//   2. Reverses `caller-save.c` (`save_call_clobbered_regs` /
//      `setup_save_areas`) save/restore pairs and unifies call-split
//      `InvertAlterReg` pseudos across calls without oracle sideband hints.
//   3. Reverses `global=1` spill/fill reload sequences (`reload_as_needed` /
//      `emit_reload_insns` / `subst_reloads`), unallocated pseudo stack-slot
//      replacements (`alter_reg`), and `PARALLEL` earlyclobber output reloads.
//   4. Reconstructs pre-reload `RtlSidebandData` (`reg_renumber`, `reg_n_refs`,
//      `reg_n_sets`, `reg_n_deaths`, `reg_live_length`, `reg_n_calls_crossed`,
//      `basic_block_live_at_start`, `frame_size_bytes`, `saved_gpr_regnos`,
//      `saved_fpr_regnos`) for upstream register allocator inversion.
//
// Upstream Phantom-Pseudo Contract (Steps 35..38 & `ReconstructRtlLiveness`):
//   When `InvertReload` detects an unreferenced 8-byte `alter_reg` stack-slot
//   gap between `starting_frame_offset` (above any pre-reload local frame
//   slots) and `pre_caller_save_top`, it synthesizes an unallocated phantom
//   pseudo `p >= 68` (`reg_renumber` absent, `reg_n_refs[p] = 2`,
//   `reg_n_sets` absent/`0`, `reg_n_deaths[p] = 1`, `reg_live_length[p] = 2`,
//   zero RTL occurrences) numbered in ascending 8-byte slot order interleaved
//   with real spilled pseudos (`combine.c:56`, `reload1.c:654-658`).
//   - Step 35 (`InvertGlobalAlloc`), Step 36 (`InvertBlockAlloc`), and Step 37
//     (`InvertUpdateEquivRegs`) preserve these zero-occurrence sideband entries
//     unchanged and pass them upstream to Step 38 (`InvertTryCombine`), which
//     recovers the combine-deleted temporary.
//   - Any post-`combine` stage (`.06.combine`..`.08.lreg`) that invokes
//     `ReconstructRtlLiveness` must pass
//     `RtlLivenessOptions{.preserve_phantom_pseudos = true}` so zero-occurrence
//     phantom counts are preserved; at `.05.flow` (`preserve_phantom_pseudos ==
//     false`), `ReconstructRtlLiveness` clears `reg_n_refs` on zero-occurrence
//     pseudos just like `flow.c:life_analysis`.
//   - Canonical pre-image for unreferenced local aggregates (`non_invalidated_begin == 0`):
//     an unreferenced 8-byte-aligned local aggregate allocated at
//     `starting_frame_offset` by `stmt.c:expand_decl` (e.g., `char buf[8];`
//     with zero RTL references) produces the exact same unaccessed 8-byte
//     `.09.greg` frame gap as an unallocated phantom `alter_reg` pseudo; both
//     pre-images satisfy $f(g(x)) = x$ across `reload`, and `InvertReload`
//     canonically attributes the gap to a phantom spilled pseudo (which
//     upstream `InvertGlobalAlloc`..`InvertExpandDecl` must account for).
absl::StatusOr<RtlControlFlowGraph> InvertReload(const RtlControlFlowGraph& reload_cfg,
                                                 const CompilationProfile& profile);
absl::StatusOr<RtlControlFlowGraph> InvertReload(const RtlControlFlowGraph& reload_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_INVERT_RELOAD_H_
