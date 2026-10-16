#include "lifter/loop_analyzer.h"

#include <cstdint>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/dominator_tree.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::NotNull;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

TEST(LoopAnalyzerTest, AcyclicGraphHasNoLoops) {
  // Linear sequence: Block 0 -> Block 1
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x03E00008,  // 0x08: jr $ra
      0x00000000,  // 0x0C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  EXPECT_THAT(loops.AllLoops(), IsEmpty());
  EXPECT_THAT(loops.TopLevelLoops(), IsEmpty());
  EXPECT_FALSE(loops.IsInLoop(0));
  EXPECT_FALSE(loops.IsInLoop(1));
  EXPECT_EQ(loops.LoopDepth(0), 0u);
  EXPECT_EQ(loops.LoopDepth(1), 0u);
  EXPECT_EQ(loops.GetLoopFor(0), nullptr);
}

TEST(LoopAnalyzerTest, SimpleWhileLoop) {
  // While loop structure:
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
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  ASSERT_THAT(loops.TopLevelLoops(), SizeIs(1));

  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 1u);
  EXPECT_THAT(loop->latches, ElementsAre(2u));
  EXPECT_THAT(loop->blocks, UnorderedElementsAre(1u, 2u));
  EXPECT_THAT(loop->exit_blocks, ElementsAre(3u));
  EXPECT_EQ(loop->type, LoopType::kWhile);
  EXPECT_EQ(loop->depth, 1u);
  EXPECT_EQ(loop->parent, nullptr);
  EXPECT_THAT(loop->sub_loops, IsEmpty());

  EXPECT_FALSE(loops.IsInLoop(0));
  EXPECT_TRUE(loops.IsInLoop(1));
  EXPECT_TRUE(loops.IsInLoop(2));
  EXPECT_FALSE(loops.IsInLoop(3));

  EXPECT_EQ(loops.LoopDepth(0), 0u);
  EXPECT_EQ(loops.LoopDepth(1), 1u);
  EXPECT_EQ(loops.LoopDepth(2), 1u);
  EXPECT_EQ(loops.LoopDepth(3), 0u);

  EXPECT_EQ(loops.GetLoopFor(1), loop);
  EXPECT_EQ(loops.GetLoopFor(2), loop);
}

TEST(LoopAnalyzerTest, SingleBlockDoWhileLoop) {
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
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 1u);
  EXPECT_THAT(loop->latches, ElementsAre(1u));
  EXPECT_THAT(loop->blocks, UnorderedElementsAre(1u));
  EXPECT_THAT(loop->exit_blocks, ElementsAre(2u));
  EXPECT_EQ(loop->type, LoopType::kDoWhile);
}

TEST(LoopAnalyzerTest, MultiBlockDoWhileLoop) {
  // Multi-block do-while loop:
  // Block 0 (Entry):
  // 0x00: beq $zero, $zero, +1 (to Header at 0x08)
  // 0x04: nop
  // Block 1 (Header/Body):
  // 0x08: beq $zero, $zero, +1 (to Latch at 0x10)
  // 0x0C: nop
  // Block 2 (Latch):
  // 0x10: bne $a0, $zero, -3 (to Header at 0x08)
  // 0x14: nop (fallthrough to Exit at 0x18)
  // Block 3 (Exit):
  // 0x18: jr $ra
  // 0x1C: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x10000001,  // 0x08: beq $zero, $zero, +1 (0x10)
      0x00000000,  // 0x0C: nop
      0x1480FFFD,  // 0x10: bne $a0, $zero, -3 (0x08)
      0x00000000,  // 0x14: nop
      0x03E00008,  // 0x18: jr $ra
      0x00000000,  // 0x1C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;
  ASSERT_EQ(cfg.Blocks().size(), 4u);

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 1u);
  EXPECT_THAT(loop->latches, ElementsAre(2u));
  EXPECT_THAT(loop->blocks, UnorderedElementsAre(1u, 2u));
  EXPECT_THAT(loop->exit_blocks, ElementsAre(3u));
  EXPECT_EQ(loop->type, LoopType::kDoWhile);
}

