#ifndef ROM_NOM_NOM_LIFTER_REORG_MODEL_H_
#define ROM_NOM_NOM_LIFTER_REORG_MODEL_H_

#include <cstddef>

#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// MIPS delay-slot bearing instruction categories (`config/mips/mips.md:125-138`).
enum class MipsDelaySlotKind {
  // Conditional branch (`define_delay (eq_attr "type" "branch")`, `mips.md:125-128`).
  kBranch,
  // Unconditional jump or return (`define_delay (eq_attr "type" "jump")`, `mips.md:130-133`).
  kJump,
  // Function call (`define_delay (and (eq_attr "type" "call") (eq_attr "abicalls" "no"))`,
  // `mips.md:135-138`).
  kCall,
};

// Delay-slot annulling mode (`reorg.c:140-143, 1524-1566`, `config/mips/mips.md:125-138`).
enum class MipsDelaySlotAnnulMode {
  // Non-annulled delay slot (`eligible_for_delay`, slot 0 predicate).
  kNormal,
  // Annul when branch taken (`eligible_for_annul_true`, slot 1 predicate: always `(nil)` on MIPS).
  kAnnulTrue,
  // Annul when branch not taken (`eligible_for_annul_false`, slot 2 predicate:
  // `branch_likely == "yes"` on MIPS2+ branches only).
  kAnnulFalse,
};

// Target and slot-context configuration for `IsEligibleForMipsDelaySlot`.
struct MipsDelaySlotOptions {
  bool gp64 = false;
  bool fp64 = false;
  // MIPS ISA level (`-mips1` => 1, `-mips2` => 2; controls `branch_likely` at
  // `config/mips/mips.md:102-106`).
  int mips_isa = 1;
  // True under `-mabicalls` (`config/mips/mips.md:114-115, 135`).
  bool abicalls = false;
  // True when `-G N` (`N > 0`) permits single-instruction `$gp`-relative
  // small-data symbolic stores (`R` constraint, `length == 1`).
  bool allow_sdata_symbolic_store = false;
  MipsDelaySlotKind kind = MipsDelaySlotKind::kBranch;
  MipsDelaySlotAnnulMode annul_mode = MipsDelaySlotAnnulMode::kNormal;
};

// Models GCC 2.7.2 MIPS `define_delay` eligibility (`config/mips/mips.md:79-82,
// 102-106, 125-138`, `dslot == "no" && length == 1`, plus `branch_likely` /
// `abicalls` slot-kind predicates).
bool IsEligibleForMipsDelaySlot(const RtlExpr& pat, const MipsDelaySlotOptions& options = {});

// Models GCC 2.7.2 `reorg.c:1336-1429` (`mostly_true_jump`) + `reorg.c:1437-1474`
// (`get_branch_condition`) condition and branch-direction heuristic:
// returns `1` for likely-taken branches (`CONST_INT`, `NE`, `GE/GT` vs `0`, or
// backward branches `target_b <= block_b`) and `0` for unlikely-taken branches
// (`EQ`, `LE/LT` vs `0`, or forward branches).
int MostlyTrueJumpScore(const RtlExpr& jump_pat, size_t block_b, size_t target_b);

// Full CFG-aware model of GCC 2.7.2 `reorg.c:1287-1429` (`rare_destination` +
// `mostly_true_jump`), additionally scoring `NOTE_INSN_LOOP_BEG` (`2`),
// `NOTE_INSN_LOOP_VTOP` (`1`), and `rare_fallthrough - rare_dest` (`-1..2`)
// before falling back to `MostlyTrueJumpScore(jump_pat, block_b, target_b)`.
int MostlyTrueJumpScore(const RtlControlFlowGraph& cfg, size_t block_b, const RtlExpr& jump_pat,
                        size_t target_b);

// Mirrors GCC 2.7.2 `rtlanal.c:121-156` (`rtx_addr_can_trap_p`): returns false
// for static symbols, labels, stack/frame/arg pointer addresses (plus constant
// offset or `lo_sum`), and true for general addresses that may trap.
bool RtxAddrCanTrapForReorg(const RtlExpr& addr);

// Mirrors GCC 2.7.2 `rtlanal.c:1563-1634` (`may_trap_p`) used by
// `reorg.c:3376-3377` (`fill_slots_from_thread`): returns true if evaluating
// `pat` may trap (trapping `MEM`, `UNSPEC_VOLATILE`, integer division/mod by
// non-constant or zero, or floating-point arithmetic).
bool PatternMayTrapForReorg(const RtlExpr& pat);

// Returns true if `pat_notes` carries a `reorg.c:916-921` (`emit_delay_sequence`)
// placeholder `(expr_list:REG_DEAD (const_int 0) ...)` note.
bool HasRegDeadConstZeroNote(const RtlPatternAndNotes& pat_notes);

// Mirrors `reorg.c:791-795` (`find_end_label`): returns true if `blk` contains
// at least one `(use ...)` / `(clobber ...)` insn and no other non-note insns
// (when `skip_notes` is false, notes are also rejected).
bool BlockHasOnlyUseOrClobberInsns(const RtlBasicBlock& blk, bool skip_notes = true);

// Mirrors `reorg.c:791-795`: returns true if the first non-note instruction in
// `blk` is `(use ...)` or `(clobber ...)`.
bool BlockStartsWithUseOrClobberInsn(const RtlBasicBlock& blk);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_REORG_MODEL_H_
