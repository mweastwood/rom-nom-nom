#include "lifter/invert_simplify_rtx.h"

#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
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

// Inverse provider that ONLY wires `InvertSimplifyRtx`, leaving all other 25
// subfunction methods as empty stubs (`NoOpSubfunctionInverses`).
class SimplifyRtxOnlyInverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  std::string InvertSimplifyRtx(std::string_view simplified_rtx_x) const override {
    return ::rom_nom_nom::lifter::InvertSimplifyRtx(simplified_rtx_x);
  }
};

// Corrupted inverse provider for negative right-inverse verification testing:
// returns an incorrect unsimplified expression so replaying `simplify_rtx`
// produces a mismatched expression.
class CorruptedSimplifyRtxInverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  std::string InvertSimplifyRtx(std::string_view simplified_rtx_x) const override {
    (void)simplified_rtx_x;
    return "(plus:SI (reg:SI 3 v1) (const_int 999))";
  }
};

class InvertSimplifyRtxTest : public ::testing::Test {
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

GccContext* InvertSimplifyRtxTest::ctx = nullptr;
FunctionHooks* InvertSimplifyRtxTest::hooks = nullptr;

// =============================================================================
// Positive Synthetic Right-Inverse Tests
// =============================================================================

TEST_F(InvertSimplifyRtxTest, InvertsRelationalComparisonsViaLogicalXorComplement) {
  SimplifyRtxOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  const std::vector<std::string> relational_cases = {
      "(ge:SI (reg:SI 4 a0) (const_int 1))",     "(lt:SI (reg:SI 4 a0) (const_int 1))",
      "(gt:SI (reg:SI 4 a0) (const_int 0))",     "(le:SI (reg:SI 4 a0) (const_int 0))",
      "(eq:SI (reg:SI 4 a0) (const_int 0))",     "(ne:SI (reg:SI 4 a0) (const_int 0))",
      "(ltu:SI (reg:SI 72) (reg:SI 73))",        "(geu:SI (reg:SI 72) (reg:SI 73))",
      "(leu:SI (reg:SI 72) (const_int 255))",    "(gtu:SI (reg:SI 72) (const_int 255))",
      "(eq:SI (reg:SF 32 $f0) (reg:SF 34 $f2))", "(lt:SI (reg:DF 32 $f0) (reg:DF 34 $f2))",
      "(lt:CC_FP (reg/v:SF 72) (reg:SF 73))",    "(ge:CC_REV_FP (reg/v:DF 74) (reg:DF 75))",
  };

  for (const std::string& expr_str : relational_cases) {
    const RightInverseResult res = verifier.VerifySimplifyRtxRightInverse(expr_str);
    EXPECT_TRUE(res.matches) << "Failed for " << expr_str << ": " << res.diff_detail;
  }
}

TEST_F(InvertSimplifyRtxTest, InvertsUnaryNotAndNegAndLogicalAndIorXorRules) {
  SimplifyRtxOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  // `(not:SI x)` <- `(neg:SI (plus:SI x (const_int 1)))` (`combine.c:3351`).
  EXPECT_EQ(InvertSimplifyRtx("(not:SI (reg:SI 4 a0))"),
            "(neg:SI (plus:SI (reg:SI 4 a0) (const_int 1)))");
  // `(neg:SI x)` <- `(not:SI (plus:SI x (const_int -1)))` (`combine.c:3313`).
  EXPECT_EQ(InvertSimplifyRtx("(neg:SI (reg:SI 4 a0))"),
            "(not:SI (plus:SI (reg:SI 4 a0) (const_int -1)))");
  // `(and:SI a c)` <- `(and:SI (and:SI a c) c)` (`combine.c:3143`).
  EXPECT_EQ(InvertSimplifyRtx("(and:SI (reg:SI 4 a0) (const_int 255))"),
            "(and:SI (and:SI (reg:SI 4 a0) (const_int 255)) (const_int 255))");
  // `(and:SI (not:SI a) b)` <- `(ior:SI (and:SI (not:SI a) b) (const_int 0))` (`cse.c:3698`).
  EXPECT_EQ(InvertSimplifyRtx("(and:SI (not:SI (reg:SI 4 a0)) (reg:SI 5 a1))"),
            "(ior:SI (and:SI (not:SI (reg:SI 4 a0)) (reg:SI 5 a1)) (const_int 0))");
  // `(xor:SI a b)` <- `(xor:SI (not:SI a) (not:SI b))` (`combine.c:4690`).
  EXPECT_EQ(InvertSimplifyRtx("(xor:SI (reg:SI 4 a0) (reg:SI 5 a1))"),
            "(xor:SI (not:SI (reg:SI 4 a0)) (not:SI (reg:SI 5 a1)))");

  const std::vector<std::string> logical_cases = {
      "(not:SI (reg:SI 4 a0))",
      "(neg:SI (reg:SI 4 a0))",
      "(and:SI (not:SI (reg:SI 4 a0)) (not:SI (reg:SI 5 a1)))",
      "(and:SI (not:SI (reg:SI 4 a0)) (reg:SI 5 a1))",
      "(and:SI (reg:SI 4 a0) (const_int 255))",
      "(and:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(ior:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(ior:SI (reg:SI 4 a0) (const_int 16))",
      "(xor:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(xor:SI (reg:SI 4 a0) (const_int 7))",
  };

  for (const std::string& expr_str : logical_cases) {
    const RightInverseResult res = verifier.VerifySimplifyRtxRightInverse(expr_str);
    EXPECT_TRUE(res.matches) << "Failed for " << expr_str << ": " << res.diff_detail;
  }
}

TEST_F(InvertSimplifyRtxTest, InvertsPlusMinusShiftsSymbolLoSumAndModeExtensions) {
  SimplifyRtxOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  // `(plus:SI x -1)` <- `(not:SI (neg:SI x))` (`combine.c:3318`).
  EXPECT_EQ(InvertSimplifyRtx("(plus:SI (reg:SI 4 a0) (const_int -1))"),
            "(not:SI (neg:SI (reg:SI 4 a0)))");
  // `(plus:SI x 1)` <- `(neg:SI (not:SI x))` (`combine.c:3355`).
  EXPECT_EQ(InvertSimplifyRtx("(plus:SI (reg:SI 4 a0) (const_int 1))"),
            "(neg:SI (not:SI (reg:SI 4 a0)))");
  // `(minus:SI a b)` <- `(plus:SI (minus:SI a b) (const_int 0))` (`cse.c:3649`).
  EXPECT_EQ(InvertSimplifyRtx("(minus:SI (reg:SI 4 a0) (reg:SI 5 a1))"),
            "(plus:SI (minus:SI (reg:SI 4 a0) (reg:SI 5 a1)) (const_int 0))");
  // `(ashift:SI a 4)` <- `(ashift:SI (ashift:SI a 1) 3)` (`combine.c:7960`).
  EXPECT_EQ(InvertSimplifyRtx("(ashift:SI (reg:SI 4 a0) (const_int 4))"),
            "(ashift:SI (ashift:SI (reg:SI 4 a0) (const_int 1)) (const_int 3))");
  // `(symbol_ref:SI ("ext_sym"))` <- `(plus:SI sym (const_int 0))` (`cse.c:3649`).
  EXPECT_EQ(InvertSimplifyRtx("(symbol_ref:SI (\"ext_sym\"))"),
            "(plus:SI (symbol_ref:SI (\"ext_sym\")) (const_int 0))");

  const std::vector<std::string> arithmetic_and_ext_cases = {
      "(plus:SI (reg:SI 4 a0) (const_int -1))",
      "(plus:SI (reg:SI 4 a0) (const_int 1))",
      "(plus:SI (reg:SI 4 a0) (const_int 16))",
      "(plus:SI (reg:SI 29 sp) (const_int 4))",
      "(plus:SI (reg:SI 29 sp) (const_int 16))",
      "(plus:SI (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)) (const_int 16))",
      "(minus:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(mult:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(div:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(udiv:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(mod:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(umod:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(ashift:SI (reg:SI 4 a0) (const_int 4))",
      "(ashiftrt:SI (reg:SI 4 a0) (const_int 8))",
      "(lshiftrt:SI (reg:SI 4 a0) (const_int 12))",
      "(ashift:SI (reg:SI 4 a0) (const_int 1))",
      "(ashift:SI (reg:SI 4 a0) (reg:SI 5 a1))",
      "(symbol_ref:SI (\"ext_sym\"))",
      "(const:SI (plus:SI (symbol_ref:SI (\"ext_sym\")) (const_int 12)))",
      "(high:SI (symbol_ref:SI (\"ext_sym\")))",
      "(lo_sum:SI (reg:SI 2 v0) (symbol_ref:SI (\"ext_sym\")))",
      "(zero_extend:SI (reg:QI 72))",
      "(zero_extend:SI (subreg:QI (reg:SI 72) 3))",
      "(sign_extend:SI (subreg:HI (reg:SI 72) 2))",
      "(sign_extend:SI (mem:HI (reg:SI 4 a0)))",
      "(plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))",
      "(float_extend:DF (reg:SF 32 $f0))",
      "(fix:SI (reg:DF 32 $f0))",
  };

  for (const std::string& expr_str : arithmetic_and_ext_cases) {
    const RightInverseResult res = verifier.VerifySimplifyRtxRightInverse(expr_str);
    EXPECT_TRUE(res.matches) << "Failed for " << expr_str << ": " << res.diff_detail;
  }
}

TEST_F(InvertSimplifyRtxTest, InvertsWholeCfgAndVerifiesSubfunctionBoundarySnapshot) {
  SimplifyRtxOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  RtlControlFlowGraph flow_cfg;
  flow_cfg.pass = RtlPassKind::kFlow;
  flow_cfg.rpo_blocks.resize(1);
  flow_cfg.rpo_blocks[0].id = 0;
  flow_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (ashift:SI (reg:SI 72) (const_int 3)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 74) (zero_extend:SI (subreg:QI (reg:SI 73) 3)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (xor:SI (reg:SI 74) (reg:SI 5 a1)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kSimplifyRtx, flow_cfg);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);

  absl::StatusOr<RtlControlFlowGraph> pre_cfg = InvertSimplifyRtxCfg(snap.post_state);
  ASSERT_TRUE(pre_cfg.ok()) << pre_cfg.status();
  EXPECT_EQ(pre_cfg->pass, RtlPassKind::kFlow);

  const SubfunctionBoundarySnapshot replay =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kSimplifyRtx, *pre_cfg);
  EXPECT_EQ(replay.post_state.FormatSummary(), snap.post_state.FormatSummary());

  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;
}

