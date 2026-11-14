#include "lifter/pass02_jump2_cross_jump.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_do_cross_jump.h"
#include "lifter/invert_fill_eager_delay_slots.h"
#include "lifter/invert_fill_simple_delay_slots.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"
#include "lifter/invert_gas_mips_ip.h"
#include "lifter/invert_make_return_insns.h"
#include "lifter/invert_relax_delay_slots.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::PrologueEpilogueFrameSpec;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionPass02InverseProvider final : public SubfunctionInverseProvider {
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
};

RtlBasicBlock MakeBlock(uint32_t id, bool has_label, std::vector<std::string> instructions,
                        std::vector<size_t> successors) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.has_label = has_label;
  blk.instructions = std::move(instructions);
  blk.successors = std::move(successors);
  blk.SyncRtlInsnsFromInstructions();
  return blk;
}

class Pass02Jump2CrossJumpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RtlControlFlowGraph dummy;
    hooks_.ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  GccContext ctx_;
  FunctionHooks hooks_{&ctx_};
  ProductionPass02InverseProvider provider_;
  RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_F(Pass02Jump2CrossJumpTest, SyntheticPositiveCrossJumpedJoinBeforeReturn) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 6 a2) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 10))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 6 a2) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 20))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph lifted = InvertPass02Jump2CrossJump(cfg);
  EXPECT_EQ(lifted.pass, RtlPassKind::kSched2);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[3].instructions.size(), 3u);

  const RightInverseResult res = verifier_.VerifyJump2CrossJumpStageRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(Pass02Jump2CrossJumpTest, SyntheticPositiveMultiPredecessorCrossJumpedJoin) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 7 a3) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 11))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(2, true,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                 "(label_ref L1) (pc)))"},
                {4, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg:SI 7 a3) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 22))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 7 a3) (const_int 3))",
                    "(set (reg:SI 3 v1) (const_int 33))",
                },
                {5}),
      MakeBlock(5, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2)))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph lifted = InvertPass02Jump2CrossJump(cfg);
  EXPECT_EQ(lifted.pass, RtlPassKind::kSched2);
  ASSERT_EQ(lifted.rpo_blocks.size(), 6u);

  const RightInverseResult res = verifier_.VerifyJump2CrossJumpStageRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(Pass02Jump2CrossJumpTest, SyntheticPositiveChainedStage0Stage1Stage2Composition) {
  // Construct a `.12.dbr` CFG with cross-jumped blocks and delay slots,
  // assemble to machine words, chain `InvertFinalAndGasFunction` (Stage 0) ->
  // `InvertPass01DbrSchedule` (Stage 1) -> `InvertPass02Jump2CrossJump` (Stage 2),
  // and replay forward through `kDoCrossJump` -> `RunDbrSchedule` -> `RunFinal` ->
  // `RunGasMdAssemble`.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 6 a2) (const_int 1)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (label_ref L1))) (insn (set (reg:SI 3 v1) "
                 "(const_int 10)))])"},
                {3}),
      MakeBlock(2, true,
                {"(set (reg:SI 6 a2) (const_int 2))", "(set (reg:SI 3 v1) (const_int 20))"}, {3}),
      MakeBlock(3, true,
                {"(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                 "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                 "(insn (set (reg:SI 2 v0) (reg:SI 3 v1)))])"},
                {}),
  };

  ctx_.MaterializeFromCfg(dbr_cfg);
  const std::string orig_asm = hooks_.RunFinal();
  const std::vector<uint32_t> orig_words = hooks_.RunGasMdAssemble(orig_asm);
  ASSERT_FALSE(orig_words.empty());

  const RtlControlFlowGraph lifted_dbr = InvertFinalAndGasFunction(orig_words);
  ASSERT_FALSE(lifted_dbr.rpo_blocks.empty());
  EXPECT_EQ(lifted_dbr.pass, RtlPassKind::kDbr);

  const RtlControlFlowGraph lifted_jump2 = InvertPass01DbrSchedule(lifted_dbr);
  ASSERT_FALSE(lifted_jump2.rpo_blocks.empty());
  EXPECT_EQ(lifted_jump2.pass, RtlPassKind::kJump2);

  const RtlControlFlowGraph lifted_sched2 = InvertPass02Jump2CrossJump(lifted_jump2);
  ASSERT_FALSE(lifted_sched2.rpo_blocks.empty());
  EXPECT_EQ(lifted_sched2.pass, RtlPassKind::kSched2);

  const RightInverseResult jump2_res =
      verifier_.VerifyJump2CrossJumpStageRightInverse(lifted_jump2);
  EXPECT_TRUE(jump2_res.matches) << jump2_res.diff_detail;

  const RtlControlFlowGraph replayed_jump2 =
      hooks_
          .ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, lifted_sched2, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"})
          .post_state;
  EXPECT_TRUE(replayed_jump2.CompareAgainst(lifted_jump2).MatchesExactly());

  ctx_.MaterializeFromCfg(replayed_jump2);
  hooks_.RunDbrSchedule();
  const std::string replayed_asm = hooks_.RunFinal();
  const std::vector<uint32_t> replayed_words = hooks_.RunGasMdAssemble(replayed_asm);
  EXPECT_EQ(replayed_words, orig_words);
}

