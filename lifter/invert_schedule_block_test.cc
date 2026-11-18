#include "lifter/invert_schedule_block.h"

#include <cstddef>
#include <string>
#include <vector>

#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

class ProductionScheduleBlockProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
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
                                             bool sched2_enabled) const override {
    RtlControlFlowGraph cfg =
        sched2_enabled ? lifter::InvertScheduleBlock(scheduled_cfg_x, profile) : scheduled_cfg_x;
    return InvertThreadPrologueAndEpilogueInsns(cfg, profile);
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
  hooks_.ApplyCompilationProfile(
      {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}});
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
  sched1_cfg.rpo_blocks[0].rtl_insns.back().has_explicit_call_fusage = true;
  sched1_cfg.rpo_blocks[0].rtl_insns.back().call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };

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

TEST_F(InvertScheduleBlockTest, InterleavedMips1FpProducersAndCallArgsAcrossStoresUnschedule) {
  hooks_.ApplyCompilationProfile(
      {.optimize_level = 2, .c_flags = {"-O2", "-mips1", "-funsigned-char"}});
  RtlControlFlowGraph sched1_cfg;
  sched1_cfg.pass = RtlPassKind::kSched;
  sched1_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 30, 73, 74};
  sched1_cfg.rpo_blocks.resize(1);
  sched1_cfg.rpo_blocks[0].id = 0;
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 30 $fp) (const_int 16)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 5 a1) (const_int 1)) (expr_list:REG_EQUAL (const_int 1) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"init_vec4\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 30 $fp) (const_int 32)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 5 a1) (const_int 2)) (expr_list:REG_EQUAL (const_int 2) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg/v:SF 73))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 75) (plus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 20))) (reg/v:SF 74))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 76) (minus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 24))) (reg:SF 75)) "
      "(expr_list:REG_DEAD (reg:SF 75) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 28))) (reg:SF 76)) "
      "(expr_list:REG_DEAD (reg:SF 76) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"init_vec4\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 78) (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 16))))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 79) (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 20))))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 80) (plus:SF (reg:SF 78) (reg:SF 79))) "
      "(expr_list:REG_DEAD (reg:SF 78) (expr_list:REG_DEAD (reg:SF 79) (nil)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 80) (reg/v:SF 73))) "
      "(expr_list:REG_DEAD (reg:SF 80) (expr_list:REG_DEAD (reg/v:SF 73) (nil)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/v:SF 74)) (expr_list:REG_DEAD (reg/v:SF 74) (nil))");
  sched1_cfg.rpo_blocks[0].rtl_insns[2].has_explicit_call_fusage = true;
  sched1_cfg.rpo_blocks[0].rtl_insns[2].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
  };
  sched1_cfg.rpo_blocks[0].rtl_insns[11].has_explicit_call_fusage = true;
  sched1_cfg.rpo_blocks[0].rtl_insns[11].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
  };

  const RtlControlFlowGraph unscheduled = InvertScheduleBlock(sched1_cfg);
  ASSERT_EQ(unscheduled.BlockCount(), 1u);
  ASSERT_EQ(unscheduled.rpo_blocks[0].instructions.size(), 18u);
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[3],
            "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg/v:SF 73))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[4],
            "(set (mem/s:SF (plus:SI (reg:SI 30 $fp) (const_int 20))) (reg/v:SF 74))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[5],
            "(set (reg:SF 75) (plus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[7],
            "(set (reg:SF 76) (minus:SF (reg/v:SF 73) (reg/v:SF 74)))");
  EXPECT_EQ(unscheduled.rpo_blocks[0].instructions[9],
            "(set (reg:SI 4 a0) (plus:SI (reg:SI 30 $fp) (const_int 32)))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched1_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, PostReloadPrologueFpSetupInterleavedWithBodyLoadUnschedules) {
  // In `sched2`, `schedule_block(0)` can hoist a body load ahead of the
  // prologue's `(set (reg:SI 30 $fp) (reg:SI 29 sp))` instruction so that the
  // `$fp` setup fills the load delay slot.
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.outgoing_args_size = 16;
  sched2_cfg.sideband.basic_block_live_at_start[0] = {4, 29, 30, 31};
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
    RtlControlFlowGraph InvertSched2PostReload(const RtlControlFlowGraph& scheduled_cfg_x,
                                               const CompilationProfile& profile,
                                               bool sched2_enabled) const override {
      RtlControlFlowGraph cfg =
          sched2_enabled ? lifter::InvertScheduleBlock(scheduled_cfg_x, profile) : scheduled_cfg_x;
      return InvertThreadPrologueAndEpilogueInsns(cfg, profile);
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
    RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                            const CompilationProfile& profile) const override {
      return InvertUpdateFlowInfo(lifter::InvertScheduleBlock(scheduled_cfg_x, profile));
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
  multi_cfg.rpo_blocks.resize(4);
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
  multi_cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref L1))");
  multi_cfg.rpo_blocks[1].successors = {3};

  multi_cfg.rpo_blocks[2].id = 2;
  multi_cfg.rpo_blocks[2].has_label = true;
  multi_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  multi_cfg.rpo_blocks[2].successors = {3};

  multi_cfg.rpo_blocks[3].id = 3;
  multi_cfg.rpo_blocks[3].has_label = true;
  multi_cfg.rpo_blocks[3].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  multi_cfg.rpo_blocks[3].successors = {};

  const RtlControlFlowGraph lifted = InvertScheduleBlock(multi_cfg);
  ASSERT_EQ(lifted.BlockCount(), 4u);
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

