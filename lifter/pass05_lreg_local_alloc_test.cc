#include "lifter/pass05_lreg_local_alloc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
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

class ProductionPass05InverseProvider final : public SubfunctionInverseProvider {
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
    return lifter::InvertScheduleBlock(scheduled_cfg_x);
  }

  RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                          const CompilationProfile& profile) const override {
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

class CorruptedPass05InverseProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg_x,
                                       const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertBlockAlloc(lreg_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
  using SubfunctionInverseProvider::InvertBlockAlloc;

  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg_x,
                                            const CompilationProfile& /*profile*/) const override {
    RtlControlFlowGraph bad = lreg_cfg_x;
    bad.pass = RtlPassKind::kSched;
    if (!bad.rpo_blocks.empty()) {
      bad.rpo_blocks[0].instructions = {"(set (reg/i:SI 2 v0) (const_int 888888))",
                                        "(use (reg/i:SI 2 v0))"};
      bad.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }
    return bad;
  }
  using SubfunctionInverseProvider::InvertUpdateEquivRegs;
};

class Pass05LregLocalAllocTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    trace_ctx = new GccContext();
    trace_hooks = new FunctionHooks(trace_ctx);
    verify_ctx = new GccContext();
    verify_hooks = new FunctionHooks(verify_ctx);
    provider = new ProductionPass05InverseProvider();
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
    delete trace_hooks;
    trace_hooks = nullptr;
    delete trace_ctx;
    trace_ctx = nullptr;
  }

  static GccContext* trace_ctx;
  static FunctionHooks* trace_hooks;
  static GccContext* verify_ctx;
  static FunctionHooks* verify_hooks;
  static ProductionPass05InverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* Pass05LregLocalAllocTest::trace_ctx = nullptr;
FunctionHooks* Pass05LregLocalAllocTest::trace_hooks = nullptr;
GccContext* Pass05LregLocalAllocTest::verify_ctx = nullptr;
FunctionHooks* Pass05LregLocalAllocTest::verify_hooks = nullptr;
ProductionPass05InverseProvider* Pass05LregLocalAllocTest::provider = nullptr;
RightInverseVerifier* Pass05LregLocalAllocTest::verifier = nullptr;

TEST_F(Pass05LregLocalAllocTest, PositiveOptimizationProfilesEquivDeletionAndLocalScratch) {
  constexpr std::string_view kStage05ProfileSrc = R"(
extern double ext_blend(int, double, int);
double stage05_profile_fn(int a, int b, int c, int d, int stack_arg5, const int* arr) {
  int i;
  double acc = (double)(arr[0] * arr[1]);
  int s0 = arr[2] + a;
  int s1 = arr[3] ^ b;
  for (i = 0; i < c; ++i) {
    int local_mul = (s0 * 7) + (i * d);
    acc = ext_blend(local_mul, acc, s1);
    s0 += 3;
  }
  if (s1 > 0) {
    return acc + (double)stack_arg5;
  }
  return acc;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O3", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips1", "-Wa,-O2"}}) {
    trace_ctx->BeginFunction("harness_init");
    trace_hooks->ApplyCompilationProfile({2, flags, {}});
    regclass_init();
    const FunctionCompilationTrace trace = trace_hooks->TraceForwardCompilation(
        kStage05ProfileSrc, "stage05_profile_fn", flags, "pass05_profiles");
    ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kLreg));
    const RtlControlFlowGraph& lreg_cfg = trace.pass_snapshots.at(RtlPassKind::kLreg);

    const RtlControlFlowGraph lifted_sched =
        InvertPass05LregLocalAlloc(lreg_cfg, trace.stage0_input.profile);
    ASSERT_FALSE(lifted_sched.rpo_blocks.empty());
    EXPECT_EQ(lifted_sched.pass, RtlPassKind::kSched);

    verify_ctx->BeginFunction("harness_func");
    verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
    const RightInverseResult stage_res = verifier->VerifyLocalAllocStageRightInverse(lreg_cfg);
    EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;

    const RightInverseResult suffix_res =
        verifier->VerifySuffixRightInverse(RtlPassKind::kSched, trace.stage0_input);
    EXPECT_TRUE(suffix_res.matches) << suffix_res.diff_detail;
  }
}

