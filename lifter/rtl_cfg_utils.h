#ifndef ROM_NOM_NOM_LIFTER_RTL_CFG_UTILS_H_
#define ROM_NOM_NOM_LIFTER_RTL_CFG_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// GCC 2.7.2 interned `(const_int C)` bound (`emit-rtl.c:167-174, 308-309,
// 3323-3327`, `#define MAX_SAVED_CONST_INT 64`). Constants in
// `[-kGccMaxSavedConstInt, kGccMaxSavedConstInt]` share a single `const_int_rtx`
// pointer, which affects pointer-equality checks (e.g., `REG_EQUAL`/`REG_EQUIV`
// preservation across `jump.c:find_cross_jump`).
inline constexpr int64_t kGccMaxSavedConstInt = 64;

// Controls how `IsValidCfg` treats blocks with empty `instructions`.
enum class CfgEmptyBlockPolicy {
  // Permits empty blocks at any index (used by `InvertMakeReturnInsns`).
  kAllowAnyEmptyBlock,
  // Permits an empty block only at the trailing block when `num_blocks > 1`
  // (used by `InvertRelaxDelaySlots`, `InvertFillEagerDelaySlots`,
  // `InvertFillSimpleDelaySlots`).
  kAllowTrailingEmptyBlock,
  // Permits an empty block only when it is a terminal trailing end-label block
  // (`b + 1 == num_blocks && num_blocks > 1 && successors.empty() &&
  // addr_vec_targets.empty()`; used by `InvertDoCrossJump`).
  kAllowTrailingTerminalEmptyBlockOnly,
};

// Options controlling canonical label synchronization in
// `SynchronizeCanonicalLabels`.
struct CanonicalLabelSyncOptions {
  // When true, swaps a 2-successor conditional branch block's successors so
  // the taken target is `successors[0]` and the fallthrough `b + 1` is
  // `successors[1]`.
  bool normalize_cond_branch_successor_order = false;
  // When true, formats rewritten non-sequence instructions without
  // `CALL_INSN_FUNCTION_USAGE` (`FormatInsnWithoutCallFusage`).
  bool strip_call_fusage_on_rewrite = false;
};

// Validates that an `RtlExpr` subtree contains no unknown opcodes, no bare
// `(return)` outside a MIPS `return_internal` `(parallel [(return) ...])`
// (`config/mips/mips.md:5934`), and that any `(sequence [...])` node is a
// bracketed sequence of at least 2 valid inner insn wrappers.
bool IsValidRtlExprSubtree(const RtlExpr& root);

// Validates that `sexpr` parses cleanly as a structured `RtlPatternAndNotes`
// AST with recognized RTL operators and well-formed pattern and `REG_NOTES`
// subtrees.
bool IsValidRtlPatternSexpr(std::string_view sexpr);

// Validates that `cfg` is a structurally well-formed `RtlControlFlowGraph`
// (non-empty block list, in-range successors and `addr_vec_targets`, valid RTL
// patterns, at least one instruction overall, and empty-block compliance per
// `empty_policy`).
bool IsValidCfg(const RtlControlFlowGraph& cfg,
                CfgEmptyBlockPolicy empty_policy = CfgEmptyBlockPolicy::kAllowTrailingEmptyBlock);

// Rewrites all `(label_ref ...)` nodes in `expr` to `new_label_tok`.
void RewriteJumpTargetLabelRef(RtlExpr* expr, std::string_view new_label_tok);

// Recomputes canonical label names (`L0`, `L1`, ...) in linear RPO chain order
// and synchronizes `has_label` and `(label_ref L<k>)` across all jump
// instructions, delay-slot sequences, and tablejumps (`addr_vec` labels).
void SynchronizeCanonicalLabels(RtlControlFlowGraph* cfg,
                                const CanonicalLabelSyncOptions& options = {});

// Builds the per-block predecessor adjacency list from `successors` and
// deduplicated `addr_vec_targets`.
std::vector<std::vector<size_t>> BuildPredecessors(const RtlControlFlowGraph& cfg);

// Inserts `new_blk` at `insert_idx` in `cfg->rpo_blocks` and increments all
// successor and `addr_vec_targets` indices `>= insert_idx` on existing blocks.
void InsertBlockAt(RtlControlFlowGraph* cfg, size_t insert_idx, RtlBasicBlock new_blk);

// Returns true for MIPS `return_internal` (`config/mips/mips.md:5934`):
// `(parallel [(return) ...])`.
bool IsReturnExpr(const RtlExpr& expr);

// Mirrors `jump.c:2933-2940` (`simplejump_p`): returns true for
// `(set (pc) (label_ref ...))`.
bool IsSimpleUncondJumpExpr(const RtlExpr& expr);

// Parses `pat` and returns true iff its pattern satisfies
// `IsSimpleUncondJumpExpr`.
bool IsSimpleUncondJumpInsn(std::string_view pat);

// Returns true for MIPS `tablejump_internal1`/`tablejump_internal2`
// (`config/mips/mips.md:5881-5910`):
// `(parallel [(set (pc) (reg ...)) (use (label_ref ...)) ...])`.
bool IsTableJumpExpr(const RtlExpr& expr);

