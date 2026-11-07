#ifndef GCC_HARNESS_SUBFUNCTION_INVERSES_H_
#define GCC_HARNESS_SUBFUNCTION_INVERSES_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "gcc_harness/rtl_cfg.h"

namespace rom_nom_nom::gcc_harness {

// Frame parameters recovered by `SubfunctionInverses::InvertFunctionPrologueEpilogue`
// for `mips.c:function_prologue` and `mips.c:function_epilogue`.
struct PrologueEpilogueFrameSpec {
  int var_size = 0;
  uint32_t saved_gpr_mask = 0;
};

// Pre-substitution arguments `(target_expr, from_reg, to_expr)` recovered by
// `SubfunctionInverses::InvertSubst` for `combine.c:subst`.
struct SubstInverseSpec {
  std::string target_expr;
  std::string from_reg;
  std::string to_expr;
};

// Reference 1-to-1 mathematical right-inverses $g_i$ for each of the 26
// individual KMC GCC 2.7.2 (`cc1`) and GNU Binutils 2.6 (`as`) C functions $f_i$
// exposed in `FunctionHooks`.
//
// Each method $g_i$ takes a target post-function representation $x$ and
// constructs a pre-function representation $y = g_i(x)$ such that executing the
// genuine in-process GCC/GAS C function $f_i(y)$ yields $f_i(g_i(x)) == x$
// with zero post-pass normalization or secret instruction fixups.
class SubfunctionInverses {
 public:
  // ---------------------------------------------------------------------------
  // Stage 0: Binutils 2.6 GAS (`tc-mips.c`) & GCC 2.7.2 Final (`final.c`, `mips.c`)
  // ---------------------------------------------------------------------------

  // 1. Inverse of `tc-mips.c:mips_ip`: decodes a 32-bit big-endian MIPS
  // instruction word `word_x` into its canonical GAS assembly line `y`.
  static std::string InvertGasMipsIp(uint32_t word_x);

  // 2. Inverse of `tc-mips.c:macro`: collapses a multi-word GAS macro expansion
  // `macro_words_x` (e.g. `lui` + `ori` / `addiu`) back into its single macro
  // assembly line `y` (e.g. `li $2,0x80012345`).
  static std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x);