// =============================================================================
// Negative Tests
// =============================================================================

TEST_F(InvertSimplifyRtxTest, RejectsEmptyMalformedUnknownAndStatementInputs) {
  EXPECT_EQ(InvertSimplifyRtx(""), "");
  EXPECT_EQ(InvertSimplifyRtx("   \t\n  "), "");
  EXPECT_EQ(InvertSimplifyRtx("(plus:SI (reg:SI 4 a0)"), "");
  EXPECT_EQ(InvertSimplifyRtx("not_an_sexpr"), "");
  EXPECT_EQ(InvertSimplifyRtx("(not_a_real_opcode:SI (reg:SI 4 a0))"), "");
  EXPECT_EQ(InvertSimplifyRtx("(plus:NOTAMODE (reg:SI 4 a0) (const_int 1))"), "");
  EXPECT_EQ(InvertSimplifyRtx("(reg:SI -5)"), "");
  EXPECT_EQ(InvertSimplifyRtx("(reg 4)"), "");
  EXPECT_EQ(InvertSimplifyRtx("(subreg:SI (reg:DI 72) -1)"), "");
  EXPECT_EQ(InvertSimplifyRtx("(plus:SI (reg:SI 4 a0))"), "");
  EXPECT_EQ(InvertSimplifyRtx("(not:SI (reg:SI 4 a0) (reg:SI 5 a1))"), "");
  EXPECT_EQ(InvertSimplifyRtx("(return)"), "");
  EXPECT_EQ(InvertSimplifyRtx("(barrier)"), "");
  EXPECT_EQ(InvertSimplifyRtx("(set (reg:SI 2 v0) (reg:SI 4 a0))"), "");

  RtlExpr bad_reg = RtlExpr::MakeReg(RtlMachineMode::kSI, -3);
  EXPECT_FALSE(InvertSimplifyRtxExpr(bad_reg).ok());

  RtlExpr ret_expr = RtlExpr::MakeReturn();
  EXPECT_FALSE(InvertSimplifyRtxExpr(ret_expr).ok());
}

