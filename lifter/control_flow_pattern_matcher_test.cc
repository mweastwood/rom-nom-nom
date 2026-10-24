#include "lifter/control_flow_pattern_matcher.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

TEST(ControlFlowPatternMatcherTest, DecisionTreeSwitchTwoCases) {
  // 2-case linear decision tree:
  // Block 0: addiu $at, $zero, 1; beq $a0, $at, Case 1 (0x20); nop
  // Block 1: addiu $at, $zero, 2; beq $a0, $at, Case 2 (0x2C); nop
  // Block 2: j Join (0x38); nop (Default target)
  // Block 3 (Case 1 at 0x20): addiu $v0, $zero, 10; j Join (0x38); nop
  // Block 4 (Case 2 at 0x2C): addiu $v0, $zero, 20; j Join (0x38); nop
  // Block 5 (Join at 0x38): jr $ra; nop
  std::vector<uint32_t> words = {
      0x24010001,  // 0x00: addiu $at, $zero, 1
      0x10810006,  // 0x04: beq $a0, $at, +6 (0x20)
      0x00000000,  // 0x08: nop
      0x24010002,  // 0x0C: addiu $at, $zero, 2
      0x10810006,  // 0x10: beq $a0, $at, +6 (0x2C)
      0x00000000,  // 0x14: nop
      0x0800000E,  // 0x18: j 0x80000038
      0x00000000,  // 0x1C: nop
      0x2402000A,  // 0x20: addiu $v0, $zero, 10
      0x0800000E,  // 0x24: j 0x80000038
      0x00000000,  // 0x28: nop
      0x24020014,  // 0x2C: addiu $v0, $zero, 20
      0x0800000E,  // 0x30: j 0x80000038
      0x00000000,  // 0x34: nop
      0x03E00008,  // 0x38: jr $ra
      0x00000000,  // 0x3C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  EXPECT_EQ(region->type, RegionType::kSequence);
  ASSERT_GE(region->children.size(), 2u);
  EXPECT_EQ(region->children[0]->type, RegionType::kSwitch);
  EXPECT_EQ(region->children[0]->switch_register, Register::kA0);

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("switch ($a0)"));
  EXPECT_THAT(s, HasSubstr("case 1:"));
  EXPECT_THAT(s, HasSubstr("case 2:"));
  EXPECT_THAT(s, HasSubstr("default:"));
  EXPECT_THAT(s, HasSubstr("block_5\n"));
}

