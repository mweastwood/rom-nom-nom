#include "lifter/control_flow_structurer.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;
using ::testing::SizeIs;

TEST(ControlFlowStructurerTest, LinearSequence) {
  // Linear 3-block function: Block 0 -> Block 1 -> Block 2
  // Using unconditional jumps (j)
  std::vector<uint32_t> words = {
      0x08000002,  // 0x00: j 0x80000008 (target index 2)
      0x00000000,  // 0x04: nop
      0x08000004,  // 0x08: j 0x80000010 (target index 4)
      0x00000000,  // 0x0C: nop
      0x03E00008,  // 0x10: jr $ra
      0x00000000,  // 0x14: nop
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

  std::string s = region->ToString();
  EXPECT_EQ(s, "block_0\nblock_1\nblock_2\n");
}

TEST(ControlFlowStructurerTest, IfThen) {
  // If-Then CFG:
  // Block 0:
  // 0x00: beq $a0, $zero, +3 (skip Then to Merge at 0x10)
  // 0x04: nop
  // Block 1 (Then):
  // 0x08: addiu $a0, $a0, 1
  // 0x0C: nop (fallthrough to Merge at 0x10)
  // Block 2 (Merge):
  // 0x10: jr $ra
  // 0x14: nop
  std::vector<uint32_t> words = {
      0x10800003,  // 0x00: beq $a0, $zero, +3 (target 0x10)
      0x00000000,  // 0x04: nop
      0x24840001,  // 0x08: addiu $a0, $a0, 1 (Then body)
      0x00000000,  // 0x0C: nop
      0x03E00008,  // 0x10: jr $ra (Merge)
      0x00000000,  // 0x14: nop
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
  EXPECT_THAT(s, HasSubstr("if ("));
  EXPECT_THAT(s, HasSubstr("block_1"));
  EXPECT_THAT(s, HasSubstr("block_2"));
}

TEST(ControlFlowStructurerTest, IfThenElse) {
  // Diamond CFG:
  // Block 0:
  // 0x00: bne $a0, $zero, +3 (to Else at 0x10)
  // 0x04: nop
  // Block 1 (Then):
  // 0x08: j 0x80000018 (Merge at 0x18)
  // 0x0C: nop
  // Block 2 (Else):
  // 0x10: j 0x80000018 (Merge at 0x18)
  // 0x14: nop
  // Block 3 (Merge):
  // 0x18: jr $ra
  // 0x1C: nop
  std::vector<uint32_t> words = {
      0x14800003,  // 0x00: bne $a0, $zero, +3 (target 0x10)
      0x00000000,  // 0x04: nop
      0x08000006,  // 0x08: j 0x80000018 (target index 6)
      0x00000000,  // 0x0C: nop
      0x08000006,  // 0x10: j 0x80000018 (target index 6)
      0x00000000,  // 0x14: nop
      0x03E00008,  // 0x18: jr $ra
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
  EXPECT_THAT(s, HasSubstr("if (!block_0) {"));
  EXPECT_THAT(s, HasSubstr("} else {"));
  EXPECT_THAT(s, HasSubstr("block_3"));
}

TEST(ControlFlowStructurerTest, WhileLoop) {
  // While loop:
  // Block 0 (Entry):
  // 0x00: beq $zero, $zero, +1 (to Header at 0x08)
  // 0x04: nop
  // Block 1 (Header):
  // 0x08: bne $a0, $zero, +3 (Exit to 0x18)
  // 0x0C: nop
  // Block 2 (Body / Latch):
  // 0x10: j 0x80000008 (Back-edge to Header at 0x08)
  // 0x14: nop
  // Block 3 (Exit):
  // 0x18: jr $ra
  // 0x1C: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x14800003,  // 0x08: bne $a0, $zero, +3 (0x18)
      0x00000000,  // 0x0C: nop
      0x08000002,  // 0x10: j 0x80000008 (index 2)
      0x00000000,  // 0x14: nop
      0x03E00008,  // 0x18: jr $ra
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
  EXPECT_THAT(s, HasSubstr("block_0"));
  EXPECT_THAT(s, HasSubstr("while (block_1) {"));
  EXPECT_THAT(s, HasSubstr("block_2"));
  EXPECT_THAT(s, HasSubstr("block_3"));
}

TEST(ControlFlowStructurerTest, DoWhileLoop) {
  // Single block do-while loop:
  // Block 0 (Entry):
  // 0x00: beq $zero, $zero, +1 (to Header at 0x08)
  // 0x04: nop
  // Block 1 (Header & Latch):
  // 0x08: addiu $a0, $a0, -1
  // 0x0C: nop
  // 0x10: bne $a0, $zero, -3 (target 0x08)
  // 0x14: nop (fallthrough to Exit at 0x18)
  // Block 2 (Exit):
  // 0x18: jr $ra
  // 0x1C: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x2484FFFF,  // 0x08: addiu $a0, $a0, -1
      0x00000000,  // 0x0C: nop
      0x1480FFFD,  // 0x10: bne $a0, $zero, -3 (target 0x08)
      0x00000000,  // 0x14: nop
      0x03E00008,  // 0x18: jr $ra
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
  EXPECT_THAT(s, HasSubstr("block_0"));
  EXPECT_THAT(s, HasSubstr("do {"));
  EXPECT_THAT(s, HasSubstr("block_1"));
  EXPECT_THAT(s, HasSubstr("} while (block_1);"));
  EXPECT_THAT(s, HasSubstr("block_2"));
}

TEST(ControlFlowStructurerTest, MultiBlockDoWhileLoop) {
  // Multi-block do-while loop:
  // Block 0 (Entry):
  // 0x00: beq $zero, $zero, +1 (to Header at 0x08)
  // 0x04: nop
  // Block 1 (Header):
  // 0x08: addiu $v0, $v0, 1
  // 0x0C: nop (fallthrough to Latch at 0x10)
  // Block 2 (Latch):
  // 0x10: addiu $a0, $a0, -1
  // 0x14: bne $a0, $zero, -4 (target Header at 0x08)
  // 0x18: nop (delay slot, fallthrough to Exit at 0x1C)
  // Block 3 (Exit):
  // 0x1C: jr $ra
  // 0x20: nop
  std::vector<uint32_t> words = {
      0x08000002,  // 0x00: j 0x80000008 (target Header at 0x08)
      0x00000000,  // 0x04: nop
      0x08000004,  // 0x08: j 0x80000010 (target Latch at 0x10)
      0x00000000,  // 0x0C: nop
      0x2484FFFF,  // 0x10: addiu $a0, $a0, -1 (Latch)
      0x1480FFFC,  // 0x14: bne $a0, $zero, -4 (target Header at 0x08)
      0x00000000,  // 0x18: nop (delay slot, fallthrough to Exit at 0x1C)
      0x03E00008,  // 0x1C: jr $ra
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
  EXPECT_THAT(s, HasSubstr("block_0"));
  EXPECT_THAT(s, HasSubstr("do {"));
  EXPECT_THAT(s, HasSubstr("block_1"));
  EXPECT_THAT(s, HasSubstr("block_2"));
  EXPECT_THAT(s, HasSubstr("} while (block_2);"));
  EXPECT_THAT(s, HasSubstr("block_3"));
  EXPECT_THAT(s, Not(HasSubstr("continue")));
}

TEST(ControlFlowStructurerTest, StructureSwitchStatement) {
  std::vector<Instruction> instructions;

  // 0x80000000: sltiu $v0, $v1, 2
  Instruction sltiu;
  sltiu.opcode = Opcode::kSltiu;
  sltiu.rt = Register::kV0;
  sltiu.rs = Register::kV1;
  sltiu.immediate = 2;
  sltiu.vram = 0x80000000;
  instructions.push_back(sltiu);

  // 0x80000004: beqz $v0, 0x80000030 (target: join)
  Instruction beqz;
  beqz.opcode = Opcode::kBeq;
  beqz.rs = Register::kV0;
  beqz.rt = Register::kZero;
  beqz.immediate = 10;  // 0x80000008 + (10 << 2) = 0x80000030
  beqz.vram = 0x80000004;
  instructions.push_back(beqz);

  // 0x80000008: sll $v0, $v1, 2
  Instruction sll;
  sll.opcode = Opcode::kSll;
  sll.rd = Register::kV0;
  sll.rt = Register::kV1;
  sll.shift_amount = 2;
  sll.vram = 0x80000008;
  instructions.push_back(sll);

  // 0x8000000C: jr $v0
  Instruction jr;
  jr.opcode = Opcode::kJr;
  jr.rs = Register::kV0;
  jr.vram = 0x8000000C;
  instructions.push_back(jr);

  // 0x80000010: nop
  Instruction nop;
  nop.opcode = Opcode::kSll;
  nop.rd = Register::kZero;
  nop.rt = Register::kZero;
  nop.vram = 0x80000010;
  instructions.push_back(nop);

  // Case 0 (0x80000014 - 0x8000001C): jumps to 0x80000030
  Instruction j0;
  j0.opcode = Opcode::kJ;
  j0.target = 0x80000030 >> 2;
  j0.vram = 0x80000014;
  instructions.push_back(j0);

  Instruction nop0 = nop;
  nop0.vram = 0x80000018;
  instructions.push_back(nop0);

  // Case 1 (0x8000001C - 0x80000024): jumps to 0x80000030
  Instruction j1;
  j1.opcode = Opcode::kJ;
  j1.target = 0x80000030 >> 2;
  j1.vram = 0x8000001C;
  instructions.push_back(j1);

  Instruction nop1 = nop;
  nop1.vram = 0x80000020;
  instructions.push_back(nop1);

  // Exit / Join block (0x80000030): jr $ra
  Instruction ret;
  ret.opcode = Opcode::kJr;
  ret.rs = Register::kRa;
  ret.vram = 0x80000030;
  instructions.push_back(ret);

  Instruction nop_ret = nop;
  nop_ret.vram = 0x80000034;
  instructions.push_back(nop_ret);

  JumpTable table;
  table.switch_vram = 0x8000000C;
  table.bounds_branch_vram = 0x80000004;
  table.default_target_vram = 0x80000030;
  table.entries = {
      JumpTableEntry{.case_index = 0, .case_value = 0, .target_vram = 0x80000014},
      JumpTableEntry{.case_index = 1, .case_value = 1, .target_vram = 0x8000001C},
  };

  auto cfg_or = ControlFlowGraph::Build(instructions, {table});
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("switch"));
  EXPECT_THAT(s, HasSubstr("case 0:"));
  EXPECT_THAT(s, HasSubstr("case 1:"));
  EXPECT_THAT(s, HasSubstr("default:\n    break;"));
}

TEST(ControlFlowStructurerTest, StructureSwitchPreservesBoundsBlock) {
  std::vector<Instruction> instructions;

  Instruction nop;
  nop.opcode = Opcode::kSll;
  nop.rd = Register::kZero;
  nop.rt = Register::kZero;

  // Block 0: Bounds check (0x80000000)
  Instruction beqz;
  beqz.opcode = Opcode::kBeq;
  beqz.rs = Register::kV0;
  beqz.rt = Register::kZero;
  beqz.immediate = 0x80000030 - (0x80000000 + 4);
  beqz.vram = 0x80000000;
  instructions.push_back(beqz);

  Instruction nop_delay = nop;
  nop_delay.vram = 0x80000004;
  instructions.push_back(nop_delay);

  // Block 1: Jump table indirect jump (0x80000008)
  Instruction jr;
  jr.opcode = Opcode::kJr;
  jr.rs = Register::kAt;
  jr.vram = 0x80000008;
  instructions.push_back(jr);

  Instruction nop_jr = nop;
  nop_jr.vram = 0x8000000C;
  instructions.push_back(nop_jr);

  // Case 0 (0x80000010): jr $ra
  Instruction ret;
  ret.opcode = Opcode::kJr;
  ret.rs = Register::kRa;
  ret.vram = 0x80000010;
  instructions.push_back(ret);
  Instruction nop_ret0 = nop;
  nop_ret0.vram = 0x80000014;
  instructions.push_back(nop_ret0);

  // Default (0x80000030): jr $ra
  Instruction ret_def;
  ret_def.opcode = Opcode::kJr;
  ret_def.rs = Register::kRa;
  ret_def.vram = 0x80000030;
  instructions.push_back(ret_def);
  Instruction nop_ret_def = nop;
  nop_ret_def.vram = 0x80000034;
  instructions.push_back(nop_ret_def);

  JumpTable table;
  table.switch_vram = 0x80000008;
  table.bounds_branch_vram = 0x80000000;
  table.default_target_vram = 0x80000030;
  table.entries = {
      JumpTableEntry{.case_index = 0, .case_value = 0, .target_vram = 0x80000010},
  };

  auto cfg_or = ControlFlowGraph::Build(instructions, {table});
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  EXPECT_EQ(region->type, RegionType::kSequence);
  ASSERT_GE(region->children.size(), 2u);
  EXPECT_EQ(region->children[0]->type, RegionType::kBlock);
  EXPECT_EQ(region->children[0]->block_id, 0u);
  EXPECT_EQ(region->children[1]->type, RegionType::kSwitch);
}

TEST(ControlFlowStructurerTest, DoWhileLoopEarlyBreak) {
  // Multi-block do-while loop with an early break before latch:
  // Block 0 (Entry):
  //   0x00: beq $zero, $zero, +1 (to Header at 0x08)
  //   0x04: nop
  // Block 1 (Header with early break check):
  //   0x08: addiu $v0, $v0, 1
  //   0x0C: nop
  //   0x10: beq $a0, $zero, +3 (to Exit at 0x20)
  //   0x14: nop (delay slot, fallthrough to Latch at 0x18)
  // Block 2 (Latch):
  //   0x18: bne $v0, $zero, -5 (to Header at 0x08)
  //   0x1C: nop (fallthrough to Exit at 0x20)
  // Block 3 (Exit):
  //   0x20: jr $ra
  //   0x24: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x24420001,  // 0x08: addiu $v0, $v0, 1
      0x00000000,  // 0x0C: nop
      0x10800003,  // 0x10: beq $a0, $zero, +3 (0x20)
      0x00000000,  // 0x14: nop
      0x1440FFFB,  // 0x18: bne $v0, $zero, -5 (0x08)
      0x00000000,  // 0x1C: nop
      0x03E00008,  // 0x20: jr $ra
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
  EXPECT_THAT(s, HasSubstr("block_0"));
  EXPECT_THAT(s, HasSubstr("do {"));
  EXPECT_THAT(s, HasSubstr("if (block_1) {\n    break\n  }"));
  EXPECT_THAT(s, HasSubstr("} while (block_2);"));
  EXPECT_THAT(s, HasSubstr("block_3"));
}

TEST(ControlFlowStructurerTest, DoWhileLoopEarlyBreakInverted) {
  // Multi-block do-while loop with an early break where fallthrough exits:
  // Block 0 (Header):
  //   0x00: beq $zero, $zero, +1 (to Header at 0x08)
  //   0x04: nop
  // Block 1 (Early break check):
  //   0x08: addiu $v0, $v0, 1
  //   0x0C: bne $a0, $zero, +4 (to Latch at 0x20)
  //   0x10: nop (fallthrough to Exit jump at 0x14)
  // Block 2 (Exit jump / body):
  //   0x14: j 0x80000028 (Exit at 0x28)
  //   0x18: nop
  // Block 3 (Latch):
  //   0x20: bne $v0, $zero, -7 (to Header at 0x08)
  //   0x24: nop (fallthrough to Exit at 0x28)
  // Block 4 (Exit):
  //   0x28: jr $ra
  //   0x2C: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x24420001,  // 0x08: addiu $v0, $v0, 1
      0x14800004,  // 0x0C: bne $a0, $zero, +4 (0x20)
      0x00000000,  // 0x10: nop
      0x0800000A,  // 0x14: j 0x80000028
      0x00000000,  // 0x18: nop
      0x00000000,  // 0x1C: nop
      0x1440FFF9,  // 0x20: bne $v0, $zero, -7 (0x08)
      0x00000000,  // 0x24: nop
      0x03E00008,  // 0x28: jr $ra
      0x00000000,  // 0x2C: nop
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
  EXPECT_THAT(s, HasSubstr("block_0"));
  EXPECT_THAT(s, HasSubstr("do {"));
  EXPECT_THAT(s, HasSubstr("if (!block_1) {\n    block_2\n    break\n  }"));
  EXPECT_THAT(s, HasSubstr("} while (block_4);"));
  EXPECT_THAT(s, HasSubstr("block_5"));
}

TEST(ControlFlowStructurerTest, WhileLoopConditionPolarity) {
  // Case A: Branch in header targets Exit -> invert_condition is true
  std::vector<uint32_t> words_exit_on_branch = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x14800003,  // 0x08: bne $a0, $zero, +3 (Exit at 0x18)
      0x00000000,  // 0x0C: nop
      0x08000002,  // 0x10: j 0x80000008 (Latch to Header at 0x08)
      0x00000000,  // 0x14: nop
      0x03E00008,  // 0x18: jr $ra
      0x00000000,  // 0x1C: nop
  };

  auto insts_a = *DecodeSequence(words_exit_on_branch, 0x80000000);
  auto cfg_a = *ControlFlowGraph::Build(insts_a);
  DominatorTree dt_a = DominatorTree::Compute(cfg_a);
  DominatorTree post_dt_a = DominatorTree::ComputePostDominators(cfg_a);
  LoopInfo loops_a = LoopInfo::Analyze(cfg_a, dt_a);
  auto region_a = ControlFlowStructurer::Structure(cfg_a, dt_a, post_dt_a, loops_a);
  ASSERT_THAT(region_a, NotNull());
  ASSERT_EQ(region_a->type, RegionType::kSequence);
  ASSERT_GE(region_a->children.size(), 2u);
  EXPECT_EQ(region_a->children[1]->type, RegionType::kLoop);
  EXPECT_TRUE(region_a->children[1]->invert_condition);

  // Case B: Branch in header targets Body -> invert_condition is false
  // 0x08: bne $a0, $zero, +3 -> targets Body at 0x18
  // 0x10: jr $ra -> Exit on fallthrough
  // 0x18: j 0x80000008 -> Latch back to Header
  std::vector<uint32_t> words_body_on_branch = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x14800003,  // 0x08: bne $a0, $zero, +3 (0x18)
      0x00000000,  // 0x0C: nop
      0x03E00008,  // 0x10: jr $ra (Exit)
      0x00000000,  // 0x14: nop
      0x08000002,  // 0x18: j 0x80000008 (Latch to Header at 0x08)
      0x00000000,  // 0x1C: nop
  };

  auto insts_b = *DecodeSequence(words_body_on_branch, 0x80000000);
  auto cfg_b = *ControlFlowGraph::Build(insts_b);
  DominatorTree dt_b = DominatorTree::Compute(cfg_b);
  DominatorTree post_dt_b = DominatorTree::ComputePostDominators(cfg_b);
  LoopInfo loops_b = LoopInfo::Analyze(cfg_b, dt_b);
  auto region_b = ControlFlowStructurer::Structure(cfg_b, dt_b, post_dt_b, loops_b);
  ASSERT_THAT(region_b, NotNull());
  ASSERT_EQ(region_b->type, RegionType::kSequence);
  ASSERT_GE(region_b->children.size(), 2u);
  EXPECT_EQ(region_b->children[1]->type, RegionType::kLoop);
  EXPECT_FALSE(region_b->children[1]->invert_condition);
}

TEST(ControlFlowStructurerTest, DoWhileLoopMultipleExitsFollow) {
  // Loop with multiple exits:
  // Exit 1 (0x28) is reachable via early break from Block 1.
  // Exit 2 (0x20) is reachable via fallthrough from latch Block 2.
  // The follow block should be selected as Exit 2 (the latch exit).
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x24420001,  // 0x08: addiu $v0, $v0, 1
      0x10800006,  // 0x0C: beq $a0, $zero, +6 (Exit 1 at 0x28)
      0x00000000,  // 0x10: nop
      0x1440FFFC,  // 0x14: bne $v0, $zero, -4 (0x08)
      0x00000000,  // 0x18: nop (fallthrough to Exit 2 at 0x20)
      0x00000000,  // 0x1C: nop
      0x03E00008,  // 0x20: jr $ra (Exit 2)
      0x00000000,  // 0x24: nop
      0x03E00008,  // 0x28: jr $ra (Exit 1)
      0x00000000,  // 0x2C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("do {"));
  EXPECT_THAT(s, HasSubstr("} while (block_2);"));
  // Exit 2 (block_3) follows immediately after the loop
  EXPECT_THAT(s, HasSubstr("} while (block_2);\nblock_3\n"));
}

TEST(ControlFlowStructurerTest, IntraLoopConditionalJoinFallback) {
  // Loop containing an if-then branch whose post-dominator is outside the loop.
  // The structurer should fall back to joining at the latch.
  // Block 0: Header (0x00) -> falls through to Block 1 (0x08)
  // Block 1: Body (0x08) -> beq $a0, $zero to Latch (0x18); fallthrough to Block 2 (0x10)
  // Block 2: Work (0x10) -> jumps to Latch (0x18)
  // Block 3: Latch (0x18) -> bne $v0, $zero to Header (0x00); fallthrough to Exit (0x24)
  // Block 4: Exit (0x24) -> jr $ra
  std::vector<uint32_t> words = {
      0x08000002,  // 0x00: j 0x80000008 (Header to Body)
      0x00000000,  // 0x04: nop
      0x10800003,  // 0x08: beq $a0, $zero, +3 (to Latch at 0x18)
      0x00000000,  // 0x0C: nop
      0x08000006,  // 0x10: j 0x80000018 (to Latch at 0x18)
      0x00000000,  // 0x14: nop
      0x1440FFFA,  // 0x18: bne $v0, $zero, -6 (Latch to Header at 0x00)
      0x00000000,  // 0x1C: nop (fallthrough to Exit at 0x24)
      0x00000000,  // 0x20: nop
      0x03E00008,  // 0x24: jr $ra (Exit)
      0x00000000,  // 0x28: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("if (!block_2) {\n    block_3\n  }"));
  EXPECT_THAT(s, HasSubstr("block_5"));
}

TEST(ControlFlowStructurerTest, SwitchEarlyReturnSharedTailBreak) {
  // Jump-table switch where one case returns early and two other cases jump to a shared tail.
  // Block 0 (0x80000000): switch dispatch: jr $t0; nop
  // Block 1 (0x80000008): case 0 (returns early): jr $ra; nop
  // Block 2 (0x80000010): case 1: addiu $v0, $zero, 1; j 0x80000028; nop
  // Block 3 (0x8000001C): case 2: addiu $v0, $zero, 2; j 0x80000028; nop
  // Block 4 (0x80000028): shared tail (default target): addiu $v0, $v0, 10; jr $ra; nop
  std::vector<uint32_t> words = {
      0x01000008,  // 0x00: jr $t0
      0x00000000,  // 0x04: nop
      0x03E00008,  // 0x08: jr $ra (Case 0 return)
      0x00000000,  // 0x0C: nop
      0x24020001,  // 0x10: addiu $v0, $zero, 1
      0x0800000A,  // 0x14: j 0x80000028
      0x00000000,  // 0x18: nop
      0x24020002,  // 0x1C: addiu $v0, $zero, 2
      0x0800000A,  // 0x20: j 0x80000028
      0x00000000,  // 0x24: nop
      0x2442000A,  // 0x28: addiu $v0, $v0, 10
      0x03E00008,  // 0x2C: jr $ra
      0x00000000,  // 0x30: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  JumpTable table;
  table.switch_vram = 0x80000000;
  table.default_target_vram = 0x80000028;
  table.entries = {
      JumpTableEntry{.case_index = 0, .case_value = 0, .target_vram = 0x80000008},
      JumpTableEntry{.case_index = 1, .case_value = 1, .target_vram = 0x80000010},
      JumpTableEntry{.case_index = 2, .case_value = 2, .target_vram = 0x8000001C},
  };

  auto cfg_or = ControlFlowGraph::Build(insts, {table});
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
  // Shared tail (block 4) is structured after the switch
  EXPECT_EQ(region->children[1]->type, RegionType::kBlock);

  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("switch"));
  EXPECT_THAT(s, HasSubstr("case 0:"));
  EXPECT_THAT(s, HasSubstr("case 1:"));
  EXPECT_THAT(s, HasSubstr("case 2:"));
  EXPECT_THAT(s, HasSubstr("default:\n    break;"));
  EXPECT_THAT(s, HasSubstr("block_4\n"));
}

TEST(ControlFlowStructurerTest, OrdersJumpTableSwitchCasesByTargetBlockVramOrder) {
  // Case 2's target block (0x80000008) precedes Case 0's target block (0x80000014) in .text.
  std::vector<uint32_t> words = {
      0x00800008,  // 0x00: jr $a0
      0x00000000,  // 0x04: nop
      0x24020014,  // 0x08: addiu $v0, $zero, 20 (case 2 target at 0x08)
      0x08000008,  // 0x0C: j 0x80000020
      0x00000000,  // 0x10: nop
      0x2402000A,  // 0x14: addiu $v0, $zero, 10 (case 0 target at 0x14)
      0x08000008,  // 0x18: j 0x80000020
      0x00000000,  // 0x1C: nop
      0x03E00008,  // 0x20: jr $ra
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  JumpTable table;
  table.switch_vram = 0x80000000;
  table.default_target_vram = 0x80000020;
  table.entries = {
      JumpTableEntry{.case_index = 0, .case_value = 0, .target_vram = 0x80000014},
      JumpTableEntry{.case_index = 1, .case_value = 1, .target_vram = 0x80000020},
      JumpTableEntry{.case_index = 2, .case_value = 2, .target_vram = 0x80000008},
  };

  auto cfg_or = ControlFlowGraph::Build(insts, {table});
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  size_t pos_case2 = s.find("case 2:");
  size_t pos_case0 = s.find("case 0:");
  ASSERT_NE(pos_case2, std::string::npos);
  ASSERT_NE(pos_case0, std::string::npos);
  EXPECT_LT(pos_case2, pos_case0);
}

TEST(ControlFlowStructurerTest, EmitsBreakForLoopExitInsideNestedConditional) {
  // Loop header (block 0) -> if/else (block 1) where one branch (block 2) exits
  // the loop to block 5 (loop_follow) while the other branch (block 3) merges at
  // the loop latch (block 4) which branches back to block 0.
  std::vector<uint32_t> words = {
      0x24840001,  // 00 (block 0): addiu $a0, $a0, 1
      0x10850003,  // 04 (block 1): beq   $a0, $a1, +3 (to 0x14 block 3)
      0x00000000,  // 08:           nop
      0x10860004,  // 0C (block 2): beq   $a0, $a2, +4 (to 0x20 block 5 exit)
      0x00000000,  // 10:           nop
      0x24840002,  // 14 (block 3): addiu $a0, $a0, 2
      0x1480FFF9,  // 18 (block 4): bne   $a0, $zero, -7 (to 0x00 block 0)
      0x00000000,  // 1C:           nop
      0x03E00008,  // 20 (block 5): jr    $ra
      0x00801021,  // 24:           move  $v0, $a0
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
  EXPECT_THAT(s, HasSubstr("break"));
  EXPECT_THAT(s, Not(HasSubstr("goto block_5")));
}

TEST(ControlFlowStructurerTest, InnerLoopExitToSuccessorOfLatchFollowEmitsGotoInsteadOfInlining) {
  // Outer do-while loop (header 0x00) containing an inner do-while loop (header 0x04)
  // whose early exit branches to 0x18 (the successor of the inner loop's latch fallthrough
  // block 0x14):
  //   00 (block 0, outer header): addiu $a2, $a2, 1
  //   04 (block 1, inner header): beq   $a0, $a1, +5 (to 0x1C block 4)
  //   08:                         addiu $v0, $zero, 32
  //   0C (block 2, inner latch):  addiu $a0, $a0, 1
  //   10:                         bne   $a0, $a3, -4 (to 0x04 block 1)
  //   14:                         nop
  //   18 (block 3, latch follow): addiu $v0, $zero, 32 (falls through to 0x1C block 4)
  //   1C (block 4, outer body):   bne   $a2, $v0, -8 (to 0x00 block 0)
  //   20:                         nop
  //   24 (block 5, return):       jr    $ra
  //   28:                         nop
  std::vector<uint32_t> words = {
      0x24C60001,  // 00: addiu $a2, $a2, 1
      0x10850005,  // 04: beq   $a0, $a1, +5 (to 0x1C)
      0x24020020,  // 08: addiu $v0, $zero, 32
      0x24840001,  // 0C: addiu $a0, $a0, 1
      0x1487FFFC,  // 10: bne   $a0, $a3, -4 (to 0x04)
      0x00000000,  // 14: nop
      0x24020020,  // 18: addiu $v0, $zero, 32
      0x14C2FFF8,  // 1C: bne   $a2, $v0, -8 (to 0x00)
      0x00000000,  // 20: nop
      0x03E00008,  // 24: jr    $ra
      0x00000000,  // 28: nop
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
  EXPECT_THAT(s, HasSubstr("goto block_4"));
}

TEST(ControlFlowStructurerTest, FoldsRootEqualityPivotIntoChildDecisionTreeSwitch) {
  // Entry block loads selector $v1 from memory and checks $v1 == 1 (pivot).
  // Fallthrough splits on $v1 < 2: left checks $v1 == 0, right checks $v1 == 2.
  // All three cases (0, 1, 2) must fold into a single `switch ($v1)`.
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 0x00: addiu $sp, $sp, -24
      0xAFBF0014,  // 0x04: sw    $ra, 20($sp)
      0x3C038019,  // 0x08: lui   $v1, 0x8019
      0x8C639074,  // 0x0C: lw    $v1, -0x6F8C($v1)
      0x24020001,  // 0x10: addiu $v0, $zero, 1
      0x1062000B,  // 0x14: beq   $v1, $v0, +11 -> 0x44 (case 1)
      0x28620002,  // 0x18: slti  $v0, $v1, 2
      0x50400005,  // 0x1C: beql  $v0, $zero, +5 -> 0x34 (right subtree: >= 2)
      0x24020002,  // 0x20: addiu $v0, $zero, 2
      0x1460000D,  // 0x24: bne   $v1, $zero, +13 -> 0x5C (default/epilogue)
      0x00000000,  // 0x28: nop
      0x24040050,  // 0x2C: addiu $a0, $zero, 80 (case 0)
      0x08000014,  // 0x30: j     0x80000050 (shared call)
      0x24050068,  // 0x34: addiu $a1, $zero, 104
      0x14620009,  // 0x38: bne   $v1, $v0, +9 -> 0x60 (default/epilogue)
      0x00000000,  // 0x3C: nop
      0x24040060,  // 0x40: addiu $a0, $zero, 96 (case 2)
      0x08000014,  // 0x44: j     0x80000050 (shared call)
      0x24050078,  // 0x48: addiu $a1, $zero, 120
      0x24040058,  // 0x4C: addiu $a0, $zero, 88 (case 1)
      0x24050070,  // 0x50: addiu $a1, $zero, 112
      0x0C000400,  // 0x54: jal   0x80001000
      0x00000000,  // 0x58: nop
      0x8FBF0014,  // 0x5C: lw    $ra, 20($sp)
      0x03E00008,  // 0x60: jr    $ra
      0x27BD0018,  // 0x64: addiu $sp, $sp, 24
  };
  // Fix branch offsets in `words`:
  // 0x14: beq $v1, $v0, to 0x4C -> offset = (0x4C - 0x18)/4 = 13 (0x000D)
  words[5] = 0x1062000D;
  // 0x1C: beql $v0, $zero, to 0x38 -> offset = (0x38 - 0x20)/4 = 6 (0x0006)
  words[7] = 0x50400006;
  // 0x24: bne $v1, $zero, to 0x5C -> offset = (0x5C - 0x28)/4 = 13 (0x000D)
  words[9] = 0x1460000D;
  // 0x30: j 0x80000054 -> target = 0x80000054 >> 2 = 0x00000015
  words[12] = 0x08000015;
  // 0x38: bne $v1, $v0, to 0x5C -> offset = (0x5C - 0x3C)/4 = 8 (0x0008)
  words[14] = 0x14620008;
  // 0x44: j 0x80000054 -> target = 0x00000015
  words[17] = 0x08000015;

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
  EXPECT_THAT(s, HasSubstr("switch ($v1)"));
  EXPECT_THAT(s, HasSubstr("case 0:"));
  EXPECT_THAT(s, HasSubstr("case 1:"));
  EXPECT_THAT(s, HasSubstr("case 2:"));
}

TEST(ControlFlowStructurerTest, DoWhileEarlyExitWithBranchLikelyDelayMatchingLoopFollowEmitsBreak) {
  // Block 0 (preheader):
  //   0x00: addiu $v1, $zero, 0
  //   0x04: andi  $v0, $a0, 0xFF
  // Block 1 (loop header):
  //   0x08: bnel  $v0, $zero, +4 -> 0x1C (block 4, successor of loop_follow block 3)
  //   0x0C: sh    $v1, 0($a1)            (delay slot matches loop_follow block 3)
  // Block 2 (loop latch):
  //   0x10: addiu $v1, $v1, 1
  //   0x14: bne   $v1, $a2, -4 -> 0x08 (block 1)
  //   0x18: andi  $v0, $a0, 0xFF
  // Block 3 (loop_follow):
  //   0x1C: sh    $v1, 0($a1)
  // Block 4 (after loop_follow):
  //   0x20: jr    $ra
  //   0x24: nop
  std::vector<uint32_t> words = {
      0x24030000,  // 0x00: addiu $v1, $zero, 0
      0x308200FF,  // 0x04: andi  $v0, $a0, 0xFF
      0x54400004,  // 0x08: bnel  $v0, $zero, +4 -> 0x1C (wait: 0x0C + 4*4 = 0x1C, let's target 0x20
                   // -> +4 is 0x1C, +5 is 0x20)
      0xA4A30000,  // 0x0C: sh    $v1, 0($a1)
      0x24630001,  // 0x10: addiu $v1, $v1, 1
      0x1466FFFC,  // 0x14: bne   $v1, $a2, -4 -> 0x08
      0x308200FF,  // 0x18: andi  $v0, $a0, 0xFF
      0xA4A30000,  // 0x1C: sh    $v1, 0($a1)
      0x03E00008,  // 0x20: jr    $ra
      0x00000000,  // 0x24: nop
  };
  words[2] = 0x54400004;      // 0x08: bnel $v0, $zero, +4 -> 0x0C + 0x10 = 0x1C; +5 -> 0x20
  words[2] = 0x54400004 + 1;  // +5 -> 0x20

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
  EXPECT_THAT(s, HasSubstr("do {"));
  EXPECT_THAT(s, HasSubstr("break"));
  EXPECT_THAT(s, Not(HasSubstr("goto")));
}

TEST(ControlFlowStructurerTest, StructuresNestedDecisionTreeSwitchInsideOuterSwitchCase) {
  // Outer jump-table switch on $a0 (cases 0, 1):
  // Case 0 contains an inner 2-case decision-tree switch on $a1 (cases 10, 20) with its own
  // trampoline jump to the outer join block (0x44).
  std::vector<uint32_t> words = {
      0x00800008,  // 0x00: jr    $a0 (outer jump-table switch)
      0x00000000,  // 0x04: nop
      0x2402000A,  // 0x08: addiu $v0, $zero, 10 (outer case 0: inner switch root)
      0x10A20005,  // 0x0C: beq   $a1, $v0, +5 -> 0x24 (inner case 10)
      0x00000000,  // 0x10: nop
      0x24020014,  // 0x14: addiu $v0, $zero, 20
      0x10A20005,  // 0x18: beq   $a1, $v0, +5 -> 0x30 (inner case 20)
      0x00000000,  // 0x1C: nop
      0x08000011,  // 0x20: j     0x80000044 (inner default trampoline) -> wait: 0x24 is inner case
                   // 10, so need nop at 0x24 and inner case 10 at 0x28!
  };
  words = {
      0x00800008,  // 0x00: jr    $a0
      0x00000000,  // 0x04: nop
      0x2402000A,  // 0x08: addiu $v0, $zero, 10 (outer case 0: inner switch root)
      0x10A20006,  // 0x0C: beq   $a1, $v0, +6 -> 0x28 (inner case 10)
      0x00000000,  // 0x10: nop
      0x24020014,  // 0x14: addiu $v0, $zero, 20
      0x10A20006,  // 0x18: beq   $a1, $v0, +6 -> 0x34 (inner case 20)
      0x00000000,  // 0x1C: nop
      0x08000012,  // 0x20: j     0x80000048 (join)
      0x00000000,  // 0x24: nop
      0x24020001,  // 0x28: addiu $v0, $zero, 1 (inner case 10)
      0x08000012,  // 0x2C: j     0x80000048 (join)
      0x00000000,  // 0x30: nop
      0x24020002,  // 0x34: addiu $v0, $zero, 2 (inner case 20)
      0x08000012,  // 0x38: j     0x80000048 (join)
      0x00000000,  // 0x3C: nop
      0x24020003,  // 0x40: addiu $v0, $zero, 3 (outer case 1)
      0x00000000,  // 0x44: nop
      0x03E00008,  // 0x48: jr    $ra (join)
      0x00000000,  // 0x4C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  JumpTable table;
  table.switch_vram = 0x80000000;
  table.default_target_vram = 0x80000048;
  table.entries = {
      JumpTableEntry{.case_index = 0, .case_value = 0, .target_vram = 0x80000008},
      JumpTableEntry{.case_index = 1, .case_value = 1, .target_vram = 0x80000040},
  };

  auto cfg = *ControlFlowGraph::Build(insts, {table});
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("switch ($a1)"));
  EXPECT_THAT(s, HasSubstr("case 10:"));
  EXPECT_THAT(s, HasSubstr("case 20:"));
}

TEST(ControlFlowStructurerTest,
     EarlySwitchCaseJumpingToSharedTailWithUnvisitedFallthroughEmitsGoto) {
  // Jump-table switch where Case 0 (0x08) jumps to a shared tail block (0x18),
  // Case 1 (0x14) falls through into the shared tail block (0x18), and Default
  // jumps directly to the post-switch return join block (0x20).
  // Case 0 must emit `goto block_3` so Case 1 can fall through into `block_3`
  // and `break;`, leaving `default:` as an empty `break;`.
  std::vector<uint32_t> words = {
      0x00800008,  // 0x00: jr    $a0 (block 0: switch)
      0x00000000,  // 0x04: nop
      0x2404000A,  // 0x08: addiu $a0, $zero, 10 (block 1: case 0)
      0x08000006,  // 0x0C: j     0x80000018 (jumps to shared tail block 3)
      0x00000000,  // 0x10: nop
      0x24040014,  // 0x14: addiu $a0, $zero, 20 (block 2: case 1, falls through to 0x18)
      0x0C000400,  // 0x18: jal   0x80001000 (block 3: shared tail, falls through to 0x20)
      0x00000000,  // 0x1C: nop
      0x03E00008,  // 0x20: jr    $ra (block 4: post-switch join)
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  JumpTable table;
  table.switch_vram = 0x80000000;
  table.default_target_vram = 0x80000020;
  table.entries = {
      JumpTableEntry{.case_index = 0, .case_value = 0, .target_vram = 0x80000008},
      JumpTableEntry{.case_index = 1, .case_value = 1, .target_vram = 0x80000014},
  };

  auto cfg = *ControlFlowGraph::Build(insts, {table});
  DominatorTree dt = DominatorTree::Compute(cfg);
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  auto region = ControlFlowStructurer::Structure(cfg, dt, post_dt, loops);
  ASSERT_THAT(region, NotNull());
  std::string s = region->ToString();
  EXPECT_THAT(s, HasSubstr("case 0:\n    block_1\n    goto block_3\n"));
  EXPECT_THAT(s, HasSubstr("case 1:\n    block_2\n    block_3\n    break;\n"));
  EXPECT_THAT(s, HasSubstr("default:\n    break;\n"));
}

}  // namespace
}  // namespace rom_nom_nom