TEST_F(InvertSimplifyRtxTest, RejectsInvalidCfgStructuresAndSidebandEntries) {
  RtlControlFlowGraph empty_cfg;
  EXPECT_FALSE(InvertSimplifyRtxCfg(empty_cfg).ok());

  RtlControlFlowGraph bad_succ;
  bad_succ.rpo_blocks.resize(1);
  bad_succ.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_succ.rpo_blocks[0].successors = {2};
  EXPECT_FALSE(InvertSimplifyRtxCfg(bad_succ).ok());

  RtlControlFlowGraph bad_addr_vec;
  bad_addr_vec.rpo_blocks.resize(1);
  bad_addr_vec.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_addr_vec.rpo_blocks[0].addr_vec_targets = {4};
  EXPECT_FALSE(InvertSimplifyRtxCfg(bad_addr_vec).ok());

  RtlControlFlowGraph bad_insn;
  bad_insn.rpo_blocks.resize(1);
  bad_insn.rpo_blocks[0].AddPatternOnly("(unknown_op:SI (reg:SI 4 a0))");
  EXPECT_FALSE(InvertSimplifyRtxCfg(bad_insn).ok());

  RtlControlFlowGraph bad_sideband;
  bad_sideband.rpo_blocks.resize(1);
  bad_sideband.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_sideband.sideband.reg_renumber[2] = 4;
  EXPECT_FALSE(InvertSimplifyRtxCfg(bad_sideband).ok());

  RtlControlFlowGraph bad_fcr31_sideband;
  bad_fcr31_sideband.rpo_blocks.resize(1);
  bad_fcr31_sideband.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_fcr31_sideband.sideband.reg_renumber[kMipsStatusRegnum] = 2;
  EXPECT_FALSE(InvertSimplifyRtxCfg(bad_fcr31_sideband).ok());
}

