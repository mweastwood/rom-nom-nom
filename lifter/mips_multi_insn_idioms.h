#ifndef LIFTER_MIPS_MULTI_INSN_IDIOMS_H_
#define LIFTER_MIPS_MULTI_INSN_IDIOMS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_idiom_utils.h"

namespace rom_nom_nom::lifter {

void CollectCompareDiamondDiPairs(const std::vector<std::vector<RtlPatternAndNotes>>& blocks,
                                  absl::flat_hash_set<int>* out_di_even_regs);
void CollectExistingDiEvenRegs(const std::vector<std::vector<RtlPatternAndNotes>>& blocks,
                               absl::flat_hash_set<int>* out_di_even_regs, bool* out_has_any_di);

// Folds `mips.md` `!TARGET_64BIT` multi-instruction `DImode` arithmetic, shift,
// and widening multiply templates (`mulsidi3_internal`, `umulsidi3_internal`,
// `adddi3_internal_1`, `adddi3_internal_2`, `subdi3_internal`,
// `ashldi3_internal2/3`, `ashrdi3_internal2/3`, `lshrdi3_internal2/3`).
bool TryFoldGp32DiMultiInsnIdiomsInBlock(const RtlControlFlowGraph& cfg, size_t b,
                                         bool mark_newly_folded_sentinel,
                                         std::vector<RtlPatternAndNotes>* insns,
                                         bool compute_live_in_when_sideband_empty = false);

struct MipsWitnessedDiFoldOptions {
  // True in Stage 1 (`reorg.c:try_split` after delay-slot unpacking) where the
  // two halves of a `movdi` register/HILO move may be separated by non-conflicting
  // instructions in the same block; false in Stage 0 (`final_scan_insn`) where
  // `mips_move_2words` emits contiguous instruction pairs.
  bool allow_noncontiguous_reg_move = false;
  bool mark_newly_folded_sentinel = false;
};

// Folds `mips.md` `!TARGET_64BIT` witnessed `DImode` bitwise, memory, register/HILO
// move, 64-bit constant load, and `$2` `DImode` return `(use ...)` patterns.
bool TryFoldGp32DiWitnessedIdioms(const RtlControlFlowGraph& cfg, int frame_size,
                                  const absl::flat_hash_set<int>& compare_di_pairs,
                                  const absl::flat_hash_set<int>& existing_di_regs,
                                  const MipsWitnessedDiFoldOptions& options,
                                  std::vector<std::vector<RtlPatternAndNotes>>* blocks);

// Folds contiguous 2-word `SImode` memory load/store pairs (`mips_move_2words`)
// into `DFmode` for GPR pairs `r` (`r < 32`, even) that already appear as
// `(reg:DF r)` in `blocks` (and are not `DImode`-witnessed).
bool TryFoldGp32DfInGprPairMoves(const RtlControlFlowGraph& cfg, int frame_size,
                                 const absl::flat_hash_set<int>& compare_di_pairs,
                                 const absl::flat_hash_set<int>& existing_di_regs,
                                 bool mark_newly_folded_sentinel,
                                 std::vector<std::vector<RtlPatternAndNotes>>* blocks);

struct MipsMovstrsiFoldOptions {
  bool gp64 = false;
  bool fp64 = false;
  bool has_di_witness = false;
  bool skip_unfilled_dbr_jump_tail = false;
  bool allow_delay_slot_split_movstrsi = false;
  bool mark_newly_folded_sentinel = false;
  bool compute_live_in_when_sideband_empty = false;
};

// Folds `mips_expand_block_move` / `output_block_move` (`mips.md:3573,3609`)
// load-then-store bursts into `movstrsi_internal` (and, when enabled,
// `movstrsi_internal2` + `movstrsi_internal3` split across a trailing delay slot).
bool TryFoldMovstrsiBlockMovesInBlock(
    const RtlControlFlowGraph& cfg, size_t b, const MipsMovstrsiFoldOptions& options,
    std::vector<RtlPatternAndNotes>* insns,
    std::optional<RtlPatternAndNotes>* epilogue_delay_pn = nullptr);

// Stage-0 `.12.dbr` wrapper around `RestoreNoConflictExtendClobbersWithTailCheck`
// that additionally checks whether a trailing `(sequence ...)` or
// `epilogue_delay_pattern` consumes the `DImode` pair once unpacked and folded.
void RestoreStage0DbrNoConflictExtendClobbers(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                              bool runs_sched2 = false);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_MIPS_MULTI_INSN_IDIOMS_H_
