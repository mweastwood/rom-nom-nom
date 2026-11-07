#ifndef GCC_HARNESS_SUBFUNCTION_INVERSE_PROVIDER_H_
#define GCC_HARNESS_SUBFUNCTION_INVERSE_PROVIDER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::gcc_harness {

// Frame parameters recovered by `InvertFunctionPrologueEpilogue` for
// `mips.c:function_prologue` and `mips.c:function_epilogue`.
struct PrologueEpilogueFrameSpec {
  int var_size = 0;
  uint32_t saved_gpr_mask = 0;
};

// Pre-substitution arguments `(target_expr, from_reg, to_expr)` recovered by
// `InvertSubst` for `combine.c:subst`.
struct SubstInverseSpec {
  std::string target_expr;
  std::string from_reg;
  std::string to_expr;
};

// Abstract interface providing 1-to-1 mathematical right-inverses $g_i$ for
// each of the 26 individual KMC GCC 2.7.2 (`cc1`) and GNU Binutils 2.6 (`as`)
// C functions $f_i$ exposed in `FunctionHooks`.
//
// `RightInverseVerifier` depends only on `SubfunctionInverseProvider`, allowing
// both the miniature synthetic self-test fixture (`ToySubfunctionInverses`) and
// the production `lifter/` stage/subfunction inverses to be verified directly
// against the genuine in-process GCC 2.7.2 and Binutils 2.6 functions.
class SubfunctionInverseProvider {
 public:
  virtual ~SubfunctionInverseProvider() = default;

  // ---------------------------------------------------------------------------
  // Stage 0: Binutils 2.6 GAS (`tc-mips.c`) & GCC 2.7.2 Final (`final.c`, `mips.c`)
  // ---------------------------------------------------------------------------

  // 1. Inverse of `tc-mips.c:mips_ip`: decodes a 32-bit big-endian MIPS
  // instruction word `word_x` into its canonical GAS assembly line `y`.
  virtual std::string InvertGasMipsIp(uint32_t word_x) const = 0;

  // 2. Inverse of `tc-mips.c:macro`: collapses a multi-word GAS macro expansion
  // `macro_words_x` (e.g. `lui` + `ori` / `addiu`) back into its single macro
  // assembly line `y` (e.g. `li $2,0x80012345`).
  virtual std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const = 0;

  // 3. Inverse of `tc-mips.c:append_insn` (in `.set reorder` mode): recovers
  // the pre-`append_insn` sequence of assembly lines `y` from the emitted
  // machine words `words_x`.
  virtual std::vector<std::string> InvertGasAppendInsn(
      absl::Span<const uint32_t> words_x) const = 0;

  // 4. Inverse of `final.c:final_scan_insn`: lifts a single MIPS assembly
  // instruction emitted by `final_scan_insn` back into its RTL pattern `y`.
  virtual std::string InvertFinalScanInsn(std::string_view asm_line_x) const = 0;

  // 5. Inverse of `mips.c:function_prologue` + `mips.c:function_epilogue`:
  // recovers `(var_size, saved_gpr_mask)` from emitted prologue/epilogue
  // assembly text `asm_text_x`.
  virtual PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view asm_text_x) const = 0;

  // Whole-stage inverse for `final` + `md_assemble`: lifts a sequence of
  // 32-bit machine words `machine_words_x` into its `.dbr` `RtlControlFlowGraph`.
  virtual RtlControlFlowGraph InvertFinalAndGasFunction(
      absl::Span<const uint32_t> machine_words_x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 1: Delayed-Branch Scheduling (`reorg.c`)
  // ---------------------------------------------------------------------------

  // 6. Inverse of `reorg.c:make_return_insns` + `find_end_label`.
  virtual RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& x) const = 0;

  // 7. Inverse of `reorg.c:relax_delay_slots`.
  virtual RtlControlFlowGraph InvertRelaxDelaySlots(const RtlControlFlowGraph& x) const = 0;

  // 8. Inverse of `reorg.c:fill_eager_delay_slots` + `fill_slots_from_thread`.
  virtual RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& x) const = 0;

  // 9. Inverse of `reorg.c:fill_simple_delay_slots`.
  virtual RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& x) const = 0;

  // 10. Inverse of `reorg.c:dbr_schedule`.
  virtual RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 2: Post-Reload Cross-Jumping (`jump.c`)
  // ---------------------------------------------------------------------------

  // 11. Inverse of `jump.c:do_cross_jump` + `find_cross_jump`.
  virtual RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
  // ---------------------------------------------------------------------------

  // 12. Inverse of `sched.c:schedule_block` / `schedule_insns`.
  virtual RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 4 & Stage 5: Register Allocation & Reload (`reload1.c`, `local-alloc.c`, `global.c`)
  // ---------------------------------------------------------------------------

  // 13. Inverse of `reload1.c:alter_reg` (and `global.c:global_alloc` / `reload`).
  virtual RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& x,
                                             int pseudo_regno) const = 0;
  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& x) const {
    return InvertAlterReg(x, 72);
  }

  // 14. Inverse of `local-alloc.c:block_alloc`.
  virtual RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& x) const = 0;

  // 15. Inverse of `local-alloc.c:update_equiv_regs`.
  virtual RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 7: Instruction Combination (`combine.c`)
  // ---------------------------------------------------------------------------

  // 16. Inverse of `combine.c:simplify_rtx`.
  virtual std::string InvertSimplifyRtx(std::string_view simplified_rtx_x) const = 0;

  // 17. Inverse of `combine.c:subst`.
  virtual SubstInverseSpec InvertSubst(std::string_view subst_rtx_x) const = 0;

  // 18. Inverse of `combine.c:try_combine` / `combine_instructions`.
  virtual RtlControlFlowGraph InvertTryCombine(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 8: Flow & Life Analysis (`flow.c`)
  // ---------------------------------------------------------------------------

  // 19. Inverse of `flow.c:find_basic_blocks`.
  virtual RtlControlFlowGraph InvertFindBasicBlocks(const RtlControlFlowGraph& x) const = 0;

  // 20. Inverse of `flow.c:propagate_block` / `life_analysis` / `flow_analysis`.
  virtual RtlControlFlowGraph InvertPropagateBlock(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
  // ---------------------------------------------------------------------------

  // 21. Inverse of `cse.c:fold_rtx`.
  virtual std::string InvertFoldRtx(std::string_view folded_rtx_x) const = 0;

  // 22. Inverse of `cse.c:cse_basic_block` / `cse_main`.
  virtual RtlControlFlowGraph InvertCseBasicBlock(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 10: Loop Optimization (`loop.c`)
  // ---------------------------------------------------------------------------

  // 23. Inverse of `loop.c:scan_loop`.
  virtual RtlControlFlowGraph InvertScanLoop(const RtlControlFlowGraph& x) const = 0;

  // 24. Inverse of `loop.c:strength_reduce`.
  virtual RtlControlFlowGraph InvertStrengthReduce(const RtlControlFlowGraph& x) const = 0;

  // ---------------------------------------------------------------------------
  // Stage 12: Initial Jump Optimization (`jump.c`)
  // ---------------------------------------------------------------------------

  // 25. Inverse of `jump.c:redirect_jump` / `invert_jump`.
  virtual RtlControlFlowGraph InvertJumpRedirect(const RtlControlFlowGraph& x) const = 0;

  // 26. Inverse of `jump.c:jump_optimize` (`cross_jump=0, noop_moves=0`).
  virtual RtlControlFlowGraph InvertJump1Optimize(const RtlControlFlowGraph& x) const = 0;
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_SUBFUNCTION_INVERSE_PROVIDER_H_