TEST_F(Pass05LregLocalAllocTest, LinkedRomMultiSwitchSpillAndConstantPoolSuffixRightInverse) {
  constexpr std::string_view kLinkedStage05Src = R"(
extern int g_table_bias[8];
extern int g_small_counter;
extern double ext_callback(int, double);

int linked_stage05_dispatch(int op1, int op2, const int* vec, double scale, int fifth_arg) {
  int acc = g_small_counter + vec[0];
  double f = scale * 1.61803398875;
  switch (op1) {
    case 0: acc += g_table_bias[0] + vec[1]; break;
    case 1: acc -= g_table_bias[1] ^ vec[2]; break;
    case 2: acc *= g_table_bias[2] + 3; break;
    case 3: acc += (int)ext_callback(acc, f); break;
    case 4: acc ^= g_table_bias[4] + vec[3]; break;
    default: acc += fifth_arg; break;
  }
  switch (op2) {
    case 10: f += 2.71828182845; acc += (int)f; break;
    case 11: acc -= g_table_bias[5]; break;
    case 12: acc += (int)ext_callback(op1 + acc, f * 0.5); break;
    case 13: acc ^= g_table_bias[6] + g_small_counter; break;
    case 14: acc += g_table_bias[7] * 5; break;
    default: acc -= 9; break;
  }
  g_small_counter = acc;
  return acc;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips1", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"}}) {
    trace_ctx->BeginFunction("harness_init");
    trace_hooks->ApplyCompilationProfile({2, flags, {}});
    regclass_init();
    const FunctionCompilationTrace trace = trace_hooks->TraceForwardCompilation(
        kLinkedStage05Src, "linked_stage05_dispatch", flags, "pass05_linked");
    ASSERT_GE(trace.stage0_input.jump_tables.size(), 2u);

    const RightInverseResult reloc_res =
        verifier->VerifySuffixRightInverse(RtlPassKind::kSched, trace.stage0_input);
    EXPECT_TRUE(reloc_res.matches) << reloc_res.diff_detail;

    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(trace.stage0_input);
    ASSERT_NE(linked.base_vram, 0u);
    ASSERT_TRUE(linked.relocations.empty());

    const RightInverseResult linked_res =
        verifier->VerifySuffixRightInverse(RtlPassKind::kSched, linked);
    EXPECT_TRUE(linked_res.matches) << linked_res.diff_detail;

    // Negative check: corrupting jump-table VRAM in linked ROM input must fail `kSched` suffix.
    Stage0FunctionInput bad_jt = linked;
    bad_jt.jump_tables[0].table_vram += 128u;
    EXPECT_FALSE(verifier->VerifySuffixRightInverse(RtlPassKind::kSched, bad_jt).matches);
  }
}

TEST_F(Pass05LregLocalAllocTest, RejectsEmptyMalformedAndCorruptedInputs) {
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertPass05LregLocalAlloc(empty_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertLregLocalAlloc(empty_cfg).rpo_blocks.empty());

  RtlControlFlowGraph malformed_cfg;
  malformed_cfg.pass = RtlPassKind::kLreg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 72) (plus:SI (reg:SI 4 a0)"};
  malformed_cfg.rpo_blocks.push_back(bb0);
  EXPECT_TRUE(InvertPass05LregLocalAlloc(malformed_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertPass05LregLocalAlloc(malformed_cfg, {.optimize_level = 0, .c_flags = {"-O0"}})
                  .rpo_blocks.empty());

  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.pass = RtlPassKind::kLreg;
  RtlBasicBlock bb_bad_edge;
  bb_bad_edge.id = 0;
  bb_bad_edge.instructions = {"(set (reg/i:SI 2 v0) (const_int 0))", "(use (reg/i:SI 2 v0))"};
  bb_bad_edge.successors = {5};
  bb_bad_edge.SyncRtlInsnsFromInstructions();
  bad_succ_cfg.rpo_blocks.push_back(bb_bad_edge);
  EXPECT_TRUE(InvertPass05LregLocalAlloc(bad_succ_cfg).rpo_blocks.empty());

  for (const std::string& opt_flag : {std::string("-O2"), std::string("-O0")}) {
    const FunctionCompilationTrace trace = trace_hooks->TraceForwardCompilation(
        "int simple_add(int a, int b) { return a + b + 3; }\n", "simple_add", {opt_flag});
    ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kLreg));
    CorruptedPass05InverseProvider bad_provider;
    RightInverseVerifier bad_verifier(verify_ctx, verify_hooks, &bad_provider);
    verify_ctx->BeginFunction("harness_func");
    verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
    EXPECT_FALSE(
        bad_verifier.VerifyLocalAllocStageRightInverse(trace.pass_snapshots.at(RtlPassKind::kLreg))
            .matches);
  }
}

