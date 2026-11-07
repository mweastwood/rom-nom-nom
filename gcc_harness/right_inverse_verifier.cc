#include "gcc_harness/right_inverse_verifier.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gcc_harness/toy_subfunction_inverses.h"

namespace rom_nom_nom::gcc_harness {
namespace {

const SubfunctionInverseProvider* DefaultInverseProvider() {
  static const auto* const default_toy_inverses = new ToySubfunctionInverses();
  return default_toy_inverses;
}

std::string FormatWordsSummary(absl::Span<const uint32_t> words) {
  std::vector<std::string> hex_words;
  hex_words.reserve(words.size());
  for (uint32_t w : words) {
    hex_words.push_back(absl::StrFormat("0x%08x", w));
  }
  return absl::StrJoin(hex_words, ", ");
}

std::string ExtractInstructionLines(std::string_view asm_text) {
  std::vector<std::string> lines;
  for (std::string_view raw : absl::StrSplit(asm_text, '\n')) {
    std::string_view line = absl::StripAsciiWhitespace(raw);
    if (line.empty() || line.front() == '#' || line.front() == '.') {
      continue;
    }
    lines.emplace_back(line);
  }
  return absl::StrJoin(lines, "\n");
}

}  // namespace

RightInverseVerifier::RightInverseVerifier(GccContext* ctx, FunctionHooks* hooks,
                                           const SubfunctionInverseProvider* inverses)
    : ctx_(ctx),
      hooks_(hooks),
      inverses_(inverses != nullptr ? inverses : DefaultInverseProvider()) {}

RightInverseResult RightInverseVerifier::VerifyRtlPassRightInverse(
    const RtlControlFlowGraph& target_x, InverseCfgFn inverse_g, ForwardHookFn forward_f,
    RtlPassKind target_pass) const {
  RightInverseResult result;
  result.expected_summary = target_x.FormatSummary();

  const RtlControlFlowGraph lifted_y = inverse_g(target_x);
  ctx_->MaterializeFromCfg(lifted_y);
  forward_f(ctx_, hooks_);

  const RtlControlFlowGraph actual_fx = ctx_->DematerializeToCfg(ctx_->GetInsns(), target_pass);

  result.actual_summary = actual_fx.FormatSummary();
  result.cfg_comparison = actual_fx.CompareAgainst(target_x);
  result.matches = result.cfg_comparison.MatchesExactly();
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  return result;
}

// -----------------------------------------------------------------------------
// Stage 0: Final Assembly (`final.c`, `mips.c`) & GAS (`tc-mips.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyGasMipsIpRightInverse(uint32_t word_x) const {
  RightInverseResult result;
  result.expected_words = {word_x};
  result.expected_summary = FormatWordsSummary(result.expected_words);