TEST_F(InvertScheduleBlockTest, CalleeSavedFpAluSunkAcrossCallUnschedulesBeforeFirstCall) {
  hooks_.ApplyCompilationProfile(
      {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}});
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.basic_block_live_at_start[0] = {29, 52, 54, 56, 60};
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 44 $f12) (reg:SF 52 $f20)) "
      "(expr_list:REG_DEAD (reg:SF 52 $f20) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (const_int 0))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"Step\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 52 $f20) (reg:SF 32 $f0)) "
      "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 44 $f12) (plus:SF (reg:SF 52 $f20) (reg:SF 56 $f24))) "
      "(expr_list:REG_DEAD (reg:SF 52 $f20) (expr_list:REG_DEAD (reg:SF 56 $f24) (nil)))");
  // Sunk callee-saved `$f28 += $f22` interleaved into second call's arg setup:
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 60 $f28) (plus:SF (reg:SF 60 $f28) (reg:SF 54 $f22))) "
      "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (const_int 1))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"Step\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 60 $f28))) "
      "(expr_list:REG_DEAD (reg:SF 60 $f28) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))");
  sched2_cfg.rpo_blocks[0].rtl_insns[2].has_explicit_call_fusage = true;
  sched2_cfg.rpo_blocks[0].rtl_insns[2].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSf, 44)),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
  };
  sched2_cfg.rpo_blocks[0].rtl_insns[7].has_explicit_call_fusage = true;
  sched2_cfg.rpo_blocks[0].rtl_insns[7].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSf, 44)),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
  };

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 10u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2],
            "(set (reg:SF 60 $f28) (plus:SF (reg:SF 60 $f28) (reg:SF 54 $f22))) "
            "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched2_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;
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
    RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                            const CompilationProfile& /*profile*/) const override {
      return InvertScheduleBlock(scheduled_cfg_x);
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

TEST_F(InvertScheduleBlockTest, MultiBlockFallthroughPreservesLabelsWithoutLiveAtStartSideband) {
  hooks_.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {}});
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  ASSERT_TRUE(sched2_cfg.sideband.basic_block_live_at_start.empty());
  sched2_cfg.rpo_blocks.resize(2);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 8)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].successors = {1};

  sched2_cfg.rpo_blocks[1].id = 1;
  sched2_cfg.rpo_blocks[1].has_label = false;
  sched2_cfg.rpo_blocks[1].predecessors = {0};
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 8)))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(lifted.BlockCount(), 2u);
  EXPECT_TRUE(lifted.rpo_blocks[1].has_label);
  EXPECT_TRUE(lifted.sideband.basic_block_live_at_start.empty());

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched2_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;

  gcc_harness::SubfunctionBoundarySnapshot sched2_snap;
  sched2_snap.subfunction_id = gcc_harness::SubfunctionId::kScheduleBlockSched2;
  sched2_snap.optimize_level = 2;
  sched2_snap.post_state = sched2_cfg;
  const gcc_harness::RightInverseResult boundary_res =
      verifier_.VerifySubfunctionBoundaryRightInverse(sched2_snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest,
       ZeroArgCallWithoutExplicitFusageDoesNotGuessArgRegsAndInlineFusageWorks) {
  hooks_.ApplyCompilationProfile(
      {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}});

  // 1. Fusage-free 0-argument `CALL_INSN`: `$a0..$a3` must NOT be heuristically guessed as read by
  // the call when `has_explicit_call_fusage == false`.
  RtlControlFlowGraph zero_arg_cfg;
  zero_arg_cfg.pass = RtlPassKind::kSched2;
  zero_arg_cfg.sideband.basic_block_live_at_start[0] = {29, 31, 54, 60};
  zero_arg_cfg.rpo_blocks.resize(1);
  zero_arg_cfg.rpo_blocks[0].id = 0;
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 16)))");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 0))) (reg:SI 31 ra))");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 8))) (reg:DF 60 $f28))");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"StepZero\"))) "
      "(const_int 0))) (clobber (reg:SI 31 ra)) ] )");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 60 $f28) (plus:SF (reg:SF 60 $f28) (reg:SF 54 $f22))) "
      "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 4 a0) (const_int 1)) "
      "(expr_list:REG_UNUSED (reg:SI 4 a0) (nil))");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"StepZero\"))) "
      "(const_int 0))) (clobber (reg:SI 31 ra)) ] )");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 60 $f28))) "
      "(expr_list:REG_DEAD (reg:SF 60 $f28) (nil))");
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))");
  ASSERT_FALSE(zero_arg_cfg.rpo_blocks[0].rtl_insns[3].has_explicit_call_fusage);
  ASSERT_FALSE(zero_arg_cfg.rpo_blocks[0].rtl_insns[6].has_explicit_call_fusage);

  const RtlControlFlowGraph lifted_zero_arg = InvertScheduleBlock(zero_arg_cfg);
  ASSERT_EQ(lifted_zero_arg.BlockCount(), 1u);
  ASSERT_EQ(lifted_zero_arg.rpo_blocks[0].instructions.size(), 9u);
  // `$f28 += $f22` stays at index 4 (after the first `StepZero` call) rather than being falsely
  // hoisted across it as if `(set (reg:SI 4 a0) (const_int 1))` were an argument setup.
  EXPECT_EQ(lifted_zero_arg.rpo_blocks[0].instructions[4],
            "(set (reg:SF 60 $f28) (plus:SF (reg:SF 60 $f28) (reg:SF 54 $f22))) "
            "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");

  // With explicit empty `call_fusage = {}` (`(nil)`, as dematerialized by `GccContext` for a
  // 0-argument `CALL_INSN`), both `VerifySched2StageRightInverse` and
  // `VerifySubfunctionBoundaryRightInverse` pass `.matches == true`.
  RtlControlFlowGraph explicit_zero_arg_cfg = zero_arg_cfg;
  explicit_zero_arg_cfg.rpo_blocks[0].rtl_insns[3].has_explicit_call_fusage = true;
  explicit_zero_arg_cfg.rpo_blocks[0].rtl_insns[6].has_explicit_call_fusage = true;

  const gcc_harness::RightInverseResult zero_arg_stage_res =
      verifier_.VerifySched2StageRightInverse(explicit_zero_arg_cfg);
  EXPECT_TRUE(zero_arg_stage_res.matches) << zero_arg_stage_res.diff_detail;

  class PrologueAndSched2Provider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertScheduleBlock(
        const RtlControlFlowGraph& scheduled_cfg_x) const override {
      return InvertThreadPrologueAndEpilogueInsns(lifter::InvertScheduleBlock(scheduled_cfg_x));
    }
    RtlControlFlowGraph InvertSched2PostReload(const RtlControlFlowGraph& scheduled_cfg_x,
                                               const CompilationProfile& profile,
                                               bool sched2_enabled) const override {
      RtlControlFlowGraph cfg =
          sched2_enabled ? lifter::InvertScheduleBlock(scheduled_cfg_x, profile) : scheduled_cfg_x;
      return InvertThreadPrologueAndEpilogueInsns(cfg, profile);
    }
  };
  PrologueAndSched2Provider prologue_provider;
  gcc_harness::RightInverseVerifier prologue_verifier(&ctx_, &hooks_, &prologue_provider);
  gcc_harness::SubfunctionBoundarySnapshot zero_arg_snap;
  zero_arg_snap.subfunction_id = gcc_harness::SubfunctionId::kScheduleBlockSched2;
  zero_arg_snap.optimize_level = 2;
  zero_arg_snap.c_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  zero_arg_snap.post_state = explicit_zero_arg_cfg;
  const gcc_harness::RightInverseResult zero_arg_boundary_res =
      prologue_verifier.VerifySubfunctionBoundaryRightInverse(zero_arg_snap);
  EXPECT_TRUE(zero_arg_boundary_res.matches) << zero_arg_boundary_res.diff_detail;

  // 2. Inline `call_fusage` S-expression on `instructions` (with empty `rtl_insns`) and multi-word
  // `DImode` span (`(use (reg:DI 4 a0))` covering both `a0` (`4`) and `a1` (`5`)).
  RtlControlFlowGraph inline_fusage_cfg;
  inline_fusage_cfg.pass = RtlPassKind::kSched2;
  inline_fusage_cfg.sideband.basic_block_live_at_start[0] = {4, 29, 52, 54, 56, 60};
  inline_fusage_cfg.rpo_blocks.resize(1);
  inline_fusage_cfg.rpo_blocks[0].id = 0;
  inline_fusage_cfg.rpo_blocks[0].instructions = {
      "(set (reg:SF 44 $f12) (reg:SF 52 $f20)) (expr_list:REG_DEAD (reg:SF 52 $f20) (nil))",
      "(set (reg:SI 5 a1) (const_int 0))",
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"StepDi\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] ) "
      "(expr_list (use (reg:SF 44 $f12)) (expr_list (use (reg:DI 4 a0)) (nil)))",
      "(set (reg:SF 52 $f20) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
      "(set (reg:SF 44 $f12) (plus:SF (reg:SF 52 $f20) (reg:SF 56 $f24))) "
      "(expr_list:REG_DEAD (reg:SF 52 $f20) (expr_list:REG_DEAD (reg:SF 56 $f24) (nil)))",
      "(set (reg:SF 60 $f28) (plus:SF (reg:SF 60 $f28) (reg:SF 54 $f22))) "
      "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))",
      "(set (reg:SI 5 a1) (const_int 1))",
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"StepDi\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] ) "
      "(expr_list (use (reg:SF 44 $f12)) (expr_list (use (reg:DI 4 a0)) (nil)))",
      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 60 $f28))) "
      "(expr_list:REG_DEAD (reg:SF 60 $f28) (nil))",
      "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))",
  };
  ASSERT_TRUE(inline_fusage_cfg.rpo_blocks[0].rtl_insns.empty());

  const RtlControlFlowGraph lifted_inline = InvertScheduleBlock(inline_fusage_cfg);
  ASSERT_EQ(lifted_inline.BlockCount(), 1u);
  ASSERT_EQ(lifted_inline.rpo_blocks[0].instructions.size(), 10u);
  EXPECT_EQ(lifted_inline.rpo_blocks[0].instructions[2],
            "(set (reg:SF 60 $f28) (plus:SF (reg:SF 60 $f28) (reg:SF 54 $f22))) "
            "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");

  // Also verify right-inverse replay on the canonical `RtlInsn::call_fusage` form of the same
  // `DImode` argument span.
  RtlControlFlowGraph canon_di_fusage_cfg = inline_fusage_cfg;
  canon_di_fusage_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  for (size_t k : {2u, 7u}) {
    ASSERT_TRUE(canon_di_fusage_cfg.rpo_blocks[0].rtl_insns[k].has_explicit_call_fusage);
    canon_di_fusage_cfg.rpo_blocks[0].instructions[k] =
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"StepDi\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )";
  }
  const gcc_harness::RightInverseResult di_sb_res =
      verifier_.VerifyScheduleBlockRightInverse(canon_di_fusage_cfg);
  EXPECT_TRUE(di_sb_res.matches) << di_sb_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, PrologueSmallStructShiftsZeroAndNonZeroFrameRoundTrip) {
  // 1. `tsize == 0` leaf function taking 1-byte (`$4 >> 24`) and 2-byte (`$5 >> 16`) structs.
  const std::string leaf_src = R"(
typedef struct { unsigned char v; } S1;
typedef struct { unsigned char hi; signed char lo; } S2;
int sum_small_structs(S1 a, S2 b) {
  return (int)a.v + (int)b.hi - (int)b.lo;
}
)";
  const auto leaf_trace = hooks_.TraceForwardCompilation(leaf_src, "sum_small_structs",
                                                         {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                                                         "prologue_small_struct_leaf");
  bool checked_leaf = false;
  for (const auto& snap : leaf_trace.subfunction_snapshots) {
    if (snap.subfunction_id != gcc_harness::SubfunctionId::kScheduleBlockSched2) {
      continue;
    }
    checked_leaf = true;
    const RtlControlFlowGraph lifted =
        InvertThreadPrologueAndEpilogueInsns(InvertScheduleBlock(snap.post_state));
    ASSERT_EQ(lifted.sideband.prologue_args.size(), 2u);
    EXPECT_EQ(lifted.sideband.prologue_args[0].byte_size, 1);
    EXPECT_EQ(lifted.sideband.prologue_args[1].byte_size, 2);
    const auto replayed = hooks_.ExecuteSubfunctionBoundary(snap, lifted);
    const auto cmp = replayed.post_state.CompareAgainst(snap.post_state);
    EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
  }
  EXPECT_TRUE(checked_leaf);

  // 2. `tsize > 0` function taking a 3-byte (`BLKmode`, `$4 >> 8`) struct.
  const std::string blk_src = R"(
typedef struct { unsigned char r, g, b; } S3;
int sum_rgb_struct(S3 c, int scale) {
  return ((int)c.r + (int)c.g + (int)c.b) * scale;
}
)";
  const auto blk_trace = hooks_.TraceForwardCompilation(blk_src, "sum_rgb_struct",
                                                        {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                                                        "prologue_small_struct_blk");
  bool checked_blk = false;
  for (const auto& snap : blk_trace.subfunction_snapshots) {
    if (snap.subfunction_id != gcc_harness::SubfunctionId::kScheduleBlockSched2) {
      continue;
    }
    checked_blk = true;
    const RtlControlFlowGraph lifted =
        InvertThreadPrologueAndEpilogueInsns(InvertScheduleBlock(snap.post_state));
    ASSERT_EQ(lifted.sideband.prologue_args.size(), 1u);
    EXPECT_EQ(lifted.sideband.prologue_args[0].byte_size, 3);
    const auto replayed = hooks_.ExecuteSubfunctionBoundary(snap, lifted);
    const auto cmp = replayed.post_state.CompareAgainst(snap.post_state);
    EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
  }
  EXPECT_TRUE(checked_blk);
}

