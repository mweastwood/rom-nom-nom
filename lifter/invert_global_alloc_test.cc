#include "lifter/invert_global_alloc.h"

#include <string>
#include <vector>

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
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class GlobalAllocOnlyInverseProvider : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertGlobalAlloc(greg_cfg);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

class CorruptedGlobalAllocInverseProvider : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg) const override {
    RtlControlFlowGraph corrupted = greg_cfg;
    corrupted.pass = RtlPassKind::kLreg;
    corrupted.sideband.reg_renumber[72] = 15;
    if (!corrupted.rpo_blocks.empty()) {
      corrupted.rpo_blocks[0].instructions.clear();
      corrupted.rpo_blocks[0].instructions.push_back("(set (reg:SI 2 v0) (const_int 999999))");
      corrupted.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }
    return corrupted;
  }
};

class InvertGlobalAllocTest : public ::testing::Test {
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

GccContext* InvertGlobalAllocTest::ctx = nullptr;
FunctionHooks* InvertGlobalAllocTest::hooks = nullptr;

// Positive synthetic test 1: Isolated `global_alloc` pseudo recovery when live at block start.
TEST_F(InvertGlobalAllocTest, ReversesIsolatedSingleBlockGlobalAllocation) {
  RtlControlFlowGraph lreg_pre;
  lreg_pre.pass = RtlPassKind::kLreg;
  lreg_pre.sideband.basic_block_live_at_start[0] = {72};
  lreg_pre.rpo_blocks.resize(1);
  lreg_pre.rpo_blocks[0].id = 0;
  lreg_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))");
  lreg_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 2)))");
  lreg_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, lreg_pre);
  ASSERT_EQ(snap.post_state.pass, RtlPassKind::kGreg);
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(72));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
  EXPECT_TRUE(lifted->sideband.reg_renumber.empty());
  EXPECT_GE(lifted->sideband.max_regno, 73);

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(snap.post_state);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 2: Multi-block CFG partitioning global vs. block-local pseudos.
TEST_F(InvertGlobalAllocTest, PartitionsMultiBlockGlobalPseudoFromBlockLocalPseudo) {
  RtlControlFlowGraph lreg_pre;
  lreg_pre.pass = RtlPassKind::kLreg;
  lreg_pre.sideband.max_regno = 74;
  // Pseudo 72 is local to block 0 and pre-assigned by `local-alloc` to `$3` (`v1`).
  // Pseudo 73 spans blocks 0, 1, and 2 and is left unassigned for `global_alloc`.
  lreg_pre.sideband.reg_renumber[72] = 3;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 73) (ashift:SI (reg:SI 72) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (pc) (if_then_else (gt:SI (reg:SI 5 a1) (const_int 0)) (label_ref L0) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
  };
  bb0.successors = {1, 2};
  bb0.SyncRtlInsnsFromInstructions();

  RtlBasicBlock bb1;
  bb1.id = 1;
  bb1.instructions = {
      "(set (reg:SI 73) (plus:SI (reg:SI 73) (const_int 10)))",
  };
  bb1.successors = {2};
  bb1.SyncRtlInsnsFromInstructions();

  RtlBasicBlock bb2;
  bb2.id = 2;
  bb2.has_label = true;
  bb2.instructions = {
      "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
  };
  bb2.successors = {};
  bb2.SyncRtlInsnsFromInstructions();

  lreg_pre.rpo_blocks = {bb0, bb1, bb2};

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, lreg_pre);
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(72));
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(73));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
  // Local pseudo 72 remains assigned; multi-block pseudo 73 is stripped.
  EXPECT_TRUE(lifted->sideband.reg_renumber.contains(72));
  EXPECT_EQ(lifted->sideband.reg_renumber.at(72), 3);
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(73));

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(snap.post_state);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 3: Single-block pseudo live at block start (`basic_block_live_at_start`)
// or with non-unit `reg_n_deaths` is identified as global and stripped.
TEST_F(InvertGlobalAllocTest, StripsSingleBlockPseudoLiveAtBlockStartOrWithMultipleDeaths) {
  RtlControlFlowGraph lreg_pre;
  lreg_pre.pass = RtlPassKind::kLreg;
  lreg_pre.sideband.max_regno = 75;
  lreg_pre.sideband.reg_renumber[72] = 3;  // Block-local pseudo in block 0.
  lreg_pre.sideband.reg_n_deaths[72] = 1;
  lreg_pre.sideband.reg_n_deaths[73] = 1;
  lreg_pre.sideband.reg_n_deaths[74] = 2;
  lreg_pre.sideband.basic_block_live_at_start[0] = {4, 5};
  lreg_pre.sideband.basic_block_live_at_start[1] = {73};

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 3))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 74) (plus:SI (reg:SI 72) (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
      "(set (reg:SI 73) (ashift:SI (reg:SI 74) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))",
      "(set (reg:SI 74) (plus:SI (reg:SI 73) (const_int 5)))",
      "(set (reg:SI 73) (plus:SI (reg:SI 73) (reg:SI 74))) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))",
  };
  bb0.successors = {1};
  bb0.SyncRtlInsnsFromInstructions();

  RtlBasicBlock bb1;
  bb1.id = 1;
  bb1.has_label = true;
  bb1.instructions = {
      "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
  };
  bb1.successors = {};
  bb1.SyncRtlInsnsFromInstructions();

  lreg_pre.rpo_blocks = {bb0, bb1};

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, lreg_pre);
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(72));
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(73));
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(74));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
  EXPECT_TRUE(lifted->sideband.reg_renumber.contains(72));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(73));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(74));

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult boundary_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;
  RightInverseResult direct_res = verifier.VerifyGlobalAllocRightInverse(snap.post_state);
  EXPECT_TRUE(direct_res.matches) << direct_res.diff_detail;
}

