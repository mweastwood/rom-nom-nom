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
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_block_alloc.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"
#include "lifter/pass04_greg_reload.h"

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
// 25 subfunction methods as empty stubs (`SubfunctionInverseProvider` defaults).
class UpdateEquivRegsOnlyInverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertUpdateEquivRegs(x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x,
                                            const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res =
        ::rom_nom_nom::lifter::InvertUpdateEquivRegs(x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

// Corrupted inverse provider for negative right-inverse verification testing:
// corrupts the first instruction so replaying `kUpdateEquivRegs` fails to match.
class CorruptedUpdateEquivRegsInverseProvider final
    : public gcc_harness::SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x,
                                            const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res =
        ::rom_nom_nom::lifter::InvertUpdateEquivRegs(x, profile);
    if (!res.ok() || res->rpo_blocks.empty() || res->rpo_blocks[0].instructions.empty()) {
      return RtlControlFlowGraph{};
    }
    res->rpo_blocks[0].instructions[0] = "(set (reg/i:SI 2 v0) (const_int 999))";
    res->rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return *res;
  }
  using SubfunctionInverseProvider::InvertUpdateEquivRegs;
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
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

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

TEST_F(InvertUpdateEquivRegsTest,
       StripsStoreDerivedMemEquivAndLoadEquivAcrossNonAliasingStackStore) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kSched;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 30 fp) (const_int -8))))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int -16))) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (const_int 5))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_cfg.sideband.reg_n_sets[72] = 1;
  pre_cfg.sideband.reg_n_sets[73] = 1;
  pre_cfg.sideband.reg_live_length[72] = 3;
  pre_cfg.sideband.reg_live_length[73] = 3;

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, pre_cfg);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);
  EXPECT_EQ(snap.post_state.sideband.reg_live_length.at(72), 3);
  EXPECT_EQ(snap.post_state.sideband.reg_live_length.at(73), 6);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertUpdateEquivRegs(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.reg_live_length.at(72), 3);
  EXPECT_EQ(lifted->sideband.reg_live_length.at(73), 3);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int -8))))");

  const RightInverseResult res = verifier.VerifyUpdateEquivRegsRightInverse(snap.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;
}

TEST_F(InvertUpdateEquivRegsTest,
       StripsBothUnchangingLoadEquivAndSubsequentStoreEquivHalvingLiveLengthOnce) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

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