TEST_F(InvertScheduleBlockTest, PrologueVariadicHomeStoresZeroAndNonZeroFrameRoundTrip) {
  const std::string var_src = R"(
typedef char* va_list;
#define va_start(ap, last) ((ap) = (va_list)&(last) + 4)
#define va_arg(ap, type) (((type*)((ap) += 4))[-1])
#define va_end(ap) ((void)0)
int var_sum(int base, int count, ...) {
  va_list ap;
  int i, total = base;
  va_start(ap, count);
  for (i = 0; i < count; ++i) {
    total += va_arg(ap, int);
  }
  va_end(ap);
  return total;
}
)";
  for (const auto& flags : {std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                            std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
    const auto trace = hooks_.TraceForwardCompilation(var_src, "var_sum", flags, "prologue_var");
    bool checked = false;
    for (const auto& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id != gcc_harness::SubfunctionId::kScheduleBlockSched2) {
        continue;
      }
      checked = true;
      RtlControlFlowGraph unsched =
          (snap.optimize_level >= 2) ? InvertScheduleBlock(snap.post_state) : snap.post_state;
      const RtlControlFlowGraph lifted = InvertThreadPrologueAndEpilogueInsns(unsched);
      EXPECT_TRUE(lifted.sideband.is_variadic);
      ASSERT_EQ(lifted.sideband.prologue_args.size(), 2u);
      const auto replayed = hooks_.ExecuteSubfunctionBoundary(snap, lifted);
      const auto cmp = replayed.post_state.CompareAgainst(snap.post_state);
      EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
    }
    EXPECT_TRUE(checked);
  }
}

