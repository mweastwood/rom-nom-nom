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
#include "lifter/invert_update_flow_info.h"
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

  // Under `-O2 -fno-schedule-insns` (`!RunsSched1()`), `InvertScheduleBlock` unschedules
  // the load latency slot (`alu` before `load`). Under `-O2` (`RunsSched1()`), Stage 3
  // selects the unique `sched1`-achievable preimage (`load` before `alu`), since `sched1`
  // always hoists `ready=3` loads ahead of `ready=1` ALU ops feeding the same add.
  const CompilationProfile no_s1_profile{.optimize_level = 2,
                                         .c_flags = {"-O2", "-fno-schedule-insns"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(sched2_cfg, no_s1_profile);
  ASSERT_EQ(lifted_no_s1.BlockCount(), 1u);
  EXPECT_EQ(lifted_no_s1.pass, RtlPassKind::kGreg);
  ASSERT_EQ(lifted_no_s1.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted_no_s1.rpo_blocks[0].instructions[0],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  EXPECT_EQ(lifted_no_s1.rpo_blocks[0].instructions[1],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  EXPECT_EQ(lifted.pass, RtlPassKind::kGreg);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");

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

  const CompilationProfile no_s1_profile{.optimize_level = 2,
                                         .c_flags = {"-O2", "-fno-schedule-insns"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(sched2_cfg, no_s1_profile);
  ASSERT_EQ(lifted_no_s1.BlockCount(), 1u);
  ASSERT_EQ(lifted_no_s1.rpo_blocks[0].instructions.size(), 5u);
  EXPECT_EQ(lifted_no_s1.rpo_blocks[0].instructions[0],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 7))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  EXPECT_EQ(lifted_no_s1.rpo_blocks[0].instructions[1],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 5u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
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
  const CompilationProfile mips1_profile{.optimize_level = 2,
                                         .c_flags = {"-O2", "-mips1", "-funsigned-char"}};
  hooks_.ApplyCompilationProfile(mips1_profile);
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

  const RtlControlFlowGraph unscheduled = InvertScheduleBlock(sched1_cfg, mips1_profile);
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

TEST_F(InvertScheduleBlockTest, UpdateFlowInfoHandlesNonAdjacentSubregAndInterferingPairs) {
  // 1. Non-adjacent `lui` / `ori` split pair separated by an independent instruction, plus a
  //    `(subreg:SI (reg:DI 74) 1)` split pair (`mips.md:3209` `register_operand`).
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched;
  cfg.sideband.reg_n_sets[72] = 2;
  cfg.sideband.reg_n_sets[74] = 2;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 305397760))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 4 a0) (const_int 5)))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (ior:SI (reg:SI 72) (const_int 22136))) "
      "(expr_list:REG_EQUAL (const_int 305419896) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (subreg:SI (reg:DI 74) 1) (const_int 305397760))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (subreg:SI (reg:DI 74) 1) (ior:SI (subreg:SI (reg:DI 74) 1) (const_int 22136)))");

  const RtlControlFlowGraph merged = InvertUpdateFlowInfo(cfg);
  ASSERT_EQ(merged.BlockCount(), 1u);
  ASSERT_EQ(merged.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(merged.rpo_blocks[0].instructions[0],
            "(set (reg:SI 73) (plus:SI (reg:SI 4 a0) (const_int 5)))");
  EXPECT_EQ(merged.rpo_blocks[0].instructions[1],
            "(set (reg:SI 72) (const_int 305419896)) "
            "(expr_list:REG_EQUAL (const_int 305419896) (nil))");
  EXPECT_EQ(merged.rpo_blocks[0].instructions[2],
            "(set (subreg:SI (reg:DI 74) 1) (const_int 305419896))");
  EXPECT_EQ(merged.sideband.reg_n_sets.at(72), 1);
  EXPECT_EQ(merged.sideband.reg_n_sets.at(74), 1);

  // 2. Intervening read of the target register must block merging.
  RtlControlFlowGraph blocked_cfg;
  blocked_cfg.pass = RtlPassKind::kSched;
  blocked_cfg.sideband.reg_n_sets[72] = 2;
  blocked_cfg.rpo_blocks.resize(1);
  blocked_cfg.rpo_blocks[0].id = 0;
  blocked_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 305397760))");
  blocked_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (reg:SI 72))");
  blocked_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (ior:SI (reg:SI 72) (const_int 22136)))");
  const RtlControlFlowGraph not_merged = InvertUpdateFlowInfo(blocked_cfg);
  ASSERT_EQ(not_merged.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(not_merged.sideband.reg_n_sets.at(72), 2);

  // 2b. Mid-block `NOTE_INSN_LOOP_BEG` or `unspec_volatile` barrier also blocks non-adjacent merge.
  RtlControlFlowGraph loop_barrier_cfg;
  loop_barrier_cfg.pass = RtlPassKind::kSched;
  loop_barrier_cfg.sideband.reg_n_sets[72] = 2;
  loop_barrier_cfg.rpo_blocks.resize(1);
  loop_barrier_cfg.rpo_blocks[0].id = 0;
  loop_barrier_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 305397760))");
  loop_barrier_cfg.rpo_blocks[0].AddLoopNote(RtlLoopNoteKind::kLoopBeg);
  loop_barrier_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (ior:SI (reg:SI 72) (const_int 22136)))");
  const RtlControlFlowGraph not_merged_barrier = InvertUpdateFlowInfo(loop_barrier_cfg);
  ASSERT_EQ(not_merged_barrier.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(not_merged_barrier.sideband.reg_n_sets.at(72), 2);

  // 3. Hardened validation rejects negative regnos and out-of-bounds sideband entries.
  RtlControlFlowGraph bad_sideband = cfg;
  bad_sideband.sideband.basic_block_live_at_start[5] = {4};
  EXPECT_TRUE(InvertScheduleBlock(bad_sideband).rpo_blocks.empty());
  EXPECT_TRUE(InvertUpdateFlowInfo(bad_sideband).rpo_blocks.empty());

  RtlControlFlowGraph neg_reg_sideband = cfg;
  neg_reg_sideband.sideband.reg_n_sets[-1] = 1;
  EXPECT_TRUE(InvertScheduleBlock(neg_reg_sideband).rpo_blocks.empty());
  EXPECT_TRUE(InvertUpdateFlowInfo(neg_reg_sideband).rpo_blocks.empty());
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

  const CompilationProfile no_s1_profile{.optimize_level = 2,
                                         .c_flags = {"-O2", "-fno-schedule-insns"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(multi_cfg, no_s1_profile);
  ASSERT_EQ(lifted_no_s1.BlockCount(), 4u);
  EXPECT_EQ(lifted_no_s1.rpo_blocks[1].instructions[0],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 6 a2) (const_int 4))) "
            "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  EXPECT_EQ(lifted_no_s1.rpo_blocks[1].instructions[1],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(multi_cfg);
  ASSERT_EQ(lifted.BlockCount(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[1],
            "(set (reg:SI 3 v1) (plus:SI (reg:SI 6 a2) (const_int 4))) "
            "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");

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

  const CompilationProfile no_s1_profile{
      .optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-fno-schedule-insns"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(sched2_cfg, no_s1_profile);
  ASSERT_EQ(lifted_no_s1.BlockCount(), 1u);
  ASSERT_EQ(lifted_no_s1.rpo_blocks[0].instructions.size(), 10u);
  EXPECT_EQ(lifted_no_s1.rpo_blocks[0].instructions[2],
            "(set (reg:SF 60 $f28) (plus:SF (reg:SF 60 $f28) (reg:SF 54 $f22))) "
            "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 10u);
  // Under `-O2`, `$f28 += $f22` (`fadd`, ready 4) is still unscheduled before the first
  // call (at index 1, ahead of `a1 = 0`, for `sched1` achievability).
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
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
  // instruction (`operand 1` loaded before `operand 0`, e.g., on `r4000` where `t1` depends on
  // an earlier spill store `sw 20($sp)` giving `t1` higher backward-schedule priority while
  // `Blockage(kLoad, kStore) == 2 > Blockage(kLoad, kLoad) == 1` stalls `sw` behind `t2`),
  // `InvertScheduleBlock` restores GCC `find_reloads` ascending operand order (`operand 0`
  // reload before `operand 1` reload) while preserving exact `kSched2` replay.
  const CompilationProfile profile{.optimize_level = 2, .c_flags = {"-O2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(profile);
  RtlControlFlowGraph pre_sched2_cfg;
  pre_sched2_cfg.pass = RtlPassKind::kSched2;
  pre_sched2_cfg.sideband.frame_size_bytes = 32;
  pre_sched2_cfg.sideband.outgoing_args_size = 16;
  pre_sched2_cfg.sideband.saved_gpr_regnos = {16, 17, 18, 19, 20, 21, 22, 23, 30, 31};
  pre_sched2_cfg.rpo_blocks.resize(2);

  // Two 8-byte reload spill buckets at `16` (`off = 20`) and `24` (`off = 28`).
  pre_sched2_cfg.rpo_blocks[0].id = 0;
  pre_sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 4 a0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pre_sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 5 a1)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  pre_sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (pc) (label_ref L0))");
  pre_sched2_cfg.rpo_blocks[0].successors = {1};

  // Pre-`sched2` `B1`: `[sp+20] -> t1` (operand 0) precedes `[sp+28] -> t2` (operand 1).
  // `t2` is hoisted ahead of `sw 20($sp)` by `sched2` (`[a3 = mem(a2), t2, sw 20($sp), t1,
  // minus]`); `InvertScheduleBlock` sinks `t2` back to `minus` (producing `[t2, t1, minus]`) and
  // then `CanonicalizeAdjacentSpillInputReloadPairs` restores ascending operand order `[t1, t2,
  // minus]` while preserving exact `kSched2` replay!
  pre_sched2_cfg.rpo_blocks[1].id = 1;
  pre_sched2_cfg.rpo_blocks[1].has_label = true;
  pre_sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 7 a3) (mem:SI (reg:SI 6 a2))) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  pre_sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 7 a3))");
  pre_sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  pre_sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
  pre_sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (minus:SI (reg:SI 9 t1) (reg:SI 10 t2))) "
      "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) "
      "(expr_list:REG_DEAD (reg:SI 9 t1) (nil)))");
  pre_sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
      gcc_harness::SubfunctionId::kScheduleBlockSched2, pre_sched2_cfg, {}, 2, profile.c_flags);
  const RtlControlFlowGraph& sched2_cfg = fwd.post_state;
  ASSERT_EQ(sched2_cfg.BlockCount(), 2u);
  ASSERT_EQ(sched2_cfg.rpo_blocks[1].instructions.size(), 6u);
  // Confirm real GCC `sched2` hoisted `t2` (operand 1) ahead of `sw 20($sp)` and `t1` (operand 0).
  EXPECT_EQ(sched2_cfg.rpo_blocks[1].instructions[1],
            "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))")
      << sched2_cfg.FormatSummary();
  EXPECT_EQ(sched2_cfg.rpo_blocks[1].instructions[3],
            "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))")
      << sched2_cfg.FormatSummary();

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg, profile);
  ASSERT_EQ(lifted.BlockCount(), 2u);
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 6u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[2],
            "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[3],
            "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifySubfunctionBoundaryRightInverse(fwd);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;
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