TEST_F(InvertUpdateEquivRegsTest, RestoresDeletedCrossBlockConstantAndStackParamEquivInitInsns) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  // Case 1: Cross-block constant equivalence (`reg_equiv_init_insn[72]` deleted in block 0,
  // substituted into block 2).
  RtlControlFlowGraph const_pre_cfg;
  const_pre_cfg.pass = RtlPassKind::kSched;
  const_pre_cfg.rpo_blocks.resize(3);
  const_pre_cfg.rpo_blocks[0].id = 0;
  const_pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (const_int 23)) (expr_list:REG_EQUAL (const_int 23) (nil))");
  const_pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L2) (pc)))");
  const_pre_cfg.rpo_blocks[0].successors = {2, 1};

  const_pre_cfg.rpo_blocks[1].id = 1;
  const_pre_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 1)))");
  const_pre_cfg.rpo_blocks[1].successors = {2};

  const_pre_cfg.rpo_blocks[2].id = 2;
  const_pre_cfg.rpo_blocks[2].has_label = true;
  const_pre_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 5 a1) (reg:SI 72))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  const_pre_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot const_flow =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, const_pre_cfg);
  const SubfunctionBoundarySnapshot const_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, const_flow.post_state);
  // Verify `update_equiv_regs` genuinely deleted the `equiv_insn` in block 0!
  ASSERT_EQ(const_snap.post_state.rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(const_snap.post_state.sideband.reg_n_sets.at(72), 1);
  EXPECT_FALSE(const_snap.post_state.sideband.reg_n_refs.contains(72));

  absl::StatusOr<RtlControlFlowGraph> lifted_const = InvertUpdateEquivRegs(const_snap.post_state);
  ASSERT_TRUE(lifted_const.ok()) << lifted_const.status();
  EXPECT_EQ(lifted_const->rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(lifted_const->sideband.reg_n_refs.at(72), 2);
  EXPECT_EQ(lifted_const->sideband.reg_n_deaths.at(72), 1);

  EXPECT_TRUE(verifier.VerifyUpdateEquivRegsRightInverse(const_snap.post_state).matches);
  EXPECT_TRUE(verifier.VerifySubfunctionBoundaryRightInverse(const_snap).matches);

  // Case 2: Cross-block incoming stack-parameter `MEM` equivalence (`assign_parms`)
  // where deleting `equiv_insn` in block 0 collapses block 0 into labeled block 1!
  RtlControlFlowGraph mem_pre_cfg;
  mem_pre_cfg.pass = RtlPassKind::kSched;
  mem_pre_cfg.rpo_blocks.resize(3);
  mem_pre_cfg.rpo_blocks[0].id = 0;
  mem_pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/v:SI 72) (mem:SI (plus:SI (reg:SI 0) (const_int 16)))) "
      "(expr_list:REG_EQUIV (mem:SI (plus:SI (reg:SI 0) (const_int 16))) (nil))");
  mem_pre_cfg.rpo_blocks[0].successors = {1};

  mem_pre_cfg.rpo_blocks[1].id = 1;
  mem_pre_cfg.rpo_blocks[1].has_label = true;
  mem_pre_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))");
  mem_pre_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))");
  mem_pre_cfg.rpo_blocks[1].successors = {1, 2};

  mem_pre_cfg.rpo_blocks[2].id = 2;
  mem_pre_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (reg/v:SI 72)) (expr_list:REG_DEAD (reg/v:SI 72) (nil))");
  mem_pre_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot mem_flow =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, mem_pre_cfg);
  const SubfunctionBoundarySnapshot mem_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, mem_flow.post_state);
  ASSERT_EQ(mem_snap.post_state.BlockCount(), 2u);
  EXPECT_TRUE(mem_snap.post_state.sideband.basic_block_live_at_start.at(0).contains(72));

  absl::StatusOr<RtlControlFlowGraph> lifted_mem = InvertUpdateEquivRegs(mem_snap.post_state);
  ASSERT_TRUE(lifted_mem.ok()) << lifted_mem.status();
  EXPECT_EQ(lifted_mem->BlockCount(), 3u);
  EXPECT_EQ(lifted_mem->sideband.reg_n_refs.at(72), 2);
  EXPECT_EQ(lifted_mem->sideband.reg_n_deaths.at(72), 1);

  const RightInverseResult mem_res =
      verifier.VerifyUpdateEquivRegsRightInverse(mem_snap.post_state);
  EXPECT_TRUE(mem_res.matches) << mem_res.diff_detail;
  const RightInverseResult mem_snap_res = verifier.VerifySubfunctionBoundaryRightInverse(mem_snap);
  EXPECT_TRUE(mem_snap_res.matches) << mem_snap_res.diff_detail;
}

