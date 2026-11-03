#include "lifter/inverse_jump1_cfg_cleanup_stage.h"

#include <cstddef>
#include <vector>

#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

TEST(InverseJump1CfgCleanupStageTest, ExpandsStoreFlagComparisonIntoBranchDiamond) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a1, $a1, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("slt   $v0, $a0, $a1");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $zero, $zero, $zero");

  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[1].successors, (std::vector<size_t>{3}));
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].successors, (std::vector<size_t>{3}));
  EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 2u);
  EXPECT_TRUE(cfg.rpo_blocks[3].successors.empty());
  EXPECT_TRUE(cfg.rpo_blocks[3].instructions.empty());
}

TEST(InverseJump1CfgCleanupStageTest, SinksSpeculativelyHoistedBranchMove) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $v0, $a1, $zero");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[1].AddPatternOnly("addu  $v0, $a2, $zero");

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].successors = {};

  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[1].successors, (std::vector<size_t>{3}));
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].successors, (std::vector<size_t>{3}));
  EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 2u);
  EXPECT_TRUE(cfg.rpo_blocks[3].successors.empty());
}

TEST(InverseJump1CfgCleanupStageTest, UnrotatesForLoopExitTestAndInsertsExitTrampoline) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $v1, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].successors = {1, 2};
  cfg.rpo_blocks[1].AddPatternOnly("addu  $v1, $v1, $a0");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $a0, $a0, -1");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].successors = {};
  cfg.rpo_blocks[2].AddPatternOnly("addu  $v0, $v1, $zero");

  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 6u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{1}));
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(cfg.rpo_blocks[1].successors, (std::vector<size_t>{5, 2}));
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[2].successors, (std::vector<size_t>{3}));
  EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[3].successors, (std::vector<size_t>{4}));
  EXPECT_EQ(cfg.rpo_blocks[3].instructions.size(), 2u);
  EXPECT_TRUE(cfg.rpo_blocks[4].successors.empty());
  EXPECT_EQ(cfg.rpo_blocks[5].successors, (std::vector<size_t>{1}));
  EXPECT_EQ(cfg.rpo_blocks[5].instructions.size(), 2u);
}

TEST(InverseJump1CfgCleanupStageTest, UnswapsEarlyReturnBranchesAndRestoresFallthroughJump) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].successors = {3};
  cfg.rpo_blocks[1].AddPatternOnly("addiu $v0, $a0, 10");
  cfg.rpo_blocks[1].AddPatternOnly("sll   $v0, $v0, 1");
  cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref))");

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].successors = {3};
  cfg.rpo_blocks[2].AddPatternOnly("addiu $v0, $zero, -1");

  cfg.rpo_blocks[3].id = 3;
  cfg.rpo_blocks[3].successors = {};

  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 3u);
}

TEST(InverseJump1CfgCleanupStageTest, DistinguishesVoidStoreOnlyBlockFromValueReturningBlock) {
  RtlControlFlowGraph void_cfg;
  void_cfg.rpo_blocks.resize(1);
  void_cfg.rpo_blocks[0].id = 0;
  void_cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  void_cfg.rpo_blocks[0].AddPatternOnly("sw    $zero, 0($a0)");

  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(&void_cfg);
  EXPECT_EQ(void_cfg.rpo_blocks.size(), 1u);

  RtlControlFlowGraph non_void_cfg;
  non_void_cfg.rpo_blocks.resize(1);
  non_void_cfg.rpo_blocks[0].id = 0;
  non_void_cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  non_void_cfg.rpo_blocks[0].AddPatternOnly("addiu $v0, $a0, 1");

  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(&non_void_cfg);
  ASSERT_EQ(non_void_cfg.rpo_blocks.size(), 2u);
  EXPECT_EQ(non_void_cfg.rpo_blocks[0].successors, (std::vector<size_t>{1}));
  EXPECT_EQ(non_void_cfg.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_TRUE(non_void_cfg.rpo_blocks[1].successors.empty());
  EXPECT_TRUE(non_void_cfg.rpo_blocks[1].instructions.empty());
}

TEST(InverseJump1CfgCleanupStageTest, PreservesExactRtlSexprPatternsOnStoreFlagAndBranchInversion) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("srl   $v0, $a0, 31");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $zero, $zero, $zero");

  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
  ASSERT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns[1].rtl_sexpr_pattern,
            "(set (pc) (if_then_else (ge:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");
  ASSERT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[0].rtl_sexpr_pattern, "(set (reg:SI 2 v0) (const_int 1))");
  ASSERT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns[0].rtl_sexpr_pattern, "(set (reg:SI 2 v0) (const_int 0))");
}

}  // namespace
}  // namespace rom_nom_nom
