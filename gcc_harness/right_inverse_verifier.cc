#include "gcc_harness/right_inverse_verifier.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_sexpr_utils.h"
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
    const size_t hash_pos = raw.find('#');
    if (hash_pos != std::string_view::npos) {
      raw = raw.substr(0, hash_pos);
    }
    std::string_view line = absl::StripAsciiWhitespace(raw);
    if (line.empty() || line.front() == '.') {
      continue;
    }
    if (line.back() == ':' && line.find_first_of(" \t,()") == std::string_view::npos) {
      continue;
    }
    lines.emplace_back(line);
  }
  return absl::StrJoin(lines, "\n");
}

std::string StripFileDirectives(std::string_view asm_text) {
  std::vector<std::string> lines;
  for (std::string_view raw : absl::StrSplit(asm_text, '\n')) {
    std::string_view trimmed = absl::StripAsciiWhitespace(raw);
    if (absl::StartsWith(trimmed, ".file")) {
      continue;
    }
    lines.emplace_back(raw);
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
  if (!target_x.rpo_blocks.empty() && lifted_y.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
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
  if (asm_y.empty()) {
    result.matches = false;
    result.diff_detail = "InvertGasMipsIp returned empty string";
    return result;
  }
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
  if (macro_words_x.empty() || absl::StripAsciiWhitespace(macro_asm_y).empty()) {
    result.matches = false;
    result.diff_detail = "InvertGasMacro returned empty assembly string";
    return result;
  }
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
  if (!words_x.empty() && asm_lines_y.empty()) {
    result.matches = false;
    result.diff_detail = "InvertGasAppendInsn returned empty assembly lines";
    return result;
  }
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
  if (spec.var_size < 0) {
    result.expected_summary = std::string(asm_text_x);
    result.matches = false;
    result.diff_detail = "InvertFunctionPrologueEpilogue returned invalid var_size < 0";
    return result;
  }
  std::string fn_name = "harness_func";
  for (std::string_view raw : absl::StrSplit(asm_text_x, '\n')) {
    std::string_view line = absl::StripAsciiWhitespace(raw);
    if (absl::StartsWith(line, ".ent") && line.size() > 4 &&
        absl::ascii_isspace(static_cast<unsigned char>(line[4]))) {
      std::string_view sym = absl::StripAsciiWhitespace(line.substr(4));
      if (!sym.empty()) {
        fn_name = std::string(sym);
        break;
      }
    }
  }
  ctx_->BeginFunction(fn_name);
  const std::string emitted =
      hooks_->RunFunctionPrologueEpilogue(spec.var_size, spec.saved_gpr_mask);

  if (absl::StrContains(asm_text_x, ".frame")) {
    result.expected_summary = StripFileDirectives(asm_text_x);
    result.actual_summary = StripFileDirectives(emitted);
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

RightInverseResult RightInverseVerifier::VerifyReloadRightInverse(
    const RtlControlFlowGraph& reload_cfg_x) const {
  RightInverseResult result;
  result.expected_summary = reload_cfg_x.FormatSummary();

  const RtlControlFlowGraph lifted_y = inverses_->InvertReload(reload_cfg_x);
  if (!reload_cfg_x.rpo_blocks.empty() && lifted_y.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  const SubfunctionBoundarySnapshot replayed =
      hooks_->ExecuteSubfunctionBoundary(SubfunctionId::kReload, lifted_y);
  result.actual_summary = replayed.post_state.FormatSummary();
  result.cfg_comparison = replayed.post_state.CompareAgainst(reload_cfg_x);
  result.matches = result.cfg_comparison.MatchesExactly();
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  return result;
}

RightInverseResult RightInverseVerifier::VerifyGlobalAllocRightInverse(
    const RtlControlFlowGraph& greg_cfg_x) const {
  RightInverseResult result;
  result.expected_summary = greg_cfg_x.FormatSummary();

  const RtlControlFlowGraph lifted_y = inverses_->InvertGlobalAlloc(greg_cfg_x);
  if (!greg_cfg_x.rpo_blocks.empty() && lifted_y.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  const SubfunctionBoundarySnapshot replayed =
      hooks_->ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, lifted_y);
  result.actual_summary = replayed.post_state.FormatSummary();
  result.cfg_comparison = replayed.post_state.CompareAgainst(greg_cfg_x);
  const bool reg_renumber_matches =
      (replayed.post_state.sideband.reg_renumber == greg_cfg_x.sideband.reg_renumber);
  result.matches = result.cfg_comparison.MatchesExactly() && reg_renumber_matches;
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  if (result.cfg_comparison.MatchesExactly() && !reg_renumber_matches) {
    result.diff_detail = "sideband.reg_renumber mismatch after kGlobalAlloc";
  }
  return result;
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

// -----------------------------------------------------------------------------
// Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`)
// -----------------------------------------------------------------------------

namespace {

RightInverseResult BuildCfgRightInverseResult(const RtlControlFlowGraph& expected_x,
                                              const RtlControlFlowGraph& actual_fx) {
  RightInverseResult result;
  result.expected_summary = expected_x.FormatSummary();
  result.actual_summary = actual_fx.FormatSummary();
  result.cfg_comparison = actual_fx.CompareAgainst(expected_x);
  result.matches = result.cfg_comparison.MatchesExactly();
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  return result;
}

RightInverseResult BuildFailedStage13Result(const RtlControlFlowGraph& expected_x,
                                            const absl::Status& status) {
  RightInverseResult result;
  result.expected_summary = expected_x.FormatSummary();
  result.matches = false;
  result.diff_detail = std::string(status.message());
  return result;
}

}  // namespace

RightInverseResult RightInverseVerifier::VerifyInstantiateVirtualRegsRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  const RtlControlFlowGraph pre_y = inverses_->InvertInstantiateVirtualRegs(post_cfg_x);
  if (!post_cfg_x.rpo_blocks.empty() && pre_y.rpo_blocks.empty()) {
    RightInverseResult result;
    result.expected_summary = post_cfg_x.FormatSummary();
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunInstantiateVirtualRegs(*ctx_, pre_y);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandFunctionEndRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandFunctionEnd(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandFunctionEnd(*ctx_, lifted_y->body_cfg, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandReturnRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandReturn(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandReturn(*ctx_, lifted_y->ret_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandExprRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandExpr(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunExpandExpr(
      *ctx_, *lifted_y->expr, lifted_y->func_decl, lifted_y->target_pseudo_regno);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandAssignmentRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandAssignment(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandAssignment(*ctx_, *lifted_y->lhs, *lifted_y->rhs, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyDoStoreFlagRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertDoStoreFlag(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunDoStoreFlag(*ctx_, *lifted_y->comparison_expr, lifted_y->func_decl,
                             lifted_y->target_pseudo_regno, lifted_y->only_cheap);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyDoJumpRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertDoJump(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunDoJump(*ctx_, *lifted_y->cond_expr, lifted_y->func_decl, lifted_y->jump_if_true,
                        lifted_y->jump_if_false);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandCallRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandCall(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunExpandCall(
      *ctx_, lifted_y->call_expr, lifted_y->caller_decl, lifted_y->return_type,
      lifted_y->target_pseudo_regno, lifted_y->ignore_result);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandCondRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandCond(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandCond(*ctx_, lifted_y->if_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandLoopRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandLoop(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandLoop(*ctx_, *lifted_y->loop_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandCaseRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandCase(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandCase(*ctx_, lifted_y->switch_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandDeclRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandDecl(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandDecl(*ctx_, lifted_y->var_decl, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandFunctionStartRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandFunctionStart(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunExpandFunctionStart(*ctx_, *lifted_y);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifySubfunctionBoundaryRightInverse(
    const SubfunctionBoundarySnapshot& snap) const {
  switch (snap.subfunction_id) {
    case SubfunctionId::kFunctionPrologueEpilogue: {
      RightInverseResult result;
      result.expected_summary = absl::StrJoin(snap.post_asm_lines, "\n");
      const PrologueEpilogueFrameSpec spec =
          inverses_->InvertFunctionPrologueEpilogue(result.expected_summary);
      if (spec.var_size < 0) {
        result.matches = false;
        result.diff_detail = "InvertFunctionPrologueEpilogue returned invalid var_size < 0";
        return result;
      }
      RtlControlFlowGraph cand_pre = snap.post_state;
      cand_pre.sideband.frame_size_bytes = spec.var_size;
      cand_pre.sideband.saved_gpr_regnos.clear();
      for (int r = 0; r < 32; ++r) {
        if ((spec.saved_gpr_mask >> r) & 1u) {
          cand_pre.sideband.saved_gpr_regnos.push_back(r);
        }
      }
      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, cand_pre);
      result.actual_summary = absl::StrJoin(replayed.post_asm_lines, "\n");
      result.matches = (replayed.post_asm_lines == snap.post_asm_lines);
      if (!result.matches) {
        result.diff_detail =
            absl::StrFormat("kFunctionPrologueEpilogue asm lines got:\n%s\n!= expected:\n%s",
                            result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kFinalScanInsn: {
      RightInverseResult result;
      result.expected_summary = absl::StrJoin(snap.post_asm_lines, "\n");
      RtlControlFlowGraph cand_pre;
      cand_pre.pass = snap.post_state.pass;
      cand_pre.sideband = snap.post_state.sideband;

      std::vector<RtlBasicBlock> blocks;
      absl::flat_hash_map<std::string, size_t> label_to_block;
      absl::flat_hash_set<size_t> labeled_block_ids;
      RtlBasicBlock cur_blk;
      cur_blk.id = 0;

      for (size_t i = 0; i < snap.post_asm_lines.size(); ++i) {
        const std::string& line = snap.post_asm_lines[i];
        if (line == ".set reorder") {
          continue;
        }
        if (line == ".set noreorder") {
          if (i + 3 >= snap.post_asm_lines.size() || snap.post_asm_lines[i + 3] != ".set reorder") {
            result.matches = false;
            result.diff_detail = "Malformed .set noreorder block in kFinalScanInsn";
            return result;
          }
          const std::string pat0 = inverses_->InvertFinalScanInsn(snap.post_asm_lines[i + 1]);
          const std::string pat1 = inverses_->InvertFinalScanInsn(snap.post_asm_lines[i + 2]);
          if (pat0.empty() || pat1.empty()) {
            result.matches = false;
            result.diff_detail = "InvertFinalScanInsn returned empty pattern in .set noreorder";
            return result;
          }
          cur_blk.AddPatternOnly(
              absl::StrFormat("(sequence [(jump_insn %s) (insn %s)])", pat0, pat1));
          i += 3;
          continue;
        }
        if (absl::EndsWith(line, ":")) {
          std::string_view lbl(line.data(), line.size() - 1);
          if (absl::StartsWith(lbl, "$L")) {
            lbl.remove_prefix(1);
          }
          if (!cur_blk.instructions.empty() || labeled_block_ids.contains(cur_blk.id)) {
            blocks.push_back(std::move(cur_blk));
            cur_blk = RtlBasicBlock{};
            cur_blk.id = blocks.size();
          }
          label_to_block[std::string(lbl)] = cur_blk.id;
          labeled_block_ids.insert(cur_blk.id);
          continue;
        }
        const std::string pat = inverses_->InvertFinalScanInsn(line);
        if (pat.empty()) {
          result.matches = false;
          result.diff_detail =
              absl::StrFormat("InvertFinalScanInsn returned empty pattern for '%s'", line);
          return result;
        }
        cur_blk.AddPatternOnly(pat);
        if ((pat == "(return)" || absl::StartsWith(pat, "(set (pc) ")) &&
            i + 1 < snap.post_asm_lines.size() &&
            !absl::EndsWith(snap.post_asm_lines[i + 1], ":")) {
          blocks.push_back(std::move(cur_blk));
          cur_blk = RtlBasicBlock{};
          cur_blk.id = blocks.size();
        }
      }
      if (!cur_blk.instructions.empty() || labeled_block_ids.contains(cur_blk.id) ||
          blocks.empty()) {
        blocks.push_back(std::move(cur_blk));
      }
      if (blocks.size() >= 2 && labeled_block_ids.contains(blocks.back().id) &&
          blocks.back().instructions.size() == 4 &&
          blocks.back().instructions[0] == "(set (reg:SI 29 sp) (reg:SI 30 fp))" &&
          blocks.back().instructions.back() == "(return)") {
        blocks.back().instructions.clear();
        blocks.back().SyncRtlInsnsFromInstructions();
      }

      for (size_t b = 0; b < blocks.size(); ++b) {
        if (blocks[b].instructions.empty()) {
          if (b + 1 < blocks.size()) {
            blocks[b].successors = {b + 1};
          }
          continue;
        }
        const std::string& last_pat = blocks[b].instructions.back();
        if (last_pat == "(return)" ||
            absl::StartsWith(last_pat, "(sequence [(jump_insn (return))")) {
          blocks[b].successors.clear();
          continue;
        }
        const size_t lref_pos = last_pat.find("(label_ref ");
        if (lref_pos != std::string::npos) {
          const size_t tok_start = lref_pos + std::string_view("(label_ref ").size();
          const size_t tok_end = last_pat.find(')', tok_start);
          if (tok_end != std::string::npos) {
            const std::string lbl_tok(
                absl::StripAsciiWhitespace(last_pat.substr(tok_start, tok_end - tok_start)));
            auto it = label_to_block.find(lbl_tok);
            if (it != label_to_block.end()) {
              if (absl::StrContains(last_pat, "(if_then_else ")) {
                if (b + 1 < blocks.size()) {
                  blocks[b].successors = {b + 1, it->second};
                } else {
                  blocks[b].successors = {it->second};
                }
              } else {
                blocks[b].successors = {it->second};
              }
              continue;
            }
          }
        }
        if (b + 1 < blocks.size()) {
          blocks[b].successors = {b + 1};
        }
      }
      cand_pre.rpo_blocks = std::move(blocks);

      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, cand_pre);
      result.actual_summary = absl::StrJoin(replayed.post_asm_lines, "\n");
      result.matches = (replayed.post_asm_lines == snap.post_asm_lines);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kFinalScanInsn asm lines got:\n%s\n!= expected:\n%s",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kGasMipsIp: {
      RightInverseResult result;
      result.expected_words = snap.post_machine_words;
      result.expected_summary = FormatWordsSummary(snap.post_machine_words);
      std::vector<std::string> cand_asm_lines;
      cand_asm_lines.reserve(snap.post_machine_words.size());
      for (uint32_t w : snap.post_machine_words) {
        std::string asm_y = inverses_->InvertGasMipsIp(w);
        if (asm_y.empty()) {
          result.matches = false;
          result.diff_detail = "InvertGasMipsIp returned empty string";
          return result;
        }
        cand_asm_lines.push_back(std::move(asm_y));
      }
      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, snap.post_state, cand_asm_lines);
      result.actual_words = replayed.post_machine_words;
      result.actual_summary = FormatWordsSummary(replayed.post_machine_words);
      result.matches = (replayed.post_machine_words == snap.post_machine_words);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kGasMipsIp words got [%s] != expected [%s]",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kGasMacro: {
      RightInverseResult result;
      result.expected_words = snap.post_machine_words;
      result.expected_summary = FormatWordsSummary(snap.post_machine_words);
      std::vector<std::string> cand_asm_lines;
      const auto words_span = absl::MakeConstSpan(snap.post_machine_words);
      size_t i = 0;
      while (i < words_span.size()) {
        bool matched_multi = false;
        const size_t max_len = std::min<size_t>(12, words_span.size() - i);
        for (size_t len = max_len; len >= 2; --len) {
          const auto slice = words_span.subspan(i, len);
          const std::string macro_asm = inverses_->InvertGasMacro(slice);
          if (!macro_asm.empty() && absl::MakeConstSpan(hooks_->RunGasMacro(macro_asm)) == slice) {
            cand_asm_lines.push_back(macro_asm);
            i += len;
            matched_multi = true;
            break;
          }
        }
        if (matched_multi) {
          continue;
        }
        const std::string single_asm = inverses_->InvertGasMacro(words_span.subspan(i, 1));
        if (single_asm.empty()) {
          result.matches = false;
          result.diff_detail = absl::StrFormat(
              "InvertGasMacro returned empty string at word index %zu (0x%08x)", i, words_span[i]);
          return result;
        }
        cand_asm_lines.push_back(single_asm);
        ++i;
      }
      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, snap.post_state, cand_asm_lines);
      result.actual_words = replayed.post_machine_words;
      result.actual_summary = FormatWordsSummary(replayed.post_machine_words);
      result.matches = (replayed.post_machine_words == snap.post_machine_words);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kGasMacro words got [%s] != expected [%s]",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kGasAppendInsn: {
      RightInverseResult result;
      result.expected_words = snap.post_machine_words;
      result.expected_summary = FormatWordsSummary(snap.post_machine_words);

      const std::vector<std::string> cand_asm_lines =
          inverses_->InvertGasAppendInsn(snap.post_machine_words);
      if (!snap.post_machine_words.empty() && cand_asm_lines.empty()) {
        result.matches = false;
        result.diff_detail = "InvertGasAppendInsn returned empty assembly lines";
        return result;
      }

      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, snap.post_state, cand_asm_lines);
      result.actual_words = replayed.post_machine_words;
      result.actual_summary = FormatWordsSummary(replayed.post_machine_words);
      result.matches = (replayed.post_machine_words == snap.post_machine_words);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kGasAppendInsn words got [%s] != expected [%s]",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    default:
      break;
  }

  RtlControlFlowGraph cand_pre = snap.post_state;
  switch (snap.subfunction_id) {
    case SubfunctionId::kInstantiateVirtualRegs:
      cand_pre = inverses_->InvertInstantiateVirtualRegs(snap.post_state);
      break;
    case SubfunctionId::kJumpRedirect:
      cand_pre = inverses_->InvertJumpRedirect(snap.post_state);
      break;
    case SubfunctionId::kJump1Optimize:
      cand_pre = inverses_->InvertJump1Optimize(snap.post_state);
      break;
    case SubfunctionId::kFoldRtx: {
      bool called_fold = false;
      for (auto& blk : cand_pre.rpo_blocks) {
        for (std::string& pat : blk.instructions) {
          const std::string clean = StripTrailingRegNotesFromPattern(pat);
          if (!absl::StartsWith(clean, "(set ")) {
            continue;
          }
          called_fold = true;
          const std::string notes = pat.size() > clean.size() ? pat.substr(clean.size()) : "";
          const std::string unfolded = inverses_->InvertFoldRtx(clean);
          if (!absl::StartsWith(unfolded, "(set ")) {
            RightInverseResult result;
            result.expected_summary = snap.post_state.FormatSummary();
            result.matches = false;
            result.diff_detail = absl::StrFormat(
                "InvertFoldRtx returned invalid pattern '%s' for '%s'", unfolded, clean);
            return result;
          }
          if (snap.optimize_level > 0) {
            pat = absl::StrCat(unfolded, notes);
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      if (!called_fold) {
        RightInverseResult result;
        result.expected_summary = snap.post_state.FormatSummary();
        result.matches = false;
        result.diff_detail = "No (set ...) instruction found for kFoldRtx";
        return result;
      }
      break;
    }
    case SubfunctionId::kCseBasicBlockCse1:
    case SubfunctionId::kCseBasicBlockCse2:
      cand_pre = inverses_->InvertCseBasicBlock(snap.post_state);
      break;
    case SubfunctionId::kScanLoop:
      cand_pre = inverses_->InvertScanLoop(snap.post_state);
      break;
    case SubfunctionId::kStrengthReduce:
      cand_pre = inverses_->InvertStrengthReduce(snap.post_state);
      break;
    case SubfunctionId::kFindBasicBlocks:
      cand_pre = inverses_->InvertFindBasicBlocks(snap.post_state);
      break;
    case SubfunctionId::kPropagateBlock:
      cand_pre = inverses_->InvertPropagateBlock(snap.post_state);
      break;
    case SubfunctionId::kSimplifyRtx: {
      bool called_simplify = false;
      for (auto& blk : cand_pre.rpo_blocks) {
        for (std::string& pat : blk.instructions) {
          const std::string clean = StripTrailingRegNotesFromPattern(pat);
          const std::string notes = pat.size() > clean.size() ? pat.substr(clean.size()) : "";
          if (absl::StartsWith(clean, "(set ") && clean.back() == ')') {
            size_t pos = std::string_view("(set ").size();
            int depth = 0;
            for (; pos < clean.size(); ++pos) {
              if (clean[pos] == '(') ++depth;
              if (clean[pos] == ')' && --depth == 0) {
                ++pos;
                break;
              }
            }
            const std::string_view clean_view(clean);
            std::string_view dst = absl::StripAsciiWhitespace(clean_view.substr(5, pos - 5));
            std::string_view rhs =
                absl::StripAsciiWhitespace(clean_view.substr(pos, clean.size() - pos - 1));
            const bool is_compound_rhs =
                dst != "(pc)" && absl::StartsWith(rhs, "(") && !absl::StartsWith(rhs, "(reg") &&
                !absl::StartsWith(rhs, "(subreg") && !absl::StartsWith(rhs, "(const_int") &&
                !absl::StartsWith(rhs, "(mem") && !absl::StartsWith(rhs, "(clobber") &&
                !absl::StartsWith(rhs, "(call") && !absl::StartsWith(rhs, "(if_then_else") &&
                !absl::StartsWith(rhs, "(label_ref") && rhs != "(pc)";
            if (is_compound_rhs) {
              called_simplify = true;
              const std::string unsimp = inverses_->InvertSimplifyRtx(rhs);
              if (unsimp.empty()) {
                RightInverseResult result;
                result.expected_summary = snap.post_state.FormatSummary();
                result.matches = false;
                result.diff_detail =
                    absl::StrFormat("InvertSimplifyRtx returned empty pattern for '%s'", rhs);
                return result;
              }
              if (snap.optimize_level > 0) {
                pat = absl::StrFormat("(set %s %s)%s", dst, unsimp, notes);
              }
            }
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      if (!called_simplify) {
        const std::string probe = inverses_->InvertSimplifyRtx("(reg:SI 2 v0)");
        if (probe.empty()) {
          RightInverseResult result;
          result.expected_summary = snap.post_state.FormatSummary();
          result.matches = false;
          result.diff_detail = "InvertSimplifyRtx returned empty pattern";
          return result;
        }
      }
      break;
    }
    case SubfunctionId::kSubst: {
      bool called_subst = false;
      for (auto& blk : cand_pre.rpo_blocks) {
        for (std::string& pat : blk.instructions) {
          const std::string clean = StripTrailingRegNotesFromPattern(pat);
          if (!absl::StartsWith(clean, "(set ") || clean.back() != ')') {
            continue;
          }
          called_subst = true;
          const std::string notes = pat.size() > clean.size() ? pat.substr(clean.size()) : "";
          const SubstInverseSpec spec = inverses_->InvertSubst(clean);
          if (spec.target_expr.empty() || spec.from_reg.empty() || spec.to_expr.empty()) {
            RightInverseResult result;
            result.expected_summary = snap.post_state.FormatSummary();
            result.matches = false;
            result.diff_detail = absl::StrFormat("InvertSubst returned empty spec for '%s'", clean);
            return result;
          }
          size_t pos = std::string_view("(set ").size();
          int depth = 0;
          for (; pos < clean.size(); ++pos) {
            if (clean[pos] == '(') ++depth;
            if (clean[pos] == ')' && --depth == 0) {
              ++pos;
              break;
            }
          }
          std::string combined = spec.target_expr;
          const size_t from_pos = combined.find(spec.from_reg, pos);
          if (from_pos == std::string::npos) {
            RightInverseResult result;
            result.expected_summary = snap.post_state.FormatSummary();
            result.matches = false;
            result.diff_detail =
                absl::StrFormat("InvertSubst target_expr '%s' missing from_reg '%s' in RHS",
                                spec.target_expr, spec.from_reg);
            return result;
          }
          if (snap.optimize_level > 0) {
            combined.replace(from_pos, spec.from_reg.size(), spec.to_expr);
            pat = absl::StrCat(combined, notes);
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      if (!called_subst) {
        RightInverseResult result;
        result.expected_summary = snap.post_state.FormatSummary();
        result.matches = false;
        result.diff_detail = "No (set ...) instruction found for kSubst";
        return result;
      }
      break;
    }
    case SubfunctionId::kTryCombine:
      cand_pre = inverses_->InvertTryCombine(snap.post_state);
      break;
    case SubfunctionId::kScheduleBlockSched1:
    case SubfunctionId::kScheduleBlockSched2:
      cand_pre = inverses_->InvertScheduleBlock(snap.post_state);
      break;
    case SubfunctionId::kUpdateEquivRegs:
      cand_pre = inverses_->InvertUpdateEquivRegs(snap.post_state);
      break;
    case SubfunctionId::kBlockAlloc:
      cand_pre = inverses_->InvertBlockAlloc(snap.post_state);
      break;
    case SubfunctionId::kGlobalAlloc:
      cand_pre = inverses_->InvertGlobalAlloc(snap.post_state);
      break;
    case SubfunctionId::kReload:
      cand_pre = inverses_->InvertReload(snap.post_state);
      break;
    case SubfunctionId::kAlterReg:
      cand_pre = inverses_->InvertAlterReg(snap.post_state);
      break;
    case SubfunctionId::kDoCrossJump:
      cand_pre = inverses_->InvertDoCrossJump(snap.post_state);
      break;
    case SubfunctionId::kFillSimpleDelaySlots:
      cand_pre = inverses_->InvertFillSimpleDelaySlots(snap.post_state);
      break;
    case SubfunctionId::kFillEagerDelaySlots:
      cand_pre = inverses_->InvertFillEagerDelaySlots(snap.post_state);
      break;
    case SubfunctionId::kRelaxDelaySlots:
      cand_pre = inverses_->InvertRelaxDelaySlots(snap.post_state);
      break;
    case SubfunctionId::kMakeReturnInsns:
      cand_pre = inverses_->InvertMakeReturnInsns(snap.post_state);
      break;
    default:
      break;
  }

  if (!snap.post_state.rpo_blocks.empty() && cand_pre.rpo_blocks.empty()) {
    RightInverseResult result;
    result.expected_summary = snap.post_state.FormatSummary();
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }

  const SubfunctionBoundarySnapshot replayed = hooks_->ExecuteSubfunctionBoundary(snap, cand_pre);
  RightInverseResult result = BuildCfgRightInverseResult(snap.post_state, replayed.post_state);
  if (result.matches && snap.subfunction_id == SubfunctionId::kGlobalAlloc &&
      replayed.post_state.sideband.reg_renumber != snap.post_state.sideband.reg_renumber) {
    result.matches = false;
    result.diff_detail = "sideband.reg_renumber mismatch after kGlobalAlloc";
  }
  return result;
}

}  // namespace rom_nom_nom::gcc_harness