// Positive synthetic test 4: Call-crossing callee-saved GPR and FPR global allocation.
TEST_F(InvertGlobalAllocTest, ReversesCallCrossingCalleeSavedAndFloatingPointGlobalAlloc) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "double ext_fn(double x);\n"
      "double compute(double a, int n) {\n"
      "  int i;\n"
      "  double acc = a;\n"
      "  for (i = 0; i < n; ++i) {\n"
      "    acc += ext_fn(acc);\n"
      "  }\n"
      "  return acc;\n"
      "}\n",
      "compute", {"-O2"});

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_global_alloc = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kGlobalAlloc) {
      found_global_alloc = true;
      absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(snap.post_state);
      ASSERT_TRUE(lifted.ok()) << lifted.status();
      EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
      EXPECT_LT(lifted->sideband.reg_renumber.size(), snap.post_state.sideband.reg_renumber.size());

      RightInverseResult boundary_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;

      RightInverseResult direct_res = verifier.VerifyGlobalAllocRightInverse(snap.post_state);
      EXPECT_TRUE(direct_res.matches) << direct_res.diff_detail;
    }
  }
  EXPECT_TRUE(found_global_alloc);
}

// Positive synthetic test 5: `-O0` `stupid_life_analysis` global allocation boundary.
TEST_F(InvertGlobalAllocTest, ReversesStupidLifeAnalysisAtO0) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int sum_range(int a, int b) {\n"
      "  register int total = 0;\n"
      "  register int i;\n"
      "  for (i = a; i < b; ++i) {\n"
      "    total += i;\n"
      "  }\n"
      "  return total;\n"
      "}\n",
      "sum_range", {"-O0"});

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_global_alloc = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kGlobalAlloc) {
      found_global_alloc = true;
      absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(snap.post_state);
      ASSERT_TRUE(lifted.ok()) << lifted.status();
      EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
      EXPECT_TRUE(lifted->sideband.reg_renumber.empty());

      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_global_alloc);
}

// Negative test 1: Rejects empty CFG.
TEST_F(InvertGlobalAllocTest, RejectsEmptyCfg) {
  RtlControlFlowGraph empty_cfg;
  EXPECT_FALSE(InvertGlobalAlloc(empty_cfg).ok());
}

// Negative test 2: Rejects malformed S-expression instruction.
TEST_F(InvertGlobalAllocTest, RejectsMalformedSexprInstruction) {
  RtlControlFlowGraph bad_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 72) (plus:SI (reg:SI 4 a0)"};
  bad_cfg.rpo_blocks.push_back(bb0);

  EXPECT_FALSE(InvertGlobalAlloc(bad_cfg).ok());
}

