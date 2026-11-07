#ifndef GCC_HARNESS_RIGHT_INVERSE_VERIFIER_H_
#define GCC_HARNESS_RIGHT_INVERSE_VERIFIER_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"

namespace rom_nom_nom::gcc_harness {

// Captures the result of verifying the mathematical right-inverse property
// f(g(x)) = x for an inverse function g and in-process GCC/GAS hook f.
struct RightInverseResult {
  bool matches = false;
  std::string expected_summary;
  std::string actual_summary;
  std::string diff_detail;
  RtlCfgPassComparison cfg_comparison;
  std::vector<uint32_t> expected_words;
  std::vector<uint32_t> actual_words;
};

// Verifies the right-inverse property f_i(g_i(x)) = x across all 26 individual
// KMC GCC 2.7.2 (`cc1`) and Binutils 2.6 (`as`) C functions exposed in
// `FunctionHooks`, using the 1-to-1 reference inverses in
// `SubfunctionInverses` with zero post-pass normalization or secret instruction
// injection.
class RightInverseVerifier {
 public:
  using InverseCfgFn = std::function<RtlControlFlowGraph(const RtlControlFlowGraph&)>;
  using ForwardHookFn = std::function<void(GccContext*, FunctionHooks*)>;

  RightInverseVerifier(GccContext* ctx, FunctionHooks* hooks);

  // Generic RTL CFG right-inverse verifier:
  //   1. Computes y = inverse_g(target_x)
  //   2. Materializes y into `GccContext`
  //   3. Executes `forward_f(ctx_, hooks_)` in-process
  //   4. Dematerializes f(y) into an `RtlControlFlowGraph` and compares it
  //      directly against `target_x` (with zero post-pass normalization).
  RightInverseResult VerifyRtlPassRightInverse(const RtlControlFlowGraph& target_x,
                                               InverseCfgFn inverse_g, ForwardHookFn forward_f,
                                               RtlPassKind target_pass = RtlPassKind::kDbr) const;

  // ---------------------------------------------------------------------------
  // Stage 0: Final Assembly (`final.c`, `mips.c`) & GAS (`tc-mips.c`)
  // ---------------------------------------------------------------------------

  // 1. `tc-mips.c:mips_ip` right-inverse: f(g(word_x)) == word_x.
  RightInverseResult VerifyGasMipsIpRightInverse(uint32_t word_x) const;

  // 2. `tc-mips.c:macro` right-inverse: f(g(macro_words_x)) == macro_words_x.
  RightInverseResult VerifyGasMacroRightInverse(absl::Span<const uint32_t> macro_words_x) const;

  // 3. `tc-mips.c:append_insn` (`.set reorder`) right-inverse:
  // f(g(words_x)) == words_x.
  RightInverseResult VerifyGasAppendInsnRightInverse(absl::Span<const uint32_t> words_x) const;

  // 4. `final.c:final_scan_insn` right-inverse: f(g(asm_line_x)) == asm_line_x.
  RightInverseResult VerifyFinalScanInsnRightInverse(std::string_view asm_line_x) const;

  // 5. `mips.c:function_prologue` + `mips.c:function_epilogue` right-inverse:
  // f(g(asm_text_x)) == asm_text_x.
  RightInverseResult VerifyFunctionPrologueEpilogueRightInverse(std::string_view asm_text_x) const;

  // Whole-stage `final` + `md_assemble` right-inverse on `machine_words_x`.
  RightInverseResult VerifyFinalAndGasStageRightInverse(
      absl::Span<const uint32_t> machine_words_x) const;

  // ---------------------------------------------------------------------------
  // Stage 1: Delayed-Branch Scheduling (`reorg.c`)
  // ---------------------------------------------------------------------------

  // 6. `reorg.c:make_return_insns` + `find_end_label` right-inverse.
  RightInverseResult VerifyMakeReturnInsnsRightInverse(
      const RtlControlFlowGraph& dbr_return_cfg_x) const;

  // 7. `reorg.c:relax_delay_slots` right-inverse.
  RightInverseResult VerifyRelaxDelaySlotsRightInverse(
      const RtlControlFlowGraph& relaxed_cfg_x) const;

  // 8. `reorg.c:fill_eager_delay_slots` + `fill_slots_from_thread` right-inverse.
  RightInverseResult VerifyFillEagerDelaySlotsRightInverse(
      const RtlControlFlowGraph& scheduled_cfg_x) const;

  // 9. `reorg.c:fill_simple_delay_slots` right-inverse.
  RightInverseResult VerifyFillSimpleDelaySlotsRightInverse(
      const RtlControlFlowGraph& scheduled_cfg_x) const;

