#include "lifter/invert_global_alloc.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_alter_reg.h"
#include "lifter/invert_reload.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"

extern "C" void regclass_init();  // NOLINT(readability-identifier-naming)

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

class GlobalAllocOnlyInverseProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertGlobalAlloc(greg_cfg);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg,
                                        const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res =
        ::rom_nom_nom::lifter::InvertGlobalAlloc(greg_cfg, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

class CorruptedGlobalAllocInverseProvider final : public SubfunctionInverseProvider {
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
  using SubfunctionInverseProvider::InvertGlobalAlloc;
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

// Positive synthetic test 1: Upward-exposed single-block `global_alloc` pseudo recovery
// without relying on incoming oracle liveness sidebands.
TEST_F(InvertGlobalAllocTest, ReversesIsolatedSingleBlockGlobalAllocation) {
  ctx->BeginFunction("isolated_fn");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);

  RtlControlFlowGraph lreg_pre;
  lreg_pre.pass = RtlPassKind::kLreg;
  lreg_pre.rpo_blocks.resize(1);
  lreg_pre.rpo_blocks[0].id = 0;
  lreg_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 72) (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  lreg_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  lreg_pre.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, lreg_pre);
  ASSERT_EQ(snap.post_state.pass, RtlPassKind::kGreg);
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(72));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(snap.post_state, o2_profile);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
  EXPECT_TRUE(lifted->sideband.reg_renumber.empty());
  EXPECT_TRUE(lifted->sideband.basic_block_live_at_start[0].contains(72));
  EXPECT_GE(lifted->sideband.max_regno, 73);

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(snap.post_state);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 2: Multi-block CFG partitioning global vs. block-local pseudos.
TEST_F(InvertGlobalAllocTest, PartitionsMultiBlockGlobalPseudoFromBlockLocalPseudo) {
  ctx->BeginFunction("partition_fn");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);

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

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(snap.post_state, o2_profile);
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
// or with non-unit `reg_n_deaths` is identified as global and stripped purely via RTL liveness.
TEST_F(InvertGlobalAllocTest, StripsSingleBlockPseudoLiveAtBlockStartOrWithMultipleDeaths) {
  ctx->BeginFunction("strip_sb_fn");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);

  RtlControlFlowGraph lreg_pre;
  lreg_pre.pass = RtlPassKind::kLreg;
  lreg_pre.sideband.max_regno = 75;
  lreg_pre.sideband.reg_renumber[72] = 3;  // Block-local pseudo in block 0.

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

  RtlControlFlowGraph stripped = snap.post_state;
  stripped.sideband.basic_block_live_at_start.clear();
  stripped.sideband.reg_n_deaths.clear();
  stripped.sideband.reg_n_refs.clear();
  stripped.sideband.reg_n_sets.clear();
  stripped.sideband.reg_live_length.clear();
  stripped.sideband.reg_n_calls_crossed.clear();

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(stripped, o2_profile);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
  EXPECT_TRUE(lifted->sideband.reg_renumber.contains(72));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(73));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(74));

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult direct_res = verifier.VerifyGlobalAllocRightInverse(stripped);
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
      absl::StatusOr<RtlControlFlowGraph> lifted =
          InvertGlobalAlloc(snap.post_state, trace.stage0_input.profile);
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

// Positive synthetic test 5: `-O0` `stupid_life_analysis` global allocation boundary
// WITHOUT relying on `reg_n_deaths[0]` in incoming sidebands.
TEST_F(InvertGlobalAllocTest, ReversesStupidLifeAnalysisAtO0WithoutSidebandSentinel) {
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
      RtlControlFlowGraph stripped = snap.post_state;
      stripped.sideband.basic_block_live_at_start.clear();
      stripped.sideband.reg_n_deaths.clear();
      stripped.sideband.reg_n_refs.clear();
      stripped.sideband.reg_n_sets.clear();
      stripped.sideband.reg_live_length.clear();
      stripped.sideband.reg_n_calls_crossed.clear();
      ASSERT_FALSE(stripped.sideband.reg_n_deaths.contains(0));

      absl::StatusOr<RtlControlFlowGraph> lifted =
          InvertGlobalAlloc(stripped, trace.stage0_input.profile);
      ASSERT_TRUE(lifted.ok()) << lifted.status();
      EXPECT_EQ(lifted->pass, RtlPassKind::kLreg);
      EXPECT_FALSE(lifted->sideband.reg_renumber.empty());
      for (const auto& [regno, hr] : lifted->sideband.reg_renumber) {
        EXPECT_GE(regno, 72);
        EXPECT_GE(hr, 0);
        EXPECT_LT(hr, 68);
      }

      RightInverseResult boundary_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;

      hooks->ApplyCompilationProfile(trace.stage0_input.profile);
      RightInverseResult stripped_res = verifier.VerifyGlobalAllocRightInverse(stripped);
      EXPECT_TRUE(stripped_res.matches) << stripped_res.diff_detail;
    }
  }
  EXPECT_TRUE(found_global_alloc);
}