TEST_F(InvertScheduleBlockTest, PrologueLargeStackFrameOver32KbRoundTrip) {
  const std::string large_src = R"(
extern int sink_buf(int* buf, int n);
int large_frame_fn(int seed) {
  int big_buf[8500];
  big_buf[0] = seed;
  big_buf[8499] = seed + 1;
  return sink_buf(big_buf, 8500) + seed;
}
)";
  for (const auto& flags : {std::vector<std::string>{"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                            std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
    const auto trace =
        hooks_.TraceForwardCompilation(large_src, "large_frame_fn", flags, "prologue_large_frame");
    bool checked = false;
    for (const auto& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id != gcc_harness::SubfunctionId::kScheduleBlockSched2) {
        continue;
      }
      checked = true;
      RtlControlFlowGraph unsched =
          (snap.optimize_level >= 2) ? InvertScheduleBlock(snap.post_state) : snap.post_state;
      const RtlControlFlowGraph lifted = InvertThreadPrologueAndEpilogueInsns(unsched);
      EXPECT_GE(lifted.sideband.frame_size_bytes, 34000);
      EXPECT_FALSE(lifted.sideband.is_leaf_function);
      // Verify no `$sp` subtraction remains in entry block 0 before replay.
      for (const std::string& pat_str : lifted.rpo_blocks[0].instructions) {
        auto parsed = ParsePatternAndNotes(pat_str);
        ASSERT_TRUE(parsed.ok());
        if (parsed->pattern.IsSet() && parsed->pattern.SetDest().IsReg()) {
          EXPECT_NE(parsed->pattern.SetDest().RegNo(), 29);
        }
      }
      const auto replayed = hooks_.ExecuteSubfunctionBoundary(snap, lifted);
      const auto cmp = replayed.post_state.CompareAgainst(snap.post_state);
      EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
    }
    EXPECT_TRUE(checked);
  }
}

