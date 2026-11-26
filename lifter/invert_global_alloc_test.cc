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
  greg_cfg.sideband.reg_renumber[75] = 64;

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

TEST_F(InvertGlobalAllocTest, Sched1LockedLiveLengthPreservesAllocnoOrderAcrossSched1WriteBack) {
  // Mirrors `sched.c:4911-4966` and `global.c:760, 1078-1095`: when `-O2`
  // (`flag_schedule_insns`) runs, `schedule_insns` overwrites positive
  // `reg_live_length[r]` with `sched_reg_live_length[r]` while preserving
  // `reg_n_refs[r]`. Verify `InvertGlobalAlloc` reconciles inverted global
  // allocno priorities strictly via `reg_n_refs` (preserving `reg_live_length`)
  // so genuine GCC `sched1 -> local_alloc -> global_alloc` reproduces target
  // `reg_renumber` for both non-call-crossing and call-crossing callee-saved
  // allocnos.
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  ctx->BeginFunction("harness_init");
  hooks->ApplyCompilationProfile(o2_prof);
  regclass_init();

  // Case 1: Two non-call-crossing loop-carried pseudos (`72`, `73`) with equal
  // natural `(reg_n_refs, reg_live_length)` where target assignment puts `73`
  // ahead of `72` (`73 -> $3`, `72 -> $5`).
  RtlControlFlowGraph loop_cfg;
  loop_cfg.pass = RtlPassKind::kGreg;
  loop_cfg.sideband.max_regno = 75;
  loop_cfg.sideband.reg_renumber = {{72, 5}, {73, 3}, {74, 2}};

  RtlBasicBlock lb0;
  lb0.id = 0;
  lb0.successors = {2, 1};
  lb0.AddPatternOnly("(set (reg:SI 72) (const_int 0))");
  lb0.AddPatternOnly("(set (reg:SI 73) (const_int 0))");
  lb0.AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 4 a0) (const_int 0)) (label_ref L2) (pc)))");
  loop_cfg.rpo_blocks.push_back(std::move(lb0));

  RtlBasicBlock lb1;
  lb1.id = 1;
  lb1.has_label = true;
  lb1.successors = {1, 2};
  lb1.AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))");
  lb1.AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 73) (const_int 2)))");
  lb1.AddPatternOnly("(set (reg:SI 74) (lt:SI (reg:SI 72) (reg:SI 4 a0)))");
  lb1.AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 74) (const_int 0)) (label_ref L1) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))");
  loop_cfg.rpo_blocks.push_back(std::move(lb1));

  RtlBasicBlock lb2;
  lb2.id = 2;
  lb2.has_label = true;
  lb2.AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))");
  lb2.AddPatternOnly("(use (reg/i:SI 2 v0))");
  loop_cfg.rpo_blocks.push_back(std::move(lb2));

  absl::StatusOr<RtlControlFlowGraph> lifted_loop = InvertGlobalAlloc(loop_cfg, o2_prof);
  ASSERT_TRUE(lifted_loop.ok()) << lifted_loop.status();
  ASSERT_TRUE(lifted_loop->sideband.reg_live_length.contains(72));
  ASSERT_TRUE(lifted_loop->sideband.reg_live_length.contains(73));
  // Both pseudos have identical `sched_reg_live_length` (7); locked-length
  // reconciliation must preserve `reg_live_length[72] == reg_live_length[73]`
  // and separate priorities via `reg_n_refs`.
  EXPECT_EQ(lifted_loop->sideband.reg_live_length.at(72),
            lifted_loop->sideband.reg_live_length.at(73));
  EXPECT_GT(lifted_loop->sideband.reg_n_refs.at(73), lifted_loop->sideband.reg_n_refs.at(72));

  // Replay through genuine GCC `sched1 -> local_alloc -> global_alloc` and
  // confirm `sched1` write-back preserves `{72: 5, 73: 3}`.
  RtlControlFlowGraph pre_sched = *lifted_loop;
  pre_sched.sideband.reg_renumber.clear();
  pre_sched.pass = RtlPassKind::kCombine;
  const SubfunctionBoundarySnapshot s1_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kScheduleBlockSched1, pre_sched, {}, o2_prof.optimize_level, o2_prof.c_flags);
  const SubfunctionBoundarySnapshot ba_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kBlockAlloc, s1_snap.post_state, {}, o2_prof.optimize_level, o2_prof.c_flags);
  const SubfunctionBoundarySnapshot ga_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kGlobalAlloc, ba_snap.post_state, {}, o2_prof.optimize_level, o2_prof.c_flags);
  ASSERT_TRUE(ga_snap.post_state.sideband.reg_renumber.contains(72));
  ASSERT_TRUE(ga_snap.post_state.sideband.reg_renumber.contains(73));
  EXPECT_EQ(ga_snap.post_state.sideband.reg_renumber.at(72), 5);
  EXPECT_EQ(ga_snap.post_state.sideband.reg_renumber.at(73), 3);

  // Case 2: Two call-crossing callee-saved pseudos (`72 -> $17`, `73 -> $16`)
  // where `72` naturally has `reg_n_refs > 4 * calls_crossed` and `73` has
  // `reg_n_refs <= 4 * calls_crossed`. Per `global.c:1078-1095`, callee-saved
  // targets in `pref_class` never reach `CALLER_SAVE_PROFITABLE`, so
  // `InvertGlobalAlloc` must keep `reg_live_length` locked and raise
  // `reg_n_refs[73]` (or lower `reg_n_refs[72]`) across `4 * calls_crossed`.
  RtlControlFlowGraph call_cfg;
  call_cfg.pass = RtlPassKind::kGreg;
  call_cfg.sideband.outgoing_args_size = 16;
  call_cfg.sideband.saved_gpr_regnos = {16, 17, 31};
  call_cfg.sideband.max_regno = 74;
  call_cfg.sideband.reg_renumber = {{72, 17}, {73, 16}};

  RtlBasicBlock cb0;
  cb0.id = 0;
  cb0.successors = {1};
  cb0.AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  cb0.AddPatternOnly("(set (reg:SI 73) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  cb0.AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))");
  cb0.AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 2)))");
  RtlInsn call_ins;
  call_ins.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Sink\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_ins.has_explicit_call_fusage = true;
  cb0.AddInsn(std::move(call_ins));
  call_cfg.rpo_blocks.push_back(std::move(cb0));

  RtlBasicBlock cb1;
  cb1.id = 1;
  cb1.has_label = true;
  cb1.AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 72) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  cb1.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 72))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  cb1.AddPatternOnly("(use (reg/i:SI 2 v0))");
  call_cfg.rpo_blocks.push_back(std::move(cb1));

  absl::StatusOr<RtlControlFlowGraph> lifted_call = InvertGlobalAlloc(call_cfg, o2_prof);
  ASSERT_TRUE(lifted_call.ok()) << lifted_call.status();
  RtlControlFlowGraph pre_call_sched = *lifted_call;
  pre_call_sched.sideband.reg_renumber.clear();
  pre_call_sched.pass = RtlPassKind::kCombine;
  const SubfunctionBoundarySnapshot cs_s1 =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kScheduleBlockSched1, pre_call_sched, {},
                                        o2_prof.optimize_level, o2_prof.c_flags);
  EXPECT_EQ(cs_s1.post_state.sideband.reg_live_length.at(72),
            lifted_call->sideband.reg_live_length.at(72));
  EXPECT_EQ(cs_s1.post_state.sideband.reg_live_length.at(73),
            lifted_call->sideband.reg_live_length.at(73));
  const SubfunctionBoundarySnapshot cs_ba = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kBlockAlloc, cs_s1.post_state, {}, o2_prof.optimize_level, o2_prof.c_flags);
  const SubfunctionBoundarySnapshot cs_ga = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kGlobalAlloc, cs_ba.post_state, {}, o2_prof.optimize_level, o2_prof.c_flags);
  ASSERT_TRUE(cs_ga.post_state.sideband.reg_renumber.contains(72));
  ASSERT_TRUE(cs_ga.post_state.sideband.reg_renumber.contains(73));
  EXPECT_EQ(cs_ga.post_state.sideband.reg_renumber.at(72), 17);
  EXPECT_EQ(cs_ga.post_state.sideband.reg_renumber.at(73), 16);
}