TEST_F(InvertUpdateEquivRegsTest, ReversesMultiInsnOptimizeRegCopy1AndOptimizeRegCopy2) {
  UpdateEquivRegsOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  // 1. Multi-insn `optimize_reg_copy_1` with an intervening unrelated instruction.
  RtlControlFlowGraph copy1_pre;
  copy1_pre.pass = RtlPassKind::kSched;
  copy1_pre.rpo_blocks.resize(1);
  copy1_pre.rpo_blocks[0].id = 0;
  copy1_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  copy1_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (reg:SI 72))");
  copy1_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 74) (plus:SI (reg:SI 5 a1) (const_int 9)))");
  copy1_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 75) (plus:SI (reg:SI 72) (reg:SI 74)))");
  copy1_pre.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 75) (reg:SI 73)))");
  copy1_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot copy1_flow =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, copy1_pre);
  const SubfunctionBoundarySnapshot copy1_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, copy1_flow.post_state);
  absl::StatusOr<RtlControlFlowGraph> copy1_lifted = InvertUpdateEquivRegs(copy1_snap.post_state);
  ASSERT_TRUE(copy1_lifted.ok()) << copy1_lifted.status();
  EXPECT_EQ(copy1_lifted->rpo_blocks[0].instructions[1], "(set (reg:SI 73) (reg:SI 72))");
  EXPECT_TRUE(verifier.VerifyUpdateEquivRegsRightInverse(copy1_snap.post_state).matches);
  EXPECT_TRUE(verifier.VerifySubfunctionBoundaryRightInverse(copy1_snap).matches);

  // 2. `optimize_reg_copy_2`: `(set 73 72) (REG_DEAD 72) ; (set 73 (plus 73 3)) ;
  //    (set 72 73) (REG_DEAD 73)` collapsed into `(set 72 72) ... (set 72 72)`.
  RtlControlFlowGraph copy2_pre;
  copy2_pre.pass = RtlPassKind::kSched;
  copy2_pre.rpo_blocks.resize(1);
  copy2_pre.rpo_blocks[0].id = 0;
  copy2_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  copy2_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (reg:SI 72))");
  copy2_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 73) (const_int 3)))");
  copy2_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (reg:SI 73))");
  copy2_pre.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 72))");
  copy2_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot copy2_flow =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, copy2_pre);
  const SubfunctionBoundarySnapshot copy2_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kUpdateEquivRegs, copy2_flow.post_state);
  EXPECT_EQ(copy2_snap.post_state.rpo_blocks[0].instructions[1], "(set (reg:SI 72) (reg:SI 72))");
  EXPECT_EQ(copy2_snap.post_state.rpo_blocks[0].instructions[3], "(set (reg:SI 72) (reg:SI 72))");

  absl::StatusOr<RtlControlFlowGraph> copy2_lifted = InvertUpdateEquivRegs(copy2_snap.post_state);
  ASSERT_TRUE(copy2_lifted.ok()) << copy2_lifted.status();
  EXPECT_NE(copy2_lifted->rpo_blocks[0].instructions[1], "(set (reg:SI 72) (reg:SI 72))");
  EXPECT_TRUE(verifier.VerifyUpdateEquivRegsRightInverse(copy2_snap.post_state).matches);
  EXPECT_TRUE(verifier.VerifySubfunctionBoundaryRightInverse(copy2_snap).matches);
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
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

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
// 4,172-Case Parameterized Round-Trip Verification Suite (Standalone + Fresh)
// =============================================================================

class InvertUpdateEquivRegsRoundTripTest : public InvertUpdateEquivRegsTest,
                                           public ::testing::WithParamInterface<RoundTripTestCase> {
};

TEST_P(InvertUpdateEquivRegsRoundTripTest, StandaloneAndFreshStage0To5AcrossAll4172Cases) {
  const RoundTripTestCase& tc = GetParam();
  const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const FunctionCompilationTrace& trace = **cached_trace;
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << tc.test_name;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

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

  // Fresh cumulative Stage 0..4 -> `InvertBlockAlloc` -> `VerifyUpdateEquivRegsRightInverse`:
  const RtlControlFlowGraph p0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(p0.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p1 = InvertPass01DbrSchedule(p0, trace.stage0_input.profile);
  ASSERT_FALSE(p1.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p2 = InvertPass02Jump2CrossJump(p1, trace.stage0_input.profile);
  ASSERT_FALSE(p2.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p3 = InvertPass03Sched2PostReload(p2, trace.stage0_input.profile);
  ASSERT_FALSE(p3.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p4 = InvertPass04GregReload(p3, trace.stage0_input.profile);
  ASSERT_FALSE(p4.rpo_blocks.empty()) << tc.test_name;
  absl::StatusOr<RtlControlFlowGraph> pre_block = InvertBlockAlloc(p4, trace.stage0_input.profile);
  ASSERT_TRUE(pre_block.ok()) << tc.test_name << ": " << pre_block.status();

  ctx->BeginFunction("harness_fresh_update_equiv");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();
  const RightInverseResult fresh_res = verifier.VerifyUpdateEquivRegsRightInverse(*pre_block);
  EXPECT_TRUE(fresh_res.matches) << "Fresh kUpdateEquivRegs right-inverse failed for "
                                 << tc.test_name << ":\n"
                                 << fresh_res.diff_detail;
}

INSTANTIATE_TEST_SUITE_P(AllRoundTripCases, InvertUpdateEquivRegsRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