TEST(ControlFlowPatternMatcherTest, DecisionTreeSwitchFourCases) {
  // 4-case binary decision tree:
  // Root: slti $at, $a0, 3; beq $at, $zero, Right branch (0x2C); nop
  // Left: addiu $at, $zero, 1; beq $a0, $at, Case 1 (0x4C); nop
  // Left cont: addiu $at, $zero, 2; beq $a0, $at, Case 2 (0x58); nop
  // Left default: j Default (0x7C); nop
  // Right: addiu $at, $zero, 3; beq $a0, $at, Case 3 (0x64); nop
  // Right cont: addiu $at, $zero, 4; beq $a0, $at, Case 4 (0x70); nop
  // Right default: j Default (0x7C); nop
  // Case 1 (0x4C): addiu $v0, $zero, 10; j Join (0x88); nop
  // Case 2 (0x58): addiu $v0, $zero, 20; j Join (0x88); nop
  // Case 3 (0x64): addiu $v0, $zero, 30; j Join (0x88); nop
  // Case 4 (0x70): addiu $v0, $zero, 40; j Join (0x88); nop
  // Default (0x7C): addiu $v0, $zero, 0; j Join (0x88); nop
  // Join (0x88): jr $ra; nop
  std::vector<uint32_t> words = {
      0x28810003,  // 0x00: slti $at, $a0, 3
      0x10200009,  // 0x04: beq $at, $zero, +9 (0x2C)
      0x00000000,  // 0x08: nop
      0x24010001,  // 0x0C: addiu $at, $zero, 1
      0x1081000E,  // 0x10: beq $a0, $at, +14 (0x4C)
      0x00000000,  // 0x14: nop
      0x24010002,  // 0x18: addiu $at, $zero, 2
      0x1081000E,  // 0x1C: beq $a0, $at, +14 (0x58)
      0x00000000,  // 0x20: nop
      0x0800001F,  // 0x24: j 0x8000007C
      0x00000000,  // 0x28: nop
      0x24010003,  // 0x2C: addiu $at, $zero, 3
      0x1081000C,  // 0x30: beq $a0, $at, +12 (0x64)
      0x00000000,  // 0x34: nop
      0x24010004,  // 0x38: addiu $at, $zero, 4
      0x1081000C,  // 0x3C: beq $a0, $at, +12 (0x70)
      0x00000000,  // 0x40: nop
      0x0800001F,  // 0x44: j 0x8000007C
      0x00000000,  // 0x48: nop
      0x2402000A,  // 0x4C: addiu $v0, $zero, 10
      0x08000022,  // 0x50: j 0x80000088
      0x00000000,  // 0x54: nop
      0x24020014,  // 0x58: addiu $v0, $zero, 20
      0x08000022,  // 0x5C: j 0x80000088
      0x00000000,  // 0x60: nop
      0x2402001E,  // 0x64: addiu $v0, $zero, 30
      0x08000022,  // 0x68: j 0x80000088
      0x00000000,  // 0x6C: nop
      0x24020028,  // 0x70: addiu $v0, $zero, 40
      0x08000022,  // 0x74: j 0x80000088
      0x00000000,  // 0x78: nop
      0x24020000,  // 0x7C: addiu $v0, $zero, 0
      0x08000022,  // 0x80: j 0x80000088
      0x00000000,  // 0x84: nop
      0x03E00008,  // 0x88: jr $ra
      0x00000000,  // 0x8C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  EXPECT_EQ(region->type, RegionType::kSequence);
  ASSERT_GE(region->children.size(), 2u);
  EXPECT_EQ(region->children[0]->type, RegionType::kSwitch);
  EXPECT_EQ(region->children[0]->switch_register, Register::kA0);

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("switch ($a0)"));
  EXPECT_THAT(s, HasSubstr("case 1:"));
  EXPECT_THAT(s, HasSubstr("case 2:"));
  EXPECT_THAT(s, HasSubstr("case 3:"));
  EXPECT_THAT(s, HasSubstr("case 4:"));
  EXPECT_THAT(s, HasSubstr("default:"));
  EXPECT_THAT(s, HasSubstr("block_12\n"));
}