// Positive synthetic test 6: Preserves Step 34 zero-occurrence phantom `alter_reg` pseudos
// (`reg_n_refs = 2, reg_n_sets = 0`) unchanged across `InvertGlobalAlloc` while still
// satisfying `VerifyGlobalAllocRightInverse`.
TEST_F(InvertGlobalAllocTest, PreservesZeroOccurrencePhantomAlterRegPseudos) {
  ctx->BeginFunction("phantom_fn");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);

  RtlControlFlowGraph lreg_pre;
  lreg_pre.pass = RtlPassKind::kLreg;
  lreg_pre.sideband.max_regno = 75;
  lreg_pre.sideband.reg_renumber[72] = 3;
  // Pseudo 74 is a zero-occurrence `combine`-eliminated phantom temporary (`reg_n_refs = 2`,
  // `reg_n_deaths = 1`, `reg_live_length = 2`, `reg_n_sets` absent, `reg_renumber` absent).
  lreg_pre.sideband.reg_n_refs[74] = 2;
  lreg_pre.sideband.reg_n_deaths[74] = 1;
  lreg_pre.sideband.reg_live_length[74] = 2;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 73) (ashift:SI (reg:SI 72) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
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
  ASSERT_FALSE(snap.post_state.sideband.reg_renumber.contains(74));

  RtlControlFlowGraph greg_in = snap.post_state;
  greg_in.sideband.basic_block_live_at_start.clear();
  greg_in.sideband.reg_n_refs.clear();
  greg_in.sideband.reg_n_sets.clear();
  greg_in.sideband.reg_n_deaths.clear();
  greg_in.sideband.reg_live_length.clear();
  greg_in.sideband.reg_n_calls_crossed.clear();
  greg_in.sideband.reg_n_refs[74] = 2;
  greg_in.sideband.reg_n_deaths[74] = 1;
  greg_in.sideband.reg_live_length[74] = 2;

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(greg_in, o2_profile);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.reg_n_refs[74], 2);
  EXPECT_EQ(lifted->sideband.reg_n_deaths[74], 1);
  EXPECT_EQ(lifted->sideband.reg_live_length[74], 2);
  EXPECT_FALSE(lifted->sideband.reg_n_sets.contains(74));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(74));
  EXPECT_GE(lifted->sideband.max_regno, 75);
  EXPECT_TRUE(lifted->sideband.reg_renumber.contains(72));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(73));

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(greg_in);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
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
  bad_renumber_key_cfg.sideband.reg_renumber[12] = 2;  // Hard reg key (< 68).
  EXPECT_FALSE(InvertGlobalAlloc(bad_renumber_key_cfg).ok());

  RtlControlFlowGraph fcr31_renumber_key_cfg;
  fcr31_renumber_key_cfg.rpo_blocks.push_back(valid_bb);
  fcr31_renumber_key_cfg.sideband.reg_renumber[67] = 2;  // $fcr31 (67) is a hard reg (< 68).
  EXPECT_FALSE(InvertGlobalAlloc(fcr31_renumber_key_cfg).ok());

  RtlControlFlowGraph bad_renumber_val_cfg;
  bad_renumber_val_cfg.rpo_blocks.push_back(valid_bb);
  bad_renumber_val_cfg.sideband.reg_renumber[72] = 80;  // Target not a hard reg (>= 68).
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
      "(set (reg:SI 72) (plus:SI (reg:SI 72) (reg:SI 4 a0)))",
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

TEST_F(InvertGlobalAllocTest, RestoresBlockAllocatedMdRegsPseudoSpilledByReloadInGlobalAlloc) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "long long mul64_pair(int a, int b, int c, int d) {\n"
      "  long long p0 = (long long)a * (long long)b;\n"
      "  long long p1 = (long long)c * (long long)d;\n"
      "  return p0 * p1 + p0;\n"
      "}\n",
      "mul64_pair", {"-O2", "-mips2", "-mcpu=r4000"});

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_global_alloc = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kGlobalAlloc) {
      found_global_alloc = true;
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_global_alloc);
}