TEST_F(InvertScheduleBlockTest,
       RunsSched1CallerSavedDeadOrderGuardAndSched2DeletedNoopMovesRightInverse) {
  // 1. `UnscheduleBlockInstructions` `runs_sched1` caller-saved `REG_DEAD` `gpr_alloc_ord` guard:
  //    under `-O3` (`runs_sched1 == true`), a general-pointer load into `$8` (`t0`, `ord = 6`)
  //    followed by a 1-cycle ALU killing `$5` (`a1`, `ord = 3 < 6`) was already in `[load, alu]`
  //    order after `sched1` (so `local_alloc` saw `$5` live across the load and picked `$8`) and
  //    must NOT be swapped into `[alu, load]` by `InvertScheduleBlock`.
  RtlControlFlowGraph o3_sched2_cfg;
  o3_sched2_cfg.pass = RtlPassKind::kSched2;
  o3_sched2_cfg.sideband.basic_block_live_at_start[0] = {5, 12, 29, 31};
  o3_sched2_cfg.rpo_blocks.resize(1);
  o3_sched2_cfg.rpo_blocks[0].id = 0;
  o3_sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 8 t0) (mem:SI (reg:SI 12 t4))) (expr_list:REG_DEAD (reg:SI 12 t4) (nil))");
  o3_sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 9 t1) (plus:SI (reg:SI 5 a1) (const_int 3))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  o3_sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))");
  o3_sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const CompilationProfile o3_prof{.optimize_level = 3, .c_flags = {"-O3", "-mips2"}};
  const RtlControlFlowGraph lifted_o3 = InvertScheduleBlock(o3_sched2_cfg, o3_prof);
  ASSERT_EQ(lifted_o3.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_NE(lifted_o3.rpo_blocks[0].instructions[0].find("(mem:SI (reg:SI 12 t4))"),
            std::string::npos);
  hooks_.ApplyCompilationProfile(o3_prof);
  const gcc_harness::RightInverseResult o3_res =
      verifier_.VerifySched2StageRightInverse(o3_sched2_cfg);
  EXPECT_TRUE(o3_res.matches) << o3_res.diff_detail;

  // Contrast: with `-fno-schedule-insns` (`runs_sched1 == false`), `InvertScheduleBlock` swaps
  // `[load, alu]` back to pre-`sched2` `[alu, load]` order and satisfies
  // `VerifySched2StageRightInverse`.
  const CompilationProfile no_s1_prof{.optimize_level = 2,
                                      .c_flags = {"-O2", "-fno-schedule-insns", "-mips2"}};
  const RtlControlFlowGraph lifted_o2 = InvertScheduleBlock(o3_sched2_cfg, no_s1_prof);
  ASSERT_EQ(lifted_o2.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_NE(lifted_o2.rpo_blocks[0].instructions[0].find("(plus:SI (reg:SI 5 a1)"),
            std::string::npos);
  hooks_.ApplyCompilationProfile(no_s1_prof);
  EXPECT_TRUE(verifier_.VerifySched2StageRightInverse(o3_sched2_cfg).matches);

  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  // 2. Stage 3 post-reload noop-move restoration (`RestoreSched2DeletedNoopMoves` inside
  //    `InvertScheduleBlock` and `RestoreSched2PreScheduleDeletedNoopMoves` folded into
  //    `InvertThreadPrologueAndEpilogueInsns`).
  RtlControlFlowGraph arg_pref_cfg;
  arg_pref_cfg.pass = RtlPassKind::kSched2;
  arg_pref_cfg.sideband.basic_block_live_at_start = {{0, {4}}, {1, {3, 4}}, {2, {3, 4}}};
  arg_pref_cfg.rpo_blocks.resize(3);
  arg_pref_cfg.rpo_blocks[0].id = 0;
  arg_pref_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (const_int 0))");
  arg_pref_cfg.rpo_blocks[0].successors = {1};
  arg_pref_cfg.rpo_blocks[1].id = 1;
  arg_pref_cfg.rpo_blocks[1].has_label = true;
  arg_pref_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1)))");
  arg_pref_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (lt:SI (reg:SI 3 v1) (reg:SI 4 a0)) (label_ref L0) (pc)))");
  arg_pref_cfg.rpo_blocks[1].successors = {1, 2};
  arg_pref_cfg.rpo_blocks[2].id = 2;
  arg_pref_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
  arg_pref_cfg.rpo_blocks[2].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted_arg_pref =
      InvertThreadPrologueAndEpilogueInsns(InvertScheduleBlock(arg_pref_cfg, o2_prof), o2_prof);
  ASSERT_GE(lifted_arg_pref.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(lifted_arg_pref.rpo_blocks[0].instructions[0],
            "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  EXPECT_TRUE(verifier_.VerifySched2StageRightInverse(arg_pref_cfg).matches);
}

TEST_F(InvertScheduleBlockTest, Sched1ModelInversionReunitesSplitLargeIntPair) {
  // Stage 6 `UnscheduleSched1WithModel`: when `sched1` interleaves an independent load between
  // a split `large_int` `lui` (`0x12340000` == `305397760`) and `ori` (`0x5678` == `22136`),
  // `InvertScheduleBlock(..., RtlPassKind::kSched)` uses `sched_model::InvertBlockOrder(kSched1)`
  // with `preferred_adjacent` pairs to reunite `lui; ori` into adjacent slots while still
  // matching exact forward `sched1` replay.
  const CompilationProfile profile{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(profile);

  RtlControlFlowGraph sched1_cfg;
  sched1_cfg.pass = RtlPassKind::kSched;
  sched1_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29, 31};
  sched1_cfg.sideband.reg_n_sets[72] = 2;
  sched1_cfg.sideband.reg_n_sets[73] = 1;
  sched1_cfg.rpo_blocks.resize(1);
  sched1_cfg.rpo_blocks[0].id = 0;
  sched1_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 305397760))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (mem:SI (reg:SI 4 a0))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (ior:SI (reg:SI 72) (const_int 22136))) "
      "(expr_list:REG_EQUAL (const_int 305419896) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 74) (plus:SI (reg:SI 5 a1) (const_int 3))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 75) (plus:SI (reg:SI 73) (reg:SI 72))) "
      "(expr_list:REG_DEAD (reg:SI 73) (expr_list:REG_DEAD (reg:SI 72) (nil)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (xor:SI (reg:SI 75) (reg:SI 74))) "
      "(expr_list:REG_DEAD (reg:SI 75) (expr_list:REG_DEAD (reg:SI 74) (nil)))");
  sched1_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph unscheduled = InvertScheduleBlock(sched1_cfg, profile);
  ASSERT_EQ(unscheduled.BlockCount(), 1u);
  EXPECT_EQ(unscheduled.pass, RtlPassKind::kCombine);
  const auto& insns = unscheduled.rpo_blocks[0].instructions;
  ASSERT_EQ(insns.size(), 7u);

  size_t lui_idx = insns.size();
  size_t ori_idx = insns.size();
  for (size_t i = 0; i < insns.size(); ++i) {
    if (insns[i].find("(const_int 305397760)") != std::string::npos) {
      lui_idx = i;
    } else if (insns[i].find("(const_int 22136)") != std::string::npos) {
      ori_idx = i;
    }
  }
  ASSERT_LT(lui_idx, insns.size());
  ASSERT_LT(ori_idx, insns.size());
  EXPECT_EQ(ori_idx, lui_idx + 1u);

  const RtlControlFlowGraph merged = InvertUpdateFlowInfo(unscheduled);
  ASSERT_EQ(merged.rpo_blocks[0].instructions.size(), 6u);
  EXPECT_EQ(merged.sideband.reg_n_sets.at(72), 1);

  const gcc_harness::RightInverseResult sb_res =
      verifier_.VerifyScheduleBlockRightInverse(sched1_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched1StageRightInverse(sched1_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest,
       Sched2PreimageSelectionPicksSched1AchievableOrderPreservingInvariants) {
  // Stage 3 `SelectSched1AchievablePreimageForSched2`: when `s0` and `s1` stay live into `B1`,
  // swapping `[load v0 = *s0, alu v1 = s1 + 2]` preserves exact hard-register interferences,
  // `REG_DEAD` order, and `MEM` equivalence validity. Under `-O2 -mips2 -mcpu=r4000`
  // (`RunsSched1() == true`), `UnscheduleBlockInstructions` produces `Q_base = [alu, load, ...]`
  // (outside `Im(F_sched1)`), and `SelectSched1AchievablePreimageForSched2` selects the
  // forward-`sched2`-verified preimage `Q_cand = [load, alu, ...]` (inside `Im(F_sched1)`).
  // With `-fno-schedule-insns` (`RunsSched1() == false`), `Q_base = [alu, load, ...]` is kept.
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.basic_block_live_at_start[0] = {16, 17};
  sched2_cfg.sideband.basic_block_live_at_start[1] = {2, 16, 17};
  sched2_cfg.rpo_blocks.resize(2);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 16 s0)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 17 s1) (const_int 2)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (pc) (label_ref L0))");
  sched2_cfg.rpo_blocks[0].successors = {1};

  sched2_cfg.rpo_blocks[1].id = 1;
  sched2_cfg.rpo_blocks[1].has_label = true;
  sched2_cfg.rpo_blocks[1].predecessors = {0};
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
      "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 17 s1))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))");
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_o2 = InvertScheduleBlock(sched2_cfg, o2_prof);
  ASSERT_EQ(lifted_o2.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_NE(lifted_o2.rpo_blocks[0].instructions[0].find("(mem:SI (reg:SI 16 s0))"),
            std::string::npos);
  hooks_.ApplyCompilationProfile(o2_prof);
  const gcc_harness::RightInverseResult o2_stage =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(o2_stage.matches) << o2_stage.diff_detail;

  const CompilationProfile no_s1_prof{
      .optimize_level = 2, .c_flags = {"-O2", "-fno-schedule-insns", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(sched2_cfg, no_s1_prof);
  ASSERT_EQ(lifted_no_s1.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_NE(lifted_no_s1.rpo_blocks[0].instructions[0].find("(plus:SI (reg:SI 17 s1)"),
            std::string::npos);
  hooks_.ApplyCompilationProfile(no_s1_prof);
  const gcc_harness::RightInverseResult no_s1_stage =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(no_s1_stage.matches) << no_s1_stage.diff_detail;
}

TEST_F(InvertScheduleBlockTest, Stage3SelectorAllowsDistinctHardRegInterferenceChange) {
  const CompilationProfile o2_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(o2_profile);

  // Two independent ALU+store chains (`v0` / `v1`) whose hard-reg interferences differ
  // between `sched2` and `sched1` orders (`v0` dying before `v1` is born).
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 6, 29};
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (reg:SI 6 a2)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (minus:SI (reg:SI 5 a1) (reg:SI 6 a2))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SI 3 v1)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg, o2_profile);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
  // Gate (b) allows distinct hard-reg interference changes (`v0` dying before `v1` is born),
  // selecting `[v0=a1+a2; mem[a0]=v0; v1=a1-a2; mem[a0+4]=v1]` in `Im(F_sched1)`.
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
            "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2],
            "(set (reg:SI 3 v1) (minus:SI (reg:SI 5 a1) (reg:SI 6 a2))) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))");

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, Stage3SelectorPreservesMdOutputAndStackReloadContiguity) {
  const CompilationProfile o2_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(o2_profile);

  // Post-reload `sched2`-scheduled block containing a `smulsi3_highpart` -> `mfhi` (`reg 64`)
  // output reload (with `sched2` having interleaved `lw t0, 8(sp)` and `v1 = a2 + 1` in the
  // multiply latency shadow). `InvertScheduleBlock` must restore `mfhi` contiguously after
  // `mult` and keep the plain stack-slot input reload `lw t0` contiguous before its consumer,
  // while preserving exact `sched2` right-inverse replay.
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 6, 29};
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI "
      "(sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1))) (const_int 32)))) "
      "(clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 8))))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 6 a2) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (reg:SI 64 hi)) (expr_list:REG_DEAD (reg:SI 64 hi) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 8 t0))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_cfg, o2_profile);
  ASSERT_EQ(lifted.BlockCount(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 7u);
  const auto& insns = lifted.rpo_blocks[0].instructions;
  auto find_substr = [&](const std::string& sub) -> int {
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].find(sub) != std::string::npos) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  const int mul_pos = find_substr("(mult:DI");
  const int mfhi_pos = find_substr("(set (reg:SI 2 v0) (reg:SI 64 hi))");
  const int stack_ld_pos = find_substr("(mem:SI (plus:SI (reg:SI 29 sp) (const_int 8)))");
  const int stack_use_pos = find_substr("(plus:SI (reg:SI 2 v0) (reg:SI 8 t0))");
  ASSERT_GE(mul_pos, 0);
  ASSERT_GE(mfhi_pos, 0);
  ASSERT_GE(stack_ld_pos, 0);
  ASSERT_GE(stack_use_pos, 0);
  EXPECT_EQ(mfhi_pos, mul_pos + 1);
  EXPECT_EQ(stack_ld_pos + 1, stack_use_pos);

  const gcc_harness::RightInverseResult stage_res =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, ReadjoinsAndPreservesCrossBankFprToGprOutputReloadContiguity) {
  // Mirrors `reload1.c:5641, 6239-6285` (`emit_reload_insns`): a cross-bank
  // `"=f"` FPR->GPR (`mfc1`) output reload `(set (reg:SF 5 a1) (reg:SF 34 $f2))`
  // carrying no `REG_DEAD $f2` was emitted immediately after its producing FP
  // arithmetic insn `(set (reg:SF 34 $f2) (mult:SF ...))` and then separated by
  // `sched2` interleaving an independent GPR arg setup into the FP latency
  // shadow. Both `ReadjoinCrossBankFprOutputReloads` and
  // `sched_model::IsAdjacentReloadContiguityPair` must reunite and preserve
  // adjacency through `SelectSched1AchievablePreimageForSched2`.
  const CompilationProfile profile{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(profile);

  auto build_cfg = [](bool move_has_reg_dead) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start[0] = {16, 29, 31, 44, 46};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 34 $f2) (mult:SF (reg:SF 44 $f12) (reg:SF 46 $f14))) "
        "(expr_list:REG_DEAD (reg:SF 44 $f12) (expr_list:REG_DEAD (reg:SF 46 $f14) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 4 a0) (mem:SI (reg:SI 16 s0)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        move_has_reg_dead
            ? "(set (reg:SF 5 a1) (reg:SF 34 $f2)) (expr_list:REG_DEAD (reg:SF 34 $f2) (nil))"
            : "(set (reg:SF 5 a1) (reg:SF 34 $f2))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[0].rtl_insns.back().has_explicit_call_fusage = true;
    cfg.rpo_blocks[0].rtl_insns.back().call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSf, 5)),
    };
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 16 s0) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
    return cfg;
  };

  // Positive: reload-emitted `mfc1` (no `REG_DEAD $f2`) is reunited directly
  // after `mult:SF` and survives `SelectSched1AchievablePreimageForSched2`.
  const RtlControlFlowGraph reload_cfg = build_cfg(/*move_has_reg_dead=*/false);
  const RtlControlFlowGraph lifted_reload = InvertScheduleBlock(reload_cfg, profile);
  ASSERT_EQ(lifted_reload.rpo_blocks.size(), 1u);
  const auto& reload_insns = lifted_reload.rpo_blocks[0].instructions;
  ASSERT_EQ(reload_insns.size(), 6u);
  EXPECT_NE(reload_insns[0].find("(mult:SF"), std::string::npos) << reload_insns[0];
  EXPECT_EQ(reload_insns[1], "(set (reg:SF 5 a1) (reg:SF 34 $f2))") << reload_insns[1];
  const gcc_harness::RightInverseResult pos_res =
      verifier_.VerifySched2StageRightInverse(reload_cfg);
  EXPECT_TRUE(pos_res.matches) << pos_res.diff_detail;

  // Negative: when `(set (reg:SF 5 a1) (reg:SF 34 $f2))` carries `REG_DEAD $f2`,
  // it is a pre-reload pseudo copy rather than a `reload1.c` output reload and
  // is not forcibly hoisted ahead of `(set (reg:SI 4 a0) (reg:SI 16 s0))`.
  const RtlControlFlowGraph copy_cfg = build_cfg(/*move_has_reg_dead=*/true);
  const RtlControlFlowGraph lifted_copy = InvertScheduleBlock(copy_cfg, profile);
  ASSERT_EQ(lifted_copy.rpo_blocks.size(), 1u);
  const auto& copy_insns = lifted_copy.rpo_blocks[0].instructions;
  ASSERT_EQ(copy_insns.size(), 6u);
  EXPECT_NE(copy_insns[1].find("(set (reg:SI 4 a0) (mem:SI (reg:SI 16 s0)))"), std::string::npos)
      << copy_insns[1];
  const gcc_harness::RightInverseResult neg_res = verifier_.VerifySched2StageRightInverse(copy_cfg);
  EXPECT_TRUE(neg_res.matches) << neg_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest, SelectSched1AchievablePreimageHoistsDisplacedBlock0EntryArgCopy) {
  // Post-reload `sched2` on block 0 frequently interleaves prologue callee-save stores
  // (`toplev.c:3103`, `function.c:5516`) and an early body load ahead of a second
  // entry-parameter copy `(set s2 a1)` (`function.c:5129-5144`, `sched.c:3191-3215`).
  // Stage 3 `SelectSched1AchievablePreimageForSched2` must hoist `(set s2 a1)` back
  // into the block-0 entry-arg prefix while verifying bit-exact `sched2` right-inverse.
  const CompilationProfile profile{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(profile);

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(3);
  cfg.sideband.basic_block_live_at_start[0] = {4, 5, 17, 18, 29, 31};
  cfg.sideband.basic_block_live_at_start[1] = {3, 17, 18, 29, 31};
  cfg.sideband.basic_block_live_at_start[2] = {3, 17, 18, 29, 31};

  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 17 s1)) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 17 s1) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 31 ra))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 18 s2)) "
      "(expr_list:REG_DEAD (reg:SI 18 s2) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:HI 2 v0) (mem:HI (plus:SI (reg:SI 17 s1) (const_int 12)))) "
      "(expr_list:REG_EQUIV (mem:HI (plus:SI (reg:SI 17 s1) (const_int 12))) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 18 s2) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (const_int 0))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (and:SI (subreg:SI (reg:HI 2 v0) 0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:HI 2 v0) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref L0) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].predecessors = {0, 1};
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1)))");

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].has_label = true;
  cfg.rpo_blocks[2].predecessors = {0, 1};
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 17 s1) (reg:SI 18 s2))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (expr_list:REG_DEAD (reg:SI 18 s2) (nil)))");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted = InvertScheduleBlock(cfg, profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  const auto& b0 = lifted.rpo_blocks[0].instructions;
  ASSERT_EQ(b0.size(), 10u);
  auto idx_of = [](const std::vector<std::string>& insns, std::string_view needle) -> int {
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].find(needle) != std::string::npos) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  const int s2_copy_idx = idx_of(b0, "(set (reg:SI 18 s2) (reg:SI 5 a1))");
  const int load_idx = idx_of(b0, "(mem:HI (plus:SI (reg:SI 17 s1) (const_int 12)))");
  const int sw_s2_idx = idx_of(b0, "(mem:SI (plus:SI (reg:SI 29 sp) (const_int 24)))");
  ASSERT_GE(s2_copy_idx, 0);
  ASSERT_GE(load_idx, 0);
  ASSERT_GE(sw_s2_idx, 0);
  // Positive: displaced `(set s2 a1)` stays after prologue save `sw s2` and hoists ahead of load.
  EXPECT_LT(sw_s2_idx, s2_copy_idx);
  EXPECT_LT(s2_copy_idx, load_idx);

  const gcc_harness::RightInverseResult res = verifier_.VerifySched2StageRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  // Negative: when `-fno-schedule-insns` disables `sched1`, Stage 3 keeps the post-`sched2`
  // placement without hoisting `(set s2 a1)` ahead of the load.
  const CompilationProfile no_s1_prof{
      .optimize_level = 2, .c_flags = {"-O2", "-fno-schedule-insns", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(cfg, no_s1_prof);
  ASSERT_EQ(lifted_no_s1.rpo_blocks.size(), 3u);
  const auto& no_s1_b0 = lifted_no_s1.rpo_blocks[0].instructions;
  EXPECT_GT(idx_of(no_s1_b0, "(set (reg:SI 18 s2) (reg:SI 5 a1))"),
            idx_of(no_s1_b0, "(mem:HI (plus:SI (reg:SI 17 s1) (const_int 12)))"));
}

TEST_F(InvertScheduleBlockTest, Stage3PreimageSelectionDisablesImplicitPostCallRmwSplits) {
  // Mirrors `flow.c:2506-2515` and Stage 4's `REG_DEAD` conservation invariant:
  // when a post-call RMW `A = (set $s0 (plus:SI $s0 $v0))` has NO
  // `(REG_DEAD $s0)` note, Stage 4 keeps `$s0` as a single multi-set pseudo
  // (`reg_n_sets == 2`, `birthing_insn_p == false`), so `sched1` always places
  // `A` before a single-set post-call load `B = (set $v1 (mem:SI $s1))`
  // (`birthing_insn_p == true`). When `sched2` hoists `B` before `A` to hide
  // load latency, disabling implicit `allow_post_call_combine_regs_split` makes
  // Stage 3 reject `B; A` and un-schedule back to `A; B`. With an explicit
  // `(REG_DEAD $s0)` note on `A`, `A` is single-set (`birthing_insn_p == true`)
  // and `B; A` is already `kSched1`-achievable so Stage 3 leaves `B` before `A`.
  const CompilationProfile profile{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(profile);

  auto make_post_sched2_cfg = [&](bool attach_rmw_dead_note) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(1);
    cfg.sideband.frame_size_bytes = 32;
    cfg.sideband.is_leaf_function = false;
    cfg.sideband.basic_block_live_at_start[0] = {4, 5, 16, 17, 29, 31};

    RtlBasicBlock& b0 = cfg.rpo_blocks[0];
    b0.id = 0;
    b0.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))");
    b0.AddPatternOnly(
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0)) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
    b0.AddPatternOnly(
        "(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    b0.AddPatternOnly(
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 17 s1)) "
        "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))");
    b0.AddPatternOnly(
        "(set (reg:SI 17 s1) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    b0.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 31 ra))");
    b0.AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    b0.rtl_insns[6].has_explicit_call_fusage = true;
    b0.rtl_insns[6].call_fusage = {};
    b0.AddPatternOnly(
        "(set (reg:SI 2 v0) (mem:SI (reg:SI 2 v0))) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    b0.AddPatternOnly(
        "(set (reg:SI 3 v1) (plus:SI (reg:SI 17 s1) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))");
    b0.AddPatternOnly(
        attach_rmw_dead_note
            ? "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 2 v0))) "
              "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))"
            : "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 2 v0))) "
              "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    b0.AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 3 v1))) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
    b0.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
    return cfg;
  };

  auto idx_of = [](const std::vector<std::string>& insns, std::string_view needle) -> int {
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].find(needle) != std::string::npos) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  const std::string_view rmw_pat = "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 2 v0)))";
  const std::string_view chain_pat = "(set (reg:SI 3 v1) (plus:SI (reg:SI 17 s1) (const_int 1)))";

  // Positive: `sched2` hoists the 2-step `v1` chain head before `rmw` (`prio 3 > 2`);
  // without `(REG_DEAD s0)`, Stage 3 knows `rmw` is multi-set (`birthing == false`)
  // and un-schedules `rmw` back before `chain_pat`.
  const RtlControlFlowGraph pos_cfg = make_post_sched2_cfg(/*attach_rmw_dead_note=*/false);
  ASSERT_LT(idx_of(pos_cfg.rpo_blocks[0].instructions, chain_pat),
            idx_of(pos_cfg.rpo_blocks[0].instructions, rmw_pat))
      << pos_cfg.FormatSummary();
  const RtlControlFlowGraph pos_lifted = InvertScheduleBlock(pos_cfg, profile);
  EXPECT_LT(idx_of(pos_lifted.rpo_blocks[0].instructions, rmw_pat),
            idx_of(pos_lifted.rpo_blocks[0].instructions, chain_pat))
      << pos_lifted.FormatSummary();
  const gcc_harness::RightInverseResult pos_res = verifier_.VerifySched2StageRightInverse(pos_cfg);
  EXPECT_TRUE(pos_res.matches) << pos_res.diff_detail;

  // Negative: when `rmw` carries an explicit `(REG_DEAD s0)` note, `chain; rmw`
  // is already `kSched1`-achievable, so Stage 3 leaves `chain_pat` before `rmw`.
  const RtlControlFlowGraph dead_cfg = make_post_sched2_cfg(/*attach_rmw_dead_note=*/true);
  ASSERT_LT(idx_of(dead_cfg.rpo_blocks[0].instructions, chain_pat),
            idx_of(dead_cfg.rpo_blocks[0].instructions, rmw_pat))
      << dead_cfg.FormatSummary();
  const RtlControlFlowGraph dead_lifted = InvertScheduleBlock(dead_cfg, profile);
  EXPECT_LT(idx_of(dead_lifted.rpo_blocks[0].instructions, chain_pat),
            idx_of(dead_lifted.rpo_blocks[0].instructions, rmw_pat))
      << dead_lifted.FormatSummary();
  const gcc_harness::RightInverseResult dead_res =
      verifier_.VerifySched2StageRightInverse(dead_cfg);
  EXPECT_TRUE(dead_res.matches) << dead_res.diff_detail;
}