TEST_F(InvertSimplifyRtxTest, CorruptedInverseIsRejectedByRightInverseVerifier) {
  CorruptedSimplifyRtxInverseProvider bad_provider;
  RightInverseVerifier bad_verifier(ctx, hooks, &bad_provider);

  const RightInverseResult expr_res =
      bad_verifier.VerifySimplifyRtxRightInverse("(ge:SI (reg:SI 4 a0) (const_int 1))");
  EXPECT_FALSE(expr_res.matches);

  RtlControlFlowGraph flow_cfg;
  flow_cfg.pass = RtlPassKind::kFlow;
  flow_cfg.rpo_blocks.resize(1);
  flow_cfg.rpo_blocks[0].id = 0;
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kSimplifyRtx, flow_cfg);
  const RightInverseResult snap_res = bad_verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_FALSE(snap_res.matches);
}

// =============================================================================
// 2,480-Case Parameterized Round-Trip Verification Suite
// =============================================================================

class InvertSimplifyRtxRoundTripTest : public InvertSimplifyRtxTest,
                                       public ::testing::WithParamInterface<RoundTripTestCase> {};

TEST_P(InvertSimplifyRtxRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const FunctionCompilationTrace& trace = **cached_trace;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

  SimplifyRtxOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  int verified_count = 0;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kSimplifyRtx) {
      continue;
    }
    const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(snap_res.matches) << "Snapshot verification failed for " << tc.test_name << ": "
                                  << snap_res.diff_detail;
    ++verified_count;
  }
  EXPECT_GT(verified_count, 0) << "No kSimplifyRtx snapshot captured for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(AllRoundTripCases, InvertSimplifyRtxRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