TEST_F(InvertGlobalAllocTest, Sched1PhantomAllocnoUnlockedLengthPreservesCfgAllocnoLockedLengths) {
  // Mirrors `sched.c:4915-4947` + `global.c:1115-1156`: a zero-occurrence
  // spilled phantom pseudo (`75`) is never touched by `sched_reg_live_length`,
  // so keeping its `reg_live_length` unlocked lets it satisfy tight density
  // caps without forcing real CFG allocnos (`72`, `73`) to fall back to
  // unlocked lengths across `sched1`.
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  ctx->BeginFunction("harness_init");
  hooks->ApplyCompilationProfile(o2_prof);
  regclass_init();

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.max_regno = 76;
  cfg.sideband.reg_renumber = {{72, 5}, {73, 3}, {74, 2}, {75, -1}};
  // Zero-occurrence spilled phantom `75` with short length `3` (density > 0.33
  // if locked, colliding with a tight density cap) alongside real CFG pseudos
  // `72` and `73` whose natural priority order must swap (`73 -> $3`, `72 -> $5`).
  cfg.sideband.reg_n_refs[75] = 2;
  cfg.sideband.reg_n_sets[75] = 0;
  cfg.sideband.reg_n_deaths[75] = 0;
  cfg.sideband.reg_live_length[75] = 3;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.successors = {2, 1};
  bb0.AddPatternOnly("(set (reg:SI 72) (const_int 0))");
  bb0.AddPatternOnly("(set (reg:SI 73) (const_int 0))");
  bb0.AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 4 a0) (const_int 0)) (label_ref L2) (pc)))");
  cfg.rpo_blocks.push_back(std::move(bb0));

  RtlBasicBlock bb1;
  bb1.id = 1;
  bb1.has_label = true;
  bb1.successors = {1, 2};
  bb1.AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))");
  bb1.AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 73) (const_int 2)))");
  bb1.AddPatternOnly("(set (reg:SI 74) (lt:SI (reg:SI 72) (reg:SI 4 a0)))");
  bb1.AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 74) (const_int 0)) (label_ref L1) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))");
  cfg.rpo_blocks.push_back(std::move(bb1));

  RtlBasicBlock bb2;
  bb2.id = 2;
  bb2.has_label = true;
  bb2.AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))");
  bb2.AddPatternOnly("(use (reg/i:SI 2 v0))");
  cfg.rpo_blocks.push_back(std::move(bb2));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(cfg, o2_prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  ASSERT_TRUE(lifted->sideband.reg_live_length.contains(72));
  ASSERT_TRUE(lifted->sideband.reg_live_length.contains(73));
  // Real CFG allocnos keep their exact locked `sched_reg_live_length` and
  // separate priority strictly via `reg_n_refs`.
  EXPECT_EQ(lifted->sideband.reg_live_length.at(72), lifted->sideband.reg_live_length.at(73));
  EXPECT_GT(lifted->sideband.reg_n_refs.at(73), lifted->sideband.reg_n_refs.at(72));

  RtlControlFlowGraph pre_sched = *lifted;
  pre_sched.sideband.reg_renumber.clear();
  pre_sched.pass = RtlPassKind::kCombine;
  const SubfunctionBoundarySnapshot s1_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kScheduleBlockSched1, pre_sched, {}, o2_prof.optimize_level, o2_prof.c_flags);
  EXPECT_EQ(s1_snap.post_state.sideband.reg_live_length.at(72),
            lifted->sideband.reg_live_length.at(72));
  EXPECT_EQ(s1_snap.post_state.sideband.reg_live_length.at(73),
            lifted->sideband.reg_live_length.at(73));
  const SubfunctionBoundarySnapshot ba_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kBlockAlloc, s1_snap.post_state, {}, o2_prof.optimize_level, o2_prof.c_flags);
  const SubfunctionBoundarySnapshot ga_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kGlobalAlloc, ba_snap.post_state, {}, o2_prof.optimize_level, o2_prof.c_flags);
  EXPECT_EQ(ga_snap.post_state.sideband.reg_renumber.at(72), 5);
  EXPECT_EQ(ga_snap.post_state.sideband.reg_renumber.at(73), 3);
}

