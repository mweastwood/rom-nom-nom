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

TEST(ControlFlowPatternMatcherTest,
     DetectsTwoCaseDecisionTreeSwitchWithLoadedSelectorAndDefaultTrampoline) {
  // Entry block loads selector $v1 from 0($a0) and branches on case 0, with sp adjustment in delay
  // slot; second decision block branches on case 2 with a stolen constant load ($v0 = 2) in delay
  // slot and falls through to a pure-jump trampoline block to the join block.
  std::vector<uint32_t> words = {
      0x8C830000,  // 0x00: lw    $v1, 0($a0)
      0x10600005,  // 0x04: beq   $v1, $zero, +5 -> 0x1C (case 0)
      0x27BDFFE8,  // 0x08: addiu $sp, $sp, -24 (prologue sp adjust in delay slot)
      0x24020002,  // 0x0C: addiu $v0, $zero, 2
      0x10620004,  // 0x10: beq   $v1, $v0, +4 -> 0x24 (case 2)
      0x24020002,  // 0x14: addiu $v0, $zero, 2 (stolen from case 2)
      0x0800000B,  // 0x18: j     0x8000002C (join trampoline)
      0x00000000,  // 0x1C: nop -> wait: 0x1C is case 0! Let's adjust offsets:
  };
  words = {
      0x8C830000,  // 0x00: lw    $v1, 0($a0)
      0x10600006,  // 0x04: beq   $v1, $zero, +6 -> 0x20 (case 0)
      0x27BDFFE8,  // 0x08: addiu $sp, $sp, -24
      0x24020002,  // 0x0C: addiu $v0, $zero, 2
      0x10620005,  // 0x10: beq   $v1, $v0, +5 -> 0x28 (case 2)
      0x24020002,  // 0x14: addiu $v0, $zero, 2
      0x0800000C,  // 0x18: j     0x80000030 (join)
      0x00000000,  // 0x1C: nop
      0xAC800004,  // 0x20: sw    $zero, 4($a0) (case 0)
      0x0800000C,  // 0x24: j     0x80000030 (join)
      0x00000000,  // 0x28: nop -> wait: let's place case 2 at 0x2C
  };
  words = {
      0x8C830000,  // 0x00: lw    $v1, 0($a0)
      0x10600006,  // 0x04: beq   $v1, $zero, +6 -> 0x20 (case 0)
      0x27BDFFE8,  // 0x08: addiu $sp, $sp, -24
      0x24020002,  // 0x0C: addiu $v0, $zero, 2
      0x10620006,  // 0x10: beq   $v1, $v0, +6 -> 0x2C (case 2)
      0x24020002,  // 0x14: addiu $v0, $zero, 2
      0x0800000D,  // 0x18: j     0x80000034 (join)
      0x00000000,  // 0x1C: nop
      0xAC800004,  // 0x20: sw    $zero, 4($a0) (case 0)
      0x0800000D,  // 0x24: j     0x80000034 (join)
      0x00000000,  // 0x28: nop
      0xAC820004,  // 0x2C: sw    $v0, 4($a0) (case 2 uses $v0)
      0x00000000,  // 0x30: nop
      0x03E00008,  // 0x34: jr    $ra (join)
      0x27BD0018,  // 0x38: addiu $sp, $sp, 24
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);
  absl::flat_hash_set<uint32_t> visited;

  auto sw = DetectDecisionTreeSwitch(cfg, &loops, visited, cfg.EntryBlockId());
  ASSERT_TRUE(sw.has_value());
  EXPECT_EQ(sw->selector_reg, Register::kV1);
  EXPECT_EQ(sw->decision_blocks.size(), 3u);

  for (const auto& dc : sw->cases) {
    if (dc.is_default) {
      EXPECT_FALSE(dc.const_reg.has_value());
      EXPECT_TRUE(dc.delay_instructions.empty());
    }
  }
}

