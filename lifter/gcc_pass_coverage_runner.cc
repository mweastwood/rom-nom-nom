#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "lifter/cumulative_lift_chain.h"
#include "lifter/invert_alter_reg.h"
#include "lifter/invert_block_alloc.h"
#include "lifter/invert_do_cross_jump.h"
#include "lifter/invert_fill_eager_delay_slots.h"
#include "lifter/invert_fill_simple_delay_slots.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"
#include "lifter/invert_gas_mips_ip.h"
#include "lifter/invert_global_alloc.h"
#include "lifter/invert_make_return_insns.h"
#include "lifter/invert_relax_delay_slots.h"
#include "lifter/invert_reload.h"
#include "lifter/invert_schedule_block.h"
#include "lifter/invert_update_equiv_regs.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"
#include "lifter/pass04_greg_reload.h"
#include "lifter/pass05_lreg_local_alloc.h"
#include "lifter/pass06_sched1_pre_alloc.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/stage0_test_utils.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::PrologueEpilogueFrameSpec;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionIdName;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class CoverageReplayInverseProvider final : public SubfunctionInverseProvider {
 public:
  std::string InvertGasMipsIp(uint32_t word_x) const override {
    return rom_nom_nom::InvertGasMipsIp(word_x);
  }

  std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const override {
    return rom_nom_nom::InvertGasMacro(macro_words_x);
  }

  std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x);
  }

  std::vector<std::string> InvertGasAppendInsn(
      absl::Span<const uint32_t> words_x, absl::Span<const std::string> c_flags) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x, c_flags);
  }

  std::string InvertFinalScanInsn(std::string_view asm_line_x) const override {
    return lifter::InvertFinalScanInsn(asm_line_x);
  }

  PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view asm_text_x) const override {
    const auto spec = lifter::InvertFunctionPrologueEpilogue(asm_text_x);
    return {
        .var_size = spec.var_size,
        .saved_gpr_mask = spec.saved_gpr_mask,
        .saved_fpr_mask = spec.saved_fpr_mask,
    };
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(
      absl::Span<const uint32_t> machine_words_x) const override {
    return lifter::InvertFinalAndGasFunction(machine_words_x);
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) const override {
    return lifter::InvertFinalAndGasFunction(input_x);
  }

  RtlControlFlowGraph InvertMakeReturnInsns(
      const RtlControlFlowGraph& dbr_return_cfg_x) const override {
    return lifter::InvertMakeReturnInsns(dbr_return_cfg_x);
  }

  RtlControlFlowGraph InvertRelaxDelaySlots(
      const RtlControlFlowGraph& dbr_relaxed_cfg_x) const override {
    return lifter::InvertRelaxDelaySlots(dbr_relaxed_cfg_x);
  }

  RtlControlFlowGraph InvertFillEagerDelaySlots(
      const RtlControlFlowGraph& dbr_eager_cfg_x) const override {
    return lifter::InvertFillEagerDelaySlots(dbr_eager_cfg_x);
  }

  RtlControlFlowGraph InvertFillSimpleDelaySlots(
      const RtlControlFlowGraph& dbr_simple_cfg_x) const override {
    return lifter::InvertFillSimpleDelaySlots(dbr_simple_cfg_x);
  }

  RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x,
                                                 const CompilationProfile& profile) const override {
    return lifter::InvertFillSimpleDelaySlots(dbr_simple_cfg_x, profile);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) const override {
    return lifter::InvertPass01DbrSchedule(dbr_cfg_x);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x,
                                        const CompilationProfile& profile) const override {
    return lifter::InvertPass01DbrSchedule(dbr_cfg_x, profile);
  }

  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
    return lifter::InvertPass02Jump2CrossJump(jump2_cfg_x);
  }

  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                        const CompilationProfile& profile) const override {
    return lifter::InvertPass02Jump2CrossJump(jump2_cfg_x, profile);
  }

  RtlControlFlowGraph InvertScheduleBlock(
      const RtlControlFlowGraph& scheduled_cfg_x) const override {
    if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
      return lifter::InvertPass06Sched1PreAlloc(scheduled_cfg_x);
    }
    return lifter::InvertScheduleBlock(scheduled_cfg_x);
  }

  RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                          const CompilationProfile& profile) const override {
    if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
      return lifter::InvertPass06Sched1PreAlloc(scheduled_cfg_x, profile);
    }
    return lifter::InvertScheduleBlock(scheduled_cfg_x, profile);
  }

  RtlControlFlowGraph InvertSched2PostReload(const RtlControlFlowGraph& scheduled_cfg_x,
                                             const CompilationProfile& profile,
                                             bool /*sched2_enabled*/) const override {
    return lifter::InvertPass03Sched2PostReload(scheduled_cfg_x, profile);
  }

  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     int pseudo_regno) const override {
    return lifter::InvertAlterReg(allocated_cfg_x, pseudo_regno);
  }

  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     const CompilationProfile& profile,
                                     int pseudo_regno) const override {
    return lifter::InvertAlterReg(allocated_cfg_x, profile, pseudo_regno);
  }

  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertReload(reload_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg_x,
                                   const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertReload(reload_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertGlobalAlloc(greg_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg_x,
                                        const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertGlobalAlloc(greg_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertBlockAlloc(lreg_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg_x,
                                       const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertBlockAlloc(lreg_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertUpdateEquivRegs(lreg_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg_x,
                                            const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertUpdateEquivRegs(lreg_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

bool IsTrackedStage0To6Subfunction(SubfunctionId id) {
  switch (id) {
    case SubfunctionId::kFillSimpleDelaySlots:
    case SubfunctionId::kFillEagerDelaySlots:
    case SubfunctionId::kRelaxDelaySlots:
    case SubfunctionId::kMakeReturnInsns:
    case SubfunctionId::kDoCrossJump:
    case SubfunctionId::kScheduleBlockSched2:
    case SubfunctionId::kAlterReg:
    case SubfunctionId::kReload:
    case SubfunctionId::kGlobalAlloc:
    case SubfunctionId::kBlockAlloc:
    case SubfunctionId::kUpdateEquivRegs:
    case SubfunctionId::kScheduleBlockSched1:
      return true;
    default:
      return false;
  }
}

int RunForwardMode(size_t shard_index, size_t num_shards) {
  const std::vector<RoundTripTestCase>& all_cases = GetSharedRoundTripTestCases();
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  for (size_t k = shard_index; k < all_cases.size(); k += num_shards) {
    const RoundTripTestCase& tc = all_cases[k];
    ctx.BeginFunction("harness_init");
    hooks.ApplyCompilationProfile({2, tc.options.c_flags, {}});
    regclass_init();
    const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(tc);
    if (trace.stage0_input.machine_words.empty()) {
      std::cerr << "[gcc_pass_coverage_runner] forward compilation produced empty words for "
                << tc.test_name << " (idx=" << k << ")\n";
      return 1;
    }
  }
  return 0;
}

bool FailMismatch(std::string_view test_name, std::string_view check_name,
                  const RightInverseResult& res) {
  std::cerr << "[gcc_pass_coverage_runner] REPLAY MISMATCH on " << test_name << " [" << check_name
            << "]: " << res.diff_detail << "\n";
  return false;
}

bool RunReplayCase(size_t case_index, const RoundTripTestCase& tc, GccContext* ctx,
                   FunctionHooks* hooks, const RightInverseVerifier& verifier) {
  const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTraceByIndex(case_index);
  if (!cached_trace.ok()) {
    std::cerr << "[gcc_pass_coverage_runner] Failed to load cached trace for " << tc.test_name
              << " (idx=" << case_index << "): " << cached_trace.status() << "\n";
    return false;
  }
  const FunctionCompilationTrace& trace = **cached_trace;
  if (trace.stage0_input.machine_words.empty()) {
    std::cerr << "[gcc_pass_coverage_runner] Empty stage0_input for " << tc.test_name << "\n";
    return false;
  }

  const CumulativeLiftChain chain = LiftCumulativeChain(trace.stage0_input, RtlPassKind::kCombine);
  if (!chain.Ok()) {
    std::cerr << "[gcc_pass_coverage_runner] LiftCumulativeChain failed at " << chain.failure_stage
              << " for " << tc.test_name << "\n";
    return false;
  }

  auto reset_ctx = [&](const char* fn_name) {
    ctx->BeginFunction(fn_name);
    hooks->ApplyCompilationProfile(chain.effective_profile);
    regclass_init();
  };

  // 1. Subfunction boundary replays across Stages 1-6.
  bool ran_do_cross_jump = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (!IsTrackedStage0To6Subfunction(snap.subfunction_id)) {
      continue;
    }
    reset_ctx("cov_subfunction");
    const RightInverseResult sub_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
    if (!sub_res.matches) {
      return FailMismatch(tc.test_name, SubfunctionIdName(snap.subfunction_id), sub_res);
    }
    if (snap.subfunction_id == SubfunctionId::kDoCrossJump) {
      ran_do_cross_jump = true;
    }
  }

  // 2. Per-stage right-inverse replays matching active-stage gating.
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(chain.effective_profile);
  const bool opt_enabled = flags.RunsOptimizerPasses();
  const bool sched2_enabled = flags.RunsSched2();
  const bool sched1_enabled = flags.RunsSched1();

  const RtlControlFlowGraph& p2 = chain.stages.at(RtlPassKind::kSched2);
  const RtlControlFlowGraph& p3 = chain.stages.at(RtlPassKind::kGreg);
  const RtlControlFlowGraph& p4 = chain.stages.at(RtlPassKind::kLreg);
  const RtlControlFlowGraph& p5 = chain.stages.at(RtlPassKind::kSched);
  const RtlControlFlowGraph& p6 = chain.stages.at(RtlPassKind::kCombine);

  if (!ran_do_cross_jump && opt_enabled && trace.pass_snapshots.contains(RtlPassKind::kJump2)) {
    const RtlControlFlowGraph& post_jump2 = trace.pass_snapshots.at(RtlPassKind::kJump2);
    const RtlControlFlowGraph pre_jump2 =
        InvertPass02Jump2CrossJump(post_jump2, chain.effective_profile);
    reset_ctx("cov_stage2_jump2");
    const RightInverseResult res =
        verifier.VerifyJump2CrossJumpStageRightInverse(post_jump2, pre_jump2);
    if (!res.matches) {
      return FailMismatch(tc.test_name, "Stage2_kJump2", res);
    }
  }
  if (sched2_enabled) {
    reset_ctx("cov_stage3_sched2");
    const RightInverseResult res = verifier.VerifySched2StageRightInverse(p2);
    if (!res.matches) {
      return FailMismatch(tc.test_name, "Stage3_kSched2", res);
    }
  }
  {
    reset_ctx("cov_stage4_greg");
    const RightInverseResult res = verifier.VerifyGlobalReloadStageRightInverse(p3, p4);
    if (!res.matches) {
      return FailMismatch(tc.test_name, "Stage4_kGreg", res);
    }
  }
  if (opt_enabled) {
    reset_ctx("cov_stage5_lreg");
    const RightInverseResult res = verifier.VerifyLocalAllocStageRightInverse(p4, p5);
    if (!res.matches) {
      return FailMismatch(tc.test_name, "Stage5_kLreg", res);
    }
  }
  if (sched1_enabled) {
    reset_ctx("cov_stage6_sched1");
    const RightInverseResult res = verifier.VerifySched1StageRightInverse(p5, p6);
    if (!res.matches) {
      return FailMismatch(tc.test_name, "Stage6_kSched", res);
    }
  }

  // 3. Deepest-stage cumulative suffix replay (relocatable `.o`).
  const RightInverseResult suffix_res =
      verifier.VerifySuffixRightInverse(RtlPassKind::kCombine, trace.stage0_input, p6);
  if (!suffix_res.matches) {
    return FailMismatch(tc.test_name, "Suffix_Reloc_kCombine", suffix_res);
  }

  // 4. Deepest-stage cumulative suffix replay on distinct linked-ROM inputs.
  const Stage0FunctionInput linked_stage0 = BakeLinkedRomStage0Input(trace.stage0_input);
  if (HasDistinctLinkedRomRepresentation(trace.stage0_input, linked_stage0)) {
    const CumulativeLiftChain linked_chain =
        LiftCumulativeChain(linked_stage0, RtlPassKind::kCombine);
    if (!linked_chain.Ok()) {
      std::cerr << "[gcc_pass_coverage_runner] Linked LiftCumulativeChain failed at "
                << linked_chain.failure_stage << " for " << tc.test_name << "\n";
      return false;
    }
    const RtlControlFlowGraph& linked_p6 = linked_chain.stages.at(RtlPassKind::kCombine);
    const RightInverseResult linked_res =
        verifier.VerifySuffixRightInverse(RtlPassKind::kCombine, linked_stage0, linked_p6);
    if (!linked_res.matches) {
      return FailMismatch(tc.test_name, "Suffix_Linked_kCombine", linked_res);
    }
  }

  return true;
}

int RunReplayMode(size_t shard_index, size_t num_shards) {
  const std::vector<RoundTripTestCase>& all_cases = GetSharedRoundTripTestCases();
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  CoverageReplayInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  for (size_t k = shard_index; k < all_cases.size(); k += num_shards) {
    if (!RunReplayCase(k, all_cases[k], &ctx, &hooks, verifier)) {
      return 1;
    }
  }
  return 0;
}

}  // namespace
}  // namespace rom_nom_nom::lifter

int main(int argc, char** argv) {
  std::string mode;
  uint32_t shard_index = 0;
  uint32_t num_shards = 1;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (absl::ConsumePrefix(&arg, "--mode=")) {
      mode.assign(arg.data(), arg.size());
    } else if (absl::ConsumePrefix(&arg, "--shard_index=")) {
      if (!absl::SimpleAtoi(arg, &shard_index)) {
        std::cerr << "Invalid --shard_index: " << arg << "\n";
        return 2;
      }
    } else if (absl::ConsumePrefix(&arg, "--num_shards=")) {
      if (!absl::SimpleAtoi(arg, &num_shards)) {
        std::cerr << "Invalid --num_shards: " << arg << "\n";
        return 2;
      }
    } else {
      std::cerr << "Unknown flag: " << arg << "\n";
      return 2;
    }
  }

  if (num_shards == 0 || shard_index >= num_shards) {
    std::cerr << "Invalid shard coordinates: shard_index=" << shard_index
              << " num_shards=" << num_shards << "\n";
    return 2;
  }

  if (mode == "forward") {
    return rom_nom_nom::lifter::RunForwardMode(shard_index, num_shards);
  }
  if (mode == "replay") {
    return rom_nom_nom::lifter::RunReplayMode(shard_index, num_shards);
  }
  std::cerr << "Required flag --mode=forward|replay (got '" << mode << "')\n";
  return 2;
}