TEST_F(InvertGlobalAllocTest, ExcludesSingleRegMdClassHighpartPseudoFromLocalAllocRenumber) {
  ctx->BeginFunction("single_md_highpart_fn");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);

  RtlControlFlowGraph greg_cfg;
  greg_cfg.pass = RtlPassKind::kGreg;
  greg_cfg.sideband.max_regno = 76;
  greg_cfg.sideband.reg_renumber[72] = 2;
  greg_cfg.sideband.reg_renumber[73] = 3;
  greg_cfg.sideband.reg_renumber[74] = 4;
  greg_cfg.sideband.reg_renumber[75] = 3;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(parallel[ (set (reg:SI 72) (truncate:SI (lshiftrt:DI (mult:DI (sign_extend:DI (reg:SI 4 "
      "a0)) (sign_extend:DI (reg:SI 5 a1))) (const_int 32)))) (clobber (reg:SI 65 lo)) (clobber "
      "(scratch:SI)) ] ) (expr_list:REG_DEAD (reg:SI 5 a1) "
      "(expr_list:REG_UNUSED (reg:SI 65 lo) (nil)))",
      "(parallel[ (set (reg:SI 75) (truncate:SI (lshiftrt:DI (mult:DI (sign_extend:DI (reg:SI "
      "4 a0)) (sign_extend:DI (reg:SI 4 a0))) (const_int 32)))) (clobber (reg:SI 65 lo)) (clobber "
      "(scratch:SI)) ] ) (expr_list:REG_DEAD (reg:SI 4 a0) "
      "(expr_list:REG_UNUSED (reg:SI 65 lo) (nil)))",
      "(set (reg:SI 74) (ashiftrt:SI (reg:SI 75) (const_int 3))) "
      "(expr_list:REG_DEAD (reg:SI 75) (nil))",
      "(set (reg:SI 73) (ashiftrt:SI (reg:SI 72) (const_int 5))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (reg:SI 74))) "
      "(expr_list:REG_DEAD (reg:SI 73) (expr_list:REG_DEAD (reg:SI 74) (nil)))",
      "(use (reg/i:SI 2 v0))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  greg_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(greg_cfg, o2_profile);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  // Pseudos 72 and 75 have preferred class `HI_REG` (`CLASS_LIKELY_SPILLED_P == 1`)
  // and only 1 GR read, so `local_alloc` skips both and `global_alloc` + `reload`
  // (`spill_hard_reg(64)` / `retry_global_alloc`) assigns them via `alt_class == GR_REGS`.
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(72));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(75));
  EXPECT_EQ(lifted->sideband.reg_renumber.at(73), 3);
  EXPECT_EQ(lifted->sideband.reg_renumber.at(74), 4);

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(greg_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

TEST_F(InvertGlobalAllocTest, ReconcilesConflictingGlobalPseudoPrioritiesAgainstTargetHardRegs) {
  ctx->BeginFunction("reconcile_prio_fn");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);

  // Construct a two-block loop where fresh pseudos 72 (`i`, higher natural ref
  // weight) and 73 (`sum`, lower natural ref weight) were allocated `$3` and
  // `$2` respectively, requiring `InvertGlobalAlloc` to reconcile their
  // `allocno_compare` priority order so `global_alloc` assigns `73 -> $2`
  // before `72 -> $3`.
  RtlControlFlowGraph greg_cfg;
  greg_cfg.pass = RtlPassKind::kGreg;
  greg_cfg.sideband.max_regno = 75;
  greg_cfg.sideband.reg_renumber[72] = 3;
  greg_cfg.sideband.reg_renumber[73] = 2;
  greg_cfg.sideband.reg_renumber[74] = 5;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.successors = {1};
  bb0.instructions = {
      "(set (reg:SI 72) (const_int 0))",
      "(set (reg:SI 73) (const_int 0))",
  };
  bb0.SyncRtlInsnsFromInstructions();

  RtlBasicBlock bb1;
  bb1.id = 1;
  bb1.has_label = true;
  bb1.successors = {1, 2};
  bb1.instructions = {
      "(set (reg:SI 73) (plus:SI (reg:SI 73) (reg:SI 72)))",
      "(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))",
      "(set (reg:SI 74) (lt:SI (reg:SI 72) (reg:SI 4 a0)))",
      "(set (pc) (if_then_else (ne:SI (reg:SI 74) (const_int 0)) (label_ref L0) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))",
  };
  bb1.SyncRtlInsnsFromInstructions();

  RtlBasicBlock bb2;
  bb2.id = 2;
  bb2.instructions = {
      "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
      "(use (reg/i:SI 2 v0))",
  };
  bb2.SyncRtlInsnsFromInstructions();

  greg_cfg.rpo_blocks = {bb0, bb1, bb2};
  ctx->MaterializeFromCfg(greg_cfg);
  greg_cfg = ctx->DematerializeToCfg(ctx->GetInsns(), RtlPassKind::kGreg);
  greg_cfg.sideband.reg_renumber[72] = 3;
  greg_cfg.sideband.reg_renumber[73] = 2;
  greg_cfg.sideband.reg_renumber[74] = 5;

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(greg_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

CompilationProfile EffectiveStage0Profile(const Stage0FunctionInput& input_x) {
  CompilationProfile effective_profile = input_x.profile;
  if (input_x.base_vram != 0 && input_x.relocations.empty()) {
    for (const auto& [vram, sym] : input_x.vram_to_symbol) {
      if (sym.empty()) {
        continue;
      }
      const auto sz_it = input_x.vram_to_symbol_size.find(vram);
      if (sz_it != input_x.vram_to_symbol_size.end() && sz_it->second > 0) {
        effective_profile.gp_symbol_sizes.try_emplace(sym, sz_it->second);
      }
    }
  }
  return effective_profile;
}

std::string CompareStage0ReplayResult(const Stage0FunctionInput& expected,
                                      const Stage0FunctionInput& actual,
                                      const std::string& combined_asm) {
  if (actual.machine_words != expected.machine_words) {
    std::string word_diffs;
    const size_t min_len = std::min(actual.machine_words.size(), expected.machine_words.size());
    for (size_t i = 0; i < min_len; ++i) {
      if (actual.machine_words[i] != expected.machine_words[i]) {
        absl::StrAppendFormat(&word_diffs, "  word[%zu]: got 0x%08x != expected 0x%08x\n", i,
                              actual.machine_words[i], expected.machine_words[i]);
      }
    }
    return absl::StrFormat(
        "Machine words mismatch: got %zu words != expected %zu words\n%sasm:\n%s",
        actual.machine_words.size(), expected.machine_words.size(), word_diffs, combined_asm);
  }
  if (expected.base_vram != 0 && expected.relocations.empty()) {
    return "";
  }
  if (actual.jump_tables.size() != expected.jump_tables.size()) {
    return absl::StrFormat("Jump-table count got %zu != expected %zu\nasm:\n%s",
                           actual.jump_tables.size(), expected.jump_tables.size(), combined_asm);
  }
  for (size_t i = 0; i < expected.jump_tables.size(); ++i) {
    if (actual.jump_tables[i].target_word_indices != expected.jump_tables[i].target_word_indices) {
      return absl::StrFormat("Jump table %zu target_word_indices mismatch\nasm:\n%s", i,
                             combined_asm);
    }
  }
  if (actual.constant_pool.size() != expected.constant_pool.size()) {
    return absl::StrFormat("Constant-pool count got %zu != expected %zu\nasm:\n%s",
                           actual.constant_pool.size(), expected.constant_pool.size(),
                           combined_asm);
  }
  if (actual.relocations.size() != expected.relocations.size()) {
    return absl::StrFormat("Relocation count got %zu != expected %zu\nasm:\n%s",
                           actual.relocations.size(), expected.relocations.size(), combined_asm);
  }
  for (size_t i = 0; i < expected.relocations.size(); ++i) {
    const Stage0Relocation& exp_r = expected.relocations[i];
    const Stage0Relocation& act_r = actual.relocations[i];
    if (act_r.word_index != exp_r.word_index || act_r.kind != exp_r.kind ||
        act_r.addend != exp_r.addend) {
      return absl::StrFormat("Relocation %zu mismatch\nasm:\n%s", i, combined_asm);
    }
  }
  return "";
}

TEST_F(InvertGlobalAllocTest, HighPressureMixedGprFprSpillToFpAndLocalEvictionDensityGuard) {
  // Exercises `-mcpu=r4000` (`MEMORY_MOVE_COST == 4`, `alt_class = ALL_REGS`, `$fp = 30` fallback
  // and `global.c` local-eviction density guard) vs default R3000 (`MEMORY_MOVE_COST == 6`,
  // `alt_class = NO_REGS`) when both callee-saved GPRs (`$s0..$s7`, `$fp`) and callee-saved FPRs
  // (`$f20..$f30`) are saturated across calls.
  constexpr const char* kMixedSrc =
      "extern float MixAxis(int tag, float a, float b);\n"
      "float MixedBankSaturation(const int* ids, const float* vals) {\n"
      "  int g0 = ids[0] + 1, g1 = ids[1] + 2, g2 = ids[2] + 3, g3 = ids[3] + 4;\n"
      "  int g4 = ids[4] ^ 5, g5 = ids[5] ^ 6, g6 = ids[6] ^ 7, g7 = ids[7] ^ 8;\n"
      "  int g8 = ids[8] - 9, g9 = ids[9] - 10, g10 = ids[10] + 11, g11 = ids[11] + 12;\n"
      "  float f0 = vals[0] + 1.0f, f1 = vals[1] - 1.0f, f2 = vals[2] * 2.0f, f3 = vals[3] * "
      "0.5f;\n"
      "  float f4 = vals[4] + f0, f5 = vals[5] + f1, f6 = vals[6] + f2, f7 = vals[7] + f3;\n"
      "  float c0 = MixAxis(g0 + g11, f0, f7);\n"
      "  float c1 = MixAxis(g1 + g10, c0 + f1, f6);\n"
      "  int sum = (g0 ^ g6) + (g1 ^ g7) + (g2 ^ g8) + (g3 ^ g9) + (g4 ^ g10) + (g5 ^ g11);\n"
      "  return c0 + c1 + (f2 * f5) + (f3 * f4) + (float)sum;\n"
      "}\n";
  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips1", "-Wa,-O2"}}) {
    ctx->BeginFunction("harness_init");
    hooks->ApplyCompilationProfile({2, flags, {}});
    regclass_init();
    const FunctionCompilationTrace trace =
        hooks->TraceForwardCompilation(kMixedSrc, "MixedBankSaturation", flags);
    bool found_global = false;
    for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id == SubfunctionId::kGlobalAlloc) {
        found_global = true;
        const RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
        EXPECT_TRUE(vres.matches) << vres.diff_detail;
      }
    }
    EXPECT_TRUE(found_global);

    // Also verify fresh `InvertAlterReg -> InvertReload` satisfies the full
    // `RightInverseVerifier::VerifyGlobalAllocRightInverse` contract (`CompareAgainst`
    // + `sideband.reg_renumber`).
    ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kGreg));
    const RtlControlFlowGraph pre_alter =
        InvertAlterReg(trace.pass_snapshots.at(RtlPassKind::kGreg), trace.stage0_input.profile, 72);
    ASSERT_FALSE(pre_alter.rpo_blocks.empty());
    const absl::StatusOr<RtlControlFlowGraph> pre_reload =
        InvertReload(pre_alter, trace.stage0_input.profile);
    ASSERT_TRUE(pre_reload.ok()) << pre_reload.status();

    hooks->ApplyCompilationProfile(trace.stage0_input.profile);
    const RightInverseResult fresh_vres = verifier.VerifyGlobalAllocRightInverse(*pre_reload);
    EXPECT_TRUE(fresh_vres.matches) << fresh_vres.diff_detail;
  }
}

TEST_F(InvertGlobalAllocTest,
       FramePointerStackStoreDoesNotPolluteRegsEverLiveAndPrunesGlobalSavedRegs) {
  // Verifies two post-`InvertReload` sideband invariants in `InvertGlobalAlloc`:
  // 1. A stack-frame store `(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int 16))) ...)` must NOT
  //    count `$fp` (`30`) as a `note_stores` register write (`regs_ever_live[30] == 0`), so `$fp`
  //    is available only after `s0..s7` rather than jumping ahead as an "already-live" callee-saved
  //    register.
  // 2. Post-reload `saved_gpr_regnos` entries belonging solely to lifted global pseudos (e.g.
  //    `16 = s0`) are pruned from pre-`global_alloc` `saved_gpr_regnos` so `regs_ever_live`
  //    entering `global_alloc` matches `flow`.
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  ctx->BeginFunction("harness_init");
  hooks->ApplyCompilationProfile(o2_prof);
  regclass_init();

  RtlControlFlowGraph greg_cfg;
  greg_cfg.pass = RtlPassKind::kGreg;
  greg_cfg.sideband.frame_pointer_needed = false;
  greg_cfg.sideband.outgoing_args_size = 24;
  greg_cfg.sideband.saved_gpr_regnos = {16, 31};  // `16` came from post-reload `mark_home_live`!
  greg_cfg.sideband.max_regno = 73;
  greg_cfg.sideband.reg_renumber[72] = 16;  // First call-crossing global pseudo -> `$s0` (`16`),
                                            // NOT `$fp` (`30`)!

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.successors = {1};
  bb0.AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  bb0.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int 16))) (reg:SI 5 a1)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Callee\"))) (const_int 24))) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  bb0.AddInsn(std::move(call_insn));
  greg_cfg.rpo_blocks.push_back(std::move(bb0));

  RtlBasicBlock bb1;
  bb1.id = 1;
  bb1.has_label = true;
  bb1.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 72))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  bb1.AddPatternOnly("(use (reg/i:SI 2 v0))");
  greg_cfg.rpo_blocks.push_back(std::move(bb1));

  ctx->MaterializeFromCfg(greg_cfg);
  RtlControlFlowGraph canonical_greg = ctx->DematerializeToCfg(ctx->GetInsns(), RtlPassKind::kGreg);
  canonical_greg.sideband = greg_cfg.sideband;

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(canonical_greg, o2_prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(72));
  EXPECT_EQ(lifted->sideband.saved_gpr_regnos, std::vector<int>({31}));

  SubfunctionBoundarySnapshot snap;
  snap.subfunction_id = SubfunctionId::kGlobalAlloc;
  snap.optimize_level = 2;
  snap.c_flags = o2_prof.c_flags;
  snap.post_state = canonical_greg;
  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  const RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