TEST_F(InvertGlobalAllocTest, BlockAllocModelTiesOperandZeroNotSecondSetInDivModParallel) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // Negative (`REG_UNUSED` on operand 0 `div` dest): `local-alloc.c:1240` ties
  // `recog_operand[0]`, NOT the second `SET` (`mod`, operand 3). Thus `r73`
  // (`renum = 3`, dying in `divmodsi4`) must NOT be combined with `r74`
  // (`renum = 14`, operand 3), so `r72`/`r73`/`r74` stay local in `reg_renumber`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 72) (and:SI (reg:SI 4 a0) (const_int 127))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 3))) "
                "(expr_list:REG_DEAD (reg:SI 72) (nil))",
                "(parallel[ (set (reg:SI 1 at) (div:SI (reg:SI 5 a1) (reg:SI 73))) "
                "(set (reg:SI 74) (mod:SI (reg:SI 5 a1) (reg:SI 73))) "
                "(clobber (reg:SI 65 lo)) (clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 73) "
                "(expr_list:REG_UNUSED (reg:SI 1 at) (expr_list:REG_UNUSED (reg:SI 66 accum) "
                "(expr_list:REG_UNUSED (reg:SI 64 hi) (expr_list:REG_UNUSED (reg:SI 65 lo) "
                "(nil)))))))",
                "(set (reg/i:SI 2 v0) (reg:SI 74)) (expr_list:REG_DEAD (reg:SI 74) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    cfg.sideband.max_regno = 75;
    cfg.sideband.reg_renumber = {{72, 3}, {73, 3}, {74, 14}};
    const absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(cfg, o2_prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    ASSERT_TRUE(lifted->sideband.reg_renumber.contains(72));
    ASSERT_TRUE(lifted->sideband.reg_renumber.contains(73));
    ASSERT_TRUE(lifted->sideband.reg_renumber.contains(74));
    EXPECT_EQ(lifted->sideband.reg_renumber.at(72), 3);
    EXPECT_EQ(lifted->sideband.reg_renumber.at(73), 3);
    EXPECT_EQ(lifted->sideband.reg_renumber.at(74), 14);
  }

  // Positive (live pseudo on operand 0 `div` dest): `local-alloc.c:1240` DOES
  // combine dead input `r73` (`renum = 3`) with operand 0 `r74` (`renum = 14`),
  // so quantity atomicity detects the incompatible hard regs and erases the
  // quantity from `reg_renumber`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 72) (and:SI (reg:SI 4 a0) (const_int 127))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 3))) "
                "(expr_list:REG_DEAD (reg:SI 72) (nil))",
                "(parallel[ (set (reg:SI 74) (div:SI (reg:SI 73) (reg:SI 5 a1))) "
                "(set (reg:SI 1 at) (mod:SI (reg:SI 73) (reg:SI 5 a1))) "
                "(clobber (reg:SI 65 lo)) (clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 73) "
                "(expr_list:REG_UNUSED (reg:SI 1 at) (expr_list:REG_UNUSED (reg:SI 66 accum) "
                "(expr_list:REG_UNUSED (reg:SI 64 hi) (expr_list:REG_UNUSED (reg:SI 65 lo) "
                "(nil)))))))",
                "(set (reg/i:SI 2 v0) (reg:SI 74)) (expr_list:REG_DEAD (reg:SI 74) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    cfg.sideband.max_regno = 75;
    cfg.sideband.reg_renumber = {{72, 3}, {73, 3}, {74, 14}};
    const absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(cfg, o2_prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    EXPECT_FALSE(lifted->sideband.reg_renumber.contains(72));
    EXPECT_FALSE(lifted->sideband.reg_renumber.contains(73));
    EXPECT_FALSE(lifted->sideband.reg_renumber.contains(74));
  }
}