  const std::string asm_y = inverses_->InvertGasMipsIp(word_x);
  const uint32_t actual_word = hooks_->RunGasMipsIp(asm_y);
  result.actual_words = {actual_word};
  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (actual_word == word_x);
  if (!result.matches) {
    result.diff_detail =
        absl::StrFormat("mips_ip(%s) got 0x%08x != expected 0x%08x", asm_y, actual_word, word_x);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyGasMacroRightInverse(
    absl::Span<const uint32_t> macro_words_x) const {
  RightInverseResult result;
  result.expected_words.assign(macro_words_x.begin(), macro_words_x.end());
  result.expected_summary = FormatWordsSummary(macro_words_x);

  const std::string macro_asm_y = inverses_->InvertGasMacro(macro_words_x);
  result.actual_words = hooks_->RunGasMacro(macro_asm_y);
  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (result.actual_words == result.expected_words);
  if (!result.matches) {
    result.diff_detail =
        absl::StrFormat("GAS macro(%s) words got [%s] != expected [%s]", macro_asm_y,
                        result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyGasAppendInsnRightInverse(
    absl::Span<const uint32_t> words_x) const {
  RightInverseResult result;
  result.expected_words.assign(words_x.begin(), words_x.end());
  result.expected_summary = FormatWordsSummary(words_x);

  const std::vector<std::string> asm_lines_y = inverses_->InvertGasAppendInsn(words_x);
  result.actual_words = hooks_->RunGasAppendInsnReorder(asm_lines_y);
  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (result.actual_words == result.expected_words);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("append_insn words got [%s] != expected [%s]",
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyFinalScanInsnRightInverse(
    std::string_view asm_line_x) const {
  RightInverseResult result;
  result.expected_summary = std::string(absl::StripAsciiWhitespace(asm_line_x));

  const std::string rtl_pat_y = inverses_->InvertFinalScanInsn(asm_line_x);
  ctx_->BeginFunction("final_scan_verify_fn");
  Rtx insn = ctx_->EmitPattern(rtl_pat_y);
  const std::string emitted = hooks_->RunFinalScanInsn(insn);
  result.actual_summary = ExtractInstructionLines(emitted);
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("final_scan_insn(%s) got '%s' != expected '%s'", rtl_pat_y,
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyFunctionPrologueEpilogueRightInverse(
    std::string_view asm_text_x) const {
  RightInverseResult result;
  const PrologueEpilogueFrameSpec spec = inverses_->InvertFunctionPrologueEpilogue(asm_text_x);
  const std::string emitted =
      hooks_->RunFunctionPrologueEpilogue(spec.var_size, spec.saved_gpr_mask);

  if (absl::StrContains(asm_text_x, ".frame")) {
    result.expected_summary = std::string(asm_text_x);
    result.actual_summary = emitted;
  } else {
    result.expected_summary = ExtractInstructionLines(asm_text_x);
    result.actual_summary = ExtractInstructionLines(emitted);
  }
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat(
        "function_prologue/epilogue(var_size=%d, mask=0x%08x) got:\n%s\n!= "
        "expected:\n%s",
        spec.var_size, spec.saved_gpr_mask, result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyFinalAndGasStageRightInverse(
    absl::Span<const uint32_t> machine_words_x) const {
  RightInverseResult result;
  result.expected_words.assign(machine_words_x.begin(), machine_words_x.end());
  result.expected_summary = FormatWordsSummary(machine_words_x);

  const RtlControlFlowGraph lifted_dbr = inverses_->InvertFinalAndGasFunction(machine_words_x);
  ctx_->MaterializeFromCfg(lifted_dbr);
  const std::string combined_asm = hooks_->RunFinal();
  result.actual_words = hooks_->RunGasMdAssemble(combined_asm);
  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (result.actual_words == result.expected_words);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("Machine words got [%s] != expected [%s]",
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

// -----------------------------------------------------------------------------
// Stage 1: Delayed-Branch Scheduling (`reorg.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyMakeReturnInsnsRightInverse(
    const RtlControlFlowGraph& dbr_return_cfg_x) const {
  return VerifyRtlPassRightInverse(
      dbr_return_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertMakeReturnInsns(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunMakeReturnInsns(); },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyRelaxDelaySlotsRightInverse(
    const RtlControlFlowGraph& relaxed_cfg_x) const {
  return VerifyRtlPassRightInverse(
      relaxed_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertRelaxDelaySlots(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunRelaxDelaySlots(); },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyFillEagerDelaySlotsRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  return VerifyRtlPassRightInverse(
      scheduled_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertFillEagerDelaySlots(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunFillEagerDelaySlots(); },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyFillSimpleDelaySlotsRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  return VerifyRtlPassRightInverse(
      scheduled_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertFillSimpleDelaySlots(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunFillSimpleDelaySlots(/*non_jumps_p=*/0);
      },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyDbrStageRightInverse(
    const RtlControlFlowGraph& dbr_cfg_x) const {
  return VerifyRtlPassRightInverse(
      dbr_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertDbrSchedule(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunDbrSchedule(); },
      RtlPassKind::kDbr);
}

// -----------------------------------------------------------------------------
// Stage 2: Post-Reload Jump Optimization & Cross-Jumping (`jump.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyDoCrossJumpRightInverse(
    const RtlControlFlowGraph& jump2_cfg_x) const {
  return VerifyRtlPassRightInverse(
      jump2_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertDoCrossJump(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunJumpOptimize(/*cross_jump=*/1, /*noop_moves=*/0,
                               /*after_regscan=*/0);
      },
      RtlPassKind::kJump2);
}

RightInverseResult RightInverseVerifier::VerifyJump2CrossJumpStageRightInverse(
    const RtlControlFlowGraph& jump2_cfg_x) const {
  return VerifyDoCrossJumpRightInverse(jump2_cfg_x);
}

// -----------------------------------------------------------------------------
// Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyScheduleBlockRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  return VerifyRtlPassRightInverse(
      scheduled_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertScheduleBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunScheduleBlock(0); },
      scheduled_cfg_x.pass);
}

RightInverseResult RightInverseVerifier::VerifySched2StageRightInverse(
    const RtlControlFlowGraph& sched2_cfg_x) const {
  return VerifyRtlPassRightInverse(
      sched2_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertScheduleBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunScheduleInsns(); },
      RtlPassKind::kSched2);
}

RightInverseResult RightInverseVerifier::VerifySched1StageRightInverse(
    const RtlControlFlowGraph& sched1_cfg_x) const {
  return VerifyRtlPassRightInverse(
      sched1_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertScheduleBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunScheduleInsns(); },
      RtlPassKind::kSched);
}

RightInverseResult RightInverseVerifier::VerifySchedStageRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
    return VerifySched1StageRightInverse(scheduled_cfg_x);
  }
  return VerifySched2StageRightInverse(scheduled_cfg_x);
}

// -----------------------------------------------------------------------------
// Stage 4 & Stage 5: Local & Global Register Allocation (`local-alloc.c`,
// `global.c`, `reload1.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyAlterRegRightInverse(
    const RtlControlFlowGraph& allocated_cfg_x) const {
  return VerifyRtlPassRightInverse(
      allocated_cfg_x,
      [this](const RtlControlFlowGraph& x) {
        return inverses_->InvertAlterReg(x, /*pseudo_regno=*/72);
      },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunAlterReg(/*regno=*/72, /*from_reg=*/-1);
      },
      allocated_cfg_x.pass);
}

RightInverseResult RightInverseVerifier::VerifyGlobalReloadStageRightInverse(
    const RtlControlFlowGraph& greg_cfg_x) const {
  return VerifyRtlPassRightInverse(
      greg_cfg_x,
      [this](const RtlControlFlowGraph& x) {
        return inverses_->InvertAlterReg(x, /*pseudo_regno=*/72);
      },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunGlobalAlloc(); },
      RtlPassKind::kGreg);
}

RightInverseResult RightInverseVerifier::VerifyBlockAllocRightInverse(
    const RtlControlFlowGraph& lreg_cfg_x) const {
  return VerifyRtlPassRightInverse(
      lreg_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertBlockAlloc(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunBlockAlloc(0); },
      RtlPassKind::kLreg);
}

RightInverseResult RightInverseVerifier::VerifyUpdateEquivRegsRightInverse(
    const RtlControlFlowGraph& equiv_cfg_x) const {
  return VerifyRtlPassRightInverse(
      equiv_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertUpdateEquivRegs(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunUpdateEquivRegs(); },
      RtlPassKind::kLreg);
}

RightInverseResult RightInverseVerifier::VerifyLocalAllocStageRightInverse(
    const RtlControlFlowGraph& lreg_cfg_x) const {
  return VerifyRtlPassRightInverse(
      lreg_cfg_x,
      [this](const RtlControlFlowGraph& x) {
        return inverses_->InvertBlockAlloc(inverses_->InvertUpdateEquivRegs(x));
      },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunLocalAlloc(); },
      RtlPassKind::kLreg);
}

// -----------------------------------------------------------------------------
// Stage 7: Instruction Combination (`combine.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifySimplifyRtxRightInverse(
    std::string_view simplified_rtx_x) const {
  RightInverseResult result;
  result.expected_summary = std::string(absl::StripAsciiWhitespace(simplified_rtx_x));

  const std::string unsimplified_y = inverses_->InvertSimplifyRtx(simplified_rtx_x);
  ctx_->BeginFunction("simplify_rtx_verify_fn");
  Rtx rtx_y = ctx_->ParseRtx(unsimplified_y);
  Rtx simplified = hooks_->RunSimplifyRtx(rtx_y, rtx_y != nullptr ? rtx_y->mode : kSiMode);
  result.actual_summary = ctx_->FormatRtx(simplified);
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("simplify_rtx(%s) got %s != expected %s", unsimplified_y,
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifySubstRightInverse(
    std::string_view subst_rtx_x) const {
  RightInverseResult result;
  result.expected_summary = std::string(absl::StripAsciiWhitespace(subst_rtx_x));

  const SubstInverseSpec spec = inverses_->InvertSubst(subst_rtx_x);
  ctx_->BeginFunction("subst_verify_fn");
  Rtx target = ctx_->ParseRtx(spec.target_expr);
  Rtx from = ctx_->ParseRtx(spec.from_reg);
  Rtx to = ctx_->ParseRtx(spec.to_expr);
  Rtx actual = hooks_->RunSubst(target, from, to, /*in_dest=*/0,
                                /*unique_copy=*/0);
  result.actual_summary = ctx_->FormatRtx(actual);
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail =
        absl::StrFormat("subst(%s, %s, %s) got %s != expected %s", spec.target_expr, spec.from_reg,
                        spec.to_expr, result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyTryCombineRightInverse(
    const RtlControlFlowGraph& combine_cfg_x) const {
  return VerifyRtlPassRightInverse(
      combine_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertTryCombine(x); },
      [](GccContext* ctx, FunctionHooks* hooks) {
        hooks->RunFlowAnalysis();
        for (Rtx insn = ctx->GetInsns(); insn != nullptr; insn = insn->fld[2].rtx) {
          if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
              insn->fld[5].rtx != nullptr && insn->fld[5].rtx->fld[0].rtx != nullptr) {
            hooks->RunTryCombine(insn, insn->fld[5].rtx->fld[0].rtx, nullptr);
          }
        }
      },
      RtlPassKind::kCombine);
}

RightInverseResult RightInverseVerifier::VerifyCombineStageRightInverse(
    const RtlControlFlowGraph& combine_cfg_x) const {
  return VerifyRtlPassRightInverse(
      combine_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertTryCombine(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunCombineInstructions(); },
      RtlPassKind::kCombine);
}

// -----------------------------------------------------------------------------
// Stage 8: Dataflow & Life Analysis (`flow.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyFindBasicBlocksRightInverse(
    const RtlControlFlowGraph& flow_cfg_x) const {
  return VerifyRtlPassRightInverse(
      flow_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertFindBasicBlocks(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunFindBasicBlocks(); },
      RtlPassKind::kFlow);
}

RightInverseResult RightInverseVerifier::VerifyPropagateBlockRightInverse(
    const RtlControlFlowGraph& flow_cfg_x) const {
  return VerifyRtlPassRightInverse(
      flow_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertPropagateBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunLifeAnalysis(); },
      RtlPassKind::kFlow);
}

RightInverseResult RightInverseVerifier::VerifyFlowStageRightInverse(
    const RtlControlFlowGraph& flow_cfg_x) const {
  return VerifyRtlPassRightInverse(
      flow_cfg_x,
      [this](const RtlControlFlowGraph& x) {
        return inverses_->InvertPropagateBlock(inverses_->InvertFindBasicBlocks(x));
      },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunFlowAnalysis(); },
      RtlPassKind::kFlow);
}

// -----------------------------------------------------------------------------
// Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyFoldRtxRightInverse(
    std::string_view folded_rtx_x) const {
  RightInverseResult result;
  result.expected_summary = std::string(absl::StripAsciiWhitespace(folded_rtx_x));

  const std::string unfolded_y = inverses_->InvertFoldRtx(folded_rtx_x);
  ctx_->BeginFunction("fold_rtx_verify_fn");
  Rtx rtx_y = ctx_->ParseRtx(unfolded_y);
  Rtx folded = hooks_->RunFoldRtx(rtx_y, /*insn=*/nullptr);
  result.actual_summary = ctx_->FormatRtx(folded);
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("fold_rtx(%s) got %s != expected %s", unfolded_y,
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyCseBasicBlockRightInverse(
    const RtlControlFlowGraph& cse_cfg_x) const {
  return VerifyRtlPassRightInverse(
      cse_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertCseBasicBlock(x); },
      [](GccContext* ctx, FunctionHooks* hooks) {
        hooks->RunCseBasicBlock(ctx->GetInsns(), /*to=*/nullptr,
                                /*next_branch=*/nullptr, /*around_loop=*/0);
      },
      cse_cfg_x.pass);
}

RightInverseResult RightInverseVerifier::VerifyCse1StageRightInverse(
    const RtlControlFlowGraph& cse_cfg_x) const {
  return VerifyRtlPassRightInverse(
      cse_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertCseBasicBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunCseMain(/*first=*/nullptr, /*nregs=*/0, /*after_loop=*/0);
      },
      RtlPassKind::kCse);
}

RightInverseResult RightInverseVerifier::VerifyCse2StageRightInverse(
    const RtlControlFlowGraph& cse2_cfg_x) const {
  return VerifyRtlPassRightInverse(
      cse2_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertCseBasicBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunCseMain(/*first=*/nullptr, /*nregs=*/0, /*after_loop=*/1);
      },
      RtlPassKind::kCse2);
}

// -----------------------------------------------------------------------------
// Stage 10: Loop Optimization (`loop.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyScanLoopRightInverse(
    const RtlControlFlowGraph& loop_cfg_x) const {
  return VerifyRtlPassRightInverse(
      loop_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertScanLoop(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunScanLoop(/*loop_start=*/nullptr, /*end=*/nullptr,
                           /*nregs=*/0);
      },
      RtlPassKind::kLoop);
}

RightInverseResult RightInverseVerifier::VerifyLoopStageRightInverse(
    const RtlControlFlowGraph& loop_cfg_x) const {
  return VerifyRtlPassRightInverse(
      loop_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertScanLoop(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunLoopOptimize(/*first=*/nullptr, /*unroll_p=*/0);
      },
      RtlPassKind::kLoop);
}

RightInverseResult RightInverseVerifier::VerifyStrengthReduceRightInverse(
    const RtlControlFlowGraph& loop_sr_cfg_x) const {
  return VerifyRtlPassRightInverse(
      loop_sr_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertStrengthReduce(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunStrengthReduce(); },
      RtlPassKind::kLoop);
}

// -----------------------------------------------------------------------------
// Stage 12: Initial Jump Optimization (`jump.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyJumpRedirectRightInverse(
    const RtlControlFlowGraph& jump_cfg_x) const {
  return VerifyRtlPassRightInverse(
      jump_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertJumpRedirect(x); },
      [](GccContext* ctx, FunctionHooks* hooks) {
        Rtx first_jump = nullptr;
        Rtx last_label = nullptr;
        for (Rtx insn = ctx->GetInsns(); insn != nullptr; insn = insn->fld[2].rtx) {
          if (insn->code == kJumpInsn && first_jump == nullptr) {
            first_jump = insn;
          } else if (insn->code == kCodeLabel) {
            last_label = insn;
          }
        }
        if (first_jump != nullptr && last_label != nullptr) {
          hooks->RunRedirectJump(first_jump, last_label);
        }
        hooks->RunJumpOptimize(/*cross_jump=*/0, /*noop_moves=*/0,
                               /*after_regscan=*/1);
      },
      RtlPassKind::kJump);
}

RightInverseResult RightInverseVerifier::VerifyJump1StageRightInverse(
    const RtlControlFlowGraph& jump_cfg_x) const {
  return VerifyRtlPassRightInverse(
      jump_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertJump1Optimize(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunJumpOptimize(/*cross_jump=*/0, /*noop_moves=*/0,
                               /*after_regscan=*/1);
      },
      RtlPassKind::kJump);
}

}  // namespace rom_nom_nom::gcc_harness