TEST_F(InvertScheduleBlockTest, Sched2RestoresNoConflictExtendClobberAndHiLoSubregDependency) {
  // Post-reload `kSched2` block with a 2-insn `zero_extendsidi2` into `(reg:DI 2 v0)` and an
  // `extendsidi2` into `(reg:DI 64 hi)` (`mtlo` separated from `mthi` by `sched2`'s 3-cycle
  // `imuldiv` output latency): `InvertScheduleBlock` restores `(clobber (reg:DI 2 v0))`,
  // `(clobber (reg:DI 64 hi))`, and `(subreg:SI (reg:DI 64 hi) 1)` on `mtlo`, and round-trips
  // cleanly through GCC's `schedule_insns`.
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.is_leaf_function = true;
  sched2_cfg.sideband.basic_block_live_at_start[0] = {4, 6, 29, 31};
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 65 lo) (reg:SI 6 a2))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 7 a3) (ashiftrt:SI (reg:SI 6 a2) (const_int 31))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (reg:SI 4 a0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 64 hi) (reg:SI 7 a3)) "
      "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:DI 8 t0) (reg:DI 64 hi)) "
      "(expr_list:REG_DEAD (reg:DI 64 hi) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 8 t0))) "
      "(expr_list:REG_DEAD (reg:DI 8 t0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:DI 2 v0)) (expr_list:REG_DEAD (reg/i:DI 2 v0) (nil))");

  const RtlControlFlowGraph unscheduled = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(unscheduled.BlockCount(), 1u);
  EXPECT_EQ(unscheduled.pass, RtlPassKind::kGreg);

  size_t di_v0_clobber_count = 0;
  size_t di_hilo_clobber_count = 0;
  bool saw_mtlo_subreg = false;
  for (const std::string& pat_str : unscheduled.rpo_blocks[0].instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    ASSERT_TRUE(parsed.ok());
    const std::string bare = parsed->pattern.ToGccPrintRtlString();
    if (bare == "(clobber (reg:DI 2 v0))") {
      ++di_v0_clobber_count;
    } else if (bare == "(clobber (reg:DI 64 hi))") {
      ++di_hilo_clobber_count;
    } else if (bare == "(set (subreg:SI (reg:DI 64 hi) 1) (reg:SI 6 a2))") {
      saw_mtlo_subreg = true;
    }
  }
  EXPECT_EQ(di_v0_clobber_count, 1u);
  EXPECT_EQ(di_hilo_clobber_count, 1u);
  EXPECT_TRUE(saw_mtlo_subreg);

  const RtlControlFlowGraph scheduled_with_clobbers =
      hooks_
          .ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kScheduleBlockSched2, unscheduled,
                                      {}, 2, {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"})
          .post_state;
  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(scheduled_with_clobbers);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest,
       Sched2HighPressureDivModPostCallAluAndOutgoingStackArgsRightInverse) {
  // Exercises all three post-reload `sched2` unscheduling transforms via `RightInverseVerifier`:
  //   1. `UnscheduleDelayedHiLoReloads`: `divmoddisi4` `mfhi`/`mflo` output reloads delayed across
  //      independent instructions by `imuldiv` latency;
  //   2. `UnschedulePostCallHoistedCalleeSavedAlu`: post-call hoisted callee-saved ALU + adjacent
  //      single-use reload load sunk back before their shared loop consumer;
  //   3. `UnscheduleSunkPreCallStackStores`: pre-call spilled stack-arg stores + `sched.c:1740`
  //      `pending_lists_length > 32` `mem/u` prologue load & `-mips1` callee-saved `[load, alu]`
  //      pairs restored to canonical `.09.greg` order.
  const std::string src = R"(
int ProbeCall(int a, int b);
int EightArgSink(int a, int b, int c, int d, int e, int f, int g, int h);

int DivModSpillPressure(const int* p, int d1, int d2) {
  int v0 = p[0], v1 = p[1], v2 = p[2], v3 = p[3], v4 = p[4], v5 = p[5];
  int q1 = v0 / d1, r1 = v0 % d1;
  int q2 = (v1 + q1) / d2, r2 = (v2 + r1) % d2;
  return (q1 ^ r2) + (q2 ^ r1) + v3 + v4 + v5;
}

int LoopCallReloadPair(const int* arr, int n, int seed) {
  int s0 = arr[0], s1 = arr[1], s2 = arr[2], s3 = arr[3], s4 = arr[4];
  int s5 = arr[5], s6 = arr[6], s7 = arr[7], acc = seed, i;
  for (i = 0; i < n; i++) {
    int c = ProbeCall(i + s0, acc ^ s7);
    acc = (acc + c + s1) ^ (s2 + s3 - s4 + s5 - s6);
  }
  return acc;
}

int SpillsPlus8Args(const int* p, int k) {
  int a0 = p[0] + 1, a1 = p[1] - 2, a2 = p[2] + 3, a3 = p[3] - 4;
  int a4 = p[4] + 5, a5 = p[5] - 6, a6 = p[6] + 7, a7 = p[7] - 8;
  int a8 = p[8] + 9, a9 = p[9] - 10, a10 = p[10] + 11, a11 = p[11] - 12;
  int a12 = p[12] + 13, a13 = p[13] - 14;
  int r = EightArgSink(a0 ^ k, a1 + k, a2 ^ a8, a3 + a9, a4 ^ a10, a5 + a11, a6 ^ a12, a7 + a13);
  return r + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13;
}
)";

  for (const auto& flags : {std::vector<std::string>{"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
                            std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
    for (const char* fn : {"DivModSpillPressure", "LoopCallReloadPair", "SpillsPlus8Args"}) {
      const auto trace = hooks_.TraceForwardCompilation(src, fn, flags, "sched2_high_pressure");
      bool found_sched2 = false;
      for (const auto& snap : trace.subfunction_snapshots) {
        if (snap.subfunction_id != gcc_harness::SubfunctionId::kScheduleBlockSched2) {
          continue;
        }
        found_sched2 = true;
        const gcc_harness::RightInverseResult boundary_res =
            verifier_.VerifySubfunctionBoundaryRightInverse(snap);
        EXPECT_TRUE(boundary_res.matches) << fn << ": " << boundary_res.diff_detail;
        hooks_.ApplyCompilationProfile(trace.stage0_input.profile);
        const gcc_harness::RightInverseResult stage_res =
            verifier_.VerifySched2StageRightInverse(snap.post_state);
        EXPECT_TRUE(stage_res.matches) << fn << ": " << stage_res.diff_detail;
      }
      EXPECT_TRUE(found_sched2) << fn;
    }
  }
}

TEST_F(InvertScheduleBlockTest, Sched2ConsecutiveCallerSaveRestoresAndEarlyclobberDiRightInverse) {
  const std::string src = R"(
int CallHook(int a, int b);

int ConsecutiveCallerSaveLoop(const int* v, int n) {
  int c0 = 1, c1 = 2, c2 = 3, c3 = 4, c4 = 5, c5 = 6, c6 = 7, c7 = 8, c8 = 9, c9 = 10;
  int m0 = v[0], m1 = v[1], m2 = v[2], m3 = v[3], m4 = v[4], m5 = v[5];
  int c10 = 11, c11 = 12, i;
  for (i = 0; i < n; i++) {
    int r = CallHook(i, c0 + c11);
    c0 = (c0 + m0) ^ r;
    c1 = (c1 + m1) + c0;
    c2 = (c2 ^ m2) + c1;
    c3 = (c3 + m3) ^ c2;
    c4 = (c4 - m4) + c3;
    c5 = (c5 + m5) ^ c4;
    c6 = (c6 ^ c5) + m5;
    c7 = (c7 + c6) ^ m4;
    c8 = (c8 - c7) + m3;
    c9 = (c9 ^ c8) + m2;
    c10 = (c10 + c9) ^ m1;
    c11 = (c11 - c10) + m0;
  }
  return c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8 + c9 + c10 + c11;
}

long long EarlyclobberDiAccum(long long a, long long b, long long c) {
  return (a + b - c) + 0x100000005LL;
}
)";

  for (const auto& flags : {std::vector<std::string>{"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
                            std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
    for (const char* fn : {"ConsecutiveCallerSaveLoop", "EarlyclobberDiAccum"}) {
      const auto trace = hooks_.TraceForwardCompilation(src, fn, flags, "sched2_cs_and_di");
      bool found_sched2 = false;
      for (const auto& snap : trace.subfunction_snapshots) {
        if (snap.subfunction_id != gcc_harness::SubfunctionId::kScheduleBlockSched2) {
          continue;
        }
        found_sched2 = true;
        const gcc_harness::RightInverseResult boundary_res =
            verifier_.VerifySubfunctionBoundaryRightInverse(snap);
        EXPECT_TRUE(boundary_res.matches) << fn << ": " << boundary_res.diff_detail;
        hooks_.ApplyCompilationProfile(trace.stage0_input.profile);
        const gcc_harness::RightInverseResult stage_res =
            verifier_.VerifySched2StageRightInverse(snap.post_state);
        EXPECT_TRUE(stage_res.matches) << fn << ": " << stage_res.diff_detail;
      }
      EXPECT_TRUE(found_sched2) << fn;
    }
  }
}

TEST_F(InvertScheduleBlockTest, Sched2HoistsSpillOutputReloadStorePastUnchangingMemRead) {
  // GCC `sched.c:828, 852` (`true_dependence` / `anti_dependence`): an unchanging non-volatile
  // `(mem/s/u:SI ...)` read (such as an incoming stack-passed parameter) never conflicts with a
  // non-unchanging reload spill-slot store `(set (mem:SI (plus:SI sp off)) ...)`. When `sched2`
  // schedules the `mem/s/u` load into the 1-cycle load-delay shadow of a reload producer load
  // `(set v1 (mem a0))`, verify `InvertScheduleBlock` hoists the spill output-reload store back up
  // to `prod_idx + 1` past the intervening `mem/s/u` load and satisfies
  // `VerifySched2StageRightInverse`.
  hooks_.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2", "-mips1"}});
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.frame_size_bytes = 8;
  sched2_cfg.sideband.outgoing_args_size = 16;
  sched2_cfg.sideband.saved_gpr_regnos = {16, 17, 18, 19, 20, 21, 22, 23, 30, 31};
  sched2_cfg.sideband.basic_block_live_at_start = {{0, {4, 29}}, {1, {6, 29}}};
  sched2_cfg.rpo_blocks.resize(2);

  // `B0`: producer `v1 = *a0` (priority 4), intervening `mem/s/u` incoming-arg load into `a2`
  // (priority 3, scheduled into `v1`'s load-delay slot), and sunk pure output-reload store
  // `[sp + 20] = v1` (`bucket 16`, `off = 16 + (8 - 4) = 20`).
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 6 a2) (mem/s/u:SI (plus:SI (reg:SI 29 sp) (const_int 80))))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 3 v1))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (pc) (label_ref L0))");
  sched2_cfg.rpo_blocks[0].successors = {1};

  // `B1`: reload from `[sp + 20]` so `IdentifySched2CallerSaveAndSpillOffsets` proves `off == 20`
  // is a reload spill slot (`all_saved_gprs_used && n_stores >= 1 && n_loads >= 1`).
  sched2_cfg.rpo_blocks[1].id = 1;
  sched2_cfg.rpo_blocks[1].has_label = true;
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2))) "
      "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil)))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const CompilationProfile profile{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg, profile);
  ASSERT_EQ(lifted.BlockCount(), 2u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 3 v1))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2],
            "(set (reg:SI 6 a2) (mem/s/u:SI (plus:SI (reg:SI 29 sp) (const_int 80))))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, CanonicalizesAdjacentSpillInputReloadPairsToOperandOrder) {
  // When `sched2` reorders two adjacent reload spill-slot input loads feeding the same binary
  // instruction (`operand 1` loaded before `operand 0`), `InvertScheduleBlock` restores GCC
  // `find_reloads` ascending operand order (`operand 0` reload before `operand 1` reload).
  hooks_.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2", "-mips2"}});
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.frame_size_bytes = 16;
  sched2_cfg.sideband.outgoing_args_size = 16;
  sched2_cfg.sideband.saved_gpr_regnos = {16, 17, 18, 19, 20, 21, 22, 23, 30, 31};
  sched2_cfg.rpo_blocks.resize(2);

  // Two 8-byte reload spill buckets at `16` (`off = 20`) and `24` (`off = 28`).
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 4 a0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 5 a1)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (pc) (label_ref L0))");
  sched2_cfg.rpo_blocks[0].successors = {1};

  // In `B1`, `[sp+28] -> t2` (`operand 1` of `minus`) appears before `[sp+20] -> t1` (`operand 0`
  // of `minus`).
  sched2_cfg.rpo_blocks[1].id = 1;
  sched2_cfg.rpo_blocks[1].has_label = true;
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (minus:SI (reg:SI 9 t1) (reg:SI 10 t2))) "
      "(expr_list:REG_DEAD (reg:SI 10 t2) "
      "(expr_list:REG_DEAD (reg:SI 9 t1) (nil)))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const CompilationProfile profile{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg, profile);
  ASSERT_EQ(lifted.BlockCount(), 2u);
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0],
            "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[1],
            "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
}