TEST(LoopAnalyzerTest, InfiniteLoop) {
  // Infinite loop: Block 0 jumps to itself
  // 0x00: j 0x80000000
  // 0x04: nop
  std::vector<uint32_t> words = {
      0x08000000,  // 0x00: j 0x80000000 (target index 0)
      0x00000000,  // 0x04: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 0u);
  EXPECT_THAT(loop->latches, ElementsAre(0u));
  EXPECT_THAT(loop->blocks, UnorderedElementsAre(0u));
  EXPECT_THAT(loop->exit_blocks, IsEmpty());
  EXPECT_EQ(loop->type, LoopType::kInfinite);
}

TEST(LoopAnalyzerTest, NestedLoops) {
  // 2-level nested loop:
  // Block 0 (Entry):
  // 0x00: beq $zero, $zero, +1 (to OuterHeader at 0x08)
  // 0x04: nop
  // Block 1 (Outer Header):
  // 0x08: bne $a0, $zero, +8 (to Exit at 0x2C)
  // 0x0C: nop (fallthrough to Inner Header at 0x10)
  // Block 2 (Inner Header):
  // 0x10: bne $a1, $zero, +3 (to Inner Exit / Outer Latch at 0x20)
  // 0x14: nop (fallthrough to Inner Body at 0x18)
  // Block 3 (Inner Latch):
  // 0x18: j 0x80000010 (back-edge to Inner Header at 0x10)
  // 0x1C: nop
  // Block 4 (Outer Latch):
  // 0x20: j 0x80000008 (back-edge to Outer Header at 0x08)
  // 0x24: nop
  // Block 5 (Exit):
  // 0x28: jr $ra
  // 0x2C: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x14800007,  // 0x08: bne $a0, $zero, +7 (target 0x28)
      0x00000000,  // 0x0C: nop
      0x14A00003,  // 0x10: bne $a1, $zero, +3 (target 0x20)
      0x00000000,  // 0x14: nop
      0x08000004,  // 0x18: j 0x80000010 (target index 4)
      0x00000000,  // 0x1C: nop
      0x08000002,  // 0x20: j 0x80000008 (target index 2)
      0x00000000,  // 0x24: nop
      0x03E00008,  // 0x28: jr $ra
      0x00000000,  // 0x2C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;
  ASSERT_EQ(cfg.Blocks().size(), 6u);

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(2));
  ASSERT_THAT(loops.TopLevelLoops(), SizeIs(1));

  const Loop* outer = loops.TopLevelLoops()[0];
  EXPECT_EQ(outer->header, 1u);
  EXPECT_EQ(outer->depth, 1u);
  EXPECT_EQ(outer->parent, nullptr);
  EXPECT_THAT(outer->blocks, UnorderedElementsAre(1u, 2u, 3u, 4u));
  ASSERT_THAT(outer->sub_loops, SizeIs(1));

  const Loop* inner = outer->sub_loops[0];
  EXPECT_EQ(inner->header, 2u);
  EXPECT_EQ(inner->depth, 2u);
  EXPECT_EQ(inner->parent, outer);
  EXPECT_THAT(inner->blocks, UnorderedElementsAre(2u, 3u));

  // Depth queries
  EXPECT_EQ(loops.LoopDepth(0), 0u);
  EXPECT_EQ(loops.LoopDepth(1), 1u);
  EXPECT_EQ(loops.LoopDepth(2), 2u);
  EXPECT_EQ(loops.LoopDepth(3), 2u);
  EXPECT_EQ(loops.LoopDepth(4), 1u);
  EXPECT_EQ(loops.LoopDepth(5), 0u);

  // Innermost loop queries
  EXPECT_EQ(loops.GetLoopFor(2), inner);
  EXPECT_EQ(loops.GetLoopFor(3), inner);
  EXPECT_EQ(loops.GetLoopFor(1), outer);
  EXPECT_EQ(loops.GetLoopFor(4), outer);
}

TEST(LoopAnalyzerTest, TrampolineJumpLatchFolding) {
  // Loop with a synthesized unconditional jump trampoline to the header:
  // Block 0 (Entry):
  // 0x00: beq $zero, $zero, +1 (0x08 -> Header)
  // 0x04: nop
  // Block 1 (Header):
  // 0x08: addiu $v0, $v0, 1
  // 0x0C: beq $zero, $zero, +1 (0x14 -> Body)
  // 0x10: nop
  // Block 2 (Body / Branching Pred):
  // 0x14: bne $v0, $a0, +3 (0x24 -> Trampoline)
  // 0x18: nop (fallthrough to Exit at 0x1C)
  // Block 3 (Exit):
  // 0x1C: jr $ra
  // 0x20: nop
  // Block 4 (Trampoline Latch):
  // 0x24: j 0x80000008 (target Header at 0x08)
  // 0x28: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x24420001,  // 0x08: addiu $v0, $v0, 1
      0x10000001,  // 0x0C: beq $zero, $zero, +1 (0x14)
      0x00000000,  // 0x10: nop
      0x14440003,  // 0x14: bne $v0, $a0, +3 (0x24)
      0x00000000,  // 0x18: nop
      0x03E00008,  // 0x1C: jr $ra
      0x00000000,  // 0x20: nop
      0x08000002,  // 0x24: j 0x80000008 (index 2)
      0x00000000,  // 0x28: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 1u);
  // Trampoline block 4 was folded, latch replaced with predecessor block 2
  EXPECT_THAT(loop->latches, ElementsAre(2u));
  // Both body block 2 and trampoline block 4 are in loop blocks
  EXPECT_TRUE(loop->Contains(1u));
  EXPECT_TRUE(loop->Contains(2u));
  EXPECT_TRUE(loop->Contains(4u));
  // Loop type is kDoWhile because latch block 2 exits to block 3
  EXPECT_EQ(loop->type, LoopType::kDoWhile);
}

