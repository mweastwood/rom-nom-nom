#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
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
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionCumulativeInverseProvider final : public SubfunctionInverseProvider {
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

struct SuffixBisectionResult {
  std::optional<RtlPassKind> first_failing_pass;
  std::string diagnostic;
};

// Invoked ONLY on suffix failure to pinpoint the earliest diverging stage
// across `kDbr`, `kJump2`, `kSched2`, `kGreg`, `kLreg`, `kSched`, `kCombine`
// without paying O(S^2) replay cost on passing corpus cases.
SuffixBisectionResult BisectCumulativeSuffixFailure(const RightInverseVerifier& verifier,
                                                    const Stage0FunctionInput& input_x,
                                                    const CumulativeLiftChain& chain) {
  constexpr std::array<RtlPassKind, 7> kOrderedPasses = {
      RtlPassKind::kDbr,  RtlPassKind::kJump2, RtlPassKind::kSched2,  RtlPassKind::kGreg,
      RtlPassKind::kLreg, RtlPassKind::kSched, RtlPassKind::kCombine,
  };
  SuffixBisectionResult out;
  for (RtlPassKind pass : kOrderedPasses) {
    const auto it = chain.stages.find(pass);
    if (it == chain.stages.end()) {
      continue;
    }
    const RightInverseResult res = verifier.VerifySuffixRightInverse(pass, input_x, it->second);
    if (!res.matches) {
      out.first_failing_pass = pass;
      out.diagnostic = absl::StrCat("Earliest diverging suffix stage: ", RtlPassExtension(pass),
                                    "\n", res.diff_detail);
      return out;
    }
  }
  out.diagnostic = "No individual stage suffix diverged during bisection";
  return out;
}

class CumulativePipelineTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    verify_ctx = new GccContext();
    verify_hooks = new FunctionHooks(verify_ctx);
    provider = new ProductionCumulativeInverseProvider();
    verifier = new RightInverseVerifier(verify_ctx, verify_hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    verifier = nullptr;
    delete provider;
    provider = nullptr;
    delete verify_hooks;
    verify_hooks = nullptr;
    delete verify_ctx;
    verify_ctx = nullptr;
  }

  static GccContext* verify_ctx;
  static FunctionHooks* verify_hooks;
  static ProductionCumulativeInverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* CumulativePipelineTest::verify_ctx = nullptr;
FunctionHooks* CumulativePipelineTest::verify_hooks = nullptr;
ProductionCumulativeInverseProvider* CumulativePipelineTest::provider = nullptr;
RightInverseVerifier* CumulativePipelineTest::verifier = nullptr;

TEST_F(CumulativePipelineTest, LiftCumulativeChainRejectsEmptyInput) {
  const CumulativeLiftChain chain =
      LiftCumulativeChain(Stage0FunctionInput{}, RtlPassKind::kCombine);
  EXPECT_FALSE(chain.Ok());
  EXPECT_EQ(chain.failure_stage, "Pass00 (kDbr)");
}

TEST_F(CumulativePipelineTest, CorruptedPass06AndIntermediateStagesPinpointedByBisection) {
  constexpr std::string_view kSrc = R"(
int cumulative_bisect_probe(int a, int b) {
  int x = (a + b) ^ 0x5a;
  if (x > 10) {
    return (x << 2) + 17;
  }
  return (x - b) + 9;
}
)";
  verify_ctx->BeginFunction("cumulative_bisect_probe");
  verify_hooks->ApplyCompilationProfile(
      {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}});
  regclass_init();
  const FunctionCompilationTrace trace = verify_hooks->TraceForwardCompilation(
      kSrc, "cumulative_bisect_probe", {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
      "cumulative_bisect_probe");
  ASSERT_FALSE(trace.stage0_input.machine_words.empty());

  const CumulativeLiftChain clean_chain =
      LiftCumulativeChain(trace.stage0_input, RtlPassKind::kCombine);
  ASSERT_TRUE(clean_chain.Ok()) << clean_chain.failure_stage;
  ASSERT_TRUE(clean_chain.stages.contains(RtlPassKind::kCombine));

  const RightInverseResult clean_res = verifier->VerifySuffixRightInverse(
      RtlPassKind::kCombine, trace.stage0_input, clean_chain.stages.at(RtlPassKind::kCombine));
  ASSERT_TRUE(clean_res.matches) << clean_res.diff_detail;

  // Negative 1: Corrupt ONLY `kCombine` (`p6`); `VerifySuffixRightInverse` must
  // reject it and `BisectCumulativeSuffixFailure` must pinpoint `kCombine`.
  {
    CumulativeLiftChain corrupted = clean_chain;
    RtlControlFlowGraph& p6 = corrupted.stages.at(RtlPassKind::kCombine);
    ASSERT_FALSE(p6.rpo_blocks.empty());
    ASSERT_FALSE(p6.rpo_blocks[0].instructions.empty());
    p6.rpo_blocks[0].instructions[0] = "(set (reg:SI 72) (const_int 12345))";
    p6.rpo_blocks[0].SyncRtlInsnsFromInstructions();

    const RightInverseResult bad_res =
        verifier->VerifySuffixRightInverse(RtlPassKind::kCombine, trace.stage0_input, p6);
    EXPECT_FALSE(bad_res.matches);

    const SuffixBisectionResult bisect =
        BisectCumulativeSuffixFailure(*verifier, trace.stage0_input, corrupted);
    ASSERT_TRUE(bisect.first_failing_pass.has_value()) << bisect.diagnostic;
    EXPECT_EQ(*bisect.first_failing_pass, RtlPassKind::kCombine) << bisect.diagnostic;
  }

  // Negative 2: Corrupt BOTH `kGreg` (Stage 3) and `kCombine` (Stage 6);
  // bisection must report the earliest diverging stage (`kGreg`).
  {
    CumulativeLiftChain corrupted = clean_chain;
    for (RtlPassKind pass : {RtlPassKind::kGreg, RtlPassKind::kCombine}) {
      RtlControlFlowGraph& cfg = corrupted.stages.at(pass);
      ASSERT_FALSE(cfg.rpo_blocks.empty());
      ASSERT_FALSE(cfg.rpo_blocks[0].instructions.empty());
      cfg.rpo_blocks[0].instructions[0] = "(set (reg:SI 2 v0) (const_int 777))";
      cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }

    const SuffixBisectionResult bisect =
        BisectCumulativeSuffixFailure(*verifier, trace.stage0_input, corrupted);
    ASSERT_TRUE(bisect.first_failing_pass.has_value()) << bisect.diagnostic;
    EXPECT_EQ(*bisect.first_failing_pass, RtlPassKind::kGreg) << bisect.diagnostic;
  }
}