class Pass05LregLocalAllocRoundTripTest : public Pass05LregLocalAllocTest,
                                          public ::testing::WithParamInterface<RoundTripTestCase> {
};

TEST_P(Pass05LregLocalAllocRoundTripTest, Stage05AndCumulativeSuffixAcrossAll4172Cases) {
  const RoundTripTestCase& tc = GetParam();
  const absl::StatusOr<const FunctionCompilationTrace*> trace_or =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(trace_or.ok()) << tc.test_name << ": " << trace_or.status();
  const FunctionCompilationTrace& trace = **trace_or;
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << tc.test_name;
  ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kLreg)) << tc.test_name;

  // 1. Standalone Stage 5 right-inverse check ($F_5(G_5(x_5)) = x_5$) on `.08.lreg` snapshot
  //    AND direct `InvertPass05LregLocalAlloc` stitcher check (lifted once, verified via preimage).
  const RtlControlFlowGraph& lreg_snap = trace.pass_snapshots.at(RtlPassKind::kLreg);
  const RtlControlFlowGraph stitched_sched =
      InvertPass05LregLocalAlloc(lreg_snap, trace.stage0_input.profile);
  ASSERT_FALSE(stitched_sched.rpo_blocks.empty()) << "Empty Pass05 output for " << tc.test_name;
  EXPECT_EQ(stitched_sched.pass, RtlPassKind::kSched);

  verify_ctx->BeginFunction("harness_func");
  verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();
  const RightInverseResult stage_res =
      verifier->VerifyLocalAllocStageRightInverse(lreg_snap, stitched_sched);
  const std::string exp_sched_sum =
      trace.pass_snapshots.contains(RtlPassKind::kSched)
          ? trace.pass_snapshots.at(RtlPassKind::kSched).FormatSummary()
          : "<no sched>";
  EXPECT_TRUE(stage_res.matches) << "Stage 5 standalone mismatch for " << tc.test_name << ":\n"
                                 << stage_res.diff_detail << "\nexpected:\n"
                                 << stage_res.expected_summary << "\nactual:\n"
                                 << stage_res.actual_summary << "\nstitched_sched:\n"
                                 << stitched_sched.FormatSummary() << "\ntrace_sched:\n"
                                 << exp_sched_sum;

  // 2. Fresh-lifted `.08.lreg` Stage 5 subfunction & whole-stage right-inverse checks
  //    (`Pass00 -> Pass01 -> Pass02 -> Pass03 -> Pass04 -> pre_block -> p5`), lifting each
  //    preimage once and passing it to the preimage verifier overloads.
  const RtlControlFlowGraph p0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(p0.rpo_blocks.empty()) << "Empty Pass00 output for " << tc.test_name;
  const RtlControlFlowGraph p1 = InvertPass01DbrSchedule(p0, trace.stage0_input.profile);
  ASSERT_FALSE(p1.rpo_blocks.empty()) << "Empty Pass01 output for " << tc.test_name;
  const RtlControlFlowGraph p2 = InvertPass02Jump2CrossJump(p1, trace.stage0_input.profile);
  ASSERT_FALSE(p2.rpo_blocks.empty()) << "Empty Pass02 output for " << tc.test_name;
  const RtlControlFlowGraph p3 = InvertPass03Sched2PostReload(p2, trace.stage0_input.profile);
  ASSERT_FALSE(p3.rpo_blocks.empty()) << "Empty Pass03 output for " << tc.test_name;
  const RtlControlFlowGraph p4 = InvertPass04GregReload(p3, trace.stage0_input.profile);
  ASSERT_FALSE(p4.rpo_blocks.empty()) << "Empty Pass04 output for " << tc.test_name;

  absl::StatusOr<RtlControlFlowGraph> pre_block = InvertBlockAlloc(p4, trace.stage0_input.profile);
  ASSERT_TRUE(pre_block.ok()) << tc.test_name << ": " << pre_block.status();
  ASSERT_FALSE(pre_block->rpo_blocks.empty()) << "Empty InvertBlockAlloc for " << tc.test_name;

  absl::StatusOr<RtlControlFlowGraph> pre_equiv =
      InvertUpdateEquivRegs(*pre_block, trace.stage0_input.profile);
  ASSERT_TRUE(pre_equiv.ok()) << tc.test_name << ": " << pre_equiv.status();
  ASSERT_FALSE(pre_equiv->rpo_blocks.empty()) << "Empty InvertUpdateEquivRegs for " << tc.test_name;
  RtlControlFlowGraph p5 = *std::move(pre_equiv);
  p5.pass = RtlPassKind::kSched;

  verify_ctx->BeginFunction("harness_fresh_block");
  verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();
  const RightInverseResult fresh_ba_res = verifier->VerifyBlockAllocRightInverse(p4, *pre_block);
  EXPECT_TRUE(fresh_ba_res.matches) << "Fresh kBlockAlloc mismatch for " << tc.test_name << ":\n"
                                    << fresh_ba_res.diff_detail;

  verify_ctx->BeginFunction("harness_fresh_equiv");
  verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();
  const RightInverseResult fresh_equiv_res =
      verifier->VerifyUpdateEquivRegsRightInverse(*pre_block, p5);
  EXPECT_TRUE(fresh_equiv_res.matches)
      << "Fresh kUpdateEquivRegs mismatch for " << tc.test_name << ":\n"
      << fresh_equiv_res.diff_detail;

  verify_ctx->BeginFunction("harness_fresh_stage5");
  verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();
  const RightInverseResult fresh_stage_res = verifier->VerifyLocalAllocStageRightInverse(p4, p5);
  EXPECT_TRUE(fresh_stage_res.matches)
      << "Fresh Stage 5 right-inverse mismatch for " << tc.test_name << ":\n"
      << fresh_stage_res.diff_detail << "\nfresh_lreg(p4):\n"
      << p4.FormatSummary() << "\nactual:\n"
      << fresh_stage_res.actual_summary << "\nfresh_sched(p5):\n"
      << p5.FormatSummary() << "\ntrace_sched:\n"
      << exp_sched_sum;

  // 3. Full fresh-`GccContext` Stage 0..5 suffix right-inverse ($F_{0..5}(G_{5..0}(x_0)) = x_0$)
  //    on BOTH relocatable `.o` (`trace.stage0_input`) AND distinct linked-ROM inputs.
  const RightInverseResult suffix_res =
      verifier->VerifySuffixRightInverse(RtlPassKind::kSched, trace.stage0_input, p5);
  EXPECT_TRUE(suffix_res.matches) << "Stage 0..5 relocatable suffix mismatch for " << tc.test_name
                                  << ":\n"
                                  << suffix_res.diff_detail << "\nfresh_lreg(p4):\n"
                                  << p4.FormatSummary() << "\nfresh_sched(p5):\n"
                                  << p5.FormatSummary() << "\ntrace_sched:\n"
                                  << exp_sched_sum;

  const Stage0FunctionInput linked_stage0 = BakeLinkedRomStage0Input(trace.stage0_input);
  if (HasDistinctLinkedRomRepresentation(trace.stage0_input, linked_stage0)) {
    const RtlControlFlowGraph lp0 = InvertFinalAndGasFunction(linked_stage0);
    ASSERT_FALSE(lp0.rpo_blocks.empty()) << "Empty linked Pass00 output for " << tc.test_name;
    const RtlControlFlowGraph lp1 = InvertPass01DbrSchedule(lp0, linked_stage0.profile);
    ASSERT_FALSE(lp1.rpo_blocks.empty()) << "Empty linked Pass01 output for " << tc.test_name;
    const RtlControlFlowGraph lp2 = InvertPass02Jump2CrossJump(lp1, linked_stage0.profile);
    ASSERT_FALSE(lp2.rpo_blocks.empty()) << "Empty linked Pass02 output for " << tc.test_name;
    const RtlControlFlowGraph lp3 = InvertPass03Sched2PostReload(lp2, linked_stage0.profile);
    ASSERT_FALSE(lp3.rpo_blocks.empty()) << "Empty linked Pass03 output for " << tc.test_name;
    const RtlControlFlowGraph lp4 = InvertPass04GregReload(lp3, linked_stage0.profile);
    ASSERT_FALSE(lp4.rpo_blocks.empty()) << "Empty linked Pass04 output for " << tc.test_name;
    const RtlControlFlowGraph lp5 = InvertPass05LregLocalAlloc(lp4, linked_stage0.profile);
    ASSERT_FALSE(lp5.rpo_blocks.empty()) << "Empty linked Pass05 output for " << tc.test_name;
    const RightInverseResult linked_suffix_res =
        verifier->VerifySuffixRightInverse(RtlPassKind::kSched, linked_stage0, lp5);
    EXPECT_TRUE(linked_suffix_res.matches)
        << "Stage 0..5 linked-ROM suffix mismatch for " << tc.test_name << ":\n"
        << linked_suffix_res.diff_detail;
  }
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass05LregLocalAllocRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