  // 3. Inverse of `tc-mips.c:append_insn` (in `.set reorder` mode): recovers
  // the pre-`append_insn` sequence of assembly lines `y` from the emitted
  // machine words `words_x` by un-swapping delay-slot instructions or stripping
  // GAS-inserted delay-slot NOPs.
  static std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x);

  // 4. Inverse of `final.c:final_scan_insn`: lifts a single MIPS assembly
  // instruction emitted by `final_scan_insn` back into its RTL pattern `y`.
  static std::string InvertFinalScanInsn(std::string_view asm_line_x);

  // 5. Inverse of `mips.c:function_prologue` + `mips.c:function_epilogue`:
  // recovers `(var_size, saved_gpr_mask)` from emitted prologue/epilogue
  // assembly text `asm_text_x`.
  static PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(std::string_view asm_text_x);

  // Whole-stage helper for `final` + `md_assemble`: lifts a sequence of
  // 32-bit machine words `machine_words_x` into its `.dbr` `RtlControlFlowGraph`
  // (including `(sequence [...])` wrappers for filled branch/return delay slots).
  static RtlControlFlowGraph InvertFinalAndGasFunction(absl::Span<const uint32_t> machine_words_x);

  // ---------------------------------------------------------------------------
  // Stage 1: Delayed-Branch Scheduling (`reorg.c`)
  // ---------------------------------------------------------------------------

  // 6. Inverse of `reorg.c:make_return_insns` + `find_end_label`: replaces
  // `(return)` (standalone or inside a `SEQUENCE`) with a jump
  // `(set (pc) (label_ref L<end>))` targeting a trailing epilogue label block.
  static RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& x);

  // 7. Inverse of `reorg.c:relax_delay_slots`: generalized across all basic
  // blocks in `x`, replaces direct conditional branches with an inverted branch
  // over an inserted unconditional-jump trampoline block.
  static RtlControlFlowGraph InvertRelaxDelaySlots(const RtlControlFlowGraph& x);

  // 8. Inverse of `reorg.c:fill_eager_delay_slots` + `fill_slots_from_thread`:
  // generalized across all basic blocks in `x`, unschedules delay-slot
  // instructions stolen from either the branch target thread (`insn/s`) or the
  // fallthrough thread (`insn`) back to the entry of their origin thread.
  static RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& x);

  // 9. Inverse of `reorg.c:fill_simple_delay_slots`: generalized across all
  // basic blocks in `x`, unschedules intra-block `SEQUENCE` delay slots back
  // before the jump/branch in the same basic block.
  static RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& x);

  // 10. Inverse of `reorg.c:dbr_schedule`: exact composition of
  // `InvertFillSimpleDelaySlots(InvertFillEagerDelaySlots(InvertRelaxDelaySlots(InvertMakeReturnInsns(x))))`.
  static RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 2: Post-Reload Cross-Jumping (`jump.c`)
  // ---------------------------------------------------------------------------

  // 11. Inverse of `jump.c:do_cross_jump` + `find_cross_jump`: clones a shared
  // cross-jumped tail block back into its jumping predecessor blocks.
  static RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
  // ---------------------------------------------------------------------------

  // 12. Inverse of `sched.c:schedule_block` / `schedule_insns`: restores
  // load-latency-interleaved basic block instructions to their pre-scheduled
  // def-use order.
  static RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 4 & Stage 5: Register Allocation & Reload (`reload1.c`, `local-alloc.c`, `global.c`)
  // ---------------------------------------------------------------------------

  // 13. Inverse of `reload1.c:alter_reg` (and `global.c:global_alloc` / `reload`):
  // un-allocates a hard register assignment back to a pseudo-register
  // `(reg:SI <pseudo_regno>)`.
  static RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& x, int pseudo_regno = 72);

  // 14. Inverse of `local-alloc.c:block_alloc`: replaces physical scratch
  // registers allocated by `block_alloc` in `(clobber (reg:SI ...))` back with
  // unallocated `(clobber (scratch:SI))` operands.
  static RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& x);

  // 15. Inverse of `local-alloc.c:update_equiv_regs`: un-propagates local copy
  // / equivalence replacements performed by `update_equiv_regs`.
  static RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 7: Instruction Combination (`combine.c`)
  // ---------------------------------------------------------------------------

  // 16. Inverse of `combine.c:simplify_rtx`: constructs an unsimplified
  // compound RTX expression `y` whose simplification under `simplify_rtx` is
  // `simplified_rtx_x`.
  static std::string InvertSimplifyRtx(std::string_view simplified_rtx_x);

  // 17. Inverse of `combine.c:subst`: decomposes a substituted RTX expression
  // `subst_rtx_x` into `(target_expr, from_reg, to_expr)` such that
  // `subst(target_expr, from_reg, to_expr) == subst_rtx_x`.
  static SubstInverseSpec InvertSubst(std::string_view subst_rtx_x);

  // 18. Inverse of `combine.c:try_combine` / `combine_instructions`: splits a
  // combined instruction back into a linked def-use instruction pair `(i2, i3)`
  // over a dead intermediate pseudo-register.
  static RtlControlFlowGraph InvertTryCombine(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 8: Flow & Life Analysis (`flow.c`)
  // ---------------------------------------------------------------------------

  // 19. Inverse of `flow.c:find_basic_blocks`: injects an unreachable basic
  // block after an unconditional jump/return barrier that `find_basic_blocks`
  // excises during CFG construction.
  static RtlControlFlowGraph InvertFindBasicBlocks(const RtlControlFlowGraph& x);

  // 20. Inverse of `flow.c:propagate_block` / `life_analysis` / `flow_analysis`:
  // strips `REG_DEAD` notes and prepends an overwritten dead store that
  // `propagate_block` deletes while recomputing `REG_DEAD` notes.
  static RtlControlFlowGraph InvertPropagateBlock(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
  // ---------------------------------------------------------------------------

  // 21. Inverse of `cse.c:fold_rtx`: constructs an unfolded arithmetic RTX
  // expression `y` whose constant/identity folding under `fold_rtx` yields
  // `folded_rtx_x`.
  static std::string InvertFoldRtx(std::string_view folded_rtx_x);

  // 22. Inverse of `cse.c:cse_basic_block` / `cse_main`: un-eliminates a common
  // subexpression reuse `(set <dst> <cse_reg>)` back into the duplicate
  // computation `(set <dst> <cse_expr>)`.
  static RtlControlFlowGraph InvertCseBasicBlock(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 10: Loop Optimization (`loop.c`)
  // ---------------------------------------------------------------------------

  // 23. Inverse of `loop.c:scan_loop`: un-hoists a loop-invariant computation
  // from the loop preheader (before `NOTE_INSN_LOOP_BEG`) back into the loop
  // body.
  static RtlControlFlowGraph InvertScanLoop(const RtlControlFlowGraph& x);

  // 24. Inverse of `loop.c:strength_reduce`: un-reduces a strength-reduced
  // general induction variable (GIV) back into its loop-body address
  // computation from the basic induction variable (BIV).
  static RtlControlFlowGraph InvertStrengthReduce(const RtlControlFlowGraph& x);

  // ---------------------------------------------------------------------------
  // Stage 12: Initial Jump Optimization (`jump.c`)
  // ---------------------------------------------------------------------------

  // 25. Inverse of `jump.c:redirect_jump` / `invert_jump`: introduces an
  // intermediate forwarding jump label or inverted branch condition that
  // `redirect_jump` / `invert_jump` collapses back to `x`.
  static RtlControlFlowGraph InvertJumpRedirect(const RtlControlFlowGraph& x);

  // 26. Inverse of `jump.c:jump_optimize` (`cross_jump=0, noop_moves=0`):
  // appends a redundant jump-to-next-label and trailing return label block that
  // initial `jump_optimize` cleans up.
  static RtlControlFlowGraph InvertJump1Optimize(const RtlControlFlowGraph& x);
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_SUBFUNCTION_INVERSES_H_