class CumulativePipelineCorpusTest : public CumulativePipelineTest,
                                     public ::testing::WithParamInterface<RoundTripTestCase> {};

TEST_P(CumulativePipelineCorpusTest, Stage0To6RelocatableAndLinkedRomPipeline) {
  const RoundTripTestCase& tc = GetParam();
  const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const FunctionCompilationTrace& trace = **cached_trace;
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << tc.test_name;

  verify_ctx->BeginFunction("harness_func");
  verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

  // 1. Relocatable `.o` chain lifted ONCE in O(S).
  const CumulativeLiftChain chain = LiftCumulativeChain(trace.stage0_input, RtlPassKind::kCombine);
  ASSERT_TRUE(chain.Ok()) << "LiftCumulativeChain failed at " << chain.failure_stage << " for "
                          << tc.test_name;

  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(chain.effective_profile);
  const bool opt_enabled = flags.RunsOptimizerPasses();
  const bool dbr_enabled = flags.RunsDbrSchedule();
  const bool sched2_enabled = flags.RunsSched2();
  const bool sched1_enabled = flags.RunsSched1();

  const RtlControlFlowGraph& p0 = chain.stages.at(RtlPassKind::kDbr);
  const RtlControlFlowGraph& p2 = chain.stages.at(RtlPassKind::kSched2);
  const RtlControlFlowGraph& p3 = chain.stages.at(RtlPassKind::kGreg);
  const RtlControlFlowGraph& p4 = chain.stages.at(RtlPassKind::kLreg);
  const RtlControlFlowGraph& p5 = chain.stages.at(RtlPassKind::kSched);
  const RtlControlFlowGraph& p6 = chain.stages.at(RtlPassKind::kCombine);

  // 2. O(1) per-stage fresh right-inverse checks on each active stage (matching
  //    `VerifySuffixRightInverse`'s exact active-stage gating).
  auto reset_for_stage = [&](const char* fn_name) {
    verify_ctx->BeginFunction(fn_name);
    verify_hooks->ApplyCompilationProfile(chain.effective_profile);
    regclass_init();
  };

  if (dbr_enabled) {
    ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kDbr)) << tc.test_name;
    const RtlControlFlowGraph& post_dbr = trace.pass_snapshots.at(RtlPassKind::kDbr);
    const RtlControlFlowGraph pre_dbr = InvertPass01DbrSchedule(post_dbr, chain.effective_profile);
    ASSERT_FALSE(pre_dbr.rpo_blocks.empty()) << "Empty Stage 1 preimage for " << tc.test_name;
    reset_for_stage("cum_stage1_dbr");
    RtlControlFlowGraph replayed_dbr = pre_dbr;
    for (gcc_harness::SubfunctionId sub_id : {
             gcc_harness::SubfunctionId::kFillSimpleDelaySlots,
             gcc_harness::SubfunctionId::kFillEagerDelaySlots,
             gcc_harness::SubfunctionId::kRelaxDelaySlots,
             gcc_harness::SubfunctionId::kMakeReturnInsns,
         }) {
      replayed_dbr = verify_hooks
                         ->ExecuteSubfunctionBoundary(sub_id, replayed_dbr, {},
                                                      chain.effective_profile.optimize_level,
                                                      chain.effective_profile.c_flags,
                                                      chain.effective_profile.gp_symbol_sizes)
                         .post_state;
    }
    const RtlCfgPassComparison cmp_dbr = replayed_dbr.CompareAgainst(post_dbr);
    EXPECT_TRUE(cmp_dbr.MatchesExactly()) << "Stage 1 (kDbr) mismatch for " << tc.test_name << ":\n"
                                          << cmp_dbr.mismatch_detail;
  }
  if (opt_enabled) {
    ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kJump2)) << tc.test_name;
    const RtlControlFlowGraph& post_jump2 = trace.pass_snapshots.at(RtlPassKind::kJump2);
    const RtlControlFlowGraph pre_jump2 =
        InvertPass02Jump2CrossJump(post_jump2, chain.effective_profile);
    ASSERT_FALSE(pre_jump2.rpo_blocks.empty()) << "Empty Stage 2 preimage for " << tc.test_name;
    reset_for_stage("cum_stage2_jump2");
    const RightInverseResult res =
        verifier->VerifyJump2CrossJumpStageRightInverse(post_jump2, pre_jump2);
    EXPECT_TRUE(res.matches) << "Stage 2 (kJump2) mismatch for " << tc.test_name << ":\n"
                             << res.diff_detail;
  }
  if (sched2_enabled) {
    reset_for_stage("cum_stage3_sched2");
    const RightInverseResult res = verifier->VerifySched2StageRightInverse(p2);
    EXPECT_TRUE(res.matches) << "Stage 3 (kSched2) mismatch for " << tc.test_name << ":\n"
                             << res.diff_detail;
  }
  {
    reset_for_stage("cum_stage4_greg");
    const RightInverseResult res = verifier->VerifyGlobalReloadStageRightInverse(p3, p4);
    EXPECT_TRUE(res.matches) << "Stage 4 (kGreg) mismatch for " << tc.test_name << ":\n"
                             << res.diff_detail;
  }
  if (opt_enabled) {
    reset_for_stage("cum_stage5_lreg");
    const RightInverseResult res = verifier->VerifyLocalAllocStageRightInverse(p4, p5);
    EXPECT_TRUE(res.matches) << "Stage 5 (kLreg) mismatch for " << tc.test_name << ":\n"
                             << res.diff_detail;
  }
  if (sched1_enabled) {
    reset_for_stage("cum_stage6_sched1");
    const RightInverseResult res = verifier->VerifySched1StageRightInverse(p5, p6);
    EXPECT_TRUE(res.matches) << "Stage 6 (kSched) mismatch for " << tc.test_name << ":\n"
                             << res.diff_detail;
  }

  // 3. Single O(S) deepest-stage cumulative suffix check (Stages 6..0 -> `.o`),
  //    with failure-only intermediate suffix bisection diagnostics.
  const RightInverseResult suffix_res =
      verifier->VerifySuffixRightInverse(RtlPassKind::kCombine, trace.stage0_input, p6);
  if (!suffix_res.matches) {
    const SuffixBisectionResult bisect =
        BisectCumulativeSuffixFailure(*verifier, trace.stage0_input, chain);
    ADD_FAILURE() << "Stage 0..6 relocatable suffix mismatch for " << tc.test_name << ":\n"
                  << suffix_res.diff_detail << "\nBisection:\n"
                  << bisect.diagnostic;
  }

  // 4. Linked-ROM chain: O(S) full chain + deepest suffix on distinct cases,
  //    or exact Stage 0 equivalence guard on non-distinct cases.
  const Stage0FunctionInput linked_stage0 = BakeLinkedRomStage0Input(trace.stage0_input);
  if (HasDistinctLinkedRomRepresentation(trace.stage0_input, linked_stage0)) {
    const CumulativeLiftChain linked_chain =
        LiftCumulativeChain(linked_stage0, RtlPassKind::kCombine);
    ASSERT_TRUE(linked_chain.Ok()) << "Linked-ROM LiftCumulativeChain failed at "
                                   << linked_chain.failure_stage << " for " << tc.test_name;
    const RtlControlFlowGraph& linked_p6 = linked_chain.stages.at(RtlPassKind::kCombine);
    const RightInverseResult linked_suffix_res =
        verifier->VerifySuffixRightInverse(RtlPassKind::kCombine, linked_stage0, linked_p6);
    if (!linked_suffix_res.matches) {
      const SuffixBisectionResult bisect =
          BisectCumulativeSuffixFailure(*verifier, linked_stage0, linked_chain);
      ADD_FAILURE() << "Stage 0..6 linked-ROM suffix mismatch for " << tc.test_name << ":\n"
                    << linked_suffix_res.diff_detail << "\nBisection:\n"
                    << bisect.diagnostic;
    }
  } else {
    const RtlControlFlowGraph linked_p0 = InvertFinalAndGasFunction(linked_stage0);
    const RtlCfgPassComparison cmp = linked_p0.CompareAgainst(p0);
    EXPECT_TRUE(cmp.MatchesExactly())
        << "Non-distinct linked-ROM Stage 0 CFG divergence for " << tc.test_name << ":\n"
        << cmp.mismatch_detail;
    EXPECT_EQ(linked_p0.sideband, p0.sideband)
        << "Non-distinct linked-ROM Stage 0 sideband divergence for " << tc.test_name;
  }
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, CumulativePipelineCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
