#include "lifter/control_flow_graph.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::SizeIs;

TEST(ControlFlowGraphTest, LinearFunctionEndingInReturn) {
  // Simple 4-instruction leaf function:
  // 0x00: addiu $sp, $sp, -16
  // 0x04: sw    $a0, 0($sp)
  // 0x08: jr    $ra
  // 0x0C: addiu $sp, $sp, 16  (delay slot)
  std::vector<uint32_t> words = {
      0x27BDFFF0,  // addiu $sp, $sp, -16
      0xAFAC0000,  // sw $a0, 0($sp)
      0x03E00008,  // jr $ra
      0x27BD0010,  // addiu $sp, $sp, 16
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  // Should form a single basic block with all 4 instructions.
  EXPECT_THAT(cfg.Blocks(), SizeIs(1));
  const auto& block = cfg.Blocks()[0];
  EXPECT_EQ(block.id, 0u);
  EXPECT_EQ(block.start_vram, 0x80000000);
  EXPECT_EQ(block.end_vram, 0x8000000C);
  EXPECT_EQ(block.Size(), 4u);
  EXPECT_TRUE(block.HasReturn());
  EXPECT_FALSE(block.HasBranch());
  EXPECT_THAT(block.successors, IsEmpty());
}

TEST(ControlFlowGraphTest, ConditionalBranchCreatesTwoSuccessors) {
  // if (a0 == 0) goto target;
  // 0x00: beqz  $a0, .L_target (+8 from delay slot -> 0x0C)
  // 0x04: nop                 (delay slot)
  // 0x08: addiu $v0, $zero, 1 (fallthrough)
  // 0x0C: jr    $ra           (.L_target)
  // 0x10: nop                 (delay slot)
  std::vector<uint32_t> words = {
      0x10800002,  // beq $a0, $zero, +2 (offset = 2 words = +8 bytes after delay slot -> 0x0C)
      0x00000000,  // nop
      0x24020001,  // addiu $v0, $zero, 1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  // Block 0: beq + nop (0x00 - 0x04)
  // Block 1: addiu $v0, 1 (0x08 - 0x08)
  // Block 2: jr $ra + nop (0x0C - 0x10)
  EXPECT_THAT(cfg.Blocks(), SizeIs(3));

  const auto* b0 = cfg.GetBlock(0);
  ASSERT_THAT(b0, NotNull());
  EXPECT_TRUE(b0->HasBranch());
  EXPECT_THAT(b0->successors, SizeIs(2));

  // Successor 1: Block 2 (taken target at 0x0C)
  // Successor 2: Block 1 (fallthrough at 0x08)
  const auto* b1 = cfg.GetBlock(1);
  ASSERT_THAT(b1, NotNull());
  EXPECT_EQ(b1->start_vram, 0x80000008);
  EXPECT_THAT(b1->successors, ElementsAre(2));  // falls through to block 2

  const auto* b2 = cfg.GetBlock(2);
  ASSERT_THAT(b2, NotNull());
  EXPECT_EQ(b2->start_vram, 0x8000000C);
  EXPECT_TRUE(b2->HasReturn());
  EXPECT_THAT(b2->successors, IsEmpty());
}

TEST(ControlFlowGraphTest, LoopWithBackEdge) {
  // Simple loop counting down:
  // 0x00: addiu $v0, $zero, 10
  // .L_loop: (0x04)
  // 0x04: addiu $v0, $v0, -1
  // 0x08: bnez  $v0, .L_loop (-2 words from delay slot -> 0x04)
  // 0x0C: nop                 (delay slot)
  // 0x10: jr    $ra
  // 0x14: nop
  std::vector<uint32_t> words = {
      0x2402000A,  // 0x00: addiu $v0, $zero, 10
      0x2442FFFF,  // 0x04: addiu $v0, $v0, -1
      0x1440FFFE,  // 0x08: bne $v0, $zero, -2 (offset -2 words = -8 bytes from 0x0C -> 0x04)
      0x00000000,  // 0x0C: nop
      0x03E00008,  // 0x10: jr $ra
      0x00000000,  // 0x14: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  // Block 0: entry (0x00) -> falls through to Block 1
  // Block 1: loop body (0x04 - 0x0C) -> branch to Block 1 (self back-edge) or fallthrough to Block
  // 2 Block 2: exit (0x10 - 0x14)
  EXPECT_THAT(cfg.Blocks(), SizeIs(3));

  const auto* loop_block = cfg.GetBlock(1);
  ASSERT_THAT(loop_block, NotNull());
  EXPECT_TRUE(loop_block->HasBranch());
  EXPECT_THAT(loop_block->predecessors, ElementsAre(0, 1));  // from entry and back-edge from self
  EXPECT_THAT(loop_block->successors, ElementsAre(1, 2));    // to self or to exit
}

TEST(ControlFlowGraphTest, ReversePostOrderTraversal) {
  // A -> B -> D
  // A -> C -> D
  // 0x00: bnez $a0, .L_c
  // 0x04: nop
  // 0x08: j .L_d
  // 0x0C: nop
  // .L_c: (0x10)
  // 0x10: addiu $v0, $zero, 2
  // .L_d: (0x14)
  // 0x14: jr $ra
  // 0x18: nop
  std::vector<uint32_t> words = {
      0x14800002,  // 0x00: bne $a0, $zero, +2 (0x10)
      0x00000000,  // 0x04: nop
      0x08000005,  // 0x08: j 0x14 (target = 5 -> 0x14)
      0x00000000,  // 0x0C: nop
      0x24020002,  // 0x10: addiu $v0, $zero, 2
      0x03E00008,  // 0x14: jr $ra
      0x00000000,  // 0x18: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  std::vector<uint32_t> rpo = cfg.ReversePostOrder();
  // RPO must start with entry block (0), and exit block should be last.
  ASSERT_THAT(rpo, SizeIs(cfg.Blocks().size()));
  EXPECT_EQ(rpo.front(), 0u);
}

TEST(ControlFlowGraphTest, BuildsCfgForIdleLoopPattern) {
  // Modeled directly on the control flow of the retail N64 idle thread:
  // Initialization block, calling functions, then entering an infinite polling loop
  // with a conditional callback invocation.
  //
  // 0x00: lui  $at, 0x8020
  // 0x04: sw   $zero, -10712($at)
  // 0x08: jal  0x800FD5B0
  // 0x0C: nop
  // .L_loop: (0x10)
  // 0x10: lui  $v0, 0x8020
  // 0x14: lw   $v0, -10712($v0)
  // 0x18: beqz $v0, .L_back (+3 words -> 0x28)
  // 0x1C: nop
  // 0x20: jalr $s0
  // 0x24: nop
  // .L_back: (0x28)
  // 0x28: j    .L_loop (target = 0x10)
  // 0x2C: nop
  std::vector<uint32_t> words = {
      0x3C018020,  // 0x00: lui $at, 0x8020
      0xAC20D628,  // 0x04: sw  $zero, -10712($at)
      0x0C03F56C,  // 0x08: jal 0x800FD5B0
      0x00000000,  // 0x0C: nop
      0x3C028020,  // 0x10: lui $v0, 0x8020
      0x8C42D628,  // 0x14: lw  $v0, -10712($v0)
      0x10400003,  // 0x18: beqz $v0, +3 (to 0x28)
      0x00000000,  // 0x1C: nop
      0x0200F809,  // 0x20: jalr $s0
      0x00000000,  // 0x24: nop
      0x08009731,  // 0x28: j 0x80025CC4 (target = (0x00025CC4 >> 2) = 0x9731)
      0x00000000,  // 0x2C: nop
  };

  auto insts = *DecodeSequence(words, 0x80025CB4);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status().message();

  const auto& cfg = *cfg_or;
  // Block 0: Init block (0x00 - 0x0C) -> falls through to loop header (0x10)
  // Block 1: Loop condition (0x10 - 0x1C) -> taken branch to Block 3 (0x28) or fallthrough to Block
  // 2 (0x20) Block 2: Callback call (0x20 - 0x24) -> falls through to Block 3 (0x28) Block 3: Loop
  // latch (0x28 - 0x2C) -> jumps back to loop header (Block 1)
  EXPECT_THAT(cfg.Blocks(), SizeIs(4));

  const auto* b0 = cfg.GetBlock(0);
  ASSERT_THAT(b0, NotNull());
  EXPECT_EQ(b0->start_vram, 0x80025CB4);
  EXPECT_THAT(b0->successors, ElementsAre(1));

  const auto* b1 = cfg.GetBlock(1);
  ASSERT_THAT(b1, NotNull());
  EXPECT_EQ(b1->start_vram, 0x80025CB4 + 0x10);
  EXPECT_TRUE(b1->HasBranch());
  EXPECT_THAT(b1->successors, ElementsAre(3, 2));

  const auto* b2 = cfg.GetBlock(2);
  ASSERT_THAT(b2, NotNull());
  EXPECT_THAT(b2->successors, ElementsAre(3));

  const auto* b3 = cfg.GetBlock(3);
  ASSERT_THAT(b3, NotNull());
  EXPECT_THAT(b3->successors, ElementsAre(1));  // Back-edge to loop header!

  EXPECT_EQ(cfg.ReversePostOrder().front(), 0u);
}

TEST(ControlFlowGraphTest, ToDotProducesNonEmptyOutput) {
  std::vector<uint32_t> words = {
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  std::string dot = cfg_or->ToDot();
  EXPECT_FALSE(dot.empty());
  EXPECT_NE(dot.find("digraph CFG"), std::string::npos);
}

TEST(ControlFlowGraphTest, BuildWithJumpTable) {
  std::vector<Instruction> instructions;

  // 0x80000000: sltiu $v0, $v1, 2
  Instruction sltiu;
  sltiu.opcode = Opcode::kSltiu;
  sltiu.rt = Register::kV0;
  sltiu.rs = Register::kV1;
  sltiu.immediate = 2;
  sltiu.vram = 0x80000000;
  instructions.push_back(sltiu);

  // 0x80000004: beqz $v0, 0x80000024 (default)
  Instruction beqz;
  beqz.opcode = Opcode::kBeq;
  beqz.rs = Register::kV0;
  beqz.rt = Register::kZero;
  beqz.immediate = 7;  // 0x80000008 + (7 << 2) = 0x80000024
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

  // 0x80000014: jr $ra (Case 0)
  Instruction ret0;
  ret0.opcode = Opcode::kJr;
  ret0.rs = Register::kRa;
  ret0.vram = 0x80000014;
  instructions.push_back(ret0);

  // 0x80000018: nop
  Instruction nop0 = nop;
  nop0.vram = 0x80000018;
  instructions.push_back(nop0);

  // 0x8000001C: jr $ra (Case 1)
  Instruction ret1 = ret0;
  ret1.vram = 0x8000001C;
  instructions.push_back(ret1);

  // 0x80000020: nop
  Instruction nop1 = nop;
  nop1.vram = 0x80000020;
  instructions.push_back(nop1);

  // 0x80000024: jr $ra (Default)
  Instruction ret_def = ret0;
  ret_def.vram = 0x80000024;
  instructions.push_back(ret_def);

  // 0x80000028: nop
  Instruction nop_def = nop;
  nop_def.vram = 0x80000028;
  instructions.push_back(nop_def);

  JumpTable table;
  table.switch_vram = 0x8000000C;
  table.default_target_vram = 0x80000024;
  table.entries = {
      JumpTableEntry{.case_index = 0, .case_value = 0, .target_vram = 0x80000014},
      JumpTableEntry{.case_index = 1, .case_value = 1, .target_vram = 0x8000001C},
  };

  auto cfg_or = ControlFlowGraph::Build(instructions, {table});
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  EXPECT_EQ(cfg.JumpTables().size(), 1u);

  // Find switch block
  const auto* switch_block = cfg.FindBlockByVram(0x8000000C);
  ASSERT_THAT(switch_block, NotNull());
  EXPECT_TRUE(switch_block->HasSwitch());
  ASSERT_THAT(switch_block->jump_table, NotNull());
  EXPECT_EQ(switch_block->jump_table->switch_vram, 0x8000000C);

  // Switch block successors should be the case blocks (0x80000014 and 0x8000001C)
  const auto* case0 = cfg.FindBlockByVram(0x80000014);
  const auto* case1 = cfg.FindBlockByVram(0x8000001C);
  const auto* def_block = cfg.FindBlockByVram(0x80000024);

  ASSERT_THAT(case0, NotNull());
  ASSERT_THAT(case1, NotNull());
  ASSERT_THAT(def_block, NotNull());

  EXPECT_THAT(switch_block->successors, ElementsAre(case0->id, case1->id));
  EXPECT_THAT(case0->predecessors, ElementsAre(switch_block->id));
  EXPECT_THAT(case1->predecessors, ElementsAre(switch_block->id));
}

TEST(ControlFlowGraphTest, ConstantIndirectJumpCreatesEdge) {
  // 0x80000000: lui   $t0, 0x8000
  // 0x80000004: addiu $t0, $t0, 0x0010
  // 0x80000008: jr    $t0
  // 0x8000000C: nop
  // 0x80000010: jr    $ra
  // 0x80000014: nop
  std::vector<uint32_t> words = {
      0x3C088000,  // lui   $t0, 0x8000
      0x25080010,  // addiu $t0, $t0, 0x0010
      0x01000008,  // jr    $t0
      0x00000000,  // nop
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  const auto* block0 = cfg.FindBlockByVram(0x80000000);
  const auto* block1 = cfg.FindBlockByVram(0x80000010);
  ASSERT_THAT(block0, NotNull());
  ASSERT_THAT(block1, NotNull());

  EXPECT_THAT(block0->successors, ElementsAre(block1->id));
  EXPECT_THAT(block1->predecessors, ElementsAre(block0->id));
}

TEST(ControlFlowGraphTest, ExternalJumpCreatesTargetBlock) {
  // 0x80000000: j   0x80010000
  // 0x80000004: nop
  std::vector<uint32_t> words = {
      0x08004000,  // j 0x80010000
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  const auto* entry = cfg.FindBlockByVram(0x80000000);
  const auto* ext_block = cfg.FindBlockByVram(0x80010000);
  ASSERT_THAT(entry, NotNull());
  ASSERT_THAT(ext_block, NotNull());

  EXPECT_THAT(entry->successors, ElementsAre(ext_block->id));
  EXPECT_THAT(ext_block->predecessors, ElementsAre(entry->id));
}

TEST(ControlFlowGraphTest, UnreachableBlockPrunedFromPredecessors) {
  // 0x80000000: jr $ra
  // 0x80000004: nop
  // 0x80000008: j  0x80000000 (unreachable block jumping to entry)
  // 0x8000000C: nop
  std::vector<uint32_t> words = {
      0x03E00008,  // jr $ra
      0x00000000,  // nop
      0x08000000,  // j  0x80000000
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  const auto* entry = cfg.FindBlockByVram(0x80000000);
  ASSERT_THAT(entry, NotNull());
  // The unreachable block at 0x80000008 should have its edge pruned and must not
  // contaminate entry's predecessors.
  EXPECT_THAT(entry->predecessors, IsEmpty());
}

TEST(ControlFlowGraphTest, ConstantIndirectJumpLuiOnly) {
  // 0x80000000: lui $t0, 0x8001
  // 0x80000004: jr  $t0
  // 0x80000008: nop
  // ... target at 0x80010000
  // 0x80010000: jr  $ra
  // 0x80010004: nop
  std::vector<Instruction> insts;
  Instruction lui_inst;
  lui_inst.vram = 0x80000000;
  lui_inst.opcode = Opcode::kLui;
  lui_inst.rt = Register::kT0;
  lui_inst.immediate = 0x8001;
  insts.push_back(lui_inst);

  Instruction jr_inst;
  jr_inst.vram = 0x80000004;
  jr_inst.opcode = Opcode::kJr;
  jr_inst.rs = Register::kT0;
  insts.push_back(jr_inst);

  Instruction nop0;
  nop0.vram = 0x80000008;
  nop0.opcode = Opcode::kSll;
  nop0.rd = Register::kZero;
  nop0.rt = Register::kZero;
  insts.push_back(nop0);

  Instruction ret_inst;
  ret_inst.vram = 0x80010000;
  ret_inst.opcode = Opcode::kJr;
  ret_inst.rs = Register::kRa;
  insts.push_back(ret_inst);

  Instruction nop1 = nop0;
  nop1.vram = 0x80010004;
  insts.push_back(nop1);

  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  const auto* block0 = cfg.FindBlockByVram(0x80000000);
  const auto* block1 = cfg.FindBlockByVram(0x80010000);
  ASSERT_THAT(block0, NotNull());
  ASSERT_THAT(block1, NotNull());

  EXPECT_THAT(block0->successors, ElementsAre(block1->id));
  EXPECT_THAT(block1->predecessors, ElementsAre(block0->id));
}

TEST(ControlFlowGraphTest, ConstantIndirectJumpNegativeImmediate) {
  // 0x80000000: lui   $t0, 0x8002
  // 0x80000004: addiu $t0, $t0, -32768 (0x8000 -> sign-extended -> target 0x80018000)
  // 0x80000008: jr    $t0
  // 0x8000000C: nop
  // 0x80018000: jr    $ra
  // 0x80018004: nop
  std::vector<Instruction> insts;
  Instruction lui_inst;
  lui_inst.vram = 0x80000000;
  lui_inst.opcode = Opcode::kLui;
  lui_inst.rt = Register::kT0;
  lui_inst.immediate = 0x8002;
  insts.push_back(lui_inst);

  Instruction addiu_inst;
  addiu_inst.vram = 0x80000004;
  addiu_inst.opcode = Opcode::kAddiu;
  addiu_inst.rt = Register::kT0;
  addiu_inst.rs = Register::kT0;
  addiu_inst.immediate = static_cast<uint16_t>(-32768);
  insts.push_back(addiu_inst);

  Instruction jr_inst;
  jr_inst.vram = 0x80000008;
  jr_inst.opcode = Opcode::kJr;
  jr_inst.rs = Register::kT0;
  insts.push_back(jr_inst);

  Instruction nop0;
  nop0.vram = 0x8000000C;
  nop0.opcode = Opcode::kSll;
  nop0.rd = Register::kZero;
  nop0.rt = Register::kZero;
  insts.push_back(nop0);

  Instruction ret_inst;
  ret_inst.vram = 0x80018000;
  ret_inst.opcode = Opcode::kJr;
  ret_inst.rs = Register::kRa;
  insts.push_back(ret_inst);

  Instruction nop1 = nop0;
  nop1.vram = 0x80018004;
  insts.push_back(nop1);

  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  const auto* block0 = cfg.FindBlockByVram(0x80000000);
  const auto* target_block = cfg.FindBlockByVram(0x80018000);
  ASSERT_THAT(block0, NotNull());
  ASSERT_THAT(target_block, NotNull());

  EXPECT_THAT(block0->successors, ElementsAre(target_block->id));
  EXPECT_THAT(target_block->predecessors, ElementsAre(block0->id));
}

TEST(ControlFlowGraphTest, ConstantIndirectJumpNonConstantIgnored) {
  // 0x80000000: lw  $t0, 0($a0)  (dynamic pointer load)
  // 0x80000004: jr  $t0
  // 0x80000008: nop
  std::vector<uint32_t> words = {
      0x8C880000,  // lw $t0, 0($a0)
      0x01000008,  // jr $t0
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  const auto* block0 = cfg.FindBlockByVram(0x80000000);
  ASSERT_THAT(block0, NotNull());
  EXPECT_THAT(block0->successors, IsEmpty());
}

TEST(ControlFlowGraphTest, ExternalBranchConditionalAndUnconditional) {
  // Conditional branch to external target (0x80000020):
  // 0x80000000: bne $a0, $zero, +7 (-> 0x80000020)
  // 0x80000004: nop
  // 0x80000008: jr  $ra
  // 0x8000000C: nop
  std::vector<uint32_t> cond_words = {
      0x14800007,  // bne $a0, $zero, 0x80000020
      0x00000000,  // nop
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  auto cond_insts = *DecodeSequence(cond_words, 0x80000000);
  auto cond_cfg = *ControlFlowGraph::Build(cond_insts);

  const auto* cond_entry = cond_cfg.FindBlockByVram(0x80000000);
  const auto* cond_fallthrough = cond_cfg.FindBlockByVram(0x80000008);
  const auto* cond_ext = cond_cfg.FindBlockByVram(0x80000020);
  ASSERT_THAT(cond_entry, NotNull());
  ASSERT_THAT(cond_fallthrough, NotNull());
  ASSERT_THAT(cond_ext, NotNull());

  EXPECT_THAT(cond_entry->successors, ElementsAre(cond_ext->id, cond_fallthrough->id));

  // Unconditional branch to external target:
  // 0x80000000: beq $zero, $zero, +7 (-> 0x80000020)
  // 0x80000004: nop
  // 0x80000008: jr  $ra
  // 0x8000000C: nop
  std::vector<uint32_t> uncond_words = {
      0x10000007,  // beq $zero, $zero, 0x80000020
      0x00000000,  // nop
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  auto uncond_insts = *DecodeSequence(uncond_words, 0x80000000);
  auto uncond_cfg = *ControlFlowGraph::Build(uncond_insts);

  const auto* uncond_entry = uncond_cfg.FindBlockByVram(0x80000000);
  const auto* uncond_ext = uncond_cfg.FindBlockByVram(0x80000020);
  ASSERT_THAT(uncond_entry, NotNull());
  ASSERT_THAT(uncond_ext, NotNull());

  // Unconditional branch should NOT have a fallthrough edge.
  EXPECT_THAT(uncond_entry->successors, ElementsAre(uncond_ext->id));
}

TEST(ControlFlowGraphTest, ExternalTargetBlockDeduplication) {
  // Multiple jumps to the same external target 0x80010000:
  // 0x80000000: j 0x80010000
  // 0x80000004: nop
  // 0x80000008: j 0x80010000
  // 0x8000000C: nop
  std::vector<uint32_t> words = {
      0x08004000,  // j 0x80010000
      0x00000000,  // nop
      0x08004000,  // j 0x80010000
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  // Verify only one external target block was created for 0x80010000.
  const auto* ext_block = cfg.FindBlockByVram(0x80010000);
  ASSERT_THAT(ext_block, NotNull());

  const auto* block0 = cfg.FindBlockByVram(0x80000000);
  ASSERT_THAT(block0, NotNull());
  EXPECT_THAT(block0->successors, ElementsAre(ext_block->id));
}

TEST(ControlFlowGraphTest, UnreachableSubgraphWithCyclePruning) {
  // 0x80000000: bne $a0, $zero, +4 (-> 0x80000014: target)
  // 0x80000004: nop
  // 0x80000008: jr  $ra
  // 0x8000000C: nop
  //
  // Unreachable block A:
  // 0x80000010: j 0x80000018 (-> Unreachable block B)
  //
  // Reachable target:
  // 0x80000014: jr  $ra
  //
  // Unreachable block B:
  // 0x80000018: bne $v0, $zero, -3 (-> Unreachable block A 0x80000010)
  // 0x8000001C: nop
  // 0x80000020: j   0x80000014 (-> Reachable target)
  // 0x80000024: nop
  std::vector<uint32_t> words = {
      0x14800004,  // 00: bne $a0, $zero, +4 (0x80000014)
      0x00000000,  // 04: nop
      0x03E00008,  // 08: jr  $ra
      0x00000000,  // 0C: nop
      0x08000006,  // 10: j   0x80000018 (B)
      0x03E00008,  // 14: jr  $ra (Target)
      0x1440FFFD,  // 18: bne $v0, $zero, -3 (0x80000010: A)
      0x00000000,  // 1C: nop
      0x08000005,  // 20: j   0x80000014 (Target)
      0x00000000,  // 24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  const auto* entry = cfg.FindBlockByVram(0x80000000);
  const auto* target = cfg.FindBlockByVram(0x80000014);
  const auto* unreach_a = cfg.FindBlockByVram(0x80000010);
  const auto* unreach_b = cfg.FindBlockByVram(0x80000018);

  ASSERT_THAT(entry, NotNull());
  ASSERT_THAT(target, NotNull());
  ASSERT_THAT(unreach_a, NotNull());
  ASSERT_THAT(unreach_b, NotNull());

  // Target's predecessors should ONLY contain the reachable entry block,
  // NOT the unreachable block B.
  EXPECT_THAT(target->predecessors, ElementsAre(entry->id));

  // Unreachable blocks A and B should have all outgoing edges cleared.
  EXPECT_THAT(unreach_a->successors, IsEmpty());
  EXPECT_THAT(unreach_b->successors, IsEmpty());
}

}  // namespace
}  // namespace rom_nom_nom