TEST_F(InvertGlobalAllocTest,
       O0Mulsidi3SpillAndObeyRegdeclsScopeSatisfiesFreshGlobalAllocRightInverse) {
  // At `-O0`, `InvertReload` folds `mulsidi3` top-of-frame stack spills and compacts killed
  // reload pseudos so `stupid_life_analysis` never assigns `MD_REGS` (`64`) to an unreferenced
  // pseudo hole `< max_regno`. Verify fresh `Pass00..InvertGlobalAlloc` satisfies full
  // `VerifyGlobalAllocRightInverse` (`CompareAgainst` + exact `sideband.reg_renumber`).
  const std::vector<std::string> o0_flags = {"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  ctx->BeginFunction("harness_init");
  hooks->ApplyCompilationProfile({0, o0_flags, {}});
  regclass_init();
  const FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "long long Mul64WithLocalScope(long long a, long long b, int sel) {\n"
      "  long long prod = a * b;\n"
      "  if (sel > 0) {\n"
      "    int bias = sel + 3;\n"
      "    prod += bias;\n"
      "  }\n"
      "  return prod;\n"
      "}\n",
      "Mul64WithLocalScope", o0_flags);

  const CompilationProfile effective_profile = EffectiveStage0Profile(trace.stage0_input);
  const RtlControlFlowGraph y0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(y0.rpo_blocks.empty());
  const RtlControlFlowGraph y1 = InvertPass01DbrSchedule(y0, effective_profile);
  const RtlControlFlowGraph y2 = InvertPass02Jump2CrossJump(y1, effective_profile);
  const RtlControlFlowGraph fresh_greg = InvertPass03Sched2PostReload(y2, effective_profile);
  const RtlControlFlowGraph lifted_pre_alter = InvertAlterReg(fresh_greg, effective_profile, 72);
  const absl::StatusOr<RtlControlFlowGraph> lifted_pre_reload =
      InvertReload(lifted_pre_alter, effective_profile);
  ASSERT_TRUE(lifted_pre_reload.ok()) << lifted_pre_reload.status();

  const absl::StatusOr<RtlControlFlowGraph> lifted_lreg =
      InvertGlobalAlloc(*lifted_pre_reload, effective_profile);
  ASSERT_TRUE(lifted_lreg.ok()) << lifted_lreg.status();
  EXPECT_FALSE(lifted_lreg->sideband.reg_renumber.empty());
  for (const auto& [regno, hr] : lifted_lreg->sideband.reg_renumber) {
    EXPECT_GE(regno, 72);
    EXPECT_GE(hr, 0);
  }
  EXPECT_EQ(lifted_lreg->rpo_blocks.size(), lifted_pre_reload->rpo_blocks.size());
  EXPECT_EQ(lifted_lreg->sideband.frame_size_bytes, lifted_pre_reload->sideband.frame_size_bytes);

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  hooks->ApplyCompilationProfile(effective_profile);
  const RightInverseResult vres = verifier.VerifyGlobalAllocRightInverse(*lifted_pre_reload);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

TEST_F(InvertGlobalAllocTest,
       EnforceBlockAllocQuantityAtomicityAndLocalPriorityReconcileRightInverse) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips1"}, {}};
  ctx->BeginFunction("harness_init");
  hooks->ApplyCompilationProfile(o2_prof);
  regclass_init();

  // 1. `EnforceBlockAllocQuantityAtomicity`: single-block local copy `r73 <- r72` with
  //    `REG_DEAD (reg:SI 72)` ties `r72` and `r73` into one `local_alloc` quantity (`offset == 0`).
  //    If `greg_cfg` carries a non-atomic assignment (`r72 -> 2`, `r73 -> 3`), `InvertGlobalAlloc`
  //    erases both `72` and `73` from `sideband.reg_renumber` so `global_alloc` / `local_alloc`
  //    atomicity is never violated.
  RtlControlFlowGraph non_atomic_cfg;
  non_atomic_cfg.pass = RtlPassKind::kGreg;
  non_atomic_cfg.sideband.max_regno = 74;
  non_atomic_cfg.sideband.reg_renumber = {{72, 2}, {73, 3}};
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  bb0.AddPatternOnly("(set (reg:SI 73) (reg:SI 72)) (expr_list:REG_DEAD (reg:SI 72) (nil))");
  bb0.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  bb0.AddPatternOnly("(use (reg/i:SI 2 v0))");
  non_atomic_cfg.rpo_blocks.push_back(std::move(bb0));

  absl::StatusOr<RtlControlFlowGraph> lifted_non_atomic =
      InvertGlobalAlloc(non_atomic_cfg, o2_prof);
  ASSERT_TRUE(lifted_non_atomic.ok()) << lifted_non_atomic.status();
  EXPECT_FALSE(lifted_non_atomic->sideband.reg_renumber.contains(72));
  EXPECT_FALSE(lifted_non_atomic->sideband.reg_renumber.contains(73));

  // 2. `ReconcileLocalBlockAllocPriorities`: two overlapping single-block local pseudos (`72` and
  //    `73`) where `73` has target hard reg `$2` (`v0`) and `72` has target hard reg `$3` (`v1`)
  //    even though `72` has an equal/shorter raw span. `ReconcileLocalBlockAllocPriorities` adjusts
  //    `reg_n_refs` so `block_alloc` + `global_alloc` reproduces `{72: 3, 73: 2}` and passes
  //    `VerifyGlobalAllocRightInverse`!
  RtlControlFlowGraph local_pri_cfg;
  local_pri_cfg.pass = RtlPassKind::kGreg;
  local_pri_cfg.sideband.max_regno = 74;
  local_pri_cfg.sideband.reg_renumber = {{72, 3}, {73, 2}};
  RtlBasicBlock pri_bb;
  pri_bb.id = 0;
  pri_bb.AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 4 a0) (const_int 5)))");
  pri_bb.AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))");
  pri_bb.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 6 a2) (const_int 4))) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  pri_bb.AddPatternOnly(
      "(set (mem:SI (reg:SI 6 a2)) (reg:SI 73)) "
      "(expr_list:REG_DEAD (reg:SI 73) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))");
  pri_bb.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (reg:SI 4 a0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pri_bb.AddPatternOnly("(use (reg/i:SI 2 v0))");
  local_pri_cfg.rpo_blocks.push_back(std::move(pri_bb));

  ctx->MaterializeFromCfg(local_pri_cfg);
  RtlControlFlowGraph canon_pri = ctx->DematerializeToCfg(ctx->GetInsns(), RtlPassKind::kGreg);
  canon_pri.sideband = local_pri_cfg.sideband;

  absl::StatusOr<RtlControlFlowGraph> lifted_pri = InvertGlobalAlloc(canon_pri, o2_prof);
  ASSERT_TRUE(lifted_pri.ok()) << lifted_pri.status();
  EXPECT_GT(lifted_pri->sideband.reg_n_refs[73], lifted_pri->sideband.reg_n_refs[72]);

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  const RightInverseResult ga_res = verifier.VerifyGlobalAllocRightInverse(canon_pri);
  EXPECT_TRUE(ga_res.matches) << ga_res.diff_detail;

  // Verify clearing local `reg_renumber` and replaying `kBlockAlloc -> kGlobalAlloc` reproduces
  // `{72: 3, 73: 2}`!
  RtlControlFlowGraph pre_ba = *lifted_pri;
  pre_ba.sideband.reg_renumber.clear();
  const SubfunctionBoundarySnapshot ba_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kBlockAlloc, pre_ba, {}, o2_prof.optimize_level, o2_prof.c_flags);
  const SubfunctionBoundarySnapshot ga_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kGlobalAlloc, ba_snap.post_state, {}, o2_prof.optimize_level, o2_prof.c_flags);
  EXPECT_EQ(ga_snap.post_state.sideband.reg_renumber.at(72), 3);
  EXPECT_EQ(ga_snap.post_state.sideband.reg_renumber.at(73), 2);
  EXPECT_TRUE(verifier.VerifySubfunctionBoundaryRightInverse(ga_snap).matches);
}

