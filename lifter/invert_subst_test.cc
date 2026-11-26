#include "lifter/invert_subst.h"

#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

// Production-only provider that delegates `InvertSubst` solely to
// `lifter/invert_subst` and leaves all other subfunction methods as default
// no-op stubs.
class SubstOnlyInverseProvider final : public SubfunctionInverseProvider {
 public:
  SubstInverseSpec InvertSubst(std::string_view combined_rtx_x) const override {
    return ::rom_nom_nom::lifter::InvertSubst(combined_rtx_x);
  }
};

// Corrupted provider that alters the constant in `target_expr` so `subst`
// evaluates to a semantically different RTL expression.
class CorruptedSubstInverseProvider final : public SubfunctionInverseProvider {
 public:
  SubstInverseSpec InvertSubst(std::string_view combined_rtx_x) const override {
    (void)combined_rtx_x;
    return SubstInverseSpec{
        .target_expr = "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 68) (const_int 999)))",
        .from_reg = "(reg:SI 68)",
        .to_expr = "(reg:SI 4 a0)",
    };
  }
};

// Provider returning a `target_expr` that omits `from_reg` in the RHS.
class MissingFromRegSubstInverseProvider final : public SubfunctionInverseProvider {
 public:
  SubstInverseSpec InvertSubst(std::string_view combined_rtx_x) const override {
    (void)combined_rtx_x;
    return SubstInverseSpec{
        .target_expr = "(set (reg/i:SI 2 v0) (reg:SI 4 a0))",
        .from_reg = "(reg:SI 68)",
        .to_expr = "(reg:SI 4 a0)",
    };
  }
};

// Empty/no-op provider that returns empty strings for `InvertSubst`.
class EmptySubstInverseProvider final : public SubfunctionInverseProvider {};

class InvertSubstTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
  }

  static void TearDownTestSuite() {
    delete hooks;
    hooks = nullptr;
    delete ctx;
    ctx = nullptr;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
};

GccContext* InvertSubstTest::ctx = nullptr;
FunctionHooks* InvertSubstTest::hooks = nullptr;

// =============================================================================
// Positive Synthetic Unit Tests
// =============================================================================

TEST_F(InvertSubstTest, DecomposesMemoryAddressingExpressionsAndVerifiesRightInverse) {
  SubstOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  const std::vector<std::string> mem_cases = {
      "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
      "(set (reg:SI 2 v0) (mem/s:SI (plus:SI (reg:SI 4 a0) (const_int 8))))",
      "(set (reg:SI 2 v0) (mem/v:SI (reg:SI 4 a0)))",
      "(set (reg:SI 2 v0) (mem:SI (lo_sum:SI (reg:SI 2 v0) (symbol_ref:SI (\"g_var\")))))",
      "(set (reg:DF 32 $f0) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 24))))",
      "(set (reg:SI 2 v0) (zero_extend:SI (mem:QI (plus:SI (reg:SI 4 a0) (const_int 3)))))",
      "(set (reg:SI 2 v0) (zero_extend:SI (mem:HI (plus:SI (reg:SI 4 a0) (const_int 2)))))",
      "(set (reg:SI 2 v0) (sign_extend:SI (mem:QI (plus:SI (reg:SI 4 a0) (const_int 1)))))",
      "(set (reg:SI 2 v0) (sign_extend:SI (mem:HI (reg:SI 4 a0))))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 4 a0))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (const_int 0))",
      "(mem:SI (plus:SI (reg:SI 29 sp) (const_int 16)))",
  };

  for (const std::string& expr : mem_cases) {
    const SubstPreImage spec = InvertSubst(expr);
    EXPECT_NE(spec.target_expr, expr) << "Expected structural decomposition for: " << expr;
    EXPECT_TRUE(absl::StrContains(spec.target_expr, spec.from_reg)) << expr;
    const RightInverseResult res = verifier.VerifySubstRightInverse(expr);
    EXPECT_TRUE(res.matches) << "Expr: " << expr << "\nDiff: " << res.diff_detail;
  }
}