TEST_F(InvertScheduleBlockTest,
       SelectSched1AchievablePreimageRefinesCanonicalSeedsAcrossSunkFpConsumers) {
  // `expr.c:4027`, `sched.c:2428`: when post-reload `sched2` sinks multi-cycle
  // FP ALU consumers (`add.d`) past subsequent `l.d` loads,
  // `SelectSched1AchievablePreimageForSched2` refines canonical pre-`sched1`
  // topological seeds (`kMaxSeedRefineRounds = 4`) to recover a
  // `kSched2`-replayable, `kSched1`-achievable preimage.
  RoundTripTestCase tc;
  tc.test_name = "SyntheticFpAccumulatorSeedRefine";
  tc.function_name = "harness_func";
  tc.original_c = R"(
double harness_func(const double* p) {
  double a = p[0], b = p[1], c = p[2], d = p[3], e = p[4];
  double f = p[5], g = p[6], h = p[7], i = p[8], j = p[9];
  double s0 = (a + b) + (c + d);
  double s1 = (e + f) + (g + h);
  return (s0 + s1) + (i + j) + (a * j) + (b * i);
}
)";
  tc.options.c_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  const gcc_harness::FunctionCompilationTrace trace = hooks_.TraceForwardCompilation(tc);
  const CompilationProfile& o2_prof = trace.stage0_input.profile;

  const gcc_harness::SubfunctionBoundarySnapshot* sched2_snap = nullptr;
  for (const gcc_harness::SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == gcc_harness::SubfunctionId::kScheduleBlockSched2) {
      sched2_snap = &snap;
      break;
    }
  }
  ASSERT_NE(sched2_snap, nullptr);

  // Positive (`-O2`): Stage 3 reorders the post-`sched2` block into a
  // `kSched1`-achievable preimage AND passes exact `kSched2` right-inverse.
  const RtlControlFlowGraph lifted_o2 = InvertScheduleBlock(sched2_snap->post_state, o2_prof);
  EXPECT_NE(lifted_o2.rpo_blocks[0].instructions,
            sched2_snap->post_state.rpo_blocks[0].instructions);
  const gcc_harness::RightInverseResult res_o2 =
      verifier_.VerifySubfunctionBoundaryRightInverse(*sched2_snap);
  EXPECT_TRUE(res_o2.matches) << res_o2.diff_detail;

  // Negative (`-fno-schedule-insns`): when `sched1` is disabled, Stage 3 does
  // not run `SelectSched1AchievablePreimageForSched2` and preserves the
  // post-`sched2` block order unchanged.
  CompilationProfile no_s1_prof = o2_prof;
  no_s1_prof.c_flags.push_back("-fno-schedule-insns");
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(sched2_snap->post_state, no_s1_prof);
  EXPECT_EQ(lifted_no_s1.rpo_blocks[0].instructions,
            sched2_snap->post_state.rpo_blocks[0].instructions);
}

TEST_F(InvertScheduleBlockTest, SelectSched1PreimageHoistsStaleInvertedRegDeadWithTransitiveDeps) {
  // `sched.c:2872-2906, 3415-3611`: when real GCC 2.7.2 `sched2` hoists an insn
  // `i` carrying `(REG_DEAD s0)` ahead of a pure reader `j` of `s0` AND `j`'s
  // transitive `kSched2` ancestor `a` (because a post-reload spill/reload raises
  // `i`'s `kSched2` priority above `a` while `kSched1` drops the matched spill),
  // `SelectSched1AchievablePreimageForSched2` must NOT fast-path skip the block
  // even if `base_view` is trivially `kSched1`-achievable; it must lift BOTH `a`
  // and `j` ahead of `i` so `(REG_DEAD s0)` rests on the true last reader.
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  hooks_.ApplyCompilationProfile(o2_prof);

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kGreg;
  pre_cfg.sideband.frame_size_bytes = 32;
  pre_cfg.sideband.basic_block_live_at_start[0] = {16, 17, 29, 31};
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 8 t0) (ashift:SI (reg:SI 17 s1) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 10 t2) (plus:SI (reg:SI 16 s0) (reg:SI 8 t0))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 9 t1) (xor:SI (reg:SI 16 s0) (const_int 7))) "
      "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 9 t1)) "
      "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (reg:SI 10 t2))) "
      "(expr_list:REG_DEAD (reg:SI 9 t1) (expr_list:REG_DEAD (reg:SI 10 t2) (nil)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const gcc_harness::SubfunctionBoundarySnapshot snap =
      hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kScheduleBlockSched2, pre_cfg);
  ASSERT_EQ(snap.post_state.rpo_blocks.size(), 1u);

  auto find_sub = [](const std::vector<std::string>& insns, const std::string& s) -> int {
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].find(s) != std::string::npos) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  // Sanity check: real GCC 2.7.2 `sched2` indeed inverted `xor` (`REG_DEAD s0`)
  // ahead of `ashift` and `add`.
  ASSERT_LT(find_sub(snap.post_state.rpo_blocks[0].instructions, "(xor:SI (reg:SI 16 s0)"),
            find_sub(snap.post_state.rpo_blocks[0].instructions, "(plus:SI (reg:SI 16 s0)"))
      << snap.post_state.FormatSummary();

  // Positive (`-O2 -mips1`): Stage 3 lifts BOTH `ashift` (`a`) and `add` (`j`)
  // ahead of `xor` (`i`, `REG_DEAD s0`) AND satisfies exact `kSched2` right-inverse.
  {
    const RtlControlFlowGraph lifted = InvertScheduleBlock(snap.post_state, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    const auto& insns = lifted.rpo_blocks[0].instructions;
    const int ashift_pos = find_sub(insns, "(ashift:SI");
    const int add_pos = find_sub(insns, "(plus:SI (reg:SI 16 s0)");
    const int xor_pos = find_sub(insns, "(xor:SI (reg:SI 16 s0)");
    ASSERT_GE(ashift_pos, 0);
    ASSERT_GE(add_pos, 0);
    ASSERT_GE(xor_pos, 0);
    EXPECT_LT(ashift_pos, add_pos) << lifted.FormatSummary();
    EXPECT_LT(add_pos, xor_pos) << lifted.FormatSummary();
    const gcc_harness::RightInverseResult res =
        verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative (`-fno-schedule-insns`): disabling Stage 3 `sched1` preimage
  // selection leaves the post-`sched2` inverted order (`xor_pos < add_pos`).
  {
    CompilationProfile no_s1_prof = o2_prof;
    no_s1_prof.c_flags.push_back("-fno-schedule-insns");
    const RtlControlFlowGraph lifted = InvertScheduleBlock(snap.post_state, no_s1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    const auto& insns = lifted.rpo_blocks[0].instructions;
    EXPECT_LT(find_sub(insns, "(xor:SI (reg:SI 16 s0)"),
              find_sub(insns, "(plus:SI (reg:SI 16 s0)"));
  }
}

TEST_F(InvertScheduleBlockTest,
       ReadjoinCrossBankFprOutputReloadsHoistsPrologueSavesInOrderAndInheritedReaders) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000", "-fno-schedule-insns"}, {}};
  auto find_sub = [](const std::vector<std::string>& insns, const std::string& sub) -> size_t {
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].find(sub) != std::string::npos) {
        return i;
      }
    }
    return insns.size();
  };

  // Positive (`reload1.c:5186-5249, 5641`, `mips.c:expand_prologue`): block 0
  // with interleaved `sw $ra,28($sp)` and `sw $s1,20($sp)` plus inherited FP
  // reader `add.s $f12,$f22,$f0` between `add.s $f0,$f0,$f30` and `mfc1 s1,$f0`
  // (followed by overwrite `li.s $f0, 0.5` before trailing `(use $f0)`).
  // Both prologue saves must hoist ahead of `add.s $f0` preserving `sw $ra`
  // before `sw $s1`, and `mfc1 s1,$f0` must re-adjoin immediately after `add.s $f0`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 80;
    cfg.sideband.outgoing_args_size = 16;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -80)))",
                    "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30))) "
                    "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 31 ra)) "
                    "(expr_list:REG_DEAD (reg:SI 31 ra) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 17 s1)) "
                    "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
                    "(set (reg:SF 44 $f12) (plus:SF (reg:SF 54 $f22) (reg:SF 32 $f0)))",
                    "(set (reg:SF 17 s1) (reg:SF 32 $f0))",
                    "(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1071644672))",
                    "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 44 $f12)))",
                    "(use (reg/i:SF 32 $f0))",
                },
            .successors = {},
        },
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    const RtlControlFlowGraph out = InvertScheduleBlock(cfg, o2_prof);
    ASSERT_EQ(out.rpo_blocks.size(), 1u);
    const auto& insns = out.rpo_blocks[0].instructions;
    const size_t ra_pos = find_sub(insns, "(const_int 28))) (reg:SI 31 ra)");
    const size_t s1_save_pos = find_sub(insns, "(const_int 20))) (reg:SI 17 s1)");
    const size_t prod_pos = find_sub(insns, "(set (reg:SF 32 $f0) (plus:SF");
    const size_t mv_pos = find_sub(insns, "(set (reg:SF 17 s1) (reg:SF 32 $f0))");
    const size_t inh_pos = find_sub(insns, "(set (reg:SF 44 $f12) (plus:SF");
    EXPECT_LT(ra_pos, s1_save_pos);
    EXPECT_LT(s1_save_pos, prod_pos);
    EXPECT_EQ(mv_pos, prod_pos + 1);
    EXPECT_LT(mv_pos, inh_pos);
  }

  // Negative: when `$f0` is NOT overwritten before `(use (reg/i:SF 32 $f0))`
  // (`$f0` remains live into the return value), `mfc1 s1,$f0` must NOT hoist
  // as a dead output-reload scratch.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30)))",
                    "(set (reg:SF 44 $f12) (plus:SF (reg:SF 54 $f22) (reg:SF 32 $f0)))",
                    "(set (reg:SF 17 s1) (reg:SF 32 $f0))",
                    "(use (reg/i:SF 32 $f0))",
                },
            .successors = {},
        },
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    const RtlControlFlowGraph out = InvertScheduleBlock(cfg, o2_prof);
    ASSERT_EQ(out.rpo_blocks.size(), 1u);
    const auto& insns = out.rpo_blocks[0].instructions;
    EXPECT_LT(find_sub(insns, "(set (reg:SF 44 $f12)"), find_sub(insns, "(set (reg:SF 17 s1)"));
  }
}