class InvertGlobalAllocCorpusTest : public InvertGlobalAllocTest,
                                    public ::testing::WithParamInterface<RoundTripTestCase> {};

void VerifyFreshCumulativeCompositionAndReplay(GccContext* ctx, FunctionHooks* hooks,
                                               const RightInverseVerifier& verifier,
                                               const RoundTripTestCase& tc,
                                               const Stage0FunctionInput& stage0_input) {
  // Fresh cumulative composition (`Pass00 -> Pass01 -> Pass02 -> Pass03 -> InvertAlterReg ->
  // InvertReload -> InvertGlobalAlloc`):
  // 1. Unconditionally verify `VerifyGlobalAllocRightInverse(*lifted_pre_reload_or)`
  // (`CompareAgainst`
  //    + `sideband.reg_renumber`) across all 4,172 corpus cases (`-O0` and `-O1..3`).
  // 2. Verify fresh-context forward replay from `kGlobalAlloc` through `Stage0` bit-exact machine
  //    words, relocations, jump tables, and constant pools.
  const CompilationProfile effective_profile = EffectiveStage0Profile(stage0_input);
  const RtlControlFlowGraph y0 = InvertFinalAndGasFunction(stage0_input);
  ASSERT_FALSE(y0.rpo_blocks.empty()) << "Empty Pass00 CFG on " << tc.test_name;
  const RtlControlFlowGraph y1 = InvertPass01DbrSchedule(y0, effective_profile);
  ASSERT_FALSE(y1.rpo_blocks.empty()) << "Empty Pass01 CFG on " << tc.test_name;
  const RtlControlFlowGraph y2 = InvertPass02Jump2CrossJump(y1, effective_profile);
  ASSERT_FALSE(y2.rpo_blocks.empty()) << "Empty Pass02 CFG on " << tc.test_name;
  const RtlControlFlowGraph fresh_greg = InvertPass03Sched2PostReload(y2, effective_profile);
  ASSERT_FALSE(fresh_greg.rpo_blocks.empty()) << "Empty Pass03 .09.greg CFG on " << tc.test_name;

  const RtlControlFlowGraph lifted_pre_alter = InvertAlterReg(fresh_greg, effective_profile, 72);
  ASSERT_FALSE(lifted_pre_alter.rpo_blocks.empty()) << tc.test_name;
  const absl::StatusOr<RtlControlFlowGraph> lifted_pre_reload_or =
      InvertReload(lifted_pre_alter, effective_profile);
  ASSERT_TRUE(lifted_pre_reload_or.ok())
      << "Fresh InvertReload failed on " << tc.test_name << ": " << lifted_pre_reload_or.status();
  const absl::StatusOr<RtlControlFlowGraph> lifted_lreg_or =
      InvertGlobalAlloc(*lifted_pre_reload_or, effective_profile);
  ASSERT_TRUE(lifted_lreg_or.ok())
      << "Fresh InvertGlobalAlloc failed on " << tc.test_name << ": " << lifted_lreg_or.status();
  const RtlControlFlowGraph& lifted_lreg = *lifted_lreg_or;
  ASSERT_FALSE(lifted_lreg.rpo_blocks.empty()) << tc.test_name;
  EXPECT_EQ(lifted_lreg.pass, RtlPassKind::kLreg) << tc.test_name;

  for (const auto& [pseudo, hard] : lifted_lreg.sideband.reg_renumber) {
    EXPECT_GE(pseudo, kMipsFirstPseudoRegister) << tc.test_name;
    EXPECT_GE(hard, 0) << tc.test_name;
    EXPECT_LT(hard, kMipsFirstPseudoRegister) << tc.test_name;
  }

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(effective_profile);
  regclass_init();
  ctx->MaterializeFromCfg(lifted_lreg);
  {
    std::string bad_insn;
    ASSERT_TRUE(hooks->ValidateMaterializedInsnsRecognizable(&bad_insn))
        << "Unrecognizable fresh lifted_lreg insn on " << tc.test_name << ": " << bad_insn;
  }

  hooks->ApplyCompilationProfile(effective_profile);
  const RightInverseResult fresh_ga_res =
      verifier.VerifyGlobalAllocRightInverse(*lifted_pre_reload_or);
  EXPECT_TRUE(fresh_ga_res.matches) << "Fresh VerifyGlobalAllocRightInverse failed on "
                                    << tc.test_name << ": " << fresh_ga_res.diff_detail;

  constexpr SubfunctionId kReplaySubs[] = {
      SubfunctionId::kGlobalAlloc,
      SubfunctionId::kReload,
      SubfunctionId::kAlterReg,
      SubfunctionId::kScheduleBlockSched2,
      SubfunctionId::kDoCrossJump,
      SubfunctionId::kFillSimpleDelaySlots,
      SubfunctionId::kFillEagerDelaySlots,
      SubfunctionId::kRelaxDelaySlots,
      SubfunctionId::kMakeReturnInsns,
  };
  RtlControlFlowGraph current_cfg = lifted_lreg;
  for (SubfunctionId sub : kReplaySubs) {
    if (sub == SubfunctionId::kGlobalAlloc) {
      regclass_init();
    }
    current_cfg = hooks
                      ->ExecuteSubfunctionBoundary(
                          sub, current_cfg, {}, effective_profile.optimize_level,
                          effective_profile.c_flags, effective_profile.gp_symbol_sizes)
                      .post_state;
  }

  hooks->ApplyCompilationProfile(effective_profile);
  ctx->MaterializeFromCfg(current_cfg);
  {
    std::string bad_insn;
    ASSERT_TRUE(hooks->ValidateMaterializedInsnsRecognizable(&bad_insn))
        << "Unrecognizable post-replay RTL insn on " << tc.test_name << ": " << bad_insn;
  }
  const std::string combined_asm = hooks->RunFinal();
  const Stage0FunctionInput actual_stage0 = hooks->RunGasMdAssembleWithMetadata(combined_asm);
  const std::string stage0_diff =
      CompareStage0ReplayResult(stage0_input, actual_stage0, combined_asm);
  EXPECT_TRUE(stage0_diff.empty())
      << "Fresh Pass00..InvertGlobalAlloc -> forward Stage0 replay mismatch on " << tc.test_name
      << ":\n"
      << stage0_diff;
}