TEST(LoopAnalyzerTest, TrampolineNotFoldedWhenInstructionsExceedThreshold) {
  // Latch has > 2 instructions before/including j, so it is not a pure trampoline.
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08 -> Header)
      0x00000000,  // 0x04: nop
      0x24420001,  // 0x08: addiu $v0, $v0, 1
      0x10000001,  // 0x0C: beq $zero, $zero, +1 (0x14 -> Body)
      0x00000000,  // 0x10: nop
      0x14440003,  // 0x14: bne $v0, $a0, +3 (0x24 -> Latch)
      0x00000000,  // 0x18: nop (fallthrough to Exit at 0x1C)
      0x03E00008,  // 0x1C: jr $ra
      0x00000000,  // 0x20: nop
      0x24630001,  // 0x24: addiu $v1, $v1, 1
      0x24630002,  // 0x28: addiu $v1, $v1, 2
      0x08000002,  // 0x2C: j 0x80000008 (target Header at 0x08)
      0x00000000,  // 0x30: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 1u);
  // Latch has 4 instructions (> 2 threshold), so block 4 remains the latch (not folded)
  EXPECT_THAT(loop->latches, ElementsAre(4u));
}

TEST(LoopAnalyzerTest, TrampolineNotFoldedWhenMultiplePredecessors) {
  // Latch has 2 predecessors (shared merge point), so it is not a dedicated trampoline.
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08 -> Header)
      0x00000000,  // 0x04: nop
      0x14800005,  // 0x08: bne $a0, $zero, +5 (0x20 -> Trampoline)
      0x00000000,  // 0x0C: nop (fallthrough to Body at 0x10)
      0x14A00003,  // 0x10: bne $a1, $zero, +3 (0x20 -> Trampoline)
      0x00000000,  // 0x14: nop (fallthrough to Exit at 0x18)
      0x03E00008,  // 0x18: jr $ra
      0x00000000,  // 0x1C: nop
      0x08000002,  // 0x20: j 0x80000008 (target Header at 0x08)
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 1u);
  // Trampoline has multiple predecessors, so it is not folded
  EXPECT_THAT(loop->latches, ElementsAre(4u));
}

TEST(LoopAnalyzerTest, LoopClassificationPrioritizesLatchExitOverHeaderExit) {
  // Loop where both header and latch have exit edges.
  // Latch exit takes priority, classifying it as kDoWhile.
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08 -> Header)
      0x00000000,  // 0x04: nop
      0x14800005,  // 0x08: bne $a0, $zero, +5 (0x20 -> Exit 1)
      0x00000000,  // 0x0C: nop (fallthrough to Latch at 0x10)
      0x14A0FFFD,  // 0x10: bne $a1, $zero, -3 (0x08 -> Header)
      0x00000000,  // 0x14: nop (fallthrough to Exit 2 at 0x18)
      0x03E00008,  // 0x18: jr $ra (Exit 2)
      0x00000000,  // 0x1C: nop
      0x03E00008,  // 0x20: jr $ra (Exit 1)
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->type, LoopType::kDoWhile);
}

TEST(LoopAnalyzerTest, LoopClassificationFallsBackToInfiniteWhenInteriorBlockExits) {
  // Loop where neither header nor latch exits, but an interior body block exits (break).
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08 -> Header)
      0x00000000,  // 0x04: nop
      0x24420001,  // 0x08: addiu $v0, $v0, 1
      0x10000001,  // 0x0C: beq $zero, $zero, +1 (0x14 -> Interior)
      0x00000000,  // 0x10: nop
      0x14440004,  // 0x14: bne $v0, $a0, +4 (0x28 -> Exit)
      0x00000000,  // 0x18: nop (fallthrough to Latch at 0x1C)
      0x24630001,  // 0x1C: addiu $v1, $v1, 1 (Latch)
      0x24630002,  // 0x20: addiu $v1, $v1, 2
      0x08000002,  // 0x24: j 0x80000008 (target Header at 0x08)
      0x00000000,  // 0x28: nop
      0x03E00008,  // 0x2C: jr $ra (Exit)
      0x00000000,  // 0x30: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dt = DominatorTree::Compute(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dt);

  ASSERT_THAT(loops.AllLoops(), SizeIs(1));
  const Loop* loop = loops.AllLoops()[0].get();
  EXPECT_EQ(loop->header, 1u);
  // Neither header (block 1) nor latch (block 3) exits; only interior block 2 exits
  EXPECT_EQ(loop->type, LoopType::kInfinite);
}

}  // namespace
}  // namespace rom_nom_nom