TEST_F(InvertScheduleBlockTest, AlignEarlyReturnNoopMoveBlockForSched1VerifiesAllThreeGates) {
  // Positive (`sched.c:4803-4818`, `jump.c:308, 427-433`): in an early-return
  // conditional-branch `kSched2` block `B0 -> {B2(exit), B1(other)}` where `$f0`
  // is dead on `B1` and read by `c.eq.s` before the branch, `InvertScheduleBlock`
  // restores `(set (reg/i:SF 32 $f0) (reg:SF 32 $f0)) (REG_DEAD $f0)` before the
  // branch AND (`RunsSched1() == true`) `AlignEarlyReturnNoopMoveBlockForSched1`
  // re-aligns `B0` into `Im(F_sched1)` (hoisting `[1]` ahead of birthing `[0]`)
  // while preserving exact `kSched2` replay (`VerifySched2StageRightInverse`).
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.sideband.basic_block_live_at_start = {{0, {4, 44, 46}}, {1, {4}}, {2, {32}}};
  sched2_cfg.rpo_blocks.resize(3);
  sched2_cfg.rpo_blocks[0].id = 0;
  sched2_cfg.rpo_blocks[0].successors = {2, 1};
  sched2_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 36 $f4) (mem:SF (reg:SI 4 a0)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14))) "
      "(expr_list:REG_DEAD (reg:SF 44 $f12) (expr_list:REG_DEAD (reg:SF 46 $f14) (nil)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 36 $f4)))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:CC_FP 67 $fcr31) (eq:CC_FP (reg:SF 32 $f0) (reg:SF 36 $f4))) "
      "(expr_list:REG_DEAD (reg:SF 36 $f4) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0)) "
      "(label_ref L0) (pc))) (expr_list:REG_DEAD (reg:CC_FP 67 $fcr31) (nil))");

  sched2_cfg.rpo_blocks[1].id = 1;
  sched2_cfg.rpo_blocks[1].predecessors = {0};
  sched2_cfg.rpo_blocks[1].successors = {2};
  sched2_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4)))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  sched2_cfg.rpo_blocks[2].id = 2;
  sched2_cfg.rpo_blocks[2].has_label = true;
  sched2_cfg.rpo_blocks[2].predecessors = {0, 1};
  sched2_cfg.rpo_blocks[2].AddPatternOnly(
      "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))");

  const CompilationProfile mips1_o2{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const RtlControlFlowGraph lifted_s1 = InvertScheduleBlock(sched2_cfg, mips1_o2);
  ASSERT_EQ(lifted_s1.rpo_blocks[0].instructions.size(), 6u) << lifted_s1.FormatSummary();
  EXPECT_EQ(lifted_s1.rpo_blocks[0].instructions[4],
            "(set (reg/i:SF 32 $f0) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))");
  EXPECT_NE(lifted_s1.rpo_blocks[0].instructions[1].find("(plus:SF (reg:SF 36 $f4)"),
            std::string::npos)
      << lifted_s1.FormatSummary();
  EXPECT_NE(lifted_s1.rpo_blocks[0].instructions[2].find("(plus:SF (reg:SF 44 $f12)"),
            std::string::npos)
      << lifted_s1.FormatSummary();
  hooks_.ApplyCompilationProfile(mips1_o2);
  const gcc_harness::RightInverseResult res_s1 =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(res_s1.matches) << res_s1.diff_detail;

  // Negative contrast (`!RunsSched1()`): skips `AlignEarlyReturnNoopMoveBlockForSched1`
  // while still restoring the noop copy and passing `VerifySched2StageRightInverse`.
  const CompilationProfile no_s1{
      .optimize_level = 2,
      .c_flags = {"-O2", "-fno-schedule-insns", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertScheduleBlock(sched2_cfg, no_s1);
  ASSERT_EQ(lifted_no_s1.rpo_blocks[0].instructions.size(), 6u) << lifted_no_s1.FormatSummary();
  EXPECT_NE(lifted_no_s1.rpo_blocks[0].instructions[1].find("(plus:SF (reg:SF 44 $f12)"),
            std::string::npos)
      << lifted_no_s1.FormatSummary();
  hooks_.ApplyCompilationProfile(no_s1);
  const gcc_harness::RightInverseResult res_no_s1 =
      verifier_.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(res_no_s1.matches) << res_no_s1.diff_detail;
}

TEST_F(InvertScheduleBlockTest,
       RestoreAndAlignCondExprNoopMoveForSched1RestoresDiamondCopyAndVerifiesGates) {
  // `sched.c:4803-4821`, `expr.c:5867-5908`, `jump.c:699-834`: on a `-O2 -mips1`
  // `COND_EXPR` diamond (`(x > 0) ? (unsigned)x : 0u`) where `sched2` deleted
  // the pre-branch noop copy `(set (reg:SI r) (reg:SI r)) (REG_DEAD r)` and
  // reordered the stack load of `x` across an independent store so `B0` is not
  // in `Im(F_sched1)`:
  //   - Positive (`-O2 -mips1`): `RestoreAndAlignCondExprNoopMoveForSched1`
  //     restores the deleted noop copy and aligns `B0` into `Im(F_sched1)`
  //     while passing exact `VerifySubfunctionBoundaryRightInverse`.
  //   - Negative (`-fno-schedule-insns`): does NOT insert the noop copy.
  RoundTripTestCase tc;
  tc.test_name = "SyntheticCondExprStackArgNoopCopy";
  tc.function_name = "harness_func";
  tc.original_c = R"(
struct Packet {
  unsigned int f0 : 5;
  unsigned int f1 : 6;
  int f2 : 5;
  unsigned int f3 : 16;
  int f4 : 11;
  unsigned int f5 : 10;
  unsigned int f6 : 11;
};
void harness_func(struct Packet* dst, unsigned int a, unsigned int b, int c, int d, int x) {
  dst->f0 = a;
  dst->f1 = b;
  dst->f2 = c;
  dst->f3 = (unsigned int)d;
  dst->f4 = x;
  dst->f5 = (x > 0) ? (unsigned int)x : 0u;
  dst->f6 = b * 4u;
}
)";
  tc.options.c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"};
  const gcc_harness::FunctionCompilationTrace trace = hooks_.TraceForwardCompilation(tc);
  const CompilationProfile& mips1_o2 = trace.stage0_input.profile;

  const gcc_harness::SubfunctionBoundarySnapshot* sched2_snap = nullptr;
  for (const gcc_harness::SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == gcc_harness::SubfunctionId::kScheduleBlockSched2) {
      sched2_snap = &snap;
      break;
    }
  }
  ASSERT_NE(sched2_snap, nullptr);
  ASSERT_GE(sched2_snap->post_state.rpo_blocks.size(), 3u)
      << sched2_snap->post_state.FormatSummary();

  // Positive: restores the deleted pre-branch noop copy and verifies exact `sched2` replay.
  {
    const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_snap->post_state, mips1_o2);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions.size(),
              sched2_snap->post_state.rpo_blocks[0].instructions.size() + 1)
        << lifted.FormatSummary();
    bool found_restored_noop_copy = false;
    for (const std::string& raw : lifted.rpo_blocks[0].instructions) {
      auto parsed = ParsePatternAndNotes(raw);
      if (parsed.ok() && parsed->pattern.IsSet() && parsed->pattern.SetDest().IsHardReg() &&
          parsed->pattern.SetSrc().IsHardReg() &&
          parsed->pattern.SetDest().RegNo() == parsed->pattern.SetSrc().RegNo()) {
        for (const RtlRegNote& note : parsed->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
              note.datum.RegNo() == parsed->pattern.SetDest().RegNo()) {
            found_restored_noop_copy = true;
          }
        }
      }
    }
    EXPECT_TRUE(found_restored_noop_copy) << lifted.FormatSummary();
    const gcc_harness::RightInverseResult res =
        verifier_.VerifySubfunctionBoundaryRightInverse(*sched2_snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative 1 (`-fno-schedule-insns`): skips `RestoreAndAlignCondExprNoopMoveForSched1`
  // and does not insert a noop copy into `B0`.
  {
    CompilationProfile no_s1 = mips1_o2;
    no_s1.c_flags.push_back("-fno-schedule-insns");
    const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_snap->post_state, no_s1);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions.size(),
              sched2_snap->post_state.rpo_blocks[0].instructions.size())
        << lifted.FormatSummary();
  }

  // Negative 2 (non-diamond fall-through arm): mutating `B1` so it no longer
  // unconditionally overwrites `cond_reg` suppresses noop-copy restoration.
  {
    RtlControlFlowGraph broken_diamond = sched2_snap->post_state;
    ASSERT_GE(broken_diamond.rpo_blocks.size(), 2u);
    broken_diamond.rpo_blocks[1].instructions.push_back(
        "(insn 999 0 0 (set (reg:SI 15 $15) (const_int 1)) -1 (nil))");
    const RtlControlFlowGraph lifted = InvertScheduleBlock(broken_diamond, mips1_o2);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions.size(),
              broken_diamond.rpo_blocks[0].instructions.size())
        << lifted.FormatSummary();
  }
}

TEST_F(InvertScheduleBlockTest,
       SelectSched1PreimageSunkConsumerHoistAndRegDeadRepairPositiveAndNegative) {
  // `sched.c:3144-3942`: when real GCC 2.7.2 `sched2` sinks an integer load's
  // consumer past a competing FP load's consumer in a genuine post-reload block,
  // Stage 3 (`SelectSched1AchievablePreimageForSched2` +
  // `CollectSched1SunkConsumerHoistCoreOrders`) lifts a `kSched1`-achievable
  // preimage while preserving exact `VerifySubfunctionBoundaryRightInverse`
  // under `-O2` (positive), whereas `-fno-schedule-insns` leaves the `sched2`
  // order untouched (negative).
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  hooks_.ApplyCompilationProfile(o2_prof);

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kGreg;
  pre_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29, 44, 46};
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 34 $f2) (mem:SF (reg:SI 4 a0))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 36 $f4) (mem:SF (reg:SI 5 a1))) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 34 $f2) (plus:SF (reg:SF 34 $f2) (reg:SF 44 $f12))) "
      "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 44 $f12) (nil)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 36 $f4) (mult:SF (reg:SF 36 $f4) (reg:SF 46 $f14))) "
      "(expr_list:REG_DEAD (reg:SF 36 $f4) (expr_list:REG_DEAD (reg:SF 46 $f14) (nil)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 36 $f4))) "
      "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 36 $f4) (nil)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))");

  const gcc_harness::SubfunctionBoundarySnapshot snap =
      hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kScheduleBlockSched2, pre_cfg);
  ASSERT_EQ(snap.post_state.rpo_blocks.size(), 1u);

  // Positive (`-O2`): Stage 3 lifts a `kSched1`-achievable preimage AND passes
  // exact `VerifySubfunctionBoundaryRightInverse`.
  {
    const RtlControlFlowGraph lifted = InvertScheduleBlock(snap.post_state, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    const gcc_harness::RightInverseResult res =
        verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative (`-fno-schedule-insns`): Stage 3 skips `SelectSched1AchievablePreimageForSched2`
  // and preserves the post-`sched2` block order unchanged.
  {
    CompilationProfile no_s1_prof = o2_prof;
    no_s1_prof.c_flags.push_back("-fno-schedule-insns");
    const RtlControlFlowGraph lifted = InvertScheduleBlock(snap.post_state, no_s1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions, snap.post_state.rpo_blocks[0].instructions);
  }
}

TEST_F(InvertScheduleBlockTest, Stage3RepairsRelaxedAchievableCandidateOnSpillsWith8OutgoingArgs) {
  // Mirrors `sched.c:2872-2906, 3415-3611` + `caller-save.c:349-620`:
  // a high-pressure post-reload block with an 8-outgoing-arg call and caller-
  // saved/spilled live ranges requires Stage 3 (`TryCommitAchievableSched1Candidate`
  // + `TrySelectSched1AchievablePreimageForBlock`) to hand off relaxed-safety
  // achievable candidates to `REG_DEAD` inversion repair (while rejecting
  // unrepaired unsafe candidates) so both `kSched2` replay and `kSched1`
  // achievability succeed under `-O2` (positive), while `-fno-schedule-insns`
  // leaves the post-`sched2` block untouched (negative).
  RoundTripTestCase tc;
  tc.test_name = "TestSpills8Args";
  tc.function_name = "TestSpills8Args";
  tc.original_c = R"(
extern int Sink8(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7);
int TestSpills8Args(const int* p) {
  int v0 = p[0] + 1;
  int v1 = p[1] + 2;
  int v2 = p[2] + 3;
  int v3 = p[3] + 4;
  int v4 = p[4] ^ 5;
  int v5 = p[5] ^ 6;
  int v6 = p[6] ^ 7;
  int v7 = p[7] ^ 8;
  int v8 = p[8] - 9;
  int v9 = p[9] - 10;
  int v10 = p[10] + 11;
  int v11 = p[11] + 12;
  int r = Sink8(v0 + v1, v2 + v3, v4 + v5, v6 + v7, v8, v9, v10, v11);
  return r + (v0 ^ v6) + (v1 ^ v7) + (v2 ^ v8) + (v3 ^ v9) + (v4 ^ v10) + (v5 ^ v11);
}
)";
  tc.options.c_flags = {"-O2", "-mips2"};
  const gcc_harness::FunctionCompilationTrace trace = hooks_.TraceForwardCompilation(tc);
  const CompilationProfile& o2_prof = trace.stage0_input.profile;

  const gcc_harness::SubfunctionBoundarySnapshot* sched2_snap = nullptr;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == gcc_harness::SubfunctionId::kScheduleBlockSched2) {
      sched2_snap = &snap;
      break;
    }
  }
  ASSERT_NE(sched2_snap, nullptr);

  // Positive (`-O2`): Stage 3 lifts a verified `kSched2` preimage matching
  // `VerifySubfunctionBoundaryRightInverse` and `VerifySched2StageRightInverse`.
  {
    hooks_.ApplyCompilationProfile(o2_prof);
    const gcc_harness::RightInverseResult res =
        verifier_.VerifySubfunctionBoundaryRightInverse(*sched2_snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    const gcc_harness::RightInverseResult stage_res =
        verifier_.VerifySched2StageRightInverse(sched2_snap->post_state);
    EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
  }

  // Negative (`-fno-schedule-insns -fno-schedule-insns2`): Stage 3 is a no-op
  // and keeps the post-`sched2` block order unchanged.
  {
    CompilationProfile no_sched_prof = o2_prof;
    no_sched_prof.c_flags.push_back("-fno-schedule-insns");
    no_sched_prof.c_flags.push_back("-fno-schedule-insns2");
    const RtlControlFlowGraph lifted = InvertScheduleBlock(sched2_snap->post_state, no_sched_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), sched2_snap->post_state.rpo_blocks.size());
    EXPECT_EQ(lifted.rpo_blocks[0].instructions,
              sched2_snap->post_state.rpo_blocks[0].instructions);
  }
}

