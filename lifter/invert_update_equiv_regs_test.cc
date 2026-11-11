#include "lifter/invert_update_equiv_regs.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
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

// Inverse provider that ONLY wires `InvertUpdateEquivRegs`, leaving all other
// 25 subfunction methods as empty stubs (`NoOpSubfunctionInverses`).
class UpdateEquivRegsOnlyInverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertUpdateEquivRegs(x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

// Corrupted inverse provider for negative right-inverse verification testing:
// leaves a `REG_EQUIV (const_int ...)` note un-demoted and corrupts the first
// instruction so replaying `kUpdateEquivRegs` fails to match.
class CorruptedUpdateEquivRegsInverseProvider final
    : public gcc_harness::SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertUpdateEquivRegs(x);
    if (!res.ok() || res->rpo_blocks.empty() || res->rpo_blocks[0].instructions.empty()) {
      return RtlControlFlowGraph{};
    }
    res->rpo_blocks[0].instructions[0] = "(set (reg/i:SI 2 v0) (const_int 999))";
    res->rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return *res;
  }
};

class InvertUpdateEquivRegsTest : public ::testing::Test {
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

GccContext* InvertUpdateEquivRegsTest::ctx = nullptr;
FunctionHooks* InvertUpdateEquivRegsTest::hooks = nullptr;

// =============================================================================
// Positive Synthetic Right-Inverse Tests
// =============================================================================

TEST_F(InvertUpdateEquivRegsTest, DemotesConstantEquivAndStripsLoadEquivAndHalvesLiveLengths) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kSched;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (const_int 17)) (expr_list:REG_EQUAL (const_int 17) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (mem:SI (reg:SI 4 a0)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_DEAD (reg:SI 73) (nil)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_cfg.sideband.reg_n_sets[72] = 1;
  pre_cfg.sideband.reg_n_sets[73] = 1;
  pre_cfg.sideband.reg_live_length[72] = 3;
  pre_cfg.sideband.reg_live_length[73] = 2;

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, pre_cfg);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);
  EXPECT_EQ(snap.post_state.sideband.reg_live_length.at(72), 6);
  EXPECT_EQ(snap.post_state.sideband.reg_live_length.at(73), 4);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertUpdateEquivRegs(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->pass, RtlPassKind::kSched);
  EXPECT_EQ(lifted->sideband.reg_live_length.at(72), 3);
  EXPECT_EQ(lifted->sideband.reg_live_length.at(73), 2);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (const_int 17)) "
            "(expr_list:REG_EQUAL (const_int 17) (nil))");
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[1], "(set (reg:SI 73) (mem:SI (reg:SI 4 a0)))");

  const RightInverseResult res = verifier.VerifyUpdateEquivRegsRightInverse(snap.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;
}

TEST_F(InvertUpdateEquivRegsTest, StripsStoreDerivedMemEquivWithoutHalvingLiveLength) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kSched;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (reg:SI 5 a1)) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 72) (nil)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_cfg.sideband.reg_n_sets[72] = 1;
  pre_cfg.sideband.reg_live_length[72] = 2;

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, pre_cfg);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);
  EXPECT_EQ(snap.post_state.sideband.reg_live_length.at(72), 2);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertUpdateEquivRegs(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.reg_live_length.at(72), 2);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const RightInverseResult res = verifier.VerifyUpdateEquivRegsRightInverse(snap.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;
}

TEST_F(InvertUpdateEquivRegsTest,
       StripsBothUnchangingLoadEquivAndSubsequentStoreEquivHalvingLiveLengthOnce) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kSched;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 75) (mem/s/u:SF (plus:SI (reg/v:SI 73) (const_int 8))))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SF 75)) "
      "(expr_list:REG_DEAD (reg:SF 75) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/v:SI 73)) (expr_list:REG_DEAD (reg/v:SI 73) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_cfg.sideband.reg_n_sets[73] = 1;
  pre_cfg.sideband.reg_n_sets[75] = 1;
  pre_cfg.sideband.reg_live_length[73] = 3;
  pre_cfg.sideband.reg_live_length[75] = 2;

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, pre_cfg);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);
  EXPECT_EQ(snap.post_state.sideband.reg_live_length.at(75), 4);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertUpdateEquivRegs(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.reg_live_length.at(75), 2);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[0],
            "(set (reg:SF 75) (mem/s/u:SF (plus:SI (reg/v:SI 73) (const_int 8))))");

  const RightInverseResult res = verifier.VerifyUpdateEquivRegsRightInverse(snap.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;
}