TEST_F(Pass02Jump2CrossJumpTest, SyntheticNegativeMalformedCfgAndCorruptedInverse) {
  // 1. Empty CFG.
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertPass02Jump2CrossJump(empty_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(empty_cfg).matches);

  // 2. Single block with empty instructions.
  RtlControlFlowGraph empty_blk_cfg;
  empty_blk_cfg.rpo_blocks.push_back(MakeBlock(0, false, {}, {}));
  EXPECT_TRUE(InvertPass02Jump2CrossJump(empty_blk_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(empty_blk_cfg).matches);

  // 3. Out-of-bounds successor edge.
  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.rpo_blocks.push_back(
      MakeBlock(0, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {4}));
  EXPECT_TRUE(InvertPass02Jump2CrossJump(bad_succ_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(bad_succ_cfg).matches);

  // 4. Malformed S-expression (and synthetic bare `(return)`).
  RtlControlFlowGraph bad_sexpr_cfg;
  bad_sexpr_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(set (reg:SI 2 v0)"}, {}));
  EXPECT_TRUE(InvertPass02Jump2CrossJump(bad_sexpr_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(bad_sexpr_cfg).matches);

  RtlControlFlowGraph bare_ret_cfg;
  bare_ret_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(return)"}, {}));
  EXPECT_TRUE(InvertPass02Jump2CrossJump(bare_ret_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(bare_ret_cfg).matches);

  // 5. Corrupted inverse provider rejected by `VerifyJump2CrossJumpStageRightInverse`.
  class BrokenJump2Provider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
      return Corrupt(jump2_cfg_x);
    }
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                          const CompilationProfile& /*profile*/) const override {
      return Corrupt(jump2_cfg_x);
    }

   private:
    static RtlControlFlowGraph Corrupt(const RtlControlFlowGraph& jump2_cfg_x) {
      RtlControlFlowGraph bad = jump2_cfg_x;
      bad.pass = RtlPassKind::kSched2;
      if (!bad.rpo_blocks.empty()) {
        bad.rpo_blocks[0].instructions = {
            "(set (reg/i:SI 2 v0) (const_int 999))",
            "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
        };
        bad.rpo_blocks[0].successors.clear();
        bad.rpo_blocks.resize(1);
        bad.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      }
      return bad;
    }
  };

  RtlControlFlowGraph valid_cfg;
  valid_cfg.pass = RtlPassKind::kJump2;
  valid_cfg.rpo_blocks = {
      MakeBlock(
          0, false,
          {"(set (reg/i:SI 2 v0) (const_int 42))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
          {}),
  };
  BrokenJump2Provider broken_provider;
  RightInverseVerifier broken_verifier(&ctx_, &hooks_, &broken_provider);
  EXPECT_FALSE(broken_verifier.VerifyJump2CrossJumpStageRightInverse(valid_cfg).matches);
}

class Pass02Jump2CrossJumpRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionPass02InverseProvider();
    verifier = new RightInverseVerifier(ctx, hooks, provider);

    RtlControlFlowGraph dummy;
    hooks->ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
  static ProductionPass02InverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* Pass02Jump2CrossJumpRoundTripTest::ctx = nullptr;
FunctionHooks* Pass02Jump2CrossJumpRoundTripTest::hooks = nullptr;
ProductionPass02InverseProvider* Pass02Jump2CrossJumpRoundTripTest::provider = nullptr;
RightInverseVerifier* Pass02Jump2CrossJumpRoundTripTest::verifier = nullptr;

TEST_P(Pass02Jump2CrossJumpRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  auto jump2_it = trace.pass_snapshots.find(RtlPassKind::kJump2);
  ASSERT_NE(jump2_it, trace.pass_snapshots.end()) << "Missing .11.jump2 for " << tc.test_name;
  const RtlControlFlowGraph& post_jump2 = jump2_it->second;

  // 1. Verify Stage 2 (`SubfunctionId::kDoCrossJump`) subfunction boundary
  // snapshot via `ProductionPass02InverseProvider`.
  bool found_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kDoCrossJump) {
      continue;
    }
    found_snap = true;
    const RightInverseResult sub_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(sub_res.matches) << "Stage 2 subfunction boundary mismatch for " << tc.test_name
                                 << ": " << sub_res.diff_detail << "\nexpected:\n"
                                 << sub_res.expected_summary << "\nactual:\n"
                                 << sub_res.actual_summary;
  }
  EXPECT_TRUE(found_snap) << "Missing kDoCrossJump snapshot for " << tc.test_name;

  // 2. Whole-stage `InvertPass02Jump2CrossJump` (`provider->InvertDoCrossJump`)
  // composition right-inverse on `.11.jump2`.
  const RtlControlFlowGraph pre_jump2 = InvertPass02Jump2CrossJump(post_jump2);
  ASSERT_FALSE(pre_jump2.rpo_blocks.empty())
      << "Empty InvertPass02Jump2CrossJump for " << tc.test_name;
  EXPECT_EQ(pre_jump2.pass, RtlPassKind::kSched2);

  const RightInverseResult stage2_res = verifier->VerifyJump2CrossJumpStageRightInverse(post_jump2);
  EXPECT_TRUE(stage2_res.matches) << "Stage 2 (.11.jump2) right-inverse mismatch for "
                                  << tc.test_name << ":\n"
                                  << stage2_res.diff_detail << "\nexpected (.11.jump2):\n"
                                  << stage2_res.expected_summary << "\nactual (.11.jump2):\n"
                                  << stage2_res.actual_summary;

  // 3. Full fresh-context `VerifySuffixRightInverse` ($F_{0..2}(G_{2..0}(x_0)) = x_0$)
  //    across all 2,480 corpus cases.
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << "Test case: " << tc.test_name;

  const RightInverseResult suffix_res =
      verifier->VerifySuffixRightInverse(RtlPassKind::kSched2, trace.stage0_input);
  EXPECT_TRUE(suffix_res.matches) << "Stage 0..2 suffix right-inverse mismatch for " << tc.test_name
                                  << ":\n"
                                  << suffix_res.diff_detail << "\nexpected:\n"
                                  << suffix_res.expected_summary << "\nactual:\n"
                                  << suffix_res.actual_summary;
}

TEST_F(Pass02Jump2CrossJumpTest, SyntheticPositiveAndNegativeSuffixRightInverse) {
  constexpr std::string_view kCrossJumpPoolSrc = R"(
extern int sink_double(double, int);
int branch_cross_jump(int sel, double x) {
  if (sel == 0) {
    return sink_double(x * 3.141592653589793, 42) + 7;
  }
  return sink_double(x + 2.718281828459045, 42) + 7;
}
)";
  const auto trace = hooks_.TraceForwardCompilation(kCrossJumpPoolSrc, "branch_cross_jump",
                                                    {"-O2", "-Wa,-g"}, "pass02_suffix_test");
  const RightInverseResult ok_res =
      verifier_.VerifySuffixRightInverse(RtlPassKind::kSched2, trace.stage0_input);
  EXPECT_TRUE(ok_res.matches) << ok_res.diff_detail;

  // Negative check: corrupting a machine word in `stage0_input` must fail
  // `VerifySuffixRightInverse(RtlPassKind::kSched2, ...)`.
  Stage0FunctionInput corrupted = trace.stage0_input;
  ASSERT_FALSE(corrupted.machine_words.empty());
  corrupted.machine_words.back() ^= 0x00000004u;
  EXPECT_FALSE(verifier_.VerifySuffixRightInverse(RtlPassKind::kSched2, corrupted).matches);

  // Full Stage 0..2 (`RtlPassKind::kSched2`) linked-ROM + multi-jump-table +
  // reversed `.rodata` jump-table order + float/double constant-pool + `$gp`
  // verification (`F_{0..2}(G_{2..0}(x_0)) = x_0`)!
  constexpr std::string_view kLinkedMultiSwitchSched2Src = R"(
struct StateBox {
  int a;
  int b;
  int c;
};
extern struct StateBox g_box;
int g_small_val;
extern int mix_fn(int, double);

int dual_switch_crossjump(int n, const int* s1, const int* s2, double v) {
  int res = g_box.c + g_small_val;
  int i;
  for (i = 0; i < n; ++i) {
    switch (s1[i]) {
      case 0: res = mix_fn(res + 1, v * 3.141592653589793) + 9; break;
      case 1: res = mix_fn(res - 2, v + 2.718281828459045) + 9; break;
      case 2: res += g_box.b; break;
      case 3: res -= g_small_val; break;
      case 4: res ^= 0x12345678; break;
      default: res += 3; break;
    }
    switch (s2[i]) {
      case 0: res += 11; break;
      case 1: res -= 22; break;
      case 2: res ^= 33; break;
      case 3: res += g_box.a; break;
      case 4: res -= 44; break;
      default: break;
    }
  }
  g_small_val = res;
  return res;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips1", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"}}) {
    const auto ms_trace = hooks_.TraceForwardCompilation(
        kLinkedMultiSwitchSched2Src, "dual_switch_crossjump", flags, "pass02_linked_multi_switch");
    ASSERT_GE(ms_trace.stage0_input.jump_tables.size(), 2u);
    bool saw_gp_small_val = false;
    for (const Stage0Relocation& rel : ms_trace.stage0_input.relocations) {
      if (rel.symbol_name == "g_small_val" && rel.kind == Stage0RelocKind::kMipsGpRel16) {
        saw_gp_small_val = true;
        break;
      }
    }
    ASSERT_TRUE(saw_gp_small_val);

    // 1. Relocatable `.o` `VerifySuffixRightInverse(RtlPassKind::kSched2, ...)`:
    const RightInverseResult reloc_res =
        verifier_.VerifySuffixRightInverse(RtlPassKind::kSched2, ms_trace.stage0_input);
    EXPECT_TRUE(reloc_res.matches) << reloc_res.diff_detail;

    // 2. Simulate linked N64 ROM (`base_vram != 0`, empty relocations, reversed `.rodata` JTs):
    Stage0FunctionInput linked;
    linked.machine_words = ms_trace.stage0_input.machine_words;
    linked.base_vram = 0x80020000u;
    linked.gp_vram = 0x80060000u;
    linked.profile = ms_trace.stage0_input.profile;
    linked.profile.gp_symbol_sizes.clear();
    linked.jump_tables = ms_trace.stage0_input.jump_tables;
    linked.constant_pool = ms_trace.stage0_input.constant_pool;

    std::vector<std::pair<std::string, uint32_t>> sym_vrams;
    auto find_sym = [&](const std::string& s, uint32_t* out) -> bool {
      for (const auto& [k, val] : sym_vrams) {
        if (k == s) {
          *out = val;
          return true;
        }
      }
      return false;
    };

    uint32_t next_rodata = 0x80030000u;
    uint32_t next_func = 0x80040000u;
    uint32_t next_gp_data = 0x80059000u;
    uint32_t next_data = 0x80031000u;
    for (Stage0JumpTable& jt : linked.jump_tables) {
      jt.table_vram = next_rodata;
      sym_vrams.emplace_back(jt.label_name, jt.table_vram);
      next_rodata += static_cast<uint32_t>(jt.target_word_indices.size() * 4u + 16u);
    }
    for (Stage0ConstantPoolEntry& cp : linked.constant_pool) {
      if (cp.section_name == ".lit4" || cp.section_name == ".lit8" || cp.section_name == ".sdata") {
        cp.entry_vram = next_gp_data;
        next_gp_data += 16u;
      } else {
        cp.entry_vram = next_rodata;
        next_rodata += 16u;
      }
      sym_vrams.emplace_back(cp.label_name, cp.entry_vram);
    }
    for (const Stage0Relocation& rel : ms_trace.stage0_input.relocations) {
      uint32_t existing = 0;
      if (find_sym(rel.symbol_name, &existing)) {
        continue;
      }
      int64_t max_add = 0;
      bool any_gp = false;
      bool any_call = false;
      for (const Stage0Relocation& r2 : ms_trace.stage0_input.relocations) {
        if (r2.symbol_name == rel.symbol_name) {
          if (r2.addend > max_add) {
            max_add = r2.addend;
          }
          if (r2.kind == Stage0RelocKind::kMipsGpRel16 ||
              r2.kind == Stage0RelocKind::kMipsLiteral) {
            any_gp = true;
          }
          if (r2.kind == Stage0RelocKind::kMips26) {
            any_call = true;
          }
        }
      }
      const uint32_t span_bytes = static_cast<uint32_t>((max_add > 0 ? max_add : 0) + 16);
      uint32_t assigned = 0;
      if (any_call) {
        assigned = next_func;
        next_func += 64u;
      } else if (any_gp) {
        assigned = next_gp_data;
        next_gp_data += (span_bytes + 15u) & ~15u;
      } else {
        assigned = next_data;
        next_data += (span_bytes + 63u) & ~63u;
      }
      sym_vrams.emplace_back(rel.symbol_name, assigned);
      linked.vram_to_symbol[assigned] = rel.symbol_name;
      const auto gp_sz_it = ms_trace.stage0_input.profile.gp_symbol_sizes.find(rel.symbol_name);
      if (gp_sz_it != ms_trace.stage0_input.profile.gp_symbol_sizes.end() && gp_sz_it->second > 0) {
        linked.vram_to_symbol_size[assigned] = gp_sz_it->second;
      } else if (max_add > 0) {
        linked.vram_to_symbol_size[assigned] = span_bytes;
      }
    }
    std::vector<bool> relocated_word(linked.machine_words.size(), false);
    for (const Stage0Relocation& rel : ms_trace.stage0_input.relocations) {
      uint32_t sym_vram = 0;
      if (rel.word_index >= linked.machine_words.size() || !find_sym(rel.symbol_name, &sym_vram)) {
        continue;
      }
      relocated_word[rel.word_index] = true;
      const uint32_t full_addr = static_cast<uint32_t>(static_cast<int64_t>(sym_vram) + rel.addend);
      if (rel.kind == Stage0RelocKind::kMips26) {
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xfc000000u) | ((full_addr >> 2) & 0x03ffffffu);
      } else if (rel.kind == Stage0RelocKind::kMipsHi16) {
        const uint32_t hi = ((full_addr + 0x8000u) >> 16) & 0xffffu;
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xffff0000u) | hi;
      } else if (rel.kind == Stage0RelocKind::kMipsLo16) {
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xffff0000u) | (full_addr & 0xffffu);
      } else if (rel.kind == Stage0RelocKind::kMipsGpRel16 ||
                 rel.kind == Stage0RelocKind::kMipsLiteral) {
        const int32_t delta = static_cast<int32_t>(full_addr - linked.gp_vram);
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xffff0000u) |
            (static_cast<uint32_t>(delta) & 0xffffu);
      }
    }
    for (size_t idx = 0; idx < linked.machine_words.size();) {
      const uint32_t w = linked.machine_words[idx];
      if (!relocated_word[idx] && ((w >> 26) & 0x3fu) == 0x02u) {
        const uint32_t local_idx = w & 0x03ffffffu;
        const uint32_t target_vram = linked.base_vram + local_idx * 4u;
        linked.machine_words[idx] = 0x08000000u | ((target_vram >> 2) & 0x03ffffffu);
      }
      const uint32_t op = (w >> 26) & 0x3fu;
      const uint32_t funct = w & 0x3fu;
      const bool has_delay =
          (op == 0x01u || op == 0x02u || op == 0x03u || (op >= 0x04u && op <= 0x07u) ||
           (op >= 0x14u && op <= 0x17u) || (op == 0x00u && (funct == 0x08u || funct == 0x09u)) ||
           (op == 0x11u && ((w >> 21) & 0x1fu) == 0x08u));
      idx += has_delay ? 2u : 1u;
    }

    // Reverse `.rodata` jump-table layout relative to RPO block order & strip label strings!
    std::reverse(linked.jump_tables.begin(), linked.jump_tables.end());
    for (auto& jt : linked.jump_tables) {
      jt.label_name.clear();
    }
    for (auto& cp : linked.constant_pool) {
      cp.label_name.clear();
    }

    const RightInverseResult linked_stage0_res =
        verifier_.VerifyFinalAndGasStageRightInverse(linked);
    EXPECT_TRUE(linked_stage0_res.matches) << linked_stage0_res.diff_detail;
    const RightInverseResult linked_sched2_res =
        verifier_.VerifySuffixRightInverse(RtlPassKind::kSched2, linked);
    EXPECT_TRUE(linked_sched2_res.matches) << linked_sched2_res.diff_detail;

    // Adversarial negative checks at `RtlPassKind::kSched2`!
    if (!linked.constant_pool.empty()) {
      Stage0FunctionInput bad_cp = linked;
      bad_cp.constant_pool[0].entry_vram += 32u;
      EXPECT_FALSE(verifier_.VerifySuffixRightInverse(RtlPassKind::kSched2, bad_cp).matches);
    }

    Stage0FunctionInput bad_jt = linked;
    bad_jt.jump_tables[0].table_vram += 128u;
    EXPECT_FALSE(verifier_.VerifySuffixRightInverse(RtlPassKind::kSched2, bad_jt).matches);
  }
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass02Jump2CrossJumpRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