  // 10. `reorg.c:dbr_schedule` right-inverse.
  RightInverseResult VerifyDbrStageRightInverse(const RtlControlFlowGraph& dbr_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 2: Post-Reload Jump Optimization & Cross-Jumping (`jump.c`)
  // ---------------------------------------------------------------------------

  // 11. `jump.c:do_cross_jump` + `find_cross_jump` right-inverse.
  RightInverseResult VerifyDoCrossJumpRightInverse(const RtlControlFlowGraph& jump2_cfg_x) const;
  RightInverseResult VerifyJump2CrossJumpStageRightInverse(
      const RtlControlFlowGraph& jump2_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
  // ---------------------------------------------------------------------------

  // 12. `sched.c:schedule_block` / `schedule_insns` right-inverse.
  RightInverseResult VerifyScheduleBlockRightInverse(
      const RtlControlFlowGraph& scheduled_cfg_x) const;
  RightInverseResult VerifySched2StageRightInverse(const RtlControlFlowGraph& sched2_cfg_x) const;
  RightInverseResult VerifySched1StageRightInverse(const RtlControlFlowGraph& sched1_cfg_x) const;
  RightInverseResult VerifySchedStageRightInverse(const RtlControlFlowGraph& scheduled_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 4 & Stage 5: Local & Global Register Allocation (`local-alloc.c`,
  // `global.c`, `reload1.c`)
  // ---------------------------------------------------------------------------

  // 13. `reload1.c:alter_reg` / `global.c:global_alloc` right-inverse.
  RightInverseResult VerifyAlterRegRightInverse(const RtlControlFlowGraph& allocated_cfg_x) const;
  RightInverseResult VerifyGlobalReloadStageRightInverse(
      const RtlControlFlowGraph& greg_cfg_x) const;

  // 14. `local-alloc.c:block_alloc` right-inverse.
  RightInverseResult VerifyBlockAllocRightInverse(const RtlControlFlowGraph& lreg_cfg_x) const;

  // 15. `local-alloc.c:update_equiv_regs` right-inverse.
  RightInverseResult VerifyUpdateEquivRegsRightInverse(
      const RtlControlFlowGraph& equiv_cfg_x) const;
  RightInverseResult VerifyLocalAllocStageRightInverse(const RtlControlFlowGraph& lreg_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 7: Instruction Combination (`combine.c`)
  // ---------------------------------------------------------------------------

  // 16. `combine.c:simplify_rtx` right-inverse.
  RightInverseResult VerifySimplifyRtxRightInverse(std::string_view simplified_rtx_x) const;

  // 17. `combine.c:subst` right-inverse.
  RightInverseResult VerifySubstRightInverse(std::string_view subst_rtx_x) const;

  // 18. `combine.c:try_combine` / `combine_instructions` right-inverse.
  RightInverseResult VerifyTryCombineRightInverse(const RtlControlFlowGraph& combine_cfg_x) const;
  RightInverseResult VerifyCombineStageRightInverse(const RtlControlFlowGraph& combine_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 8: Dataflow & Life Analysis (`flow.c`)
  // ---------------------------------------------------------------------------

  // 19. `flow.c:find_basic_blocks` right-inverse.
  RightInverseResult VerifyFindBasicBlocksRightInverse(const RtlControlFlowGraph& flow_cfg_x) const;

  // 20. `flow.c:propagate_block` / `life_analysis` / `flow_analysis`
  // right-inverse.
  RightInverseResult VerifyPropagateBlockRightInverse(const RtlControlFlowGraph& flow_cfg_x) const;
  RightInverseResult VerifyFlowStageRightInverse(const RtlControlFlowGraph& flow_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
  // ---------------------------------------------------------------------------

  // 21. `cse.c:fold_rtx` right-inverse.
  RightInverseResult VerifyFoldRtxRightInverse(std::string_view folded_rtx_x) const;

  // 22. `cse.c:cse_basic_block` / `cse_main` right-inverse.
  RightInverseResult VerifyCseBasicBlockRightInverse(const RtlControlFlowGraph& cse_cfg_x) const;
  RightInverseResult VerifyCse1StageRightInverse(const RtlControlFlowGraph& cse_cfg_x) const;
  RightInverseResult VerifyCse2StageRightInverse(const RtlControlFlowGraph& cse2_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 10: Loop Optimization (`loop.c`)
  // ---------------------------------------------------------------------------

  // 23. `loop.c:scan_loop` right-inverse.
  RightInverseResult VerifyScanLoopRightInverse(const RtlControlFlowGraph& loop_cfg_x) const;
  RightInverseResult VerifyLoopStageRightInverse(const RtlControlFlowGraph& loop_cfg_x) const;

  // 24. `loop.c:strength_reduce` right-inverse.
  RightInverseResult VerifyStrengthReduceRightInverse(
      const RtlControlFlowGraph& loop_sr_cfg_x) const;

  // ---------------------------------------------------------------------------
  // Stage 12: Initial Jump Optimization (`jump.c`)
  // ---------------------------------------------------------------------------

  // 25. `jump.c:redirect_jump` / `invert_jump` right-inverse.
  RightInverseResult VerifyJumpRedirectRightInverse(const RtlControlFlowGraph& jump_cfg_x) const;

  // 26. `jump.c:jump_optimize` (`cross_jump=0, noop_moves=0`) right-inverse.
  RightInverseResult VerifyJump1StageRightInverse(const RtlControlFlowGraph& jump_cfg_x) const;

 private:
  GccContext* ctx_;
  FunctionHooks* hooks_;
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_RIGHT_INVERSE_VERIFIER_H_