TEST_F(InvertGlobalAllocTest, LocalAllocMdClobberIgnoresAccumOnlyWideningMultButHonorsHardHiLo) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // Mirrors `local-alloc.c:1381` (`note_stores`) + `config/mips/mips.md:963-1024`:
  // a pseudo-destination widening `mult:DI` (`mulsidi3`/`umulsidi3`) only
  // clobbers `(reg:SI 66 accum)`, NOT hard regs `64 hi`/`65 lo`. Therefore:
  // - Positive: a spilled single-block widening `mult:DI` (`r79`,
  //   `reg_renumber[79] == -1`) whose live range spans a second pseudo-dest
  //   widening `mult:DI` (`r80`, `renum = 4`) is restored to `reg_renumber[79] = 64`
  //   (`hi`) by `InvertGlobalAlloc` (since `local_alloc` sees `hi`/`lo` free).
  // - Negative: when an explicit `(clobber (reg:SI 64 hi))` intervenes across
  //   `r79`'s live range, `local_alloc` marks `hi` live, so `r79` is NOT
  //   restored to `64`.
  auto build_cfg = [](bool explicit_hi_clobber) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    std::vector<std::string> insns = {
        "(parallel[ (set (reg:DI 79) (mult:DI (sign_extend:DI (reg:SI 4 a0)) "
        "(sign_extend:DI (reg:SI 5 a1)))) (clobber (reg:SI 66 accum)) ] ) "
        "(expr_list:REG_UNUSED (reg:SI 66 accum) (nil))",
    };
    if (explicit_hi_clobber) {
      insns.push_back(
          "(parallel[ (set (reg:SI 80) (div:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
          "(set (reg:SI 1 at) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
          "(clobber (reg:SI 65 lo)) (clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
          "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 5 a1) "
          "(expr_list:REG_UNUSED (reg:SI 1 at) (expr_list:REG_UNUSED (reg:SI 66 accum) "
          "(expr_list:REG_UNUSED (reg:SI 64 hi) (expr_list:REG_UNUSED (reg:SI 65 lo) "
          "(nil)))))))");
      insns.push_back(
          "(parallel[ (set (reg/i:DI 2 v0) (plus:DI (reg:DI 79) (sign_extend:DI (reg:SI 80)))) "
          "(clobber (reg:SI 81)) ] ) "
          "(expr_list:REG_DEAD (reg:DI 79) (expr_list:REG_DEAD (reg:SI 80) "
          "(expr_list:REG_UNUSED (reg:SI 81) (nil))))");
    } else {
      insns.push_back(
          "(parallel[ (set (reg:DI 80) (mult:DI (zero_extend:DI (reg:SI 4 a0)) "
          "(zero_extend:DI (reg:SI 5 a1)))) (clobber (reg:SI 66 accum)) ] ) "
          "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 5 a1) "
          "(expr_list:REG_UNUSED (reg:SI 66 accum) (nil))))");
      insns.push_back(
          "(parallel[ (set (reg/i:DI 2 v0) (plus:DI (reg:DI 79) (reg:DI 80))) "
          "(clobber (reg:SI 81)) ] ) "
          "(expr_list:REG_DEAD (reg:DI 79) (expr_list:REG_DEAD (reg:DI 80) "
          "(expr_list:REG_UNUSED (reg:SI 81) (nil))))");
    }
    insns.push_back("(use (reg/i:DI 2 v0))");
    cfg.rpo_blocks.push_back(RtlBasicBlock{.id = 0, .instructions = std::move(insns)});
    cfg.sideband.max_regno = 82;
    cfg.sideband.reg_renumber = {{80, 4}, {81, 6}};
    return cfg;
  };

  {
    const absl::StatusOr<RtlControlFlowGraph> pos =
        InvertGlobalAlloc(build_cfg(/*explicit_hi_clobber=*/false), o2_prof);
    ASSERT_TRUE(pos.ok()) << pos.status();
    ASSERT_TRUE(pos->sideband.reg_renumber.contains(79));
    EXPECT_EQ(pos->sideband.reg_renumber.at(79), 64);
  }
  {
    const absl::StatusOr<RtlControlFlowGraph> neg =
        InvertGlobalAlloc(build_cfg(/*explicit_hi_clobber=*/true), o2_prof);
    ASSERT_TRUE(neg.ok()) << neg.status();
    const bool assigned_hi =
        neg->sideband.reg_renumber.contains(79) && neg->sideband.reg_renumber.at(79) == 64;
    EXPECT_FALSE(assigned_hi);
  }
}