TEST_F(InvertScheduleBlockTest, Stage3SinksCallerSaveRestoreBeforeFirstPostCallConsumer) {
  // Mirrors `caller-save.c:410-411, 568-615` (`restore_referenced_regs`):
  // when Stage 3 selects a `kSched1`-achievable preimage for a high-pressure
  // `-O2 -mips1` block with a caller-saved GPR restored after a call, the
  // restore load must sit immediately before its first post-call consumer.
  RoundTripTestCase tc;
  tc.test_name = "TestCallerSaveRestoreSink";
  tc.function_name = "TestCallerSaveRestoreSink";
  tc.original_c = R"(
extern int Dispatch8(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7);
int TestCallerSaveRestoreSink(const int* in) {
  int x0 = in[0] + 10, x1 = in[1] + 20, x2 = in[2] + 30, x3 = in[3] + 40;
  int x4 = in[4] ^ 15, x5 = in[5] ^ 25, x6 = in[6] ^ 35, x7 = in[7] ^ 45;
  int x8 = in[8] - 5, x9 = in[9] - 15, x10 = in[10] - 25, x11 = in[11] - 35;
  int x12 = in[12] + 99, x13 = in[13] ^ 77;
  int res1 = Dispatch8(x0, x1, x2, x3, x4, x5, x6, x7);
  int res2 = Dispatch8(x13, x12, x11, x10, x9, x8, res1, x0 + x7);
  return res1 + res2 + (x1 ^ x12) + (x2 ^ x11) + (x3 ^ x10) + (x4 ^ x9) + (x5 ^ x8) + x6 + x13;
}
)";
  tc.options.c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"};
  const gcc_harness::FunctionCompilationTrace trace = hooks_.TraceForwardCompilation(tc);
  const CompilationProfile& prof = trace.stage0_input.profile;
  const gcc_harness::SubfunctionBoundarySnapshot* sched2_snap = nullptr;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == gcc_harness::SubfunctionId::kScheduleBlockSched2) {
      sched2_snap = &snap;
      break;
    }
  }
  ASSERT_NE(sched2_snap, nullptr);

  // Simulate post-Stage-0..2 input (notes erased by `final.c`, then reconstructed).
  RtlControlFlowGraph p2_sim = sched2_snap->post_state;
  for (RtlBasicBlock& blk : p2_sim.rpo_blocks) {
    for (std::string& s : blk.instructions) {
      auto p = ParsePatternAndNotes(s);
      if (p.ok()) {
        p->reg_notes.clear();
        s = p->ToSexprString();
      }
    }
    blk.rtl_insns.clear();
  }
  ReconstructPostDbrHardRegLivenessAndReturnUse(&p2_sim, prof);
  const RtlControlFlowGraph lifted = InvertScheduleBlock(p2_sim, prof);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  const auto& insns = lifted.rpo_blocks[0].instructions;
  int restore_idx = -1;
  int consumer_idx = -1;
  for (size_t i = 0; i < insns.size(); ++i) {
    if (insns[i].find("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))") !=
        std::string::npos) {
      restore_idx = static_cast<int>(i);
    } else if (restore_idx >= 0 &&
               insns[i].find("(set (reg:SI 3 v1) (plus:SI") != std::string::npos) {
      consumer_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(restore_idx, 0) << lifted.FormatSummary();
  ASSERT_GE(consumer_idx, 0) << lifted.FormatSummary();
  EXPECT_EQ(consumer_idx, restore_idx + 1) << "p2_sim:\n"
                                           << p2_sim.FormatSummary() << "\nlifted:\n"
                                           << lifted.FormatSummary();

  // Also test the exact post-Stage-0..2 `-Wa,-O2` form (interleaved prologue
  // `sp + -120` at index 5, `mem/u` loads, `lw $3, 72($sp)` hoisted before
  // `move $16, $2` after the first call).
  RtlControlFlowGraph p2_interleaved = p2_sim;
  p2_interleaved.rpo_blocks[0].instructions = {
      "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 3 v1) (mem/u:SI (reg:SI 2 v0)))",
      "(set (reg:SI 5 a1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4))) (nil))",
      "(set (reg:SI 6 a2) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 8)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 8))) (nil))",
      "(set (reg:SI 7 a3) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 12)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 12))) (nil))",
      "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -120)))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 108))) (reg:SI 23 s7)) "
      "(expr_list:REG_DEAD (reg:SI 23 s7) (nil))",
      "(set (reg:SI 23 s7) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 16)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 16))) (nil))",
      "(set (reg:SI 8 t0) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 20)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 20))) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 96))) (reg:SI 20 s4)) "
      "(expr_list:REG_DEAD (reg:SI 20 s4) (nil))",
      "(set (reg:SI 20 s4) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 28)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 28))) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 104))) (reg:SI 22 s6)) "
      "(expr_list:REG_DEAD (reg:SI 22 s6) (nil))",
      "(set (reg:SI 22 s6) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 36)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 36))) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 92))) (reg:SI 19 s3)) "
      "(expr_list:REG_DEAD (reg:SI 19 s3) (nil))",
      "(set (reg:SI 19 s3) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 40)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 40))) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 88))) (reg:SI 18 s2)) "
      "(expr_list:REG_DEAD (reg:SI 18 s2) (nil))",
      "(set (reg:SI 18 s2) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 44)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 44))) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 84))) (reg:SI 17 s1)) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
      "(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 48)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 48))) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 100))) (reg:SI 21 s5)) "
      "(expr_list:REG_DEAD (reg:SI 21 s5) (nil))",
      "(set (reg:SI 21 s5) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 52)))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 116))) (reg:SI 31 ra)) "
      "(expr_list:REG_DEAD (reg:SI 31 ra) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 112))) (reg:SI 30 $fp)) "
      "(expr_list:REG_DEAD (reg:SI 30 $fp) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 80))) (reg:SI 16 s0)) "
      "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 10)))",
      "(set (reg:SI 4 a0) (reg:SI 3 v1))",
      "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 20))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
      "(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 30))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
      "(set (reg:SI 7 a3) (plus:SI (reg:SI 7 a3) (const_int 40))) "
      "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
      "(set (reg:SI 23 s7) (xor:SI (reg:SI 23 s7) (const_int 15))) "
      "(expr_list:REG_DEAD (reg:SI 23 s7) (nil))",
      "(set (reg:SI 8 t0) (xor:SI (reg:SI 8 t0) (const_int 25))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
      "(set (reg:SI 20 s4) (xor:SI (reg:SI 20 s4) (const_int 45))) "
      "(expr_list:REG_DEAD (reg:SI 20 s4) (nil))",
      "(set (reg:SI 22 s6) (plus:SI (reg:SI 22 s6) (const_int -15))) "
      "(expr_list:REG_DEAD (reg:SI 22 s6) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))) (reg:SI 5 a1))",
      "(set (reg:SI 5 a1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))))",
      "(set (reg:SI 19 s3) (plus:SI (reg:SI 19 s3) (const_int -25))) "
      "(expr_list:REG_DEAD (reg:SI 19 s3) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 44))) (reg:SI 6 a2))",
      "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 44))))",
      "(set (reg:SI 18 s2) (plus:SI (reg:SI 18 s2) (const_int -35))) "
      "(expr_list:REG_DEAD (reg:SI 18 s2) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 60))) (reg:SI 8 t0))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 8 t0))",
      "(set (reg:SI 8 t0) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 24)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 24))) (nil))",
      "(set (reg:SI 17 s1) (plus:SI (reg:SI 17 s1) (const_int 99))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 52))) (reg:SI 7 a3))",
      "(set (reg:SI 7 a3) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 52))))",
      "(set (reg:SI 8 t0) (xor:SI (reg:SI 8 t0) (const_int 35))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 68))) (reg:SI 8 t0))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 8 t0))",
      "(set (reg:SI 8 t0) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 32)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 32))) (nil))",
      "(set (reg:SI 21 s5) (xor:SI (reg:SI 21 s5) (const_int 77))) "
      "(expr_list:REG_DEAD (reg:SI 21 s5) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 23 s7))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 20 s4))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))) (reg:SI 3 v1))",
      "(set (reg:SI 30 $fp) (plus:SI (reg:SI 8 t0) (const_int -5))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Dispatch8Args\"))) "
      "(const_int 32))) (clobber (reg:SI 31 ra)) ] )",
      "(set (reg:SI 4 a0) (reg:SI 21 s5))",
      "(set (reg:SI 5 a1) (reg:SI 17 s1))",
      "(set (reg:SI 6 a2) (reg:SI 18 s2))",
      "(set (reg:SI 7 a3) (reg:SI 19 s3))",
      "(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))",
      "(set (reg:SI 16 s0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 22 s6))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 30 $fp))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 16 s0))",
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 20 s4))) "
      "(expr_list:REG_DEAD (reg:SI 20 s4) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 3 v1)) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Dispatch8Args\"))) "
      "(const_int 32))) (clobber (reg:SI 31 ra)) ] )",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 2 v0))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))))",
      "(set (reg:SI 23 s7) (xor:SI (reg:SI 23 s7) (reg:SI 22 s6))) "
      "(expr_list:REG_DEAD (reg:SI 22 s6) (nil))",
      "(set (reg:SI 5 a1) (xor:SI (reg:SI 9 t1) (reg:SI 17 s1))) "
      "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (nil)))",
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 44))))",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
      "(set (reg:SI 6 a2) (xor:SI (reg:SI 9 t1) (reg:SI 18 s2))) "
      "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 44))) "
      "(expr_list:REG_DEAD (reg:SI 18 s2) (nil)))",
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 52))))",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 6 a2))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
      "(set (reg:SI 7 a3) (xor:SI (reg:SI 9 t1) (reg:SI 19 s3))) "
      "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 52))) "
      "(expr_list:REG_DEAD (reg:SI 19 s3) (nil)))",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 7 a3))) "
      "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 60))))",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 23 s7))) "
      "(expr_list:REG_DEAD (reg:SI 23 s7) (nil))",
      "(set (reg:SI 2 v0) (xor:SI (reg:SI 9 t1) (reg:SI 30 $fp))) "
      "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 60))) "
      "(expr_list:REG_DEAD (reg:SI 30 $fp) (nil)))",
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 68))))",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 2 v0))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 9 t1))) "
      "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 68))) (nil))",
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 21 s5))) "
      "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SI 21 s5) (nil)))",
      "(use (reg/i:SI 2 v0))",
  };
  p2_interleaved.rpo_blocks[0].rtl_insns.clear();
  ReconstructMissingCallFusage(&p2_interleaved, prof);
  const RtlControlFlowGraph lifted2 = InvertScheduleBlock(p2_interleaved, prof);
  const auto& insns2 = lifted2.rpo_blocks[0].instructions;
  int r2 = -1, c2 = -1;
  for (size_t i = 0; i < insns2.size(); ++i) {
    if (insns2[i].find("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))") !=
        std::string::npos) {
      r2 = static_cast<int>(i);
    } else if (r2 >= 0 && insns2[i].find("(set (reg:SI 3 v1) (plus:SI") != std::string::npos) {
      c2 = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(r2, 0);
  ASSERT_GE(c2, 0);
  EXPECT_EQ(c2, r2 + 1) << lifted2.FormatSummary();

  // Negative: when `-fno-schedule-insns -fno-schedule-insns2` disables both
  // scheduling passes, `InvertScheduleBlock` leaves the block order unchanged
  // (so the hoisted restore load at index 59 stays separated from its consumer
  // at index 64).
  CompilationProfile no_sched_prof = prof;
  no_sched_prof.c_flags.push_back("-fno-schedule-insns");
  no_sched_prof.c_flags.push_back("-fno-schedule-insns2");
  const RtlControlFlowGraph untouched = InvertScheduleBlock(p2_interleaved, no_sched_prof);
  EXPECT_EQ(untouched.rpo_blocks[0].instructions, p2_interleaved.rpo_blocks[0].instructions);
}

TEST_F(InvertScheduleBlockTest,
       RestoreNoConflictExtendClobbersScansCalleeSavedAcrossCallWithoutDuplication) {
  // Mirrors `sched.c:2201-2234` + `config/mips/mips.h` (`CALL_USED_REGISTERS`):
  // `sched2` can hoist `(clobber (reg:DI 18 s2))` across a `CALL_INSN` because
  // `$s2/$s3` are callee-saved. `RestoreNoConflictExtendClobbers` must find that
  // existing clobber across the call instead of synthesizing a duplicate.
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  auto make_cfg = [&](bool use_callee_saved, bool read_before_extend) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    const std::string hi = use_callee_saved ? "(reg:SI 18 s2)" : "(reg:SI 8 t0)";
    const std::string lo = use_callee_saved ? "(reg:SI 19 s3)" : "(reg:SI 9 t1)";
    const std::string di = use_callee_saved ? "(reg:DI 18 s2)" : "(reg:DI 8 t0)";
    cfg.rpo_blocks[0].AddPatternOnly("(clobber " + di + ")");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    if (read_before_extend) {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (plus:SI " + hi + " (const_int 1)))");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(set " + lo + " (reg:SI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly("(set " + hi + " (const_int 0))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (mem:DI (reg:SI 4 a0)) " + di + ")");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 1))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };
  auto count_clobber = [](const RtlControlFlowGraph& cfg, std::string_view needle) {
    int n = 0;
    for (const std::string& insn : cfg.rpo_blocks[0].instructions) {
      if (insn.find(needle) != std::string::npos) {
        ++n;
      }
    }
    return n;
  };

  // Positive: callee-saved `(clobber (reg:DI 18 s2))` hoisted above the call is
  // found across the `CALL_INSN`, so no duplicate clobber is synthesized.
  const RtlControlFlowGraph pos =
      InvertScheduleBlock(make_cfg(/*use_callee_saved=*/true, /*read_before_extend=*/false), prof);
  ASSERT_EQ(pos.rpo_blocks.size(), 1u);
  EXPECT_EQ(count_clobber(pos, "(clobber (reg:DI 18 s2))"), 1) << pos.FormatSummary();

  // Negative 1: call-clobbered `$t0/$t1` cannot hoist a clobber across `CALL_INSN`,
  // so the post-call extend synthesizes its own `(clobber (reg:DI 8 t0))`.
  const RtlControlFlowGraph neg_call_used =
      InvertScheduleBlock(make_cfg(/*use_callee_saved=*/false, /*read_before_extend=*/false), prof);
  ASSERT_EQ(neg_call_used.rpo_blocks.size(), 1u);
  EXPECT_EQ(count_clobber(neg_call_used, "(clobber (reg:DI 8 t0))"), 2)
      << neg_call_used.FormatSummary();

  // Negative 2: an intervening read of `$s2` blocks finding the earlier clobber,
  // so a new `(clobber (reg:DI 18 s2))` is synthesized for the post-read extend.
  const RtlControlFlowGraph neg_read =
      InvertScheduleBlock(make_cfg(/*use_callee_saved=*/true, /*read_before_extend=*/true), prof);
  ASSERT_EQ(neg_read.rpo_blocks.size(), 1u);
  EXPECT_EQ(count_clobber(neg_read, "(clobber (reg:DI 18 s2))"), 2) << neg_read.FormatSummary();
}

TEST_F(InvertScheduleBlockTest, RestoreInheritedSpillSlotDeadNotesSkipsMemInStructAndVolatile) {
  // Mirrors `function.c:667-735` (`assign_stack_local`) + `reload1.c:2352`:
  // compiler-allocated spill slots never carry `MEM_IN_STRUCT_P` (`mem/s`) or
  // `MEM_VOLATILE_P` (`mem/v`). User stack-array / volatile stores must NOT
  // receive synthetic `(REG_DEAD (mem/s ...))` or `(REG_DEAD (mem/v ...))` notes.
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  auto make_cfg = [&](std::string_view mem_tag) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 32;
    cfg.sideband.saved_gpr_regnos = {16};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    const std::string slot =
        "(" + std::string(mem_tag) + ":SI (plus:SI (reg:SI 29 sp) (const_int 16)))";
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 16 s0) (plus:SI (reg:SI 4 a0) (const_int 3))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(set " + slot + " (reg:SI 16 s0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };
  auto has_mem_dead_note = [](const RtlControlFlowGraph& cfg) {
    for (const std::string& insn : cfg.rpo_blocks[0].instructions) {
      if (insn.find("REG_DEAD (mem") != std::string::npos) {
        return true;
      }
    }
    return false;
  };

  // Positive: non-struct, non-volatile spill-slot store followed by reload receives
  // inherited `(REG_DEAD (mem:SI ...))`.
  const RtlControlFlowGraph pos = InvertScheduleBlock(make_cfg("mem"), prof);
  ASSERT_EQ(pos.rpo_blocks.size(), 1u);
  EXPECT_TRUE(has_mem_dead_note(pos)) << pos.FormatSummary();

  // Negative 1: `mem/s` (`MEM_IN_STRUCT_P`, e.g., local array element) does NOT.
  const RtlControlFlowGraph neg_struct = InvertScheduleBlock(make_cfg("mem/s"), prof);
  ASSERT_EQ(neg_struct.rpo_blocks.size(), 1u);
  EXPECT_FALSE(has_mem_dead_note(neg_struct)) << neg_struct.FormatSummary();

  // Negative 2: `mem/v` (`MEM_VOLATILE_P`) does NOT.
  const RtlControlFlowGraph neg_volatile = InvertScheduleBlock(make_cfg("mem/v"), prof);
  ASSERT_EQ(neg_volatile.rpo_blocks.size(), 1u);
  EXPECT_FALSE(has_mem_dead_note(neg_volatile)) << neg_volatile.FormatSummary();
}