TEST_P(InvertGlobalAllocCorpusTest, RoundTripAll4172CasesAndOracleFreeSidebandStripped) {
  const RoundTripTestCase& tc = GetParam();
  ctx->BeginFunction("harness_init");
  hooks->ApplyCompilationProfile({2, tc.options.c_flags, {}});
  regclass_init();
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);

  GlobalAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  bool found = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kGlobalAlloc) {
      found = true;
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << "Failed on " << tc.test_name << ": " << vres.diff_detail;

      // Oracle-free sideband-stripped verification: strip all pre-existing
      // compiler liveness maps (preserving only zero-occurrence phantom pseudos
      // if any exist) and verify `InvertGlobalAlloc` + `kGlobalAlloc` replay
      // still matches bit-for-bit.
      RtlControlFlowGraph stripped = snap.post_state;
      RtlSidebandState clean_sb = stripped.sideband;
      clean_sb.basic_block_live_at_start.clear();
      clean_sb.reg_n_refs.clear();
      clean_sb.reg_n_sets.clear();
      clean_sb.reg_n_deaths.clear();
      clean_sb.reg_live_length.clear();
      clean_sb.reg_n_calls_crossed.clear();
      for (const auto& [r, refs] : snap.post_state.sideband.reg_n_refs) {
        if (r < kMipsFirstPseudoRegister || refs <= 0) {
          continue;
        }
        if (auto ren_it = snap.post_state.sideband.reg_renumber.find(r);
            ren_it != snap.post_state.sideband.reg_renumber.end() && ren_it->second >= 0) {
          continue;
        }
        if (auto set_it = snap.post_state.sideband.reg_n_sets.find(r);
            set_it != snap.post_state.sideband.reg_n_sets.end() && set_it->second != 0) {
          continue;
        }
        clean_sb.reg_n_refs[r] = refs;
        if (auto d_it = snap.post_state.sideband.reg_n_deaths.find(r);
            d_it != snap.post_state.sideband.reg_n_deaths.end()) {
          clean_sb.reg_n_deaths[r] = d_it->second;
        }
        if (auto l_it = snap.post_state.sideband.reg_live_length.find(r);
            l_it != snap.post_state.sideband.reg_live_length.end()) {
          clean_sb.reg_live_length[r] = l_it->second;
        }
      }
      stripped.sideband = clean_sb;

      CompilationProfile snap_profile = trace.stage0_input.profile;
      snap_profile.optimize_level = snap.optimize_level;
      absl::StatusOr<RtlControlFlowGraph> lifted_stripped =
          InvertGlobalAlloc(stripped, snap_profile);
      ASSERT_TRUE(lifted_stripped.ok()) << tc.test_name << ": " << lifted_stripped.status();
      hooks->ApplyCompilationProfile(snap_profile);
      regclass_init();
      const SubfunctionBoundarySnapshot replayed =
          hooks->ExecuteSubfunctionBoundary(snap, *lifted_stripped);
      const RtlCfgPassComparison cmp = replayed.post_state.CompareAgainst(snap.post_state);
      EXPECT_TRUE(cmp.MatchesExactly())
          << "Stripped CFG mismatch on " << tc.test_name << ": " << cmp.mismatch_detail;
      EXPECT_EQ(replayed.post_state.sideband.reg_renumber, snap.post_state.sideband.reg_renumber)
          << "Stripped reg_renumber mismatch on " << tc.test_name;
    }
  }
  EXPECT_TRUE(found) << "Missing kGlobalAlloc snapshot in " << tc.test_name;

  VerifyFreshCumulativeCompositionAndReplay(ctx, hooks, verifier, tc, trace.stage0_input);
}

INSTANTIATE_TEST_SUITE_P(AllCases, InvertGlobalAllocCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