TEST_F(InvertSubstTest, DecomposesArithmeticShiftAndLogicalTreesAndVerifiesRightInverse) {
  SubstOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  const std::vector<std::string> exprs = {
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 12)))",
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (minus:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (div:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (udiv:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (umod:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (ashift:SI (reg:SI 4 a0) (const_int 4)))",
      "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 4 a0) (const_int 5)))",
      "(set (reg:SI 2 v0) (lshiftrt:SI (reg:SI 4 a0) (const_int 3)))",
      "(set (reg:SI 2 v0) (and:SI (reg:SI 4 a0) (const_int 15)))",
      "(set (reg:SI 2 v0) (and:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (ior:SI (reg:SI 4 a0) (const_int 7)))",
      "(set (reg:SI 2 v0) (ior:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (xor:SI (reg:SI 4 a0) (const_int 3)))",
      "(set (reg:SI 2 v0) (xor:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (neg:SI (reg:SI 4 a0)))",
      "(set (reg:SI 2 v0) (not:SI (reg:SI 4 a0)))",
      "(set (reg:SI 2 v0) (lo_sum:SI (reg:SI 2 v0) (symbol_ref:SI (\"g_table\"))))",
      // Compound nested arithmetic/shift/logical trees where a compound child
      // subexpression is extracted into `to_expr`:
      "(set (reg:SI 2 v0) (ior:SI (ashift:SI (reg:SI 4 a0) (const_int 8)) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (xor:SI (and:SI (reg:SI 4 a0) (const_int 15)) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (plus:SI (mult:SI (reg:SI 4 a0) (const_int 4)) (reg:SI 5 a1)))",
      "(plus:SI (reg:SI 4 a0) (const_int 12))",
      "(ior:SI (ashift:SI (reg:SI 4 a0) (const_int 8)) (reg:SI 5 a1))",
  };

  for (const std::string& expr : exprs) {
    const SubstPreImage spec = InvertSubst(expr);
    EXPECT_NE(spec.target_expr, expr) << "Expected decomposition for: " << expr;
    const RightInverseResult res = verifier.VerifySubstRightInverse(expr);
    EXPECT_TRUE(res.matches) << "Expr: " << expr << "\nDiff: " << res.diff_detail;
  }
}

TEST_F(InvertSubstTest, DecomposesComparisonsAndConditionalBranchesAndVerifiesRightInverse) {
  SubstOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  const std::vector<std::string> comparison_cases = {
      "(set (reg:SI 2 v0) (eq:SI (reg:SI 4 a0) (const_int 0)))",
      "(set (reg:SI 2 v0) (ne:SI (reg:SI 4 a0) (const_int 0)))",
      "(set (reg:SI 2 v0) (lt:SI (reg:SI 4 a0) (const_int 0)))",
      "(set (reg:SI 2 v0) (le:SI (reg:SI 4 a0) (const_int 0)))",
      "(set (reg:SI 2 v0) (gt:SI (reg:SI 4 a0) (const_int 0)))",
      "(set (reg:SI 2 v0) (ge:SI (reg:SI 4 a0) (const_int 0)))",
      "(set (reg:SI 2 v0) (eq:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (ne:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (lt:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (ge:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (ltu:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (geu:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 2 v0) (le:SI (reg:SI 4 a0) (const_int 9)))",
      "(set (reg:SI 2 v0) (gt:SI (reg:SI 4 a0) (const_int 9)))",
      "(set (reg:SI 2 v0) (leu:SI (reg:SI 4 a0) (const_int 9)))",
      "(set (reg:SI 2 v0) (gtu:SI (reg:SI 4 a0) (const_int 9)))",
      "(set (reg:SI 2 v0) (lt:SI (reg:SI 4 a0) (const_int 10)))",
      "(set (reg:CC_FP 67 $fcr31) (lt:CC_FP (reg/v:SF 72) (reg:SF 73)))",
      "(set (reg:CC_REV_FP 67 $fcr31) (ne:CC_REV_FP (reg/v:SF 78) (reg:SF 80)))",
      "(set (pc) (if_then_else (eq (reg:SI 4 a0) (const_int 0)) (label_ref 0) (pc)))",
      "(set (pc) (if_then_else (ne (reg:SI 4 a0) (reg:SI 5 a1)) (label_ref 0) (pc)))",
      "(set (pc) (if_then_else (ge (reg:SI 4 a0) (const_int 0)) (label_ref 0) (pc)))",
      "(set (pc) (if_then_else (ne (reg:CC_FP 67 $fcr31) (const_int 0)) (label_ref 0) (pc)))",
      "(ge:SI (reg:SI 4 a0) (const_int 0))",
  };

  for (const std::string& expr : comparison_cases) {
    const SubstPreImage spec = InvertSubst(expr);
    EXPECT_NE(spec.target_expr, expr) << "Expected decomposition for: " << expr;
    const RightInverseResult res = verifier.VerifySubstRightInverse(expr);
    EXPECT_TRUE(res.matches) << "Expr: " << expr << "\nDiff: " << res.diff_detail;
  }
}

TEST_F(InvertSubstTest, DecomposesSubregAndExtensionTreesAndVerifiesRightInverse) {
  SubstOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  const std::vector<std::string> subreg_ext_cases = {
      "(set (reg:SI 2 v0) (zero_extend:SI (subreg:QI (reg:SI 4 a0) 0)))",
      "(set (reg:SI 2 v0) (zero_extend:SI (subreg:HI (reg:SI 4 a0) 0)))",
      "(set (reg:SI 2 v0) (sign_extend:SI (subreg:QI (reg:SI 4 a0) 0)))",
      "(set (reg:SI 2 v0) (sign_extend:SI (subreg:HI (reg:SI 4 a0) 0)))",
      "(set (reg:SF 32 $f0) (subreg:SF (reg:SI 72) 0))",
      "(set (reg:SI 2 v0) (subreg:SI (reg:SF 72) 0))",
      "(set (reg:SI 72) (subreg:SI (reg:DI 73) 1))",
      "(set (reg:DF 32 $f0) (float_extend:DF (reg:SF 34 $f2)))",
      "(set (reg:SI 2 v0) (truncate:SI (reg:DI 4 a0)))",
      "(set (reg:SI 2 v0) (fix:SI (reg:DF 32 $f0)))",
      "(set (reg:DF 32 $f0) (float:DF (reg:SI 4 a0)))",
  };

  for (const std::string& expr : subreg_ext_cases) {
    const SubstPreImage spec = InvertSubst(expr);
    EXPECT_NE(spec.target_expr, expr) << "Expected decomposition for: " << expr;
    const RightInverseResult res = verifier.VerifySubstRightInverse(expr);
    EXPECT_TRUE(res.matches) << "Expr: " << expr << "\nDiff: " << res.diff_detail;
  }
}

TEST_F(InvertSubstTest, PicksFreshPseudoRegisterAboveExistingPseudosAndVerifiesRightInverse) {
  SubstOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  const std::string expr = "(set (reg:SI 95) (plus:SI (reg:SI 110) (const_int 4)))";
  absl::StatusOr<RtlExpr> parsed = RtlExpr::Parse(expr);
  ASSERT_TRUE(parsed.ok());
  absl::StatusOr<SubstExprPreImage> pre = InvertSubstExpr(*parsed);
  ASSERT_TRUE(pre.ok());
  ASSERT_TRUE(pre->from_reg.IsReg());
  EXPECT_GT(pre->from_reg.RegNo(), 110);

  const RightInverseResult res = verifier.VerifySubstRightInverse(expr);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertSubstTest, VerifiesCfgSubfunctionBoundaryRightInverseOnSyntheticSnapshot) {
  SubstOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  RtlControlFlowGraph flow_cfg;
  flow_cfg.pass = RtlPassKind::kFlow;
  flow_cfg.sideband.max_regno = 80;
  flow_cfg.rpo_blocks.resize(1);
  flow_cfg.rpo_blocks[0].id = 0;
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 8))))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 5)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (ge:SI (reg:SI 73) (const_int 0)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kSubst, flow_cfg);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);

  absl::StatusOr<RtlControlFlowGraph> pre_cfg = InvertSubstCfg(snap.post_state);
  ASSERT_TRUE(pre_cfg.ok()) << pre_cfg.status();
  EXPECT_NE(pre_cfg->rpo_blocks[0].instructions[2], snap.post_state.rpo_blocks[0].instructions[2]);

  const SubfunctionBoundarySnapshot replay =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kSubst, *pre_cfg);
  EXPECT_EQ(replay.post_state.FormatSummary(), snap.post_state.FormatSummary());

  const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// =============================================================================
