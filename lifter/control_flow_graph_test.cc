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

}  // namespace
}  // namespace rom_nom_nom