// Negative test 3: Rejects out-of-bounds successor or addr_vec_targets edge.
TEST_F(InvertGlobalAllocTest, RejectsOutOfBoundsCfgEdges) {
  RtlControlFlowGraph bad_succ_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 2 v0) (const_int 0))"};
  bb0.successors = {3};
  bad_succ_cfg.rpo_blocks.push_back(bb0);
  EXPECT_FALSE(InvertGlobalAlloc(bad_succ_cfg).ok());

  RtlControlFlowGraph bad_target_cfg;
  RtlBasicBlock bb_tbl;
  bb_tbl.id = 0;
  bb_tbl.instructions = {"(set (reg:SI 2 v0) (const_int 0))"};
  bb_tbl.addr_vec_targets = {2};
  bad_target_cfg.rpo_blocks.push_back(bb_tbl);
  EXPECT_FALSE(InvertGlobalAlloc(bad_target_cfg).ok());
}

// Negative test 4: Rejects invalid register numbers in instructions and sideband tables.
TEST_F(InvertGlobalAllocTest, RejectsInvalidRegistersAndSidebandEntries) {
  RtlControlFlowGraph neg_insn_reg_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI -1) (const_int 0))"};
  neg_insn_reg_cfg.rpo_blocks.push_back(bb0);
  EXPECT_FALSE(InvertGlobalAlloc(neg_insn_reg_cfg).ok());

  RtlBasicBlock valid_bb;
  valid_bb.id = 0;
  valid_bb.instructions = {"(set (reg:SI 2 v0) (const_int 0))"};

  RtlControlFlowGraph bad_renumber_key_cfg;
  bad_renumber_key_cfg.rpo_blocks.push_back(valid_bb);
  bad_renumber_key_cfg.sideband.reg_renumber[12] = 2;  // Hard reg key (< 67).
  EXPECT_FALSE(InvertGlobalAlloc(bad_renumber_key_cfg).ok());

  RtlControlFlowGraph bad_renumber_val_cfg;
  bad_renumber_val_cfg.rpo_blocks.push_back(valid_bb);
  bad_renumber_val_cfg.sideband.reg_renumber[72] = 80;  // Target not a hard reg (>= 67).
  EXPECT_FALSE(InvertGlobalAlloc(bad_renumber_val_cfg).ok());

  RtlControlFlowGraph bad_live_bb_cfg;
  bad_live_bb_cfg.rpo_blocks.push_back(valid_bb);
  bad_live_bb_cfg.sideband.basic_block_live_at_start[-1] = {72};  // Negative block index.
  EXPECT_FALSE(InvertGlobalAlloc(bad_live_bb_cfg).ok());

  RtlControlFlowGraph bad_saved_gpr_cfg;
  bad_saved_gpr_cfg.rpo_blocks.push_back(valid_bb);
  bad_saved_gpr_cfg.sideband.saved_gpr_regnos = {40};  // Not a GPR (>= 32).
  EXPECT_FALSE(InvertGlobalAlloc(bad_saved_gpr_cfg).ok());

  RtlControlFlowGraph bad_saved_fpr_cfg;
  bad_saved_fpr_cfg.rpo_blocks.push_back(valid_bb);
  bad_saved_fpr_cfg.sideband.saved_fpr_regnos = {16};  // Not an FPR (< 32).
  EXPECT_FALSE(InvertGlobalAlloc(bad_saved_fpr_cfg).ok());
}

// Negative test 5: Verifies that `VerifyGlobalAllocRightInverse` rejects a corrupted provider.
TEST_F(InvertGlobalAllocTest, VerifierRejectsCorruptedInverseProvider) {
  RtlControlFlowGraph greg_cfg;
  greg_cfg.pass = RtlPassKind::kGreg;
  greg_cfg.sideband.max_regno = 73;
  greg_cfg.sideband.reg_renumber[72] = 2;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))",
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 2)))",
      "(use (reg/i:SI 2 v0))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  greg_cfg.rpo_blocks.push_back(bb0);

  CorruptedGlobalAllocInverseProvider bad_provider;
  RightInverseVerifier verifier(ctx, hooks, &bad_provider);
  RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(greg_cfg);
  EXPECT_FALSE(vres.matches);
}

class InvertGlobalAllocCorpusTest : public InvertGlobalAllocTest,
                                    public ::testing::WithParamInterface<RoundTripTestCase> {};

TEST_P(InvertGlobalAllocCorpusTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  bool found_global_alloc = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kGlobalAlloc) {
      found_global_alloc = true;
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << "Case " << tc.test_name
                                << " failed VerifyGlobalAllocRightInverse: " << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_global_alloc) << "Missing kGlobalAlloc snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(AllCases, InvertGlobalAllocCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