TEST_F(InvertScheduleBlockTest, RestoresCrossBankReloadSpillRoundRobinOrder) {
  // In `reload1.c` (`compare_spill_regs` + `allocate_reload_reg`), `GR_REGS` spill registers
  // (`9 = t1`, `10 = t2`) precede `FP_REGS` spill registers (`40 = $f8`) in `spill_regs[]`.
  // Witnessing `t1` (`[sp+20]`), then `$f8` (`mtc1 $fp, $f8`), then `t2` (`[sp+28]`) proves
  // `sched2` hoisted the `t1` reload + consumer ahead of the `$f8` reload + consumer; verify
  // `InvertScheduleBlock` sinks `[g1, g1 + 1]` back immediately after `[f, f + 1]`.
  hooks_.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2", "-mips2"}});
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.frame_size_bytes = 16;
  sched2_cfg.sideband.outgoing_args_size = 16;
  sched2_cfg.sideband.saved_gpr_regnos = {16, 17, 18, 19, 20, 21, 22, 23, 30, 31};
  sched2_cfg.rpo_blocks.resize(2);

  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 4 a0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 5 a1)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (pc) (label_ref L0))");
  sched2_cfg.rpo_blocks[0].successors = {1};

  sched2_cfg.rpo_blocks[1].id = 1;
  sched2_cfg.rpo_blocks[1].has_label = true;
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 2 v0) (xor:SI (reg:SI 9 t1) (reg:SI 19 s3))) "
      "(expr_list:REG_DEAD (reg:SI 9 t1) (expr_list:REG_DEAD (reg:SI 19 s3) (nil)))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SF 40 $f8) (reg:SF 30 $fp))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SF 32 $f0) (mult:SF (reg:SF 40 $f8) (reg:SF 62 $f30))) "
      "(expr_list:REG_DEAD (reg:SF 40 $f8) (expr_list:REG_DEAD (reg:SF 62 $f30) "
      "(expr_list:REG_DEAD (reg:SF 30 $fp) (nil))))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 2 v0))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 16 s0) (xor:SI (reg:SI 16 s0) (reg:SI 10 t2))) "
      "(expr_list:REG_DEAD (reg:SI 10 t2) (nil))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SF 34 $f2) (float:SF (reg:SI 16 s0))) "
      "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
      "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil)))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))");

  const CompilationProfile profile{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg, profile);
  ASSERT_EQ(lifted.BlockCount(), 2u);
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 10u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0], "(set (reg:SF 40 $f8) (reg:SF 30 $fp))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[2],
            "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
}

