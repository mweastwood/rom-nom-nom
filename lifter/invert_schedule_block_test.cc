#include "lifter/invert_schedule_block.h"

#include <string>
#include <vector>

#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

class ProductionScheduleBlockProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertScheduleBlock(
      const RtlControlFlowGraph& scheduled_cfg_x) const override {
    return lifter::InvertScheduleBlock(scheduled_cfg_x);
  }
};

class InvertScheduleBlockTest : public ::testing::Test {
 protected:
  gcc_harness::GccContext ctx_;
  gcc_harness::FunctionHooks hooks_{&ctx_};
  ProductionScheduleBlockProvider provider_;
  gcc_harness::RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_F(InvertScheduleBlockTest, Sched2LoadLatencySlotUnscheduling) {
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  EXPECT_EQ(lifted.pass, RtlPassKind::kGreg);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched2_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, Sched1PreAllocPseudoRegLoadLatencyUnscheduling) {
  RtlControlFlowGraph sched1_cfg;
  sched1_cfg.pass = RtlPassKind::kSched;
  sched1_cfg.rpo_blocks.resize(1);
  sched1_cfg.rpo_blocks[0].id = 0;
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (ashift:SI (reg:SI 5 a1) (const_int 3))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (minus:SI (reg:SI 72) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_DEAD (reg:SI 73) (nil)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched1_cfg);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  EXPECT_EQ(lifted.pass, RtlPassKind::kCombine);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 73) (ashift:SI (reg:SI 5 a1) (const_int 3))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 72) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched1_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched1StageRightInverse(sched1_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, TwoStepConsumerChainLoadLatencyUnscheduling) {
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 7))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (xor:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 5u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 7))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched2_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, HoistedFpProducersAndCallArgsBeforeStoreClusterUnschedule) {
  RtlControlFlowGraph sched1_cfg;
  sched1_cfg.pass = RtlPassKind::kSched;
  sched1_cfg.rpo_blocks.resize(1);
  sched1_cfg.rpo_blocks[0].id = 0;
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 75) (plus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 76) (minus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg/v:SF 73)) "
      "(expr_list:REG_DEAD (reg/v:SF 73) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 20))) (reg/v:SF 74)) "
      "(expr_list:REG_DEAD (reg/v:SF 74) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 24))) (reg:SF 75)) "
      "(expr_list:REG_DEAD (reg:SF 75) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 28))) (reg:SF 76)) "
      "(expr_list:REG_DEAD (reg:SF 76) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"sink\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph unscheduled = InvertScheduleBlock(sched1_cfg);
  ASSERT_EQ(unscheduled.BlockCount(), 1u);
  ASSERT_EQ(unscheduled.rpo_blocks[0].instructions.size(), 7u);
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[0],
            "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg/v:SF 73)) "
            "(expr_list:REG_DEAD (reg/v:SF 73) (nil))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[1],
            "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 20))) (reg/v:SF 74)) "
            "(expr_list:REG_DEAD (reg/v:SF 74) (nil))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[2],
            "(set (reg:SF 75) (plus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[3],
            "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 24))) (reg:SF 75)) "
            "(expr_list:REG_DEAD (reg:SF 75) (nil))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[4],
            "(set (reg:SF 76) (minus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[5],
            "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 28))) (reg:SF 76)) "
            "(expr_list:REG_DEAD (reg:SF 76) (nil))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched1_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched1StageRightInverse(sched1_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, PostReloadPrologueFpSetupInterleavedWithBodyLoadUnschedules) {
  // In `sched2`, `schedule_block(0)` can hoist a body load ahead of the
  // prologue's `(set (reg:SI 30 $fp) (reg:SI 29 sp))` instruction so that the
  // `$fp` setup fills the load delay slot.
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.outgoing_args_size = 16;
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 40)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:SI 31 ra))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 30 $fp))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 16 s0))");
  // Hoisted body load before `(set (reg:SI 30 $fp) (reg:SI 29 sp))`:
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 30 $fp) (reg:SI 29 sp))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 30 $fp)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph unscheduled = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(unscheduled.BlockCount(), 1u);
  EXPECT_EQ(unscheduled.pass, RtlPassKind::kGreg);
  ASSERT_EQ(unscheduled.rpo_blocks[0].instructions.size(), 8u);
  // `(set (reg:SI 30 $fp) (reg:SI 29 sp))` is restored back into the prologue
  // prefix at index 4, before the body load at index 5.
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[4], "(set (reg:SI 30 $fp) (reg:SI 29 sp))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[5],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const RtlControlFlowGraph unthreaded = InvertThreadPrologueAndEpilogueInsns(unscheduled);
  ASSERT_EQ(unthreaded.BlockCount(), 1u);
  EXPECT_TRUE(unthreaded.sideband.frame_pointer_needed);
  EXPECT_FALSE(unthreaded.sideband.is_leaf_function);
  EXPECT_EQ(unthreaded.sideband.frame_size_bytes, 8);
  const std::vector<int> expected_gprs = {16, 30, 31};
  EXPECT_EQ(unthreaded.sideband.saved_gpr_regnos, expected_gprs);
  ASSERT_EQ(unthreaded.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(unthreaded.rpo_blocks[0].instructions[0],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;

  class PrologueAndSched2Provider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertScheduleBlock(
        const RtlControlFlowGraph& scheduled_cfg_x) const override {
      return InvertThreadPrologueAndEpilogueInsns(lifter::InvertScheduleBlock(scheduled_cfg_x));
    }
  };
  PrologueAndSched2Provider prologue_provider;
  gcc_harness::RightInverseVerifier prologue_verifier(&ctx_, &hooks_, &prologue_provider);
  gcc_harness::SubfunctionBoundarySnapshot sched2_snap;
  sched2_snap.subfunction_id = gcc_harness::SubfunctionId::kScheduleBlockSched2;
  sched2_snap.optimize_level = 2;
  sched2_snap.post_state = sched2_cfg;
  const gcc_harness::RightInverseResult boundary_res =
      prologue_verifier.VerifySubfunctionBoundaryRightInverse(sched2_snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, UpdateFlowInfoReMergesSplitLargeIntMoveAndUpdatesSideband) {
  // `0x12345678` == `305419896`, upper 16 bits `0x12340000` == `305397760`,
  // lower 16 bits `0x00005678` == `22136`.
  RtlControlFlowGraph split_cfg;
  split_cfg.pass = RtlPassKind::kSched;
  split_cfg.sideband.reg_n_sets[72] = 2;
  split_cfg.rpo_blocks.resize(1);
  split_cfg.rpo_blocks[0].id = 0;
  split_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 305397760))");
  split_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (ior:SI (reg:SI 72) (const_int 22136))) "
      "(expr_list:REG_EQUAL (const_int 305419896) (nil))");
  split_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))");
  split_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph merged = InvertUpdateFlowInfo(split_cfg);
  ASSERT_EQ(merged.BlockCount(), 1u);
  ASSERT_EQ(merged.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(merged.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (const_int 305419896)) "
            "(expr_list:REG_EQUAL (const_int 305419896) (nil))");
  EXPECT_EQ(merged.sideband.reg_n_sets.at(72), 1);

  class UpdateFlowInfoAndSched1Provider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertScheduleBlock(
        const RtlControlFlowGraph& scheduled_cfg_x) const override {
      return InvertUpdateFlowInfo(lifter::InvertScheduleBlock(scheduled_cfg_x));
    }
  };
  UpdateFlowInfoAndSched1Provider ufi_provider;
  gcc_harness::RightInverseVerifier ufi_verifier(&ctx_, &hooks_, &ufi_provider);
  const gcc_harness::RightInverseResult stage_res =
      ufi_verifier.VerifySched1StageRightInverse(split_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;

  gcc_harness::SubfunctionBoundarySnapshot sched1_snap;
  sched1_snap.subfunction_id = gcc_harness::SubfunctionId::kScheduleBlockSched1;
  sched1_snap.optimize_level = 2;
  sched1_snap.post_state = split_cfg;
  const gcc_harness::RightInverseResult boundary_res =
      ufi_verifier.VerifySubfunctionBoundaryRightInverse(sched1_snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, MultiBlockCfgUnschedulesEachBlockIndependently) {
  RtlControlFlowGraph multi_cfg;
  multi_cfg.pass = RtlPassKind::kSched2;
  multi_cfg.rpo_blocks.resize(3);
  multi_cfg.rpo_blocks[0].id = 0;
  multi_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  multi_cfg.rpo_blocks[0].successors = {2, 1};

  multi_cfg.rpo_blocks[1].id = 1;
  multi_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  multi_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 6 a2) (const_int 4))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  multi_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  multi_cfg.rpo_blocks[1].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  multi_cfg.rpo_blocks[1].AddPatternOnly("(return)");
  multi_cfg.rpo_blocks[1].successors = {};

  multi_cfg.rpo_blocks[2].id = 2;
  multi_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  multi_cfg.rpo_blocks[2].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  multi_cfg.rpo_blocks[2].AddPatternOnly("(return)");
  multi_cfg.rpo_blocks[2].successors = {};

  const RtlControlFlowGraph lifted = InvertScheduleBlock(multi_cfg);
  ASSERT_EQ(lifted.BlockCount(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 6 a2) (const_int 4))) "
            "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[1],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(multi_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, RejectsEmptyAndMalformedCfgs) {
  // 1. Empty CFG.
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertScheduleBlock(empty_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertUpdateFlowInfo(empty_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertThreadPrologueAndEpilogueInsns(empty_cfg).rpo_blocks.empty());

  // 2. Unbalanced parentheses in RTL pattern.
  RtlControlFlowGraph unbalanced_cfg;
  unbalanced_cfg.rpo_blocks.resize(1);
  unbalanced_cfg.rpo_blocks[0].id = 0;
  unbalanced_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1)");
  EXPECT_TRUE(InvertScheduleBlock(unbalanced_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertUpdateFlowInfo(unbalanced_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertThreadPrologueAndEpilogueInsns(unbalanced_cfg).rpo_blocks.empty());

  // 3. Unknown RTL opcode.
  RtlControlFlowGraph unknown_op_cfg;
  unknown_op_cfg.rpo_blocks.resize(1);
  unknown_op_cfg.rpo_blocks[0].id = 0;
  unknown_op_cfg.rpo_blocks[0].AddPatternOnly("(bogus_rtl_op (reg:SI 2 v0))");
  EXPECT_TRUE(InvertScheduleBlock(unknown_op_cfg).rpo_blocks.empty());

  // 4. Out-of-bounds successor index.
  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.rpo_blocks.resize(1);
  bad_succ_cfg.rpo_blocks[0].id = 0;
  bad_succ_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  bad_succ_cfg.rpo_blocks[0].successors = {5};
  EXPECT_TRUE(InvertScheduleBlock(bad_succ_cfg).rpo_blocks.empty());

  // 5. Mismatched block ID.
  RtlControlFlowGraph bad_id_cfg;
  bad_id_cfg.rpo_blocks.resize(1);
  bad_id_cfg.rpo_blocks[0].id = 3;
  bad_id_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  EXPECT_TRUE(InvertScheduleBlock(bad_id_cfg).rpo_blocks.empty());

  // 6. Out-of-bounds `addr_vec_targets` index.
  RtlControlFlowGraph bad_addr_vec_cfg;
  bad_addr_vec_cfg.rpo_blocks.resize(1);
  bad_addr_vec_cfg.rpo_blocks[0].id = 0;
  bad_addr_vec_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  bad_addr_vec_cfg.rpo_blocks[0].addr_vec_targets = {4};
  EXPECT_TRUE(InvertScheduleBlock(bad_addr_vec_cfg).rpo_blocks.empty());
}

TEST_F(InvertScheduleBlockTest, VerifierRejectsCorruptedScheduleBlockProvider) {
  class CorruptedScheduleBlockProvider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertScheduleBlock(
        const RtlControlFlowGraph& scheduled_cfg_x) const override {
      RtlControlFlowGraph bad = scheduled_cfg_x;
      if (!bad.rpo_blocks.empty() && bad.rpo_blocks[0].instructions.size() >= 3) {
        // Corrupt the schedule by moving the dependent consumer ahead of its
        // producer load.
        std::swap(bad.rpo_blocks[0].instructions[0], bad.rpo_blocks[0].instructions[2]);
        bad.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      }
      return bad;
    }
  };

  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  CorruptedScheduleBlockProvider bad_provider;
  gcc_harness::RightInverseVerifier bad_verifier(&ctx_, &hooks_, &bad_provider);
  const gcc_harness::RightInverseResult bad_res =
      bad_verifier.VerifyScheduleBlockRightInverse(sched2_cfg);
  EXPECT_FALSE(bad_res.matches);
}

class InvertScheduleBlockCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  gcc_harness::GccContext ctx_;
  gcc_harness::FunctionHooks hooks_{&ctx_};
  ProductionScheduleBlockProvider provider_;
  gcc_harness::RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_P(InvertScheduleBlockCorpusTest, Sched1AndSched2BoundaryRightInverseMatchesOnCorpusCase) {
  const RoundTripTestCase& tc = GetParam();
  const gcc_harness::FunctionCompilationTrace trace = hooks_.TraceForwardCompilation(tc);

  bool found_sched1 = false;
  bool found_sched2 = false;
  for (const gcc_harness::SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == gcc_harness::SubfunctionId::kScheduleBlockSched1) {
      found_sched1 = true;
      const gcc_harness::RightInverseResult res =
          verifier_.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(res.matches) << "kScheduleBlockSched1 failed for " << tc.test_name << ": "
                               << res.diff_detail;
    } else if (snap.subfunction_id == gcc_harness::SubfunctionId::kScheduleBlockSched2) {
      found_sched2 = true;
      const gcc_harness::RightInverseResult res =
          verifier_.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(res.matches) << "kScheduleBlockSched2 failed for " << tc.test_name << ": "
                               << res.diff_detail;
    }
  }
  EXPECT_TRUE(found_sched1);
  EXPECT_TRUE(found_sched2);
}

INSTANTIATE_TEST_SUITE_P(AllRoundTripCases, InvertScheduleBlockCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