TEST(ControlFlowPatternMatcherTest, FloatToUintConversionDiamond) {
  // Single-precision 0x4F000000 float-to-uint conversion diamond.
  std::vector<uint32_t> words = {
      0x3C014F00,  // 0x00: lui $at, 0x4F00
      0x44811000,  // 0x04: mtc1 $at, $f2
      0x4602603C,  // 0x08: c.lt.s $f12, $f2
      0x45010008,  // 0x0C: bc1t +8 (0x30)
      0x00000000,  // 0x10: nop
      0x46026101,  // 0x14: sub.s $f4, $f12, $f2
      0x4600200D,  // 0x18: trunc.w.s $f0, $f4
      0x44020000,  // 0x1C: mfc1 $v0, $f0
      0x3C018000,  // 0x20: lui $at, 0x8000
      0x00411025,  // 0x24: or $v0, $v0, $at
      0x08000010,  // 0x28: j 0x80000040
      0x00000000,  // 0x2C: nop
      0x4600600D,  // 0x30: trunc.w.s $f0, $f12
      0x44020000,  // 0x34: mfc1 $v0, $f0
      0x08000010,  // 0x38: j 0x80000040
      0x00000000,  // 0x3C: nop
      0x03E00008,  // 0x40: jr $ra
      0x00000000,  // 0x44: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("$v0 = (u32)(f32)$f12;"));
}

TEST(ControlFlowPatternMatcherTest, DoubleToUintConversionDiamond) {
  // Double-precision 0x41E00000 double-to-uint conversion diamond.
  std::vector<uint32_t> words = {
      0x3C0141E0,  // 0x00: lui $at, 0x41E0
      0x44A11000,  // 0x04: dmtc1 $at, $f2
      0x4622603C,  // 0x08: c.lt.d $f12, $f2
      0x45010008,  // 0x0C: bc1t +8 (0x30)
      0x00000000,  // 0x10: nop
      0x46226101,  // 0x14: sub.d $f4, $f12, $f2
      0x4620200D,  // 0x18: trunc.w.d $f0, $f4
      0x44020000,  // 0x1C: mfc1 $v0, $f0
      0x3C018000,  // 0x20: lui $at, 0x8000
      0x00411025,  // 0x24: or $v0, $v0, $at
      0x08000010,  // 0x28: j 0x80000040
      0x00000000,  // 0x2C: nop
      0x4620600D,  // 0x30: trunc.w.d $f0, $f12
      0x44020000,  // 0x34: mfc1 $v0, $f0
      0x08000010,  // 0x38: j 0x80000040
      0x00000000,  // 0x3C: nop
      0x03E00008,  // 0x40: jr $ra
      0x00000000,  // 0x44: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("$v0 = (u32)(f64)$f12;"));
}

TEST(ControlFlowPatternMatcherTest, DoesNotMergeOrChainWhenBranchDelaySlotOverwritesRegister) {
  // Block 0 tests beq $v1, $v0, 0x18 with delay slot addiu $v0, $zero, 1250;
  // Block 1 tests beq $v1, $v0, 0x18 with nop delay slot.
  // Because Block 0's delay slot overwrites $v0, Block 0 and Block 1 must NOT be merged into ||.
  std::vector<uint32_t> words = {
      0x240204B0,  // 0x00: addiu $v0, $zero, 1200
      0x10620004,  // 0x04: beq   $v1, $v0, 0x18
      0x240204E2,  // 0x08: addiu $v0, $zero, 1250
      0x10620002,  // 0x0C: beq   $v1, $v0, 0x18
      0x00000000,  // 0x10: nop
      0x08000007,  // 0x14: j     0x8000001C
      0x00000000,  // 0x18: nop
      0x2404004B,  // 0x18: addiu $a0, $zero, 75
      0x03E00008,  // 0x1C: jr    $ra
      0x00000000,  // 0x20: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, Not(HasSubstr("||")));
}

TEST(ControlFlowPatternMatcherTest,
     MergesAndChainWhenBranchDelaySlotDoesNotOverwriteTestedRegister) {
  // Block 0 tests beq $a0, $zero, 0x18 with delay slot slti $v0, $a1, 10;
  // Block 1 tests beq $v0, $zero, 0x18 with nop delay slot.
  // Because Block 0's delay slot writes $v0 (not tested by Block 0's beq $a0, $zero),
  // Block 0 and Block 1 should merge into a short-circuit && condition.
  std::vector<uint32_t> words = {
      0x10800004,  // 0x00: beq   $a0, $zero, 0x18
      0x28A2000A,  // 0x04: slti  $v0, $a1, 10
      0x10400002,  // 0x08: beq   $v0, $zero, 0x18
      0x00000000,  // 0x0C: nop
      0x24020001,  // 0x10: addiu $v0, $zero, 1
      0x03E00008,  // 0x14: jr    $ra
      0x00000000,  // 0x18: nop
      0x03E00008,  // 0x18: jr    $ra
      0x00000000,  // 0x1C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("!block_0 && !block_1"));
}

TEST(ControlFlowPatternMatcherTest, CollapsesThreeWayShortCircuitAndChain) {
  // Block 0 (0x00): beq $a0, $zero, 0x20; nop
  // Block 1 (0x08): beq $a1, $zero, 0x20; nop
  // Block 2 (0x10): beq $a2, $zero, 0x20; nop
  // Block 3 (0x18): addiu $v0, $zero, 1; jr $ra; nop
  // Block 4 (0x20): jr $ra; nop
  std::vector<uint32_t> words = {
      0x10800007,  // 0x00: beq   $a0, $zero, 0x20
      0x00000000,  // 0x04: nop
      0x10A00005,  // 0x08: beq   $a1, $zero, 0x20
      0x00000000,  // 0x0C: nop
      0x10C00003,  // 0x10: beq   $a2, $zero, 0x20
      0x00000000,  // 0x14: nop
      0x24020001,  // 0x18: addiu $v0, $zero, 1
      0x03E00008,  // 0x1C: jr    $ra
      0x03E00008,  // 0x20: jr    $ra
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("if (!block_0 && !block_1 && !block_2) {\n  block_3\n}"));
}

TEST(ControlFlowPatternMatcherTest, CollapsesThreeWayShortCircuitOrChain) {
  // Block 0 (0x00): bne $a0, $zero, 0x18; nop
  // Block 1 (0x08): bne $a1, $zero, 0x18; nop
  // Block 2 (0x10): beq $a2, $zero, 0x20; nop
  // Block 3 (0x18): addiu $v0, $zero, 1; jr $ra
  // Block 4 (0x20): jr $ra; nop
  std::vector<uint32_t> words = {
      0x14800005,  // 0x00: bne   $a0, $zero, 0x18
      0x00000000,  // 0x04: nop
      0x14A00003,  // 0x08: bne   $a1, $zero, 0x18
      0x00000000,  // 0x0C: nop
      0x10C00003,  // 0x10: beq   $a2, $zero, 0x20
      0x00000000,  // 0x14: nop
      0x24020001,  // 0x18: addiu $v0, $zero, 1
      0x03E00008,  // 0x1C: jr    $ra
      0x03E00008,  // 0x20: jr    $ra
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("if (block_0 || block_1 || !block_2) {\n  block_3\n}"));
}

TEST(ControlFlowPatternMatcherTest, DetectsDecisionTreeSwitchInEntryBlockWithPrologueSaves) {
  // Entry block has 7 prologue/setup instructions (including sw stores) before a 3-case switch:
  std::vector<uint32_t> exact_words = {
      0x27BDFFE0,  // 0x00: addiu $sp, $sp, -32
      0xAFBF001C,  // 0x04: sw    $ra, 28($sp)
      0xAFB20018,  // 0x08: sw    $s2, 24($sp)
      0xAFB10014,  // 0x0C: sw    $s1, 20($sp)
      0xAFB00010,  // 0x10: sw    $s0, 16($sp)
      0x308400FF,  // 0x14: andi  $a0, $a0, 0xFF
      0x24010001,  // 0x18: addiu $at, $zero, 1
      0x10810009,  // 0x1C: beq   $a0, $at, +9 (0x44: Case 1)
      0x00000000,  // 0x20: nop
      0x24010002,  // 0x24: addiu $at, $zero, 2
      0x10810009,  // 0x28: beq   $a0, $at, +9 (0x50: Case 2)
      0x00000000,  // 0x2C: nop
      0x24010003,  // 0x30: addiu $at, $zero, 3
      0x10810009,  // 0x34: beq   $a0, $at, +9 (0x5C: Case 3)
      0x00000000,  // 0x38: nop
      0x0800001A,  // 0x3C: j     0x80000068 (Join)
      0x00000000,  // 0x40: nop
      0x2402000A,  // 0x44: addiu $v0, $zero, 10 (Case 1)
      0x0800001A,  // 0x48: j     0x80000068 (Join)
      0x00000000,  // 0x4C: nop
      0x24020014,  // 0x50: addiu $v0, $zero, 20 (Case 2)
      0x0800001A,  // 0x54: j     0x80000068 (Join)
      0x00000000,  // 0x58: nop
      0x2402001E,  // 0x5C: addiu $v0, $zero, 30 (Case 3)
      0x0800001A,  // 0x60: j     0x80000068 (Join)
      0x00000000,  // 0x64: nop
      0x8FBF001C,  // 0x68: lw    $ra, 28($sp)
      0x03E00008,  // 0x6C: jr    $ra
      0x27BD0020,  // 0x70: addiu $sp, $sp, 32
  };

  auto insts = *DecodeSequence(exact_words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("switch ($a0)"));
  EXPECT_THAT(s, HasSubstr("case 1:"));
  EXPECT_THAT(s, HasSubstr("case 2:"));
  EXPECT_THAT(s, HasSubstr("case 3:"));
}

TEST(ControlFlowPatternMatcherTest, DecisionTreeSwitchSixCasesAndImpliedIntervalLeaf) {
  // 1. 6-case sparse decision tree (cases 10, 20, 30, 40, 50, 60 -> previously rejected by
  // total_cases > 5):
  //   0x00: addiu $at, $zero, 10; beq $a0, $at, 0x50 (case 10); nop
  //   0x0C: addiu $at, $zero, 20; beq $a0, $at, 0x5C (case 20); nop
  //   0x18: addiu $at, $zero, 30; beq $a0, $at, 0x68 (case 30); nop
  //   0x24: addiu $at, $zero, 40; beq $a0, $at, 0x74 (case 40); nop
  //   0x30: addiu $at, $zero, 50; beq $a0, $at, 0x80 (case 50); nop
  //   0x3C: addiu $at, $zero, 60; beq $a0, $at, 0x8C (case 60); nop
  //   0x48: j 0x98 (default); nop
  std::vector<uint32_t> six_case_words = {
      0x2401000A,  // 0x00: addiu $at, $zero, 10
      0x10810012,  // 0x04: beq   $a0, $at, +18 -> 0x50
      0x00000000,  // 0x08: nop
      0x24010014,  // 0x0C: addiu $at, $zero, 20
      0x10810012,  // 0x10: beq   $a0, $at, +18 -> 0x5C
      0x00000000,  // 0x14: nop
      0x2401001E,  // 0x18: addiu $at, $zero, 30
      0x10810012,  // 0x1C: beq   $a0, $at, +18 -> 0x68
      0x00000000,  // 0x20: nop
      0x24010028,  // 0x24: addiu $at, $zero, 40
      0x10810012,  // 0x28: beq   $a0, $at, +18 -> 0x74
      0x00000000,  // 0x2C: nop
      0x24010032,  // 0x30: addiu $at, $zero, 50
      0x10810012,  // 0x34: beq   $a0, $at, +18 -> 0x80
      0x00000000,  // 0x38: nop
      0x2401003C,  // 0x3C: addiu $at, $zero, 60
      0x10810012,  // 0x40: beq   $a0, $at, +18 -> 0x8C
      0x00000000,  // 0x44: nop
      0x08000026,  // 0x48: j     0x80000098 (default)
      0x00000000,  // 0x4C: nop
      0x24020001,  // 0x50: addiu $v0, $zero, 1
      0x08000028,  // 0x54: j     0x800000A0 (join)
      0x00000000,  // 0x58: nop
      0x24020002,  // 0x5C: addiu $v0, $zero, 2
      0x08000028,  // 0x60: j     0x800000A0
      0x00000000,  // 0x64: nop
      0x24020003,  // 0x68: addiu $v0, $zero, 3
      0x08000028,  // 0x6C: j     0x800000A0
      0x00000000,  // 0x70: nop
      0x24020004,  // 0x74: addiu $v0, $zero, 4
      0x08000028,  // 0x78: j     0x800000A0
      0x00000000,  // 0x7C: nop
      0x24020005,  // 0x80: addiu $v0, $zero, 5
      0x08000028,  // 0x84: j     0x800000A0
      0x00000000,  // 0x88: nop
      0x24020006,  // 0x8C: addiu $v0, $zero, 6
      0x08000028,  // 0x90: j     0x800000A0
      0x00000000,  // 0x94: nop
      0x2402FFFF,  // 0x98: addiu $v0, $zero, -1 (default)
      0x00000000,  // 0x9C: nop
      0x03E00008,  // 0xA0: jr    $ra (join)
      0x00000000,  // 0xA4: nop
  };
  auto six_cfg = *ControlFlowGraph::Build(*DecodeSequence(six_case_words, 0x80000000));
  DominatorTree six_dt = DominatorTree::Compute(six_cfg);
  DominatorTree six_pdt = DominatorTree::ComputePostDominators(six_cfg);
  LoopInfo six_loops = LoopInfo::Analyze(six_cfg, six_dt);
  auto six_region = ControlFlowStructurer::Structure(six_cfg, six_dt, six_pdt, six_loops);
  ASSERT_THAT(six_region, NotNull());
  std::string six_s = six_region->ToString();
  EXPECT_THAT(six_s, HasSubstr("switch ($a0)"));
  EXPECT_THAT(six_s, HasSubstr("case 10:"));
  EXPECT_THAT(six_s, HasSubstr("case 60:"));

  // 2. Non-root bounded interval [3, 3] implied leaf case:
  std::vector<uint32_t> implied_words = {
      0x24020002,  // 0x00: addiu $v0, $zero, 2
      0x10820012,  // 0x04: beq   $a0, $v0, +18 -> 0x50 (case 2)
      0x28810003,  // 0x08: slti  $at, $a0, 3
      0x1420000A,  // 0x0C: bne   $at, $zero, +10 -> 0x38 (left subtree: < 2)
      0x00000000,  // 0x10: nop
      0x24020004,  // 0x14: addiu $v0, $zero, 4
      0x1082000F,  // 0x18: beq   $a0, $v0, +15 -> 0x58 (case 4)
      0x28810005,  // 0x1C: slti  $at, $a0, 5
      0x14200012,  // 0x20: bne   $at, $zero, +18 -> 0x6C (interval [3, 3] -> implied case 3!)
      0x24020005,  // 0x24: addiu $v0, $zero, 5
      0x1082000D,  // 0x28: beq   $a0, $v0, +13 -> 0x60 (case 5)
      0x00000000,  // 0x2C: nop
      0x0800001B,  // 0x30: j     0x8000006C (default/join)
      0x00000000,  // 0x34: nop
      0x24020001,  // 0x38: addiu $v0, $zero, 1
      0x10820003,  // 0x3C: beq   $a0, $v0, +3 -> 0x4C (case 1)
      0x00000000,  // 0x40: nop
      0x0800001B,  // 0x44: j     0x8000006C (default/join)
      0x00000000,  // 0x48: nop
      0x2402000A,  // 0x4C: addiu $v0, $zero, 10 (case 1 falls through to case 2)
      0x24020014,  // 0x50: addiu $v0, $zero, 20 (case 2)
      0x0800001B,  // 0x54: j     0x8000006C
      0x24020028,  // 0x58: addiu $v0, $zero, 40 (case 4)
      0x0800001B,  // 0x5C: j     0x8000006C
      0x24020032,  // 0x60: addiu $v0, $zero, 50 (case 5)
      0x0800001B,  // 0x64: j     0x8000006C
      0x00000000,  // 0x68: nop
      0x03E00008,  // 0x6C: jr    $ra (join/default)
      0x00000000,  // 0x70: nop
  };
  auto implied_cfg = *ControlFlowGraph::Build(*DecodeSequence(implied_words, 0x80000000));
  DominatorTree implied_dt = DominatorTree::Compute(implied_cfg);
  DominatorTree implied_pdt = DominatorTree::ComputePostDominators(implied_cfg);
  LoopInfo implied_loops = LoopInfo::Analyze(implied_cfg, implied_dt);
  auto implied_region =
      ControlFlowStructurer::Structure(implied_cfg, implied_dt, implied_pdt, implied_loops);
  ASSERT_THAT(implied_region, NotNull());
  std::string implied_s = implied_region->ToString();
  EXPECT_THAT(implied_s, HasSubstr("switch ($a0)"));
  EXPECT_THAT(implied_s, HasSubstr("case 1:"));
  EXPECT_THAT(implied_s, HasSubstr("case 2:"));
  EXPECT_THAT(implied_s, HasSubstr("case 3:"));
  EXPECT_THAT(implied_s, HasSubstr("case 4:"));
  EXPECT_THAT(implied_s, HasSubstr("case 5:"));
}

TEST(ControlFlowPatternMatcherTest, BranchLikelyEqualityNodeInDecisionTreeAnnulsDelayOnDefault) {
  // Root pivot checks $a0 == 201 (with non-likely delay slot $v1 = $a2), then splits on
  // slti $v0, $a0, 202; left leaf checks $a0 == 200 using `beql` with delay slot `move $t0, $t1`.
  // The `beql` delay slot must be attached to case 200 only and NOT to the default case.
  std::vector<uint32_t> words = {
      0x240200C9,  // 0x00: addiu $v0, $zero, 201
      0x1082000E,  // 0x04: beq   $a0, $v0, +14 -> 0x40 (case 201)
      0x00C01821,  // 0x08: addu  $v1, $a2, $zero
      0x288200CA,  // 0x0C: slti  $v0, $a0, 202
      0x10400005,  // 0x10: beq   $v0, $zero, +5 -> 0x28 (right subtree >= 202)
      0x240200C8,  // 0x14: addiu $v0, $zero, 200
      0x50820007,  // 0x18: beql  $a0, $v0, +7 -> 0x38 (case 200)
      0x01204021,  // 0x1C: addu  $t0, $t1, $zero (branch-likely delay slot!)
      0x08000014,  // 0x20: j     0x80000050 (default/join)
      0x00000000,  // 0x24: nop
      0x240200CA,  // 0x28: addiu $v0, $zero, 202
      0x10820006,  // 0x2C: beq   $a0, $v0, +6 -> 0x48 (case 202)
      0x00000000,  // 0x30: nop
      0x08000014,  // 0x34: j     0x80000050 (default/join)
      0x00000000,  // 0x38: nop (delay slot of 0x34 AND target of case 200 -> Wait, put case 200 at
                   // 0x3C!)
  };
  words = {
      0x240200C9,  // 0x00: addiu $v0, $zero, 201
      0x1082000E,  // 0x04: beq   $a0, $v0, +14 -> 0x40 (case 201)
      0x00C01821,  // 0x08: addu  $v1, $a2, $zero
      0x288200CA,  // 0x0C: slti  $v0, $a0, 202
      0x10400005,  // 0x10: beq   $v0, $zero, +5 -> 0x28 (right subtree >= 202)
      0x240200C8,  // 0x14: addiu $v0, $zero, 200
      0x50820009,  // 0x18: beql  $a0, $v0, +9 -> 0x40? No: +7 -> 0x38 (case 200)
      0x01204021,  // 0x1C: addu  $t0, $t1, $zero (branch-likely delay slot!)
      0x08000014,  // 0x20: j     0x80000050 (default/join)
      0x00000000,  // 0x24: nop
      0x240200CA,  // 0x28: addiu $v0, $zero, 202
      0x50820005,  // 0x2C: beql  $a0, $v0, +5 -> 0x44 (case 202)
      0x01204021,  // 0x30: addu  $t0, $t1, $zero
      0x08000014,  // 0x34: j     0x80000050 (default/join)
      0x00000000,  // 0x38: nop
      0x08000014,  // 0x3C: j     0x80000050 (case 200)
      0x00603821,  // 0x40: addu  $a3, $v1, $zero
      0x24A8FFFF,  // 0x44: addiu $t0, $a1, -1 (case 201)
      0x08000014,  // 0x48: j     0x80000050
      0x00603821,  // 0x4C: addu  $a3, $v1, $zero
      0x03E00008,  // 0x50: jr    $ra (join)
      0x00000000,  // 0x54: nop
  };
  // Fix exact branch offsets:
  // 0x04: beq $a0, $v0, to 0x44 -> (0x44 - 0x08)/4 = 15 (0x000F)
  words[1] = 0x1082000F;
  // 0x18: beql $a0, $v0, to 0x3C -> (0x3C - 0x1C)/4 = 8 (0x0008)
  words[6] = 0x50820008;
  // 0x2C: beql $a0, $v0, to 0x50 -> wait, let's give case 202 its own block at 0x50 and join at
  // 0x58:
  words = {
      0x240200C9,  // 0x00: addiu $v0, $zero, 201
      0x1082000F,  // 0x04: beq   $a0, $v0, +15 -> 0x44 (case 201)
      0x00C01821,  // 0x08: addu  $v1, $a2, $zero
      0x288200CA,  // 0x0C: slti  $v0, $a0, 202
      0x10400005,  // 0x10: beq   $v0, $zero, +5 -> 0x28 (right subtree >= 202)
      0x240200C8,  // 0x14: addiu $v0, $zero, 200
      0x50820008,  // 0x18: beql  $a0, $v0, +8 -> 0x3C (case 200)
      0x01204021,  // 0x1C: addu  $t0, $t1, $zero (branch-likely delay slot!)
      0x08000016,  // 0x20: j     0x80000058 (default/join)
      0x00000000,  // 0x24: nop
      0x240200CA,  // 0x28: addiu $v0, $zero, 202
      0x50820007,  // 0x2C: beql  $a0, $v0, +7 -> 0x4C (case 202)
      0x01204021,  // 0x30: addu  $t0, $t1, $zero
      0x08000016,  // 0x34: j     0x80000058 (default/join)
      0x00000000,  // 0x38: nop
      0x08000016,  // 0x3C: j     0x80000058 (case 200)
      0x00603821,  // 0x40: addu  $a3, $v1, $zero
      0x24A8FFFF,  // 0x44: addiu $t0, $a1, -1 (case 201)
      0x08000016,  // 0x48: j     0x80000058
      0x00603821,  // 0x4C: addu  $a3, $v1, $zero
      0x24C7FFFF,  // 0x50: addiu $a3, $a2, -1 (case 202)
      0x00000000,  // 0x54: nop
      0x03E00008,  // 0x58: jr    $ra (join)
      0x00000000,  // 0x5C: nop
  };
  // 0x2C: beql $a0, $v0, to 0x50 -> (0x50 - 0x30)/4 = 8 (0x0008)
  words[11] = 0x50820008;

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);
  absl::flat_hash_set<uint32_t> visited;

  auto sw = DetectDecisionTreeSwitch(cfg, &loops, visited, cfg.EntryBlockId());
  ASSERT_TRUE(sw.has_value());
  EXPECT_EQ(sw->selector_reg, Register::kA0);

  bool found_case_200 = false;
  bool found_default = false;
  for (const auto& dc : sw->cases) {
    if (!dc.case_values.empty() && dc.case_values[0] == 200) {
      found_case_200 = true;
      EXPECT_EQ(dc.delay_instructions.size(), 1u);
    }
    if (dc.is_default) {
      found_default = true;
      EXPECT_TRUE(dc.delay_instructions.empty());
    }
  }
  EXPECT_TRUE(found_case_200);
  EXPECT_TRUE(found_default);
}

}  // namespace
}  // namespace rom_nom_nom