TEST_F(InvertScheduleBlockTest,
       UnschedulesSunkMdOutputReloadHoistedFprProducerAndReloadOtherDeadNotes) {
  hooks_.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2", "-mips1"}});
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  // 1. Two `mulsi3_internal` `PARALLEL`s in one block (`other_md == true`), with the second
  //    `mflo` (`(set 3 v1 (reg:SI 65 lo))`) sunk past `(set 6 a2 (plus 9 t1 6 a2))`.
  // 2. A `sched2`-hoisted `"=f"` output reload producer `(set 34 $f2 (float:SF 6 a2))` ahead of
  //    `(set 2 v0 (mem:SI 5 a1))` reading `5 a1` (`WAR` hazard preventing hoisting `(set 5 a1
  //    $f2)`).
  // 3. A `RELOAD_OTHER` cross-bank `"f"` reload `(set 36 $f4 7 a3) ; (set 36 $f4 (plus 36 $f4
  // ...))`
  //    followed by `(set 7 a3 36 $f4)`.
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_UNUSED (reg:SI 64 hi) "
      "(expr_list:REG_UNUSED (reg:SI 66 accum) (nil))))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 9 t1) (reg:SI 65 lo))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 6 a2) (reg:SI 5 a1))) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
      "(expr_list:REG_UNUSED (reg:SI 64 hi) (expr_list:REG_UNUSED (reg:SI 66 accum) (nil)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 6 a2) (plus:SI (reg:SI 9 t1) (reg:SI 6 a2))) "
      "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (reg:SI 65 lo))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 6 a2) (minus:SI (reg:SI 6 a2) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 34 $f2) (float:SF (reg:SI 6 a2))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 5 a1) (reg:SF 34 $f2))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 36 $f4) (reg:SF 7 a3)) (expr_list:REG_DEAD (reg:SI 7 a3) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 44 $f12))) "
      "(expr_list:REG_DEAD (reg:SF 44 $f12) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 7 a3) (reg:SF 36 $f4))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SF 5 a1) (expr_list:REG_DEAD (reg:SF 7 a3) (nil)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const CompilationProfile profile{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg, profile);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  const auto& insns = lifted.rpo_blocks[0].instructions;
  ASSERT_EQ(insns.size(), 14u);
  EXPECT_EQ(insns[3], "(set (reg:SI 3 v1) (reg:SI 65 lo))");
  EXPECT_EQ(insns[7],
            "(set (reg:SF 34 $f2) (float:SF (reg:SI 6 a2))) "
            "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  EXPECT_EQ(insns[8], "(set (reg:SF 5 a1) (reg:SF 34 $f2))");
  EXPECT_EQ(insns[9], "(set (reg:SF 36 $f4) (reg:SF 7 a3))");
  EXPECT_EQ(insns[10],
            "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 44 $f12))) "
            "(expr_list:REG_DEAD (reg:SF 7 a3) (expr_list:REG_DEAD (reg:SF 44 $f12) (nil)))");
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
      if (ParseCompilationProfileFlags(trace.stage0_input.profile).RunsSched2()) {
        hooks_.ApplyCompilationProfile(trace.stage0_input.profile);
        const gcc_harness::RightInverseResult sched_only_res =
            verifier_.VerifySched2StageRightInverse(snap.post_state);
        EXPECT_TRUE(sched_only_res.matches) << "VerifySched2StageRightInverse failed for "
                                            << tc.test_name << ": " << sched_only_res.diff_detail;
      }
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