TEST_F(InvertScheduleBlockTest,
       RepairSched2BlockReplayRecoversExactReplayAfterCallerSaveRestoreSinking) {
  // Mirrors `sched.c:schedule_block`: when heuristic caller-save restore sinking
  // displaces restores past equal-priority ALU/const insns and breaks exact
  // `kSched2` replay tie-break order, `RepairSched2BlockReplayIfBroken` repairs
  // the block so forward `kSched2` reproduces the target sequence exactly.
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.sideband.frame_size_bytes = 48;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 8 t0))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 9 t1))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 10 t2))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 11 t3) (const_int 7))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 12 t4) (const_int 11))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 8 t0))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 9 t1))) "
      "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 10 t2))) "
      "(expr_list:REG_DEAD (reg:SI 10 t2) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 11 t3))) "
      "(expr_list:REG_DEAD (reg:SI 11 t3) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 12 t4))) "
      "(expr_list:REG_DEAD (reg:SI 12 t4) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  // Schedule forward once under real GCC `sched2` so `target_sched2` is a
  // genuine `sched2` output block.
  const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
      gcc_harness::SubfunctionId::kScheduleBlockSched2, cfg, {}, 2, prof.c_flags);
  const gcc_harness::RightInverseResult res = verifier_.VerifySubfunctionBoundaryRightInverse(fwd);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  // Negative: when `-fno-schedule-insns2` is set, `InvertScheduleBlock` does not
  // invoke `sched2` replay repair and leaves the block untouched.
  CompilationProfile no_s2 = prof;
  no_s2.c_flags.push_back("-fno-schedule-insns");
  no_s2.c_flags.push_back("-fno-schedule-insns2");
  const RtlControlFlowGraph untouched = InvertScheduleBlock(fwd.post_state, no_s2);
  EXPECT_EQ(untouched.rpo_blocks[0].instructions, fwd.post_state.rpo_blocks[0].instructions);
}

TEST_F(InvertScheduleBlockTest, SpillSlotDeadNotesSkippedInCallFreeLeafWithoutCalleeSavedGprs) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mcpu=r4000", "-Wa,-g"};

  auto make_cfg = [](bool use_callee_saved_gpr) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 32;
    if (use_callee_saved_gpr) {
      cfg.sideband.saved_gpr_regnos = {16};
    }
    cfg.sideband.basic_block_live_at_start = {{0, {4, 5}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))");
    if (use_callee_saved_gpr) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 16 s0))");
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 16 s0) (reg:SI 4 a0))");
    }
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 0))) (reg:SI 8 t0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 5 a1))) "
        "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Negative (Fix 42E): call-free leaf using zero callee-saved GPRs (`$16..$23`)
  // cannot have reload GPR spill slots (`global.c:find_reg`), so local stack
  // slot writes inherited by a later reader do NOT receive synthetic
  // `REG_DEAD (mem:SI ...)`.
  const RtlControlFlowGraph leaf_out =
      InvertScheduleBlock(make_cfg(/*use_callee_saved_gpr=*/false), prof);
  ASSERT_EQ(leaf_out.rpo_blocks.size(), 1u);
  for (const std::string& insn : leaf_out.rpo_blocks[0].instructions) {
    EXPECT_EQ(insn.find("REG_DEAD (mem:SI"), std::string::npos) << leaf_out.FormatSummary();
  }

  // Positive: when callee-saved GPRs are saved/used, inherited spill-slot
  // `REG_DEAD(MEM)` restoration remains enabled.
  const RtlControlFlowGraph cs_out =
      InvertScheduleBlock(make_cfg(/*use_callee_saved_gpr=*/true), prof);
  ASSERT_EQ(cs_out.rpo_blocks.size(), 1u);
  bool has_mem_dead = false;
  for (const std::string& insn : cs_out.rpo_blocks[0].instructions) {
    if (insn.find("REG_DEAD (mem:SI") != std::string::npos) {
      has_mem_dead = true;
    }
  }
  EXPECT_TRUE(has_mem_dead) << cs_out.FormatSummary();
}

TEST_F(InvertScheduleBlockTest,
       ReadjoinsMdDiOutputReloadAndSinksJumpInputReloadAndCallerSaveChain) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};

  // 1. Positive (`reload1.c:6631`): `ReadjoinMdDiOutputReloads` moves the
  //    `DI` `hi/lo` output reload pair (`mfhi/mflo` + `sp` spill store) back
  //    immediately after `mult:DI` when `sched2` interleaved an independent
  //    load between them.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 32;
    cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 6}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:DI 64 hi) (mult:DI (sign_extend:DI (reg:SI 4 a0)) "
        "(sign_extend:DI (reg:SI 5 a1)))) (clobber (reg:SI 66 accum)) ] )");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:DI 10 t2) (reg:DI 64 hi))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:DI 10 t2))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (const_int 1)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 10 t2)))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

    const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kScheduleBlockSched2, cfg, {}, 2, prof.c_flags);
    const RtlControlFlowGraph out = InvertScheduleBlock(fwd.post_state, prof);
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(fwd).matches)
        << out.FormatSummary();
    auto idx_of = [](const std::vector<std::string>& insns, std::string_view needle) -> int {
      for (size_t i = 0; i < insns.size(); ++i) {
        if (insns[i].find(needle) != std::string::npos) return static_cast<int>(i);
      }
      return -1;
    };
    const int mult_i = idx_of(out.rpo_blocks[0].instructions, "(mult:DI ");
    const int s1_i = idx_of(out.rpo_blocks[0].instructions, "(set (reg:DI 10 t2) (reg:DI 64 hi))");
    const int s2_i = idx_of(out.rpo_blocks[0].instructions, "(mem:DI (plus:SI (reg:SI 29 sp)");
    ASSERT_GE(mult_i, 0);
    EXPECT_EQ(s1_i, mult_i + 1) << out.FormatSummary();
    EXPECT_EQ(s2_i, mult_i + 2) << out.FormatSummary();
  }

  // 1b. Positive + Negative (`sched.c:3144`, `reload1.c:6631`):
  //     When `sched2` schedules an independent non-`/u` load into `(m_idx, s1_idx)`
  //     and sinks `s2` past a second `mult:DI`, `ReadjoinMdDiOutputReloads`
  //     hoists the non-`/u` load ahead of `mult:DI` so `s1` and `s2` re-adjoin
  //     without requiring a speculative `/u` flag.
  //     When the load overwrites an input of `mult:DI` (anti-dependent on
  //     `mult:DI`), it must NOT be hoisted ahead of `mult:DI`.
  {
    auto idx_of = [](const std::vector<std::string>& insns, std::string_view needle) -> int {
      for (size_t i = 0; i < insns.size(); ++i) {
        if (insns[i].find(needle) != std::string::npos) return static_cast<int>(i);
      }
      return -1;
    };
    RtlControlFlowGraph pre_cfg;
    pre_cfg.pass = RtlPassKind::kSched2;
    pre_cfg.sideband.frame_size_bytes = 32;
    pre_cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 6, 7}}, {1, {4, 5, 6, 7}}, {2, {4}}};
    pre_cfg.rpo_blocks.resize(3);
    pre_cfg.rpo_blocks[0].id = 0;
    pre_cfg.rpo_blocks[0].successors = {1, 2};
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 7 a3) (const_int 0)) (label_ref L2) (pc)))");
    pre_cfg.rpo_blocks[1].id = 1;
    pre_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 8 t0) (mem:SI (reg:SI 6 a2)))");
    pre_cfg.rpo_blocks[1].AddPatternOnly(
        "(parallel[ (set (reg:DI 64 hi) (mult:DI (sign_extend:DI (reg:SI 4 a0)) "
        "(sign_extend:DI (reg:SI 5 a1)))) (clobber (reg:SI 66 accum)) ] )");
    pre_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:DI 10 t2) (reg:DI 64 hi))");
    pre_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (mem:DI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:DI 10 t2))");
    pre_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 8 t0) (plus:SI (reg:SI 8 t0) (const_int 1)))");
    pre_cfg.rpo_blocks[1].AddPatternOnly(
        "(parallel[ (set (reg:DI 64 hi) (mult:DI (zero_extend:DI (reg:SI 8 t0)) "
        "(zero_extend:DI (reg:SI 7 a3)))) (clobber (reg:SI 66 accum)) ] )");
    pre_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:DI 12 t4) (reg:DI 64 hi))");
    pre_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 10 t2) (reg:SI 12 t4)))");
    pre_cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");
    pre_cfg.rpo_blocks[2].id = 2;
    pre_cfg.rpo_blocks[2].has_label = true;
    pre_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 4 a0))");
    pre_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

    CompilationProfile s2_only_prof;
    s2_only_prof.optimize_level = 2;
    s2_only_prof.c_flags = {"-O2", "-fno-schedule-insns", "-mips2", "-mcpu=r4000", "-Wa,-g"};
    const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kScheduleBlockSched2, pre_cfg, {}, 2, s2_only_prof.c_flags);
    ASSERT_EQ(fwd.post_state.rpo_blocks.size(), 3);
    const int fwd_mult = idx_of(fwd.post_state.rpo_blocks[1].instructions, "(sign_extend:DI ");
    const int fwd_load =
        idx_of(fwd.post_state.rpo_blocks[1].instructions, "(mem:SI (reg:SI 6 a2))");
    const int fwd_s1 =
        idx_of(fwd.post_state.rpo_blocks[1].instructions, "(set (reg:DI 10 t2) (reg:DI 64 hi))");
    const int fwd_s2 =
        idx_of(fwd.post_state.rpo_blocks[1].instructions, "(mem:DI (plus:SI (reg:SI 29 sp)");
    ASSERT_LT(fwd_mult, fwd_load) << fwd.post_state.FormatSummary();
    ASSERT_LT(fwd_load, fwd_s1) << fwd.post_state.FormatSummary();
    ASSERT_GT(fwd_s2, fwd_s1 + 1) << fwd.post_state.FormatSummary();

    const RtlControlFlowGraph pos_out = InvertScheduleBlock(fwd.post_state, s2_only_prof);
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(fwd).matches)
        << pos_out.FormatSummary();
    const int pos_load = idx_of(pos_out.rpo_blocks[1].instructions, "(mem:SI (reg:SI 6 a2))");
    const int pos_mult = idx_of(pos_out.rpo_blocks[1].instructions, "(sign_extend:DI ");
    const int pos_s1 =
        idx_of(pos_out.rpo_blocks[1].instructions, "(set (reg:DI 10 t2) (reg:DI 64 hi))");
    const int pos_s2 =
        idx_of(pos_out.rpo_blocks[1].instructions, "(mem:DI (plus:SI (reg:SI 29 sp)");
    ASSERT_GE(pos_load, 0);
    ASSERT_GE(pos_mult, 0);
    EXPECT_LT(pos_load, pos_mult) << pos_out.FormatSummary();
    EXPECT_EQ(pos_s1, pos_mult + 1) << pos_out.FormatSummary();
    EXPECT_EQ(pos_s2, pos_mult + 2) << pos_out.FormatSummary();

    // Negative: when the load in `(m_idx, s1_idx)` overwrites `mult:DI`'s input
    // `$4 a0` (anti-dependent on `mult:DI`), `ReadjoinMdDiOutputReloads` must
    // not hoist it ahead of `mult:DI`.
    RtlControlFlowGraph neg_cfg = fwd.post_state;
    for (std::string& s : neg_cfg.rpo_blocks[1].instructions) {
      if (s.find("(mem:SI (reg:SI 6 a2))") != std::string::npos) {
        s = "(set (reg:SI 4 a0) (mem:SI (reg:SI 6 a2)))";
      } else if (s.find("(plus:SI (reg:SI 8 t0)") != std::string::npos) {
        s = "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))";
      }
    }
    neg_cfg.rpo_blocks[1].SyncRtlInsnsFromInstructions();
    const RtlControlFlowGraph neg_out = InvertScheduleBlock(neg_cfg, s2_only_prof);
    const int neg_load = idx_of(neg_out.rpo_blocks[1].instructions, "(mem:SI (reg:SI 6 a2))");
    const int neg_mult = idx_of(neg_out.rpo_blocks[1].instructions, "(sign_extend:DI ");
    ASSERT_GE(neg_load, 0);
    ASSERT_GE(neg_mult, 0);
    EXPECT_GT(neg_load, neg_mult) << neg_out.FormatSummary();
  }

  // 2. Positive + Negative (`caller-save.c:559-646` + `reload1.c:6583`):
  //    forward caller-save user hoisting resolves a dependent restore chain
  //    (`lw $6`, `lw $7`, `xor $6`, `addu $7, $7, $6`) and re-sinks a jump's
  //    stack-slot input reload immediately before the conditional jump.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 48;
    cfg.sideband.outgoing_args_size = 16;
    cfg.sideband.saved_gpr_regnos = {31};
    cfg.sideband.basic_block_live_at_start = {{0, {4}}, {1, {2}}, {2, {2}}};
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1, 2};
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 7 a3) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 6 a2) (xor:SI (reg:SI 6 a2) (reg:SI 2 v0)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 7 a3) (plus:SI (reg:SI 7 a3) (reg:SI 6 a2))) "
        "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 7 a3))) "
        "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 8 t0) (const_int 0)) (label_ref L2) (pc))) "
        "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].has_label = true;
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

    const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kScheduleBlockSched2, cfg, {}, 2, prof.c_flags);
    const RtlControlFlowGraph lifted = InvertScheduleBlock(fwd.post_state, prof);
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(fwd).matches)
        << lifted.FormatSummary();

    // 3. Pure jump input reload sinking (`reload1.c:6583`) in a non-call block:
    //    `sched2` hoists `(set (reg:SI 8 t0) (mem:SI (+ sp 4)))` (a proven
    //    reload spill slot at `8k+4` with all callee-saved GPRs used) ahead of
    //    an independent ALU chain in `B1`, and `InvertScheduleBlock` re-sinks
    //    it immediately before the conditional jump.
    RtlControlFlowGraph jump_cfg;
    jump_cfg.pass = RtlPassKind::kSched2;
    jump_cfg.sideband.frame_size_bytes = 64;
    jump_cfg.sideband.saved_gpr_regnos = {16, 17, 18, 19, 20, 21, 22, 23, 30};
    jump_cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 6}}, {1, {4, 5}}, {2, {2}}, {3, {2}}};
    jump_cfg.rpo_blocks.resize(4);
    jump_cfg.rpo_blocks[0].id = 0;
    jump_cfg.rpo_blocks[0].successors = {1};
    jump_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 6 a2)) "
        "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
    jump_cfg.rpo_blocks[1].id = 1;
    jump_cfg.rpo_blocks[1].has_label = true;
    jump_cfg.rpo_blocks[1].successors = {2, 3};
    jump_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
    jump_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 2 v0) (xor:SI (reg:SI 2 v0) (reg:SI 5 a1)))");
    jump_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))))");
    jump_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 8 t0) (const_int 0)) (label_ref L3) (pc))) "
        "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
    jump_cfg.rpo_blocks[2].id = 2;
    jump_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
    jump_cfg.rpo_blocks[3].id = 3;
    jump_cfg.rpo_blocks[3].has_label = true;
    jump_cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");

    const gcc_harness::SubfunctionBoundarySnapshot jfwd = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kScheduleBlockSched2, jump_cfg, {}, 2, prof.c_flags);
    RtlControlFlowGraph jpost = jfwd.post_state;
    jpost.sideband.saved_gpr_regnos = {16, 17, 18, 19, 20, 21, 22, 23, 30};
    const RtlControlFlowGraph jlifted = InvertScheduleBlock(jpost, prof);
    ASSERT_GE(jlifted.rpo_blocks[1].instructions.size(), 4u);
    const size_t jn = jlifted.rpo_blocks[1].instructions.size();
    EXPECT_NE(jlifted.rpo_blocks[1].instructions[jn - 2].find("(set (reg:SI 8 t0) (mem:SI"),
              std::string::npos)
        << jlifted.FormatSummary();
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(jfwd).matches)
        << jlifted.FormatSummary();

    // Negative: under `-O1` (no `sched2`), `InvertScheduleBlock` does not sink
    // the hoisted reload before the jump.
    CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips2"}};
    const RtlControlFlowGraph untouched = InvertScheduleBlock(jpost, o1_prof);
    ASSERT_GE(untouched.rpo_blocks[1].instructions.size(), 4u);
    EXPECT_NE(untouched.rpo_blocks[1].instructions[0].find("(set (reg:SI 8 t0) (mem:SI"),
              std::string::npos)
        << untouched.FormatSummary();
  }
}