// Mirrors `jump.c:2946-3000` (`condjump_p` / `condjump_in_parallel_p`):
// returns the `(if_then_else cond arm1 arm2)` source of a `(set (pc) ...)`
// (either top-level or element 0 of a `parallel`), or nullptr otherwise.
const RtlExpr* GetCondJumpIfThenElse(const RtlExpr& expr);
RtlExpr* GetMutableCondJumpIfThenElse(RtlExpr* expr);

// Mirrors `jump.c:2946-3000` (`condjump_p` / `condjump_in_parallel_p`) for
// conditional jumps whose arms are one `(label_ref ...)` and one `(pc)`.
bool IsCondJumpExpr(const RtlExpr& expr);

// Mirrors `jump.c:2737-2775` (`reverse_condition`): returns the negated
// relational `RtlOp`, or `RtlOp::kUnknown` for non-comparison opcodes.
RtlOp ReverseComparisonOp(RtlOp op);

// Inverts the comparison opcode of a conditional jump in-place (`jump.c:2737-
// 2775, 3643`). Returns false if `jump_pat` is not a reversible conditional
// jump.
bool TryInvertCondJumpExpr(RtlExpr* jump_pat);

// Returns true if `expr` is a top-level jump or return instruction pattern
// (including a `(sequence [(jump_insn ...) ...])` delay-slot bundle).
bool IsAnyJumpOrReturnTopExpr(const RtlExpr& expr);

// Returns true if `expr` is an unconditional jump, return, or tablejump
// (including a `(sequence [(jump_insn ...) ...])` wrapping one of those).
bool IsUncondJumpOrReturnOrTableJumpTopExpr(const RtlExpr& expr);

// Returns true if `expr` is a `(use ...)`, `(clobber ...)`, or top-level
// jump/return control instruction.
bool IsEpilogueOrControlTopExpr(const RtlExpr& expr);

// Returns true if `pat` is a `NOTE_INSN_LOOP_CONT` or `NOTE_INSN_LOOP_VTOP`
// note node (`reorg.c:1365-1373`).
bool IsLoopHeaderNote(const RtlExpr& pat);

// Constructs a MIPS hard register `RtlExpr`, setting `str_val = "$fp"` for
// register 30 (`config/mips/mips.c:239-244` `mips_reg_names[30]`).
RtlExpr MakeGccHardReg(RtlMachineMode mode, int regno, RtlFlags flags = {});

// Formats `pn` via `FormatGccPrintRtlPatternAndNotes` with empty `call_fusage`.
std::string FormatInsnWithoutCallFusage(const RtlPatternAndNotes& pn);

// Mirrors `jump.c:4070-4086` (`true_regnum`) on post-reload RTL: returns
// `regno` for `REG`, `base + word_offset` for `SUBREG` of a register, or `-1`.
int TrueRegnum(const RtlExpr& expr);

// Returns true if any node in `expr` has opcode `RtlOp::kCall`.
bool ContainsCallInExpr(const RtlExpr& expr);

// Returns true for MIPS O32 integer argument registers `$4..$7` (`$a0..$a3`)
// and floating-point argument register words `$f12..$f15` (`44..47`)
// (`config/mips/mips.h:1915-1917` `FUNCTION_ARG_REGNO_P` + 2-word DF pairs).
bool IsMipsO32CallArgReg(int regno);

// Returns true if `node` references hard register word `regno` (accounting for
// 2-word `DI`/`DF` register spans and `SUBREG` word offsets).
bool SubtreeReferencesHardReg(const RtlExpr& node, int regno);

// Appends all register words defined or clobbered by `SET`/`CLOBBER` nodes in
// `expr` (`rtlanal.c:1035` `note_stores`).
void CollectDefinedRegs(const RtlExpr& expr, std::vector<int>* out);

// Returns true if `expr` reads hard register word `regno` in a source operand
// or inside a destination `MEM` address.
bool ExprReferencesRegInSrcOrMem(const RtlExpr& expr, int regno);

// Returns true if any node in `expr` is a `MEM`.
bool ExprHasMem(const RtlExpr& expr);

// Returns true if `a` and `b` have a register RAW/WAR/WAW conflict (either
// defines a register word referenced by the other).
bool ExpressionsConflict(const RtlExpr& a, const RtlExpr& b);

// Matches a MIPS prologue stack save `(set (mem ($sp | (plus $sp off))) (reg r))`
// (`config/mips/mips.c:4587` `save_restore_insns`), writing `*out_saved_reg` and
// `*out_sp_off`.
bool IsPrologueStackRegSave(const RtlExpr& expr, int* out_saved_reg, int64_t* out_sp_off);

// Mirrors `rtlanal.c:1229` (`find_regno_note(insn, REG_DEAD, regno)`): returns
// true if `parsed.reg_notes` contains a `REG_DEAD` note for `regno`.
bool HasRegDeadForRegno(const RtlPatternAndNotes& parsed, int regno);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_RTL_CFG_UTILS_H_