TEST_F(InvertGlobalAllocTest, PreservesReloadHardRegUseOrderAcrossPhasesOnlyWhenClassExhausted) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // Mirrors `reload1.c:3606-3712` (`order_regs_for_reload`, `hard_reg_use_compare`):
  // When local `block_alloc` reconciliation bumps local `r90` (`$f0`, originally
  // 2 refs) above global `r72` (`$f20`, originally 3 refs),
  // `PreserveReloadHardRegUseOrderAcrossPhases` uniformly scales global
  // `FP_REGS` `reg_n_refs` iff `FP_REGS` has `< 2` unused even hard registers
  // (positive), and leaves global `reg_n_refs` unscaled when `>= 2` even FPRs
  // are unused (negative).
  auto build_cfg = [](bool exhaust_fp_class) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    std::vector<std::string> b0_insns = {
        "(set (reg:SF 72) (mem:SF (reg:SI 4 a0)))",
    };
    absl::flat_hash_map<int, int> renum = {{72, 52}};
    int next_p = 73;
    if (exhaust_fp_class) {
      // Occupy all remaining even FPRs (`$f4..$f18`, `$f22..$f30`) in B0 so
      // `free_fp == 0 < 2` (`$f0` and `$f2` are occupied by `r90`/`r91` in B1),
      // without changing `r72`'s own reference count (`3`).
      for (int hr : {36, 38, 40, 42, 44, 46, 48, 50, 54, 56, 58, 60, 62}) {
        const int p = next_p++;
        const int off = (p - 72) * 4;
        b0_insns.push_back("(set (reg:SF " + std::to_string(p) +
                           ") (mem:SF (plus:SI (reg:SI 4 a0) (const_int " + std::to_string(off) +
                           "))))");
        b0_insns.push_back("(set (mem:SF (plus:SI (reg:SI 5 a1) (const_int " + std::to_string(off) +
                           "))) (reg:SF " + std::to_string(p) + ")) (expr_list:REG_DEAD (reg:SF " +
                           std::to_string(p) + ") (nil))");
        renum[p] = hr;
      }
    }
    b0_insns.push_back(
        "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");

    // In B1, `r90` (span 5, target `$f0=32`) and `r91` (span 2, target `$f2=34`)
    // overlap; because `r91` has shorter span, `local_alloc` would give `$f0` to
    // `r91` unless `ReconcileSingleBlockLocalPriorities` bumps `r90`'s `n_refs`
    // above `r91` (and above global `r72`'s natural `orig_refs == 3`).
    std::vector<std::string> b1_insns = {
        "(set (reg:SF 90) (mem:SF (reg:SI 4 a0)))",
        "(set (reg:SF 91) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))))",
        "(set (reg:SF 92) (plus:SF (reg:SF 72) (reg:SF 91))) "
        "(expr_list:REG_DEAD (reg:SF 91) (nil))",
        "(set (reg:SF 92) (plus:SF (reg:SF 92) (reg:SF 72))) "
        "(expr_list:REG_DEAD (reg:SF 72) (nil))",
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 92) (reg:SF 90))) "
        "(expr_list:REG_DEAD (reg:SF 92) (expr_list:REG_DEAD (reg:SF 90) (nil)))",
        "(use (reg/i:SF 32 $f0))",
    };
    renum[90] = 32;
    renum[91] = 34;
    renum[92] = 36;

    RtlBasicBlock b0;
    b0.id = 0;
    b0.instructions = std::move(b0_insns);
    b0.successors = {1};
    RtlBasicBlock b1;
    b1.id = 1;
    b1.has_label = true;
    b1.instructions = std::move(b1_insns);
    cfg.rpo_blocks.push_back(std::move(b0));
    cfg.rpo_blocks.push_back(std::move(b1));
    cfg.sideband.max_regno = 93;
    cfg.sideband.reg_renumber = std::move(renum);
    return cfg;
  };

  // Positive: `FP_REGS` exhausted (`free_fp == 0 < 2`). `r90` (`$f0`) is bumped
  // above `r91`, and global `r72` (`$f20`, originally 3 refs > 2) is scaled so
  // `refs[72] > refs[90]`.
  const absl::StatusOr<RtlControlFlowGraph> pos =
      InvertGlobalAlloc(build_cfg(/*exhaust_fp_class=*/true), o2_prof);
  ASSERT_TRUE(pos.ok()) << pos.status();
  ASSERT_TRUE(pos->sideband.reg_n_refs.contains(72));
  ASSERT_TRUE(pos->sideband.reg_n_refs.contains(90));
  EXPECT_GT(pos->sideband.reg_n_refs.at(90), 2);
  EXPECT_GT(pos->sideband.reg_n_refs.at(72), pos->sideband.reg_n_refs.at(90));

  // Negative: `FP_REGS` NOT exhausted (`free_fp >= 2`). Even when `r90` (`$f0`)
  // is bumped above `r91`, global `r72` (`$f20`) stays at its natural ref count
  // (`3`) without uniform class scaling.
  const absl::StatusOr<RtlControlFlowGraph> neg =
      InvertGlobalAlloc(build_cfg(/*exhaust_fp_class=*/false), o2_prof);
  ASSERT_TRUE(neg.ok()) << neg.status();
  ASSERT_TRUE(neg->sideband.reg_n_refs.contains(72));
  ASSERT_TRUE(neg->sideband.reg_n_refs.contains(90));
  EXPECT_GT(neg->sideband.reg_n_refs.at(90), 2);
  EXPECT_EQ(neg->sideband.reg_n_refs.at(72), 3);
  EXPECT_GT(pos->sideband.reg_n_refs.at(72), neg->sideband.reg_n_refs.at(72));
}