TEST_F(InvertScheduleBlockTest, RestoresRoundRobinFpReloadGroupOrderWhenSched2Verified) {
  // Mirrors `reload1.c:702, 2229, 4758-4767` (`RestoreRoundRobinFpReloadGroupOrder`):
  // when `sched2` swaps two independent adjacent FP spill-register groups so
  // the higher spill reg (`$f8 = 40`) precedes the lower spill reg (`$f6 = 38`)
  // across an interleaved prologue callee-saved GPR store, Stage 3 restores the
  // `$f6` group ahead of `$f8` iff verified against exact `kSched2` replay
  // (positive), and leaves `-O1` (`!RunsSched2()`) untouched (negative).
  // Build a genuine pre-`sched2` block with `$f6` before `$f8` (on caller-saved
  // `$8 t0` / `$9 t1` so no prologue stores interleave), where `$f8`'s `div:SF`
  // has higher critical-path priority than `$f6`'s `plus:SF`, so real GCC
  // `sched2` swaps `$f8` ahead of `$f6`. Stage 3 must restore `$f6` ahead of
  // `$f8` (positive) while `-O1` leaves the block untouched (negative).
  RtlControlFlowGraph pre;
  pre.pass = RtlPassKind::kGreg;
  pre.sideband.basic_block_live_at_start[0] = {8, 9, 29, 32, 34};
  pre.rpo_blocks.resize(1);
  pre.rpo_blocks[0].id = 0;
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SF 38 $f6) (reg:SF 9 t1))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 38 $f6) (plus:SF (reg:SF 38 $f6) (reg:SF 34 $f2)))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SF 9 t1) (reg:SF 38 $f6))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SF 40 $f8) (reg:SF 8 t0))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 40 $f8) (div:SF (reg:SF 40 $f8) (reg:SF 32 $f0)))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SF 8 t0) (reg:SF 40 $f8))");
  pre.rpo_blocks[0].AddPatternOnly("(use (reg:SF 8 t0))");
  pre.rpo_blocks[0].AddPatternOnly("(use (reg:SF 9 t1))");

  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  hooks_.ApplyCompilationProfile(o2_prof);
  const gcc_harness::SubfunctionBoundarySnapshot snap =
      hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kScheduleBlockSched2, pre);
  ASSERT_EQ(snap.post_state.rpo_blocks.size(), 1u);
  const RtlControlFlowGraph& post = snap.post_state;

  // Positive (`-O2` with `sched2`): `sched2` scheduled `$f8` (`div:SF`) ahead
  // of `$f6` (`plus:SF`), and Stage 3 restores `$f6` ahead of `$f8` while
  // verifying exact `kSched2` replay.
  {
    const auto find_pos = [](const RtlControlFlowGraph& cfg, std::string_view sub) {
      for (size_t i = 0; i < cfg.rpo_blocks[0].instructions.size(); ++i) {
        if (cfg.rpo_blocks[0].instructions[i].find(sub) != std::string::npos) {
          return i;
        }
      }
      return cfg.rpo_blocks[0].instructions.size();
    };
    ASSERT_LT(find_pos(post, "(set (reg:SF 40 $f8) (reg:SF 8 t0))"),
              find_pos(post, "(set (reg:SF 38 $f6) (reg:SF 9 t1))"))
        << post.FormatSummary();
    const RtlControlFlowGraph lifted = InvertScheduleBlock(post, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_LT(find_pos(lifted, "(set (reg:SF 38 $f6) (reg:SF 9 t1))"),
              find_pos(lifted, "(set (reg:SF 40 $f8) (reg:SF 8 t0))"))
        << lifted.FormatSummary();
    const gcc_harness::RightInverseResult res = verifier_.VerifySched2StageRightInverse(post);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative (`-O1`, `!RunsSched2()`): Stage 3 does not reorder the block.
  {
    const CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips2"}};
    const RtlControlFlowGraph untouched = InvertScheduleBlock(post, o1_prof);
    ASSERT_EQ(untouched.rpo_blocks.size(), 1u);
    EXPECT_EQ(untouched.rpo_blocks[0].instructions, post.rpo_blocks[0].instructions);
  }
}

TEST_F(InvertScheduleBlockTest,
       CanonicalizesAdjacentCalleeSavedMtc1AndSpillConstDoublePairsButSkipsAbiArgMoves) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  auto find_pos = [](const RtlControlFlowGraph& cfg, std::string_view sub) {
    for (size_t i = 0; i < cfg.rpo_blocks[0].instructions.size(); ++i) {
      if (cfg.rpo_blocks[0].instructions[i].find(sub) != std::string::npos) {
        return i;
      }
    }
    return cfg.rpo_blocks[0].instructions.size();
  };

  // Positive 1 (`reload1.c:6239-6300`): adjacent callee-saved GPR->FPR `mtc1`
  // input reloads (where `$f8` is inherited into a later FP consumer) emitted
  // out of operand order (`$f8` before `$f6` for `(plus:SF $f6 $f8)`) swap into
  // left-to-right operand order (`$f6` before `$f8`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start[0] = {16, 17, 29, 31};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 40 $f8) (reg:SF 17 s1))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 38 $f6) (reg:SF 16 s0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 36 $f4) (plus:SF (reg:SF 38 $f6) (reg:SF 40 $f8)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (mult:SF (reg:SF 36 $f4) (reg:SF 40 $f8)))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SF 32 $f0))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const RtlControlFlowGraph lifted = InvertScheduleBlock(cfg, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_LT(find_pos(lifted, "(set (reg:SF 38 $f6) (reg:SF 16 s0))"),
              find_pos(lifted, "(set (reg:SF 40 $f8) (reg:SF 17 s1))"))
        << lifted.FormatSummary();
  }

  // Negative 1: caller-saved GPR moves (`$t0`, `$t1`) with `$f8` inherited into
  // a later consumer are NOT callee-saved cross-bank spill reloads and must
  // preserve their original order.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start[0] = {8, 9, 29, 31};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 40 $f8) (reg:SF 9 t1))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 38 $f6) (reg:SF 8 t0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 36 $f4) (plus:SF (reg:SF 38 $f6) (reg:SF 40 $f8)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (mult:SF (reg:SF 36 $f4) (reg:SF 40 $f8)))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SF 32 $f0))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const RtlControlFlowGraph lifted = InvertScheduleBlock(cfg, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_LT(find_pos(lifted, "(set (reg:SF 40 $f8) (reg:SF 9 t1))"),
              find_pos(lifted, "(set (reg:SF 38 $f6) (reg:SF 8 t0))"))
        << lifted.FormatSummary();
  }

  // Positive 2 (`reload1.c:6239-6300`): adjacent spill-slot input load (`$f8`)
  // followed by an output-sharing `const_double` reload (`$f10`) feeding
  // `(set $f10 (mult $f8 $f10))` swaps `const_double` ahead of the spill load.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 128;
    cfg.sideband.saved_gpr_regnos = {31};
    cfg.sideband.saved_fpr_regnos = {52, 54, 56, 58, 60, 62};
    cfg.sideband.basic_block_live_at_start[0] = {4, 29, 31, 44};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -128)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))) (reg:SI 31 ra))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 80))) (reg:DF 52 $f20))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 88))) (reg:DF 54 $f22))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 96))) (reg:DF 56 $f24))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 104))) (reg:DF 58 $f26))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 112))) (reg:DF 60 $f28))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 120))) (reg:DF 62 $f30))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 68))) (reg:SF 44 $f12))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 40 $f8) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 68))))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 42 $f10) (const_double:SF (cc0) 0 1070596096))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 42 $f10) (mult:SF (reg:SF 40 $f8) (reg:SF 42 $f10)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 60))) (reg:SF 42 $f10))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 60))))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SF 32 $f0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:DF 52 $f20) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 80))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:DF 54 $f22) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 88))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:DF 56 $f24) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 96))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:DF 58 $f26) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 104))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:DF 60 $f28) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 112))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:DF 62 $f30) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 120))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 128)))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const RtlControlFlowGraph lifted = InvertScheduleBlock(cfg, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_LT(find_pos(lifted, "(const_double:SF (cc0) 0 1070596096)"),
              find_pos(lifted, "(set (reg:SF 40 $f8) (mem:SF"))
        << lifted.FormatSummary();
  }
}

TEST_F(InvertScheduleBlockTest, ReadjoinsSunkMfloLoOutputReloadUnlessLoClobbered) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  auto build_cfg = [](bool clobber_lo_between, bool consumer_carries_lo_dead) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start[0] = {4, 5, 6, 7, 29, 31};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
        "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] )");
    if (clobber_lo_between) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 6 a2) (reg:SI 7 a3))) "
          "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] )");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (const_int 1)))");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (reg:SI 65 lo))");
    if (consumer_carries_lo_dead) {
      cfg.rpo_blocks[0].instructions.push_back(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 8 t0))) "
          "(expr_list:REG_DEAD (reg:SI 65 lo) (nil))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 8 t0)))");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    return cfg;
  };

  // Positive (`sched.c:1685-1703`, `reload1.c:emit_reload_insns`): independent
  // insn between `mult:SI -> 65 lo` and `(set v0 (reg:SI 65 lo))` allows
  // `ReadjoinMdDiOutputReloads` to pull `mflo` back immediately after `mult:SI`.
  {
    const RtlControlFlowGraph lifted = InvertScheduleBlock(
        build_cfg(/*clobber_lo_between=*/false, /*consumer_carries_lo_dead=*/false), o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    ASSERT_GE(lifted.rpo_blocks[0].instructions.size(), 3u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 2 v0) (reg:SI 65 lo))"),
              std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative 1: an intervening write to `65 lo` blocks hoisting `mflo` past it.
  {
    const RtlControlFlowGraph lifted = InvertScheduleBlock(
        build_cfg(/*clobber_lo_between=*/true, /*consumer_carries_lo_dead=*/false), o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    ASSERT_GE(lifted.rpo_blocks[0].instructions.size(), 3u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[2].find("(set (reg:SI 2 v0) (reg:SI 65 lo))"),
              std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative 2: a subsequent `(REG_DEAD (reg:SI 65 lo))` on the consumer marks
  // `mflo` as an input reload of a `65 lo`-allocated pseudo, so it stays
  // anchored immediately before its consumer.
  {
    const RtlControlFlowGraph lifted = InvertScheduleBlock(
        build_cfg(/*clobber_lo_between=*/false, /*consumer_carries_lo_dead=*/true), o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    ASSERT_GE(lifted.rpo_blocks[0].instructions.size(), 3u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[2].find("(set (reg:SI 2 v0) (reg:SI 65 lo))"),
              std::string::npos)
        << lifted.FormatSummary();
  }
}

TEST_F(InvertScheduleBlockTest,
       RestorePreSched1FlowLivenessReplacesStrictlyContainedPartialHardRegDeadNote) {
  // Mirrors `flow.c:2537` + `sched.c:2746-2753, 2888-2889` (`dead_notes`
  // conservation): when a multi-word hard reg `(reg:DI 2 v0)` has all words
  // dead at an instruction that already carries only a partial-word
  // `(REG_DEAD (reg:SI 3 v1))` note, `RestorePreSched1FlowLiveness` Step C must
  // replace the partial note with `(REG_DEAD (reg:DI 2 v0))` so `sched1` does
  // not exhaust `dead_notes` in `create_reg_dead_note`; when word 2 stays live,
  // it must keep only `(REG_DEAD (reg:SI 3 v1))`.
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1", "-Wa,-O2"}};
  auto build_cfg = [](bool word2_stays_live) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kCombine;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:DI 2 v0) (mem:DI (reg:SI 4 a0)))",
        "(set (reg:SI 72) (plus:SI (subreg:SI (reg:DI 2 v0) 1) (reg:SI 5 a1))) "
        "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
        word2_stays_live ? "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 2 v0)))"
                         : "(set (reg/i:SI 2 v0) (reg:SI 72))",
        "(use (reg/i:SI 2 v0))",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return cfg;
  };

  {
    RtlControlFlowGraph pos = build_cfg(/*word2_stays_live=*/false);
    RestorePreSched1FlowLiveness(&pos, o2_prof);
    const std::string& insn1 = pos.rpo_blocks[0].instructions[1];
    EXPECT_NE(insn1.find("(expr_list:REG_DEAD (reg:DI 2 v0)"), std::string::npos) << insn1;
    EXPECT_EQ(insn1.find("REG_DEAD (reg:SI 3 v1)"), std::string::npos) << insn1;
  }
  {
    RtlControlFlowGraph neg = build_cfg(/*word2_stays_live=*/true);
    RestorePreSched1FlowLiveness(&neg, o2_prof);
    const std::string& insn1 = neg.rpo_blocks[0].instructions[1];
    EXPECT_NE(insn1.find("REG_DEAD (reg:SI 3 v1)"), std::string::npos) << insn1;
    EXPECT_EQ(insn1.find("REG_DEAD (reg:DI 2 v0)"), std::string::npos) << insn1;
  }
}

TEST_F(InvertScheduleBlockTest, SinksPreScratchDeadChainPastMatchScratchParallelWhenVerified) {
  // Mirrors `local-alloc.c:1396-1401, 1416-1417, 2158-2182` + `mips.h:1186-1193`
  // (`SinkPreScratchDeadChainsIfVerified`): when `sched2` hoists a dead
  // lower-alloc-order chain (`$f0`) ahead of a `match_scratch` `PARALLEL`
  // (`fix_truncsfsi2` with `(clobber (reg:SF 34 $f2))` + `(clobber (reg:SI 2 v0))`)
  // while `$f0` dies before the `PARALLEL`, Stage 3 sinks the chain past the
  // `PARALLEL` iff verified under exact `kSched2` replay (positive), and leaves
  // `-O1` (`!RunsSched2()`) untouched (negative).
  const CompilationProfile mips1_o2{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
  };
  const CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips1"}};

  RtlControlFlowGraph pre;
  pre.pass = RtlPassKind::kGreg;
  pre.sideband.basic_block_live_at_start[0] = {4, 16, 29, 31, 44, 46};
  pre.rpo_blocks.resize(1);
  pre.rpo_blocks[0].id = 0;
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 32 $f0) (mult:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 3 v1) (fix:SI (reg:SF 44 $f12))) "
      "(clobber (reg:SI 2 v0)) (clobber (reg:SF 34 $f2)) ] ) "
      "(expr_list:REG_DEAD (reg:SF 44 $f12) (expr_list:REG_UNUSED (reg:SI 2 v0) "
      "(expr_list:REG_UNUSED (reg:SF 34 $f2) (nil))))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 46 $f14))) "
      "(expr_list:REG_DEAD (reg:SF 46 $f14) (nil))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SF (reg:SI 4 a0)) (reg:SF 32 $f0)) "
      "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 16 s0))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 16 s0) (nil)))");
  pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  hooks_.ApplyCompilationProfile(mips1_o2);
  const gcc_harness::SubfunctionBoundarySnapshot fwd =
      hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kScheduleBlockSched2, pre);
  ASSERT_EQ(fwd.post_state.rpo_blocks.size(), 1u);
  const RtlControlFlowGraph& post = fwd.post_state;
  auto find_pos = [](const RtlControlFlowGraph& cfg, std::string_view sub) {
    for (size_t i = 0; i < cfg.rpo_blocks[0].instructions.size(); ++i) {
      if (cfg.rpo_blocks[0].instructions[i].find(sub) != std::string::npos) {
        return i;
      }
    }
    return cfg.rpo_blocks[0].instructions.size();
  };

  // Positive: under `-O2`, if `sched2` hoisted the `$f0` death store ahead of
  // `fix:SI`, `InvertScheduleBlock` sinks it back past `fix:SI` while
  // maintaining exact `kSched2` replay right-inverse.
  {
    const RtlControlFlowGraph lifted = InvertScheduleBlock(post, mips1_o2);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    const gcc_harness::RightInverseResult res = verifier_.VerifySched2StageRightInverse(post);
    EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n" << lifted.FormatSummary();
    if (find_pos(post, "(set (mem:SF (reg:SI 4 a0))") < find_pos(post, "(fix:SI ")) {
      EXPECT_GT(find_pos(lifted, "(set (mem:SF (reg:SI 4 a0))"), find_pos(lifted, "(fix:SI "))
          << lifted.FormatSummary();
    }
  }

  // Negative: under `-O1` (`!RunsSched2()`), `SinkPreScratchDeadChainsIfVerified`
  // does not run and preserves the input order verbatim.
  {
    const RtlControlFlowGraph untouched = InvertScheduleBlock(post, o1_prof);
    ASSERT_EQ(untouched.rpo_blocks.size(), 1u);
    EXPECT_EQ(untouched.rpo_blocks[0].instructions, post.rpo_blocks[0].instructions);
  }
}