TEST(ControlFlowPatternMatcherTest,
     DoesNotCollapseSameRegisterRangeComparisonsIntoShortCircuitAnd) {
  // Block 0 (0x00): slti $v0, $a0, 2; beq $v0, $zero, 0x1C; nop
  // Block 1 (0x0C): bltz $a0, 0x1C; nop
  // Block 2 (0x14): addiu $v0, $zero, 1; jr $ra
  // Block 3 (0x1C): jr $ra; nop
  // Because both blocks compare $a0, collapsing into (a0 < 2) && (a0 >= 0) would cause GCC 2.7.2's
  // fold_range_test to merge them into a single unsigned sltiu branch.
  std::vector<uint32_t> words = {
      0x28820002,  // 0x00: slti  $v0, $a0, 2
      0x10400005,  // 0x04: beq   $v0, $zero, +5 -> 0x1C
      0x00000000,  // 0x08: nop
      0x04800003,  // 0x0C: bltz  $a0, +3 -> 0x1C
      0x00000000,  // 0x10: nop
      0x24020001,  // 0x14: addiu $v0, $zero, 1
      0x03E00008,  // 0x18: jr    $ra
      0x03E00008,  // 0x1C: jr    $ra
      0x00000000,  // 0x20: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  EXPECT_THAT(s, Not(HasSubstr("&&")));
}

TEST(ControlFlowPatternMatcherTest,
     FloatToUintConversionDiamondWithDelaySlotAndiMaskAndEarlierComparison) {
  // Single-precision 0x4F000000 float-to-uint conversion diamond where:
  // 1. c.le.s $f2, $f12 appears before an intervening instruction in the entry block.
  // 2. Both the low path's jump delay slot and the end of the high path contain a stolen
  //    join-block mask instruction `andi $a1, $v0, 0xFF` (for `(u8)(u32)f`).
  std::vector<uint32_t> words = {
      0x3C014F00,  // 0x00: lui     $at, 0x4F00
      0x44811000,  // 0x04: mtc1    $at, $f2
      0x460C103E,  // 0x08: c.le.s  $f2, $f12
      0x24040001,  // 0x0C: addiu   $a0, $zero, 1 (intervening instruction after c.le.s)
      0x45010004,  // 0x10: bc1t    +4 -> 0x24 (high path)
      0x00000000,  // 0x14: nop
      0x4600600D,  // 0x18: trunc.w.s $f0, $f12 (low path)
      0x44020000,  // 0x1C: mfc1    $v0, $f0
      0x0800000F,  // 0x20: j       0x8000003C (join)
      0x304500FF,  // 0x24: andi    $a1, $v0, 0xFF (Wait: 0x24 is delay slot of j, so high path is
                   // 0x28!)
  };
  words = {
      0x3C014F00,  // 0x00: lui       $at, 0x4F00
      0x44811000,  // 0x04: mtc1      $at, $f2
      0x460C103E,  // 0x08: c.le.s    $f2, $f12
      0x24040001,  // 0x0C: addiu     $a0, $zero, 1
      0x45010005,  // 0x10: bc1t      +5 -> 0x28 (high path)
      0x00000000,  // 0x14: nop
      0x4600600D,  // 0x18: trunc.w.s $f0, $f12 (low path)
      0x44020000,  // 0x1C: mfc1      $v0, $f0
      0x08000010,  // 0x20: j         0x80000040 (join)
      0x304500FF,  // 0x24: andi      $a1, $v0, 0xFF (delay slot of j)
      0x46026101,  // 0x28: sub.s     $f4, $f12, $f2 (high path)
      0x4600200D,  // 0x2C: trunc.w.s $f0, $f4
      0x44020000,  // 0x30: mfc1      $v0, $f0
      0x3C018000,  // 0x34: lui       $at, 0x8000
      0x00411025,  // 0x38: or        $v0, $v0, $at
      0x304500FF,  // 0x3C: andi      $a1, $v0, 0xFF (trailing instruction at end of high path)
      0x03E00008,  // 0x40: jr        $ra (join)
      0x00000000,  // 0x44: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  const auto* b_entry = cfg.GetBlock(cfg.EntryBlockId());
  ASSERT_THAT(b_entry, NotNull());
  uint32_t succ_taken = 0;
  uint32_t succ_fallthrough = 0;
  for (const auto& edge : b_entry->outgoing_edges) {
    if (edge.type == EdgeType::kBranch) {
      succ_taken = edge.to_block_id;
    } else {
      succ_fallthrough = edge.to_block_id;
    }
  }
  ASSERT_THAT(cfg.GetBlock(succ_taken), NotNull());
  ASSERT_EQ(cfg.GetBlock(succ_taken)->successors.size(), 1u);
  uint32_t join_id = cfg.GetBlock(succ_taken)->successors[0];

  auto f2u =
      DetectFloatToUintDiamond(cfg, cfg.EntryBlockId(), succ_taken, succ_fallthrough, join_id);
  ASSERT_TRUE(f2u.has_value());
  EXPECT_EQ(f2u->destination_integer_register, Register::kV0);
  EXPECT_EQ(f2u->source_float_register, FpRegister::kF12);
  EXPECT_FALSE(f2u->is_double);
  ASSERT_TRUE(f2u->trailing_instruction.has_value());
  EXPECT_EQ(f2u->trailing_instruction->opcode, Opcode::kAndi);
  EXPECT_EQ(f2u->trailing_instruction->rt, Register::kA1);
  EXPECT_EQ(f2u->trailing_instruction->rs, Register::kV0);
}

TEST(ControlFlowPatternMatcherTest,
     FindSwitchJoinBlockEliminatesSubsetSharedTailInFavorOfBroaderReturnJoin) {
  // 2-case decision tree where Case 1 (0x20) jumps to Case 2's shared tail block (0x2C),
  // which in turn falls through to the post-switch return join block (0x34), while Default
  // jumps directly to the return join block (0x34).
  // Even though all paths reach the return block (0x34), FindSwitchJoinBlock must eliminate
  // the intermediate shared tail (0x2C, reached by 2 targets) in favor of the broader
  // post-switch join block (0x34, reached by all 3 targets).
  std::vector<uint32_t> words = {
      0x24010001,  // 0x00: addiu $at, $zero, 1
      0x10810006,  // 0x04: beq   $a0, $at, +6 -> 0x20 (case 1)
      0x00000000,  // 0x08: nop
      0x24010002,  // 0x0C: addiu $at, $zero, 2
      0x10810005,  // 0x10: beq   $a0, $at, +5 -> 0x28 (case 2)
      0x00000000,  // 0x14: nop
      0x0800000D,  // 0x18: j     0x80000034 (default -> post-switch join)
      0x00000000,  // 0x1C: nop
      0x2404000A,  // 0x20: addiu $a0, $zero, 10 (case 1)
      0x0800000B,  // 0x24: j     0x8000002C (jumps to shared tail)
      0x24040014,  // 0x28: addiu $a0, $zero, 20 (delay slot / case 2) -> wait: put nop at 0x28!
  };
  words = {
      0x24010001,  // 0x00: addiu $at, $zero, 1
      0x10810006,  // 0x04: beq   $a0, $at, +6 -> 0x20 (case 1)
      0x00000000,  // 0x08: nop
      0x24010002,  // 0x0C: addiu $at, $zero, 2
      0x10810006,  // 0x10: beq   $a0, $at, +6 -> 0x2C (case 2)
      0x00000000,  // 0x14: nop
      0x0800000E,  // 0x18: j     0x80000038 (default -> post-switch join)
      0x00000000,  // 0x1C: nop
      0x2404000A,  // 0x20: addiu $a0, $zero, 10 (case 1)
      0x0800000C,  // 0x24: j     0x80000030 (jumps to shared tail)
      0x00000000,  // 0x28: nop
      0x24040014,  // 0x2C: addiu $a0, $zero, 20 (case 2 falls through to 0x30)
      0x0C000400,  // 0x30: jal   0x80001000 (shared tail for cases 1 and 2)
      0x00000000,  // 0x34: nop
      0x03E00008,  // 0x38: jr    $ra (post-switch join reached by cases 1, 2, and default)
      0x00000000,  // 0x3C: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);
  absl::flat_hash_set<uint32_t> visited;

  auto sw = DetectDecisionTreeSwitch(cfg, &loops, visited, cfg.EntryBlockId());
  ASSERT_TRUE(sw.has_value());
  auto join_id = FindSwitchJoinBlock(cfg, &post_dt, visited, sw->decision_blocks,
                                     sw->target_to_cases, sw->default_target_block, std::nullopt);
  ASSERT_TRUE(join_id.has_value());
  const auto* join_blk = cfg.GetBlock(*join_id);
  ASSERT_THAT(join_blk, NotNull());
  EXPECT_EQ(join_blk->start_vram, 0x80000038u);
}

TEST(ControlFlowPatternMatcherTest, CollapsesShortCircuitOrWithBitmaskComparisonConditionBlock) {
  // Block 0 (0x00):
  //   lhu   $v0, 0($a0)
  //   andi  $v0, $v0, 0x0004
  //   bnez  $v0, 0x1C (then_body)
  //   nop
  // Block 1 (0x10 - multi-instruction condition block with load + andi into dead $v0):
  //   lhu   $v0, 2($a0)
  //   andi  $v0, $v0, 0x0008
  //   beqz  $v0, 0x24 (join)
  //   nop
  // Block 2 (0x20 - then_body):
  //   sw    $zero, 4($a0)
  // Block 3 (0x24 - join):
  //   jr    $ra
  //   nop
  std::vector<uint32_t> words = {
      0x94820000,  // 0x00: lhu   $v0, 0($a0)
      0x30420004,  // 0x04: andi  $v0, $v0, 4
      0x14400005,  // 0x08: bnez  $v0, +5 -> 0x20 (then_body)
      0x00000000,  // 0x0C: nop
      0x94820002,  // 0x10: lhu   $v0, 2($a0)
      0x30420008,  // 0x14: andi  $v0, $v0, 8
      0x10400002,  // 0x18: beqz  $v0, +2 -> 0x24 (join)
      0x00000000,  // 0x1C: nop
      0xAC800004,  // 0x20: sw    $zero, 4($a0)
      0x03E00008,  // 0x24: jr    $ra
      0x00000000,  // 0x28: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("||"));
  EXPECT_THAT(s, Not(HasSubstr("GOTO")));
}

TEST(ControlFlowPatternMatcherTest,
     DetectsDecisionTreeSwitchWith32BitLuiAddiuConstantsAndZeroPivotSplit) {
  // Decision tree testing $a0 against 32-bit constants (0x12345, -100, 100) using a zero-pivot
  // `bltz $a0` split.
  std::vector<uint32_t> words = {
      0x3C010001,  // 0x00: lui   $at, 0x0001
      0x24212345,  // 0x04: addiu $at, $at, 0x2345
      0x1081000D,  // 0x08: beq   $a0, $at, +13 -> 0x40 (case 0x12345)
      0x00000000,  // 0x0C: nop
      0x04800006,  // 0x10: bltz  $a0, +6 -> 0x2C (< 0 branch)
      0x00000000,  // 0x14: nop
      0x24010064,  // 0x18: addiu $at, $zero, 100
      0x1081000B,  // 0x1C: beq   $a0, $at, +11 -> 0x4C (case 100)
      0x00000000,  // 0x20: nop
      0x08000019,  // 0x24: j     0x80000064 (default)
      0x00000000,  // 0x28: nop
      0x2401FF9C,  // 0x2C: addiu $at, $zero, -100
      0x10810009,  // 0x30: beq   $a0, $at, +9 -> 0x58 (case -100)
      0x00000000,  // 0x34: nop
      0x08000019,  // 0x38: j     0x80000064 (default)
      0x00000000,  // 0x3C: nop
      0x24020001,  // 0x40: addiu $v0, $zero, 1
      0x03E00008,  // 0x44: jr    $ra
      0x00000000,  // 0x48: nop
      0x24020002,  // 0x4C: addiu $v0, $zero, 2
      0x03E00008,  // 0x50: jr    $ra
      0x00000000,  // 0x54: nop
      0x24020003,  // 0x58: addiu $v0, $zero, 3
      0x03E00008,  // 0x5C: jr    $ra
      0x00000000,  // 0x60: nop
      0x24020000,  // 0x64: addiu $v0, $zero, 0
      0x03E00008,  // 0x68: jr    $ra
      0x00000000,  // 0x6C: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);
  absl::flat_hash_set<uint32_t> visited;

  auto sw = DetectDecisionTreeSwitch(cfg, &loops, visited, cfg.EntryBlockId());
  ASSERT_TRUE(sw.has_value());
  EXPECT_EQ(sw->selector_reg, Register::kA0);
  EXPECT_EQ(sw->target_to_cases.size(), 3u);
}

TEST(ControlFlowPatternMatcherTest, CollapsesShortCircuitIfElseWithShiftAddIndexedConditionBlock) {
  // Block 0 (0x00):
  //   beqz  $a1, 0x2C (then_body)
  //   nop
  // Block 1 (0x08 - extended condition block with * 3 shift-add and non-empty else):
  //   lbu   $v0, 4($a0)
  //   sll   $v1, $v0, 1
  //   addu  $v0, $v1, $v0
  //   addu  $v0, $a2, $v0
  //   lbu   $v0, 0($v0)
  //   andi  $v0, $v0, 1
  //   bnez  $v0, 0x38 (else_body)
  //   nop
  // Block 2 (0x28 -> 0x28 then_body):
  //   addiu $v0, $zero, 10
  //   j     0x3C (join)
  //   nop
  // Block 3 (0x34 -> else_body):
  //   addiu $v0, $zero, 20
  // Block 4 (0x38 -> join):
  //   jr    $ra
  //   nop
  std::vector<uint32_t> words = {
      0x10A00008,  // 0x00: beqz  $a1, +8 -> 0x24 (then_body)
      0x00000000,  // 0x04: nop
      0x90820004,  // 0x08: lbu   $v0, 4($a0)
      0x00021840,  // 0x0C: sll   $v1, $v0, 1
      0x00621021,  // 0x10: addu  $v0, $v1, $v0
      0x00C21021,  // 0x14: addu  $v0, $a2, $v0
      0x90420000,  // 0x18: lbu   $v0, 0($v0)
      0x30420001,  // 0x1C: andi  $v0, $v0, 1
      0x14400004,  // 0x20: bnez  $v0, +4 -> 0x34 (else_body) -- Wait: (0x30 - 0x24)/4 = 3
      0x00000000,  // 0x24: nop
      0x2402000A,  // 0x28: addiu $v0, $zero, 10 (then_body)
      0x0800000E,  // 0x2C: j     0x80000038 (join)
      0x00000000,  // 0x30: nop
      0x24020014,  // 0x34: addiu $v0, $zero, 20 (else_body)
      0x03E00008,  // 0x38: jr    $ra (join)
      0x00000000,  // 0x3C: nop
  };
  words[0] = 0x10A00009;  // 0x00: beqz $a1, +9 -> 0x28
  words[8] = 0x14400004;  // 0x20: bnez $v0, +4 -> 0x34

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("||"));
  EXPECT_THAT(s, Not(HasSubstr("goto")));
}

TEST(ControlFlowPatternMatcherTest,
     DetectsDecisionTreeSwitchWhenDelaySlotOverwritesSplitRegAndTrampolineIsUnscheduled) {
  // Models GCC 2.7.2 decision-tree switch where:
  // 1) Block 0 tests `beq $v1, $v0` (case 12288) and sets range split `slti $v0, $v1, 12289` in its
  //    delay slot.
  // 2) Block 1 tests `beqz $v0, right_tree` and overwrites `$v0` with `addiu $v0, $zero, 1` in its
  //    delay slot (replacing the split in `$v0` with constant 1).
  // 3) Block 2 tests `beq $v1, $v0` (case 1) and falls through to Block 3, a 3-instruction
  //    unscheduled default jump trampoline (`addu $v0, $zero, $zero; j default; nop`).
  // 4) Right tree (Block 4) tests `beq $v1, $v0` (case 12290) with `slti $v0, $v1, 12290` in its
  //    delay slot, then Block 5 branches on `bnez $v0` to doubly-bounded implied `case 12289`
  //    before Block 6 tests `case 12291` and jumps to `default`.
  std::vector<uint32_t> words = {
      0x24023000,  // 0x00 (block 0): addiu $v0, $zero, 12288
      0x10620010,  // 0x04:           beq   $v1, $v0, +16 -> 0x48 (case 12288)
      0x28623001,  // 0x08:           slti  $v0, $v1, 12289
      0x10400005,  // 0x0C (block 1): beqz  $v0, +5 -> 0x24 (right tree)
      0x24020001,  // 0x10:           addiu $v0, $zero, 1
      0x1062000A,  // 0x14 (block 2): beq   $v1, $v0, +10 -> 0x40 (case 1)
      0x00000000,  // 0x18:           nop
      0x00001021,  // 0x1C (block 3): addu  $v0, $zero, $zero (3-inst default trampoline)
      0x0800001A,  // 0x20:           j     0x80000068 (default)
      0x00000000,  // 0x24:           nop
      0x24023002,  // 0x28 (block 4): addiu $v0, $zero, 12290
      0x1062000A,  // 0x2C:           beq   $v1, $v0, +10 -> 0x58 (case 12290)
      0x28623002,  // 0x30:           slti  $v0, $v1, 12290
      0x14400006,  // 0x34 (block 5): bnez  $v0, +6 -> 0x50 (implied case 12289)
      0x24023003,  // 0x38:           addiu $v0, $zero, 12291
      0x10620008,  // 0x3C (block 6): beq   $v1, $v0, +8 -> 0x60 (case 12291)
      0x00000000,  // 0x40:           nop
      0x0800001A,  // 0x44 (block 7): j     0x80000068 (default)
      0x00001021,  // 0x48:           addu  $v0, $zero, $zero
      0x2402000A,  // 0x4C (case 1):     addiu $v0, $zero, 10
      0x03E00008,  // 0x50:              jr    $ra
      0x00000000,  // 0x54:              nop
      0x24020014,  // 0x58 (case 12288): addiu $v0, $zero, 20
      0x03E00008,  // 0x5C:              jr    $ra
      0x00000000,  // 0x60:              nop
      0x2402001E,  // 0x64 (case 12289): addiu $v0, $zero, 30
      0x03E00008,  // 0x68:              jr    $ra
      0x00000000,  // 0x6C:              nop
      0x24020028,  // 0x70 (case 12290): addiu $v0, $zero, 40
      0x03E00008,  // 0x74:              jr    $ra
      0x00000000,  // 0x78:              nop
      0x24020032,  // 0x7C (case 12291): addiu $v0, $zero, 50
      0x03E00008,  // 0x80:              jr    $ra
      0x00000000,  // 0x84:              nop
      0x03E00008,  // 0x88 (default):    jr    $ra
      0x00000000,  // 0x8C:              nop
  };
  // Fix branch offsets for the case target addresses above:
  // 0x04 -> 0x58: (0x58 - 0x08)/4 = 20 = 0x14
  words[1] = 0x10620014;
  // 0x0C -> 0x28: (0x28 - 0x10)/4 = 6
  words[3] = 0x10400006;
  // 0x14 -> 0x4C: (0x4C - 0x18)/4 = 13 = 0x0D
  words[5] = 0x1062000D;
  // 0x20 -> 0x88: 0x80000088 >> 2 = 0x02000022 -> j 0x08000022
  words[8] = 0x08000022;
  // 0x2C -> 0x70: (0x70 - 0x30)/4 = 16 = 0x10
  words[11] = 0x10620010;
  // 0x34 -> 0x64: (0x64 - 0x38)/4 = 11 = 0x0B
  words[13] = 0x1440000B;
  // 0x3C -> 0x7C: (0x7C - 0x40)/4 = 15 = 0x0F
  words[15] = 0x1062000F;
  // 0x44 -> 0x88: j 0x08000022
  words[17] = 0x08000022;

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);
  absl::flat_hash_set<uint32_t> visited;

  auto sw = DetectDecisionTreeSwitch(cfg, &loops, visited, cfg.EntryBlockId());
  ASSERT_TRUE(sw.has_value());
  EXPECT_EQ(sw->selector_reg, Register::kV1);
  EXPECT_EQ(sw->target_to_cases.size(), 5u);
  ASSERT_TRUE(sw->default_target_block.has_value());
}

TEST(ControlFlowPatternMatcherTest,
     CollapsesShortCircuitChainWithFpuConditionBlockAndNonConflictingEntryDelaySlot) {
  // Models `func_8003C318`:
  // Block 0 (0x00 - entry):
  //   lwc1   $f0, 0($a0)
  //   c.lt.s $f0, $f12
  //   nop
  //   bc1f   0x28 (else_body)
  //   andi   $v1, $a1, 0xFFFF   <-- non-conflicting delay slot
  // Block 1 (0x14 - FPU extended condition block):
  //   lwc1   $f0, 4($a0)
  //   c.lt.s $f0, $f12
  //   nop
  //   bc1f   0x28 (else_body)
  //   nop
  // Block 2 (0x24 - then_body):
  //   j      0x30 (join)
  //   sw     $zero, 8($a0)
  // Block 3 (0x2C - else_body):
  //   sw     $v1, 8($a0)
  // Block 4 (0x30 - join):
  //   jr     $ra
  //   nop
  std::vector<uint32_t> words = {
      0xC4800000,  // 0x00: lwc1   $f0, 0($a0)
      0x460C003C,  // 0x04: c.lt.s $f0, $f12
      0x00000000,  // 0x08: nop
      0x45000007,  // 0x0C: bc1f   +7 -> 0x2C (else_body)
      0x30A3FFFF,  // 0x10: andi   $v1, $a1, 0xFFFF
      0xC4800004,  // 0x14: lwc1   $f0, 4($a0)
      0x460C003C,  // 0x18: c.lt.s $f0, $f12
      0x00000000,  // 0x1C: nop
      0x45000002,  // 0x20: bc1f   +2 -> 0x2C (else_body)
      0x00000000,  // 0x24: nop
      0x0800000D,  // 0x28: j      0x80000034 (join)
      0xAC800008,  // 0x2C: sw     $zero, 8($a0)
      0xAC830008,  // 0x30: sw     $v1, 8($a0) (else_body)
      0x03E00008,  // 0x34: jr     $ra (join)
      0x00000000,  // 0x38: nop
  };
  // Fix branch targets:
  // 0x0C -> 0x30: (0x30 - 0x10)/4 = 8
  words[3] = 0x45000008;
  // 0x20 -> 0x30: (0x30 - 0x24)/4 = 3
  words[8] = 0x45000003;

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("&&"));
  EXPECT_THAT(s, Not(HasSubstr("GOTO")));
}

TEST(ControlFlowPatternMatcherTest, DetectsDecisionTreeSwitchWithBeqlAnnulledStoreDelaySlot) {
  // Models `func_80101D20`:
  // 0x00: addiu $v0, $zero, 4
  // 0x04: beq   $a1, $v0, 0x30 (case 4)
  // 0x08: slti  $v0, $a1, 5
  // 0x0C: beqz  $v0, 0x38 (split >= 5)
  // 0x10: addiu $v0, $zero, 1
  // 0x14: beql  $a1, $v0, 0x48 (case 1 -> join, with annulled store in delay slot!)
  // 0x18: sw    $a2, 0($a0)
  // 0x1C: j     0x80000048 (default -> join)
  // 0x20: nop
  // 0x24: ...
  std::vector<uint32_t> words = {
      0x24020004,  // 0x00: addiu $v0, $zero, 4
      0x10A20007,  // 0x04: beq   $a1, $v0, +7 -> 0x24 (case 4)
      0x28A20005,  // 0x08: slti  $v0, $a1, 5
      0x10400007,  // 0x0C: beqz  $v0, +7 -> 0x2C (split >= 5)
      0x24020001,  // 0x10: addiu $v0, $zero, 1
      0x50A20008,  // 0x14: beql  $a1, $v0, +8 -> 0x38 (case 1 -> join)
      0xAC860000,  // 0x18: sw    $a2, 0($a0) (annulled store delay slot)
      0x0800000E,  // 0x1C: j     0x80000038 (default -> join)
      0x00000000,  // 0x20: nop
      0x0800000E,  // 0x24: j     0x80000038 (case 4 -> join)
      0xAC860004,  // 0x28: sw    $a2, 4($a0)
      0x24020008,  // 0x2C: addiu $v0, $zero, 8
      0x50A20001,  // 0x30: beql  $a1, $v0, +1 -> 0x38 (case 8 -> join)
      0xAC860008,  // 0x34: sw    $a2, 8($a0)
      0x03E00008,  // 0x38: jr    $ra (join)
      0x00000000,  // 0x3C: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);
  absl::flat_hash_set<uint32_t> visited;

  auto sw = DetectDecisionTreeSwitch(cfg, &loops, visited, cfg.EntryBlockId());
  ASSERT_TRUE(sw.has_value());
  EXPECT_EQ(sw->selector_reg, Register::kA1);

  bool found_case_1_store = false;
  for (const auto& dc : sw->cases) {
    if (dc.case_values.size() == 1 && dc.case_values[0] == 1) {
      ASSERT_EQ(dc.delay_instructions.size(), 1u);
      EXPECT_EQ(dc.delay_instructions[0].opcode, Opcode::kSw);
      found_case_1_store = true;
    }
  }
  EXPECT_TRUE(found_case_1_store);
}

}  // namespace
}  // namespace rom_nom_nom