TEST_F(InvertGlobalAllocTest,
       ErasesPreferencePoisoningLocalPseudoOnlyWhenReducingGlobalMismatches) {
  const CompilationProfile o1_prof{1, {"-O1", "-mips2", "-Wa,-g"}, {}};
  // Mirrors `global.c:1535-1605` (`set_preference`): global pseudo `r72`
  // (`exp = $2 v0`) is copied from local pseudo `r73` (`$5 a1`). If `r73` stays
  // pre-allocated to `$5 a1` in `reg_renumber`, `set_preference` biases `r72`
  // toward `$5 a1` instead of `$2 v0`.
  // Positive (`r72` wants `$2` while `r73` holds `$5`): `InvertGlobalAlloc`
  // erases `r73` from `reg_renumber` so `global_alloc` allocates `r72` to `$2`
  // and `r73` to `$5` cleanly without mismatch.
  // Negative (`r72` wants `$5`, matching `r73` = `$5`): zero mismatches exist,
  // so `r73` retains its pre-allocated `reg_renumber[73] == 5`.
  auto build_cfg = [](int r72_target_hr) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.max_regno = 75;
    cfg.sideband.reg_renumber = {{72, r72_target_hr}, {73, 5}};
    RtlBasicBlock b0;
    b0.id = 0;
    b0.instructions = {
        "(set (reg:SI 2 v0) (const_int 10))",
        "(set (reg:SI 3 v1) (const_int 20))",
        "(set (reg:SI 4 a0) (const_int 30))",
        "(set (reg:SI 73) (plus:SI (reg:SI 16 s0) (const_int 4)))",
        "(set (mem:SI (reg:SI 16 s0)) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
        "(set (mem:SI (plus:SI (reg:SI 16 s0) (const_int 4))) (reg:SI 3 v1)) "
        "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
        "(set (mem:SI (plus:SI (reg:SI 16 s0) (const_int 8))) (reg:SI 4 a0)) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(set (reg:SI 72) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
        "(set (pc) (if_then_else (eq:SI (reg:SI 16 s0) (const_int 0)) (label_ref L1) (pc)))",
    };
    b0.successors = {2, 1};
    RtlBasicBlock b1;
    b1.id = 1;
    b1.instructions = {
        "(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))",
    };
    b1.successors = {2};
    RtlBasicBlock b2;
    b2.id = 2;
    b2.has_label = true;
    b2.instructions = {
        "(set (mem:SI (reg:SI 16 s0)) (reg:SI 72)) (expr_list:REG_DEAD (reg:SI 72) (nil))",
        "(set (reg/i:SI 2 v0) (const_int 0))",
        "(use (reg/i:SI 2 v0))",
    };
    cfg.rpo_blocks = {std::move(b0), std::move(b1), std::move(b2)};
    return cfg;
  };

  {
    const RtlControlFlowGraph cfg = build_cfg(/*r72_target_hr=*/2);
    const absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(cfg, o1_prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    EXPECT_FALSE(lifted->sideband.reg_renumber.contains(73))
        << "Expected preference-poisoning local r73 ($5) to be erased for global r72 ($2)";
    EXPECT_FALSE(lifted->sideband.reg_renumber.contains(72));
  }
  {
    const RtlControlFlowGraph cfg = build_cfg(/*r72_target_hr=*/5);
    const absl::StatusOr<RtlControlFlowGraph> lifted = InvertGlobalAlloc(cfg, o1_prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    ASSERT_TRUE(lifted->sideband.reg_renumber.contains(73));
    EXPECT_EQ(lifted->sideband.reg_renumber.at(73), 5)
        << "Expected aligned local r73 ($5) to be preserved when global r72 also wants $5";
  }
}

TEST_F(InvertGlobalAllocTest,
       LocalBlockAllocModelHandlesPseudoSubregBirthNoConflictCombineAndCase3Order) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  // 1. `local-alloc.c:1317-1352, 1958-1985` + `rtlanal.c:note_stores`:
  //    `(set (subreg:SI (reg:DI 74) 1) ...)` must NOT offset pseudo 74 to 75,
  //    and `(clobber (reg:DI 73)) (REG_LIBCALL)` + `(REG_NO_CONFLICT 72)` +
  //    `(set (reg:DI 73) (reg:DI 73)) (REG_RETVAL) (REG_EQUAL (sign_extend:DI 72))`
  //    combines 72 and 73 into one quantity without corrupting `reg_n_refs[75]`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.max_regno = 76;
    cfg.sideband.reg_renumber = {{72, 16}, {73, 16}, {74, 18}, {75, 20}};
    cfg.sideband.reg_n_refs = {{72, 3}, {73, 4}, {74, 5}, {75, 2}};
    cfg.sideband.reg_n_sets = {{72, 1}, {73, 3}, {74, 3}, {75, 1}};
    cfg.sideband.reg_n_deaths = {{72, 1}, {73, 1}, {74, 1}, {75, 1}};
    cfg.sideband.reg_n_calls_crossed = {{72, 1}, {73, 0}, {74, 1}, {75, 2}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:SI 75) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
        "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"FnA\"))) (const_int 16))) "
        "(clobber (reg:SI 31 ra)) ] )",
        "(clobber (reg:DI 73)) (insn_list:REG_LIBCALL (nil))",
        "(set (subreg:SI (reg:DI 73) 1) (reg:SI 72)) (expr_list:REG_NO_CONFLICT (reg:SI 72) (nil))",
        "(set (subreg:SI (reg:DI 73) 0) (ashiftrt:SI (reg:SI 72) (const_int 31))) "
        "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_NO_CONFLICT (reg:SI 72) (nil)))",
        "(set (reg:DI 73) (reg:DI 73)) "
        "(insn_list:REG_RETVAL (expr_list:REG_EQUAL (sign_extend:DI (reg:SI 72)) (nil)))",
        "(clobber (reg:DI 74)) (insn_list:REG_LIBCALL (nil))",
        "(set (subreg:SI (reg:DI 74) 1) (reg:SI 2 v0))",
        "(set (subreg:SI (reg:DI 74) 0) (ashiftrt:SI (reg:SI 2 v0) (const_int 31))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
        "(set (reg:SI 4 a0) (reg:SI 75)) (expr_list:REG_DEAD (reg:SI 75) (nil))",
        "(set (reg:DI 6 a2) (reg:DI 73)) (expr_list:REG_DEAD (reg:DI 73) (nil))",
        "(parallel[ (set (reg:DI 2 v0) (call (mem:SI (symbol_ref:SI (\"FnB\"))) (const_int 16))) "
        "(clobber (reg:SI 31 ra)) ] )",
        "(set (reg/i:DI 2 v0) (xor:DI (reg:DI 2 v0) (reg:DI 74))) "
        "(expr_list:REG_DEAD (reg:DI 74) (nil))",
        "(use (reg/i:DI 2 v0))",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();

    const absl::StatusOr<RtlControlFlowGraph> pre_global = InvertGlobalAlloc(cfg, o2_prof);
    ASSERT_TRUE(pre_global.ok()) << pre_global.status();
    EXPECT_EQ(pre_global->sideband.reg_renumber.at(72), 16);
    EXPECT_EQ(pre_global->sideband.reg_renumber.at(73), 16);
    EXPECT_EQ(pre_global->sideband.reg_renumber.at(74), 18);
    EXPECT_EQ(pre_global->sideband.reg_renumber.at(75), 20);
    EXPECT_LE(pre_global->sideband.reg_n_refs.at(75), pre_global->sideband.reg_n_refs.at(74));
  }

  // 2. `local-alloc.c:1491-1505` (`case 3:`): with 3 local quantities in a block
  //    where `q2` (`r76`) is Pass-1 settled on `4 a0`, `q1` (`r74, r75 -> 2 v0`)
  //    only beats `q0` (`r72, r73 -> 3 v1`) when `pri(q2) > pri(q1) > pri(q0)`;
  //    `ReconcileSingleBlockLocalPriorities` must raise `reg_n_refs[76]` so `q2`
  //    breaks the double-swap of `case 3:` -> `case 2:`.
  {
    const CompilationProfile mips1_prof{.optimize_level = 2,
                                        .c_flags = {"-O2", "-mips1", "-Wa,-O2"}};
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.max_regno = 80;
    cfg.sideband.reg_renumber = {{72, 5}, {73, 3}, {74, 3}, {75, 2},
                                 {76, 2}, {77, 2}, {78, 8}, {79, 4}};
    cfg.sideband.reg_n_refs = {{72, 4}, {73, 2}, {74, 2}, {75, 2},
                               {76, 2}, {77, 2}, {78, 3}, {79, 2}};
    cfg.sideband.reg_n_sets = {{72, 1}, {73, 1}, {74, 1}, {75, 1},
                               {76, 1}, {77, 1}, {78, 1}, {79, 1}};
    cfg.sideband.reg_n_deaths = {{72, 1}, {73, 1}, {74, 1}, {75, 1},
                                 {76, 1}, {77, 1}, {78, 1}, {79, 1}};
    cfg.rpo_blocks.resize(2);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:SI 72) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
        "(set (reg:SI 73) (ashift:SI (reg:SI 4 a0) (const_int 1)))",
        "(set (reg:SI 74) (plus:SI (reg:SI 73) (reg:SI 4 a0))) "
        "(expr_list:REG_DEAD (reg:SI 73) (nil))",
        "(set (reg:SI 75) (ashift:SI (reg:SI 72) (const_int 3)))",
        "(set (reg:SI 76) (minus:SI (reg:SI 75) (reg:SI 72))) "
        "(expr_list:REG_DEAD (reg:SI 75) (nil))",
        "(set (reg:SI 77) (plus:SI (reg:SI 76) (reg:SI 74))) "
        "(expr_list:REG_DEAD (reg:SI 76) (expr_list:REG_DEAD (reg:SI 74) (nil)))",
        "(set (reg:SI 78) (ashift:SI (reg:SI 77) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 77) (nil))",
        "(set (reg:SI 79) (lt:SI (reg:SI 4 a0) (const_int 6))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(set (pc) (if_then_else (ne:SI (reg:SI 79) (const_int 0)) (label_ref L1) (pc))) "
        "(expr_list:REG_DEAD (reg:SI 79) (nil))",
    };
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].has_label = true;
    cfg.rpo_blocks[1].successors = {};
    cfg.rpo_blocks[1].instructions = {
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 78) (reg:SI 72))) "
        "(expr_list:REG_DEAD (reg:SI 78) (expr_list:REG_DEAD (reg:SI 72) (nil)))",
        "(use (reg/i:SI 2 v0))",
    };
    for (auto& blk : cfg.rpo_blocks) {
      blk.SyncRtlInsnsFromInstructions();
    }

    const absl::StatusOr<RtlControlFlowGraph> pre_global = InvertGlobalAlloc(cfg, mips1_prof);
    ASSERT_TRUE(pre_global.ok()) << pre_global.status();
    EXPECT_GT(pre_global->sideband.reg_n_refs.at(79), 2);
  }
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