TEST_F(InvertScheduleBlockTest, AlignRestoredNoConflictRetvalNoopsForSched1PositiveAndNegative) {
  // Mirrors `sched.c:schedule_block` + `reload1.c`: after
  // `RestoreSched2PreScheduleDeletedNoopMoves` restores a synthetic
  // `(set (reg:DI dh) (reg:DI dh)) (REG_RETVAL)` no-op immediately after the last
  // `REG_NO_CONFLICT` subword set, `AlignRestoredNoConflictRetvalNoopsForSched1`
  // shifts it past an independent `argp` load when `Schedule(kSched1)` places that
  // load before `REG_RETVAL` (positive), while never moving `REG_RETVAL` past an
  // instruction that reads `dh` (negative).
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  auto find_pos = [](const RtlControlFlowGraph& cfg, std::string_view sub) {
    for (size_t i = 0; i < cfg.rpo_blocks[0].instructions.size(); ++i) {
      if (cfg.rpo_blocks[0].instructions[i].find(sub) != std::string::npos) {
        return i;
      }
    }
    return cfg.rpo_blocks[0].instructions.size();
  };

  auto build_cfg = [](bool consumer_immediately_after_subword) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start[0] = {4, 29};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 11 t3) (mem/s:SI (reg:SI 4 a0))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(clobber (reg:DI 10 t2)) (insn_list:REG_LIBCALL (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (subreg:SI (reg:DI 10 t2) 0) (const_int 0)) "
        "(expr_list:REG_NO_CONFLICT (reg:SI 11 t3) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (subreg:SI (reg:DI 10 t2) 1) (reg:SI 11 t3)) "
        "(expr_list:REG_DEAD (reg:SI 11 t3) (expr_list:REG_NO_CONFLICT (reg:SI 11 t3) (nil)))");
    if (consumer_immediately_after_subword) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 13 t5) (plus:SI (subreg:SI (reg:DI 10 t2) 1) (const_int 1))) "
          "(expr_list:REG_DEAD (reg:DI 10 t2) (nil))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg/i:SI 2 v0) (reg:SI 13 t5)) (expr_list:REG_DEAD (reg:SI 13 t5) (nil))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 12 t4) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 13 t5) (plus:SI (subreg:SI (reg:DI 10 t2) 1) (reg:SI 12 t4))) "
          "(expr_list:REG_DEAD (reg:DI 10 t2) (expr_list:REG_DEAD (reg:SI 12 t4) (nil)))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg/i:SI 2 v0) (reg:SI 13 t5)) (expr_list:REG_DEAD (reg:SI 13 t5) (nil))");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: restored `REG_RETVAL` aligns after the independent `20($sp)` load.
  {
    const RtlControlFlowGraph lifted = InvertThreadPrologueAndEpilogueInsns(
        build_cfg(/*consumer_immediately_after_subword=*/false), o2_prof);
    const size_t rv_pos = find_pos(lifted, "REG_RETVAL");
    const size_t ld_pos = find_pos(lifted, "(const_int 20)");
    ASSERT_LT(rv_pos, lifted.rpo_blocks[0].instructions.size()) << lifted.FormatSummary();
    ASSERT_LT(ld_pos, lifted.rpo_blocks[0].instructions.size()) << lifted.FormatSummary();
    EXPECT_GT(rv_pos, ld_pos) << lifted.FormatSummary();
  }

  // Negative: when the consumer of `(reg:DI 10 t2)` immediately follows
  // `(subreg:SI (reg:DI 10 t2) 1)`, `REG_RETVAL` stays strictly before the consumer.
  {
    const RtlControlFlowGraph lifted = InvertThreadPrologueAndEpilogueInsns(
        build_cfg(/*consumer_immediately_after_subword=*/true), o2_prof);
    const size_t rv_pos = find_pos(lifted, "REG_RETVAL");
    const size_t cons_pos = find_pos(lifted, "(plus:SI (subreg:SI (reg:DI 10 t2) 1)");
    ASSERT_LT(rv_pos, lifted.rpo_blocks[0].instructions.size()) << lifted.FormatSummary();
    ASSERT_LT(cons_pos, lifted.rpo_blocks[0].instructions.size()) << lifted.FormatSummary();
    EXPECT_LT(rv_pos, cons_pos) << lifted.FormatSummary();
  }
}

TEST_F(InvertScheduleBlockTest,
       OrdersCallerSaveRestoreBeforeInputReloadAndCanonicalizesInputReloads) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  auto find_pos = [](const RtlControlFlowGraph& cfg, std::string_view sub) {
    for (size_t i = 0; i < cfg.rpo_blocks[0].instructions.size(); ++i) {
      if (cfg.rpo_blocks[0].instructions[i].find(sub) != std::string::npos) {
        return i;
      }
    }
    return cfg.rpo_blocks[0].instructions.size();
  };

  // Positive 1 (`reload1.c:1906, 1938, 5641`): `save_call_clobbered_regs` runs
  // strictly before `reload_as_needed`. When real `sched2` hoists caller-save
  // restore `$f4 = [sp+16]` above independent ALU insns, Stage 3 sinks `$f4`
  // back down toward its consumer `u` while stopping strictly BEFORE `u`'s
  // contiguous input reload `$f10 = [sp+20]`.
  {
    RtlControlFlowGraph pre;
    pre.pass = RtlPassKind::kGreg;
    pre.sideband.frame_size_bytes = 48;
    pre.sideband.outgoing_args_size = 16;
    pre.sideband.saved_gpr_regnos = {16, 17, 31};
    pre.sideband.basic_block_live_at_start[0] = {16, 17, 29, 31, 36, 44};
    pre.rpo_blocks.resize(1);
    pre.rpo_blocks[0].id = 0;
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SF 44 $f12))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SF 36 $f4))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (plus:SI (reg:SI 16 s0) (const_int 1)))");
    pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 16 s0) (xor:SI (reg:SI 8 t0) (reg:SI 17 s1)))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 36 $f4) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 42 $f10) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 42 $f10)))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4))) "
        "(expr_list:REG_DEAD (reg:SF 36 $f4) (nil))");
    pre.rpo_blocks[0].AddPatternOnly("(use (reg:SI 16 s0))");
    pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SF 32 $f0))");

    const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kScheduleBlockSched2, pre, {}, 2, o2_prof.c_flags);
    ASSERT_LT(find_pos(fwd.post_state, "(set (reg:SF 36 $f4) (mem:SF"),
              find_pos(fwd.post_state, "(xor:SI "))
        << fwd.post_state.FormatSummary();
    const RtlControlFlowGraph lifted = InvertScheduleBlock(fwd.post_state, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    const size_t pos_cs = find_pos(lifted, "(set (reg:SF 36 $f4) (mem:SF");
    const size_t pos_in = find_pos(lifted, "(set (reg:SF 42 $f10) (mem:SF");
    const size_t pos_u = find_pos(lifted, "(plus:SF (reg:SF 36 $f4) (reg:SF 42 $f10))");
    EXPECT_LT(pos_cs, pos_in) << lifted.FormatSummary();
    EXPECT_LT(pos_in, pos_u) << lifted.FormatSummary();
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(fwd).matches)
        << lifted.FormatSummary();
  }

  // Positive 2 & Negative (`reload1.c:6476-6482`): when `sched2` swaps two
  // adjacent independent non-`const_double` input reloads (`$f8 = s0` and
  // `$f10 = [sp+20]`, where `l.s` has higher latency than `mtc1`) feeding
  // `(plus:SF $f8 $f10)` into reverse operand order (`$f10` before `$f8`),
  // Stage 3 canonicalizes them back to ascending operand preorder (`$f8` before
  // `$f10`), whereas a `const_double` input reload is not swapped.
  {
    RtlControlFlowGraph pre;
    pre.pass = RtlPassKind::kGreg;
    pre.sideband.frame_size_bytes = 64;
    pre.sideband.outgoing_args_size = 16;
    pre.sideband.saved_gpr_regnos = {16, 31};
    pre.sideband.basic_block_live_at_start[0] = {4, 16, 29, 31, 36, 44};
    pre.rpo_blocks.resize(1);
    pre.rpo_blocks[0].id = 0;
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SF 44 $f12))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SF 36 $f4))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 36 $f4) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))))");
    pre.rpo_blocks[0].AddPatternOnly("(set (reg:SF 40 $f8) (reg:SF 16 s0))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 42 $f10) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 38 $f6) (plus:SF (reg:SF 40 $f8) (reg:SF 42 $f10)))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 38 $f6))) "
        "(expr_list:REG_DEAD (reg:SF 38 $f6) (nil))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4))) "
        "(expr_list:REG_DEAD (reg:SF 36 $f4) (nil))");
    pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SF 32 $f0))");

    const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kScheduleBlockSched2, pre, {}, 2, o2_prof.c_flags);
    ASSERT_LT(find_pos(fwd.post_state, "(set (reg:SF 42 $f10) (mem:SF"),
              find_pos(fwd.post_state, "(set (reg:SF 40 $f8) (reg:SF 16 s0))"))
        << fwd.post_state.FormatSummary();
    const RtlControlFlowGraph lifted = InvertScheduleBlock(fwd.post_state, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_LT(find_pos(lifted, "(set (reg:SF 40 $f8) (reg:SF 16 s0))"),
              find_pos(lifted, "(set (reg:SF 42 $f10) (mem:SF"))
        << lifted.FormatSummary();
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(fwd).matches)
        << lifted.FormatSummary();

    // Negative: when the second operand reload (`$f10`) is a `const_double`
    // placed ahead of `$f8 = [sp+20]`, Stage 3 does NOT swap the `const_double`
    // reload behind the stack load.
    RtlControlFlowGraph neg_pre = pre;
    neg_pre.rpo_blocks[0].instructions.clear();
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SF 44 $f12))");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SF 36 $f4))");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 36 $f4) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))))");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 42 $f10) (const_double:SF (cc0) 0 1070596096))");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 40 $f8) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))))");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 38 $f6) (plus:SF (reg:SF 40 $f8) (reg:SF 42 $f10)))");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 38 $f6))) "
        "(expr_list:REG_DEAD (reg:SF 38 $f6) (nil))");
    neg_pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4))) "
        "(expr_list:REG_DEAD (reg:SF 36 $f4) (nil))");
    neg_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SF 32 $f0))");
    const gcc_harness::SubfunctionBoundarySnapshot neg_fwd = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kScheduleBlockSched2, neg_pre, {}, 2, o2_prof.c_flags);
    const RtlControlFlowGraph neg_lifted = InvertScheduleBlock(neg_fwd.post_state, o2_prof);
    ASSERT_EQ(neg_lifted.rpo_blocks.size(), 1u);
    EXPECT_LT(find_pos(neg_lifted, "(const_double:SF"),
              find_pos(neg_lifted, "(set (reg:SF 40 $f8) (mem:SF"))
        << neg_lifted.FormatSummary();
  }
}

TEST_F(InvertScheduleBlockTest, RepairSched2PreservesTrailingNoopSelfMoveUnderO1Sched2) {
  // `-O1 -fschedule-insns2`: `flags.RunsSched1()` is false while
  // `flags.RunsSched2()` is true. `InvertScheduleBlock` must still populate
  // `pre_unsched_cfg` and verify/repair `kSched2` replay, and must NEVER place
  // a post-reload no-op self-move (`sched.c:4803-4818`,
  // `rtx_equal_function_value_matters == 0`) at `pos < n - 1` where GCC would
  // delete it.
  CompilationProfile o1_s2_prof;
  o1_s2_prof.optimize_level = 1;
  o1_s2_prof.c_flags = {"-O1", "-fschedule-insns2", "-mips2", "-mcpu=r4000"};

  RtlControlFlowGraph pre;
  pre.pass = RtlPassKind::kSched2;
  pre.sideband.frame_size_bytes = 32;
  pre.sideband.basic_block_live_at_start = {{0, {4, 5}}};
  pre.rpo_blocks.resize(1);
  pre.rpo_blocks[0].id = 0;
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 9 t1) (plus:SI (reg:SI 5 a1) (const_int 3)))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 4 a0) (reg:SI 4 a0))");

  const gcc_harness::SubfunctionBoundarySnapshot fwd = hooks_.ExecuteSubfunctionBoundary(
      gcc_harness::SubfunctionId::kScheduleBlockSched2, pre, {}, 1, o1_s2_prof.c_flags);
  const RtlControlFlowGraph lifted = InvertScheduleBlock(fwd.post_state, o1_s2_prof);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_FALSE(lifted.rpo_blocks[0].instructions.empty());
  EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(), "(set (reg:SI 4 a0) (reg:SI 4 a0))")
      << lifted.FormatSummary();

  hooks_.ApplyCompilationProfile(o1_s2_prof);
  const gcc_harness::RightInverseResult res =
      verifier_.VerifySched2StageRightInverse(fwd.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;
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