// Negative Unit Tests
// =============================================================================

TEST_F(InvertSubstTest, RejectsMalformedAndUnrecognizedRtlInputs) {
  EXPECT_EQ(InvertSubst(""), SubstPreImage{});
  EXPECT_EQ(InvertSubst("   "), SubstPreImage{});
  EXPECT_EQ(InvertSubst("not_an_sexpr"), SubstPreImage{});
  EXPECT_EQ(InvertSubst("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))"),
            SubstPreImage{});
  EXPECT_EQ(InvertSubst("(set (reg:SI 2 v0) (bogus_op:SI (reg:SI 4 a0)))"), SubstPreImage{});
  EXPECT_EQ(InvertSubst("(set (reg:BOGUS 2 v0) (const_int 0))"), SubstPreImage{});
  EXPECT_EQ(InvertSubst("(set (reg:SI -4) (const_int 0))"), SubstPreImage{});
  EXPECT_EQ(InvertSubst("(set (reg:SI 2 v0) (subreg:SI (reg:DI 4 a0)))"), SubstPreImage{});
  EXPECT_EQ(InvertSubst("(set (reg:SI 2 v0))"), SubstPreImage{});

  RtlExpr unknown_expr;
  unknown_expr.op = RtlOp::kUnknown;
  EXPECT_FALSE(InvertSubstExpr(unknown_expr).ok());

  RtlControlFlowGraph empty_cfg;
  EXPECT_FALSE(InvertSubstCfg(empty_cfg).ok());
  EXPECT_TRUE(InvertSubst(empty_cfg).rpo_blocks.empty());

  RtlControlFlowGraph bad_max_regno_cfg;
  bad_max_regno_cfg.sideband.max_regno = -1;
  bad_max_regno_cfg.rpo_blocks.push_back(RtlBasicBlock{.id = 0});
  EXPECT_FALSE(InvertSubstCfg(bad_max_regno_cfg).ok());

  RtlControlFlowGraph bad_succ_cfg;
  RtlBasicBlock blk;
  blk.id = 0;
  blk.successors = {3};
  bad_succ_cfg.rpo_blocks.push_back(blk);
  EXPECT_FALSE(InvertSubstCfg(bad_succ_cfg).ok());

  RtlControlFlowGraph bad_insn_cfg;
  RtlBasicBlock bad_blk;
  bad_blk.id = 0;
  bad_blk.instructions = {"(set (reg:SI 2 v0) (bogus_op:SI (reg:SI 4 a0)))"};
  bad_insn_cfg.rpo_blocks.push_back(bad_blk);
  EXPECT_FALSE(InvertSubstCfg(bad_insn_cfg).ok());
}