TEST_F(InvertUpdateEquivRegsTest, ReversesOptimizeRegCopy1Propagation) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  RtlControlFlowGraph equiv_cfg_x;
  equiv_cfg_x.pass = RtlPassKind::kLreg;
  equiv_cfg_x.rpo_blocks.resize(1);
  equiv_cfg_x.rpo_blocks[0].id = 0;
  equiv_cfg_x.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  equiv_cfg_x.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (reg:SI 72)) (expr_list:REG_DEAD (reg:SI 72) (nil))");
  equiv_cfg_x.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 73) (const_int 2)))");
  equiv_cfg_x.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  equiv_cfg_x.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertUpdateEquivRegs(equiv_cfg_x);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[1], "(set (reg:SI 73) (reg:SI 72))");
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[2],
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 72) (const_int 2))) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");

  const RightInverseResult res = verifier.VerifyUpdateEquivRegsRightInverse(equiv_cfg_x);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// =============================================================================
// Negative Input & Verification Tests
// =============================================================================

TEST_F(InvertUpdateEquivRegsTest, RejectsEmptyCfg) {
  RtlControlFlowGraph empty_cfg;
  absl::StatusOr<RtlControlFlowGraph> res = InvertUpdateEquivRegs(empty_cfg);
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(InvertUpdateEquivRegsTest, RejectsMalformedSexprAndUnknownRtlOp) {
  RtlControlFlowGraph bad_sexpr;
  bad_sexpr.rpo_blocks.resize(1);
  bad_sexpr.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0)");
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_sexpr).ok());

  RtlControlFlowGraph unknown_op;
  unknown_op.rpo_blocks.resize(1);
  unknown_op.rpo_blocks[0].AddPatternOnly("(bogus_rtl_op (reg:SI 72))");
  EXPECT_FALSE(InvertUpdateEquivRegs(unknown_op).ok());
}

TEST_F(InvertUpdateEquivRegsTest, RejectsOutOfBoundsSuccessorsAndAddrVecTargets) {
  RtlControlFlowGraph bad_succ;
  bad_succ.rpo_blocks.resize(1);
  bad_succ.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_succ.rpo_blocks[0].successors = {3};
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_succ).ok());

  RtlControlFlowGraph bad_addr_vec;
  bad_addr_vec.rpo_blocks.resize(1);
  bad_addr_vec.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_addr_vec.rpo_blocks[0].addr_vec_targets = {2};
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_addr_vec).ok());
}

TEST_F(InvertUpdateEquivRegsTest, RejectsInvalidSidebandRegisterAndBlockNumbers) {
  RtlControlFlowGraph base;
  base.rpo_blocks.resize(1);
  base.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");

  RtlControlFlowGraph bad_renum_key = base;
  bad_renum_key.sideband.reg_renumber[2] = 4;
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_renum_key).ok());

  RtlControlFlowGraph bad_fcr31_key = base;
  bad_fcr31_key.sideband.reg_renumber[kMipsStatusRegnum] = 2;
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_fcr31_key).ok());

  RtlControlFlowGraph bad_renum_val = base;
  bad_renum_val.sideband.reg_renumber[72] = 75;
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_renum_val).ok());

  RtlControlFlowGraph bad_live = base;
  bad_live.sideband.reg_live_length[-1] = 2;
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_live).ok());

  RtlControlFlowGraph bad_live_bb = base;
  bad_live_bb.sideband.basic_block_live_at_start[-1] = {72};
  EXPECT_FALSE(InvertUpdateEquivRegs(bad_live_bb).ok());
}

TEST_F(InvertUpdateEquivRegsTest, CorruptedInverseIsRejectedByRightInverseVerifier) {
  CorruptedUpdateEquivRegsInverseProvider bad_provider;
  RightInverseVerifier bad_verifier(ctx, hooks, &bad_provider);

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kLreg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (const_int 17)) (expr_list:REG_EQUIV (const_int 17) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RightInverseResult res = bad_verifier.VerifyUpdateEquivRegsRightInverse(cfg);
  EXPECT_FALSE(res.matches);
}

// =============================================================================
// 2,480-Case Parameterized Round-Trip Verification Suite
// =============================================================================

class InvertUpdateEquivRegsRoundTripTest : public InvertUpdateEquivRegsTest,
                                           public ::testing::WithParamInterface<RoundTripTestCase> {
};

TEST_P(InvertUpdateEquivRegsRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);

  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  int verified_count = 0;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kUpdateEquivRegs) {
      continue;
    }
    const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(snap_res.matches) << "Snapshot verification failed for " << tc.test_name << ": "
                                  << snap_res.diff_detail;
    ++verified_count;
  }
  EXPECT_GT(verified_count, 0) << "No kUpdateEquivRegs snapshot captured for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(AllRoundTripCases, InvertUpdateEquivRegsRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