TEST_F(InvertSubstTest, VerifierRejectsEmptyCorruptedAndMissingFromRegProviders) {
  EmptySubstInverseProvider empty_provider;
  RightInverseVerifier empty_verifier(ctx, hooks, &empty_provider);
  EXPECT_FALSE(
      empty_verifier
          .VerifySubstRightInverse("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))")
          .matches);

  CorruptedSubstInverseProvider corrupted_provider;
  RightInverseVerifier corrupted_verifier(ctx, hooks, &corrupted_provider);
  EXPECT_FALSE(
      corrupted_verifier
          .VerifySubstRightInverse("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))")
          .matches);

  MissingFromRegSubstInverseProvider missing_from_provider;
  RightInverseVerifier missing_from_verifier(ctx, hooks, &missing_from_provider);

  RtlControlFlowGraph flow_cfg;
  flow_cfg.pass = RtlPassKind::kFlow;
  flow_cfg.rpo_blocks.resize(1);
  flow_cfg.rpo_blocks[0].id = 0;
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kSubst, flow_cfg);

  EXPECT_FALSE(empty_verifier.VerifySubfunctionBoundaryRightInverse(snap).matches);
  EXPECT_FALSE(corrupted_verifier.VerifySubfunctionBoundaryRightInverse(snap).matches);
  EXPECT_FALSE(missing_from_verifier.VerifySubfunctionBoundaryRightInverse(snap).matches);
}

// =============================================================================
// 2,480-Case Corpus Parameterized Right-Inverse Test
// =============================================================================

class InvertSubstRoundTripTest : public InvertSubstTest,
                                 public ::testing::WithParamInterface<RoundTripTestCase> {};

TEST_P(InvertSubstRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const FunctionCompilationTrace& trace = **cached_trace;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

  SubstOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  int verified_count = 0;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kSubst) {
      continue;
    }
    const RightInverseResult boundary_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(boundary_res.matches)
        << "Snapshot verification failed for " << tc.test_name << ": " << boundary_res.diff_detail;
    ++verified_count;
  }
  EXPECT_GT(verified_count, 0) << "No kSubst snapshot captured for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(AllRoundTripCases, InvertSubstRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
