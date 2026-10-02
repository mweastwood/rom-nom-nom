#include "lifter/dominator_tree.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/control_flow_graph.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Optional;

std::vector<Instruction> DecodeSequence(const std::vector<uint32_t>& words,
                                        uint32_t base_vram = 0x80000000) {
  std::vector<Instruction> instructions;
  for (size_t i = 0; i < words.size(); ++i) {
    auto inst_or = DecodeInstruction(words[i], base_vram + static_cast<uint32_t>(i * 4));
    EXPECT_TRUE(inst_or.ok());
    if (inst_or.ok()) {
      instructions.push_back(*inst_or);
    }
  }
  return instructions;
}

TEST(DominatorTreeTest, EmptyGraph) {
  ControlFlowGraph cfg;
  DominatorTree dt = DominatorTree::Compute(cfg);
  EXPECT_FALSE(dt.IsReachable(0));
  EXPECT_EQ(dt.ImmediateDominator(0), std::nullopt);
  EXPECT_THAT(dt.Children(0), IsEmpty());
  EXPECT_THAT(dt.DominanceFrontier(0), IsEmpty());
}

TEST(DominatorTreeTest, LinearSequence) {
  // 3-block linear sequence:
  // Block 0:
  // 0x00: beq $zero, $zero, +1 (to 0x08)
  // 0x04: nop
  // Block 1:
  // 0x08: beq $zero, $zero, +1 (to 0x10)
  // 0x0C: nop
  // Block 2:
  // 0x10: jr $ra
  // 0x14: nop
  std::vector<uint32_t> words = {
      0x10000001,  // 0x00: beq $zero, $zero, +1 (0x08)
      0x00000000,  // 0x04: nop
      0x10000001,  // 0x08: beq $zero, $zero, +1 (0x10)
      0x00000000,  // 0x0C: nop
      0x03E00008,  // 0x10: jr $ra
      0x00000000,  // 0x14: nop
  };

  auto insts = DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;
  ASSERT_EQ(cfg.Blocks().size(), 3u);

  // Forward Dominator Tree
  DominatorTree dt = DominatorTree::Compute(cfg);
  EXPECT_TRUE(dt.IsReachable(0));
  EXPECT_TRUE(dt.IsReachable(1));
  EXPECT_TRUE(dt.IsReachable(2));

  // Root has no idom
  EXPECT_EQ(dt.ImmediateDominator(0), std::nullopt);
  EXPECT_THAT(dt.ImmediateDominator(1), Optional(0u));
  EXPECT_THAT(dt.ImmediateDominator(2), Optional(1u));

  // Dominates queries
  EXPECT_TRUE(dt.Dominates(0, 0));
  EXPECT_TRUE(dt.Dominates(0, 1));
  EXPECT_TRUE(dt.Dominates(0, 2));
  EXPECT_TRUE(dt.Dominates(1, 1));
  EXPECT_TRUE(dt.Dominates(1, 2));
  EXPECT_FALSE(dt.Dominates(1, 0));
  EXPECT_FALSE(dt.Dominates(2, 1));

  // Strict dominance
  EXPECT_FALSE(dt.StrictlyDominates(0, 0));
  EXPECT_TRUE(dt.StrictlyDominates(0, 1));
  EXPECT_TRUE(dt.StrictlyDominates(0, 2));

  // Tree children
  EXPECT_THAT(dt.Children(0), ElementsAre(1u));
  EXPECT_THAT(dt.Children(1), ElementsAre(2u));
  EXPECT_THAT(dt.Children(2), IsEmpty());

  // Dominance frontiers in linear code are empty
  EXPECT_THAT(dt.DominanceFrontier(0), IsEmpty());
  EXPECT_THAT(dt.DominanceFrontier(1), IsEmpty());
  EXPECT_THAT(dt.DominanceFrontier(2), IsEmpty());

  // Post-Dominator Tree
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  EXPECT_THAT(post_dt.ImmediateDominator(0), Optional(1u));
  EXPECT_THAT(post_dt.ImmediateDominator(1), Optional(2u));
  EXPECT_EQ(post_dt.ImmediateDominator(2), std::nullopt);  // Exit node

  // Block 2 post-dominates everything
  EXPECT_TRUE(post_dt.Dominates(2, 0));
  EXPECT_TRUE(post_dt.Dominates(2, 1));
  EXPECT_TRUE(post_dt.Dominates(2, 2));
  EXPECT_FALSE(post_dt.Dominates(0, 2));
}

TEST(DominatorTreeTest, IfThenElseDiamond) {
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

  auto insts = DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;
  ASSERT_EQ(cfg.Blocks().size(), 4u);

  DominatorTree dt = DominatorTree::Compute(cfg);
  EXPECT_THAT(dt.ImmediateDominator(1), Optional(0u));
  EXPECT_THAT(dt.ImmediateDominator(2), Optional(0u));
  // Block 0 is the idom of merge Block 3 because both then and else come from Block 0
  EXPECT_THAT(dt.ImmediateDominator(3), Optional(0u));

  EXPECT_TRUE(dt.Dominates(0, 3));
  EXPECT_FALSE(dt.Dominates(1, 3));
  EXPECT_FALSE(dt.Dominates(2, 3));

  // Dominance Frontiers:
  // DF(1) = {3}, DF(2) = {3}, DF(0) = {}, DF(3) = {}
  EXPECT_THAT(dt.DominanceFrontier(0), IsEmpty());
  EXPECT_THAT(dt.DominanceFrontier(1), ElementsAre(3u));
  EXPECT_THAT(dt.DominanceFrontier(2), ElementsAre(3u));
  EXPECT_THAT(dt.DominanceFrontier(3), IsEmpty());

  // Post-Dominator Tree:
  // Block 3 (Merge/Exit) post-dominates Block 0, 1, 2
  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);
  EXPECT_THAT(post_dt.ImmediateDominator(0), Optional(3u));
  EXPECT_THAT(post_dt.ImmediateDominator(1), Optional(3u));
  EXPECT_THAT(post_dt.ImmediateDominator(2), Optional(3u));
  EXPECT_EQ(post_dt.ImmediateDominator(3), std::nullopt);

  EXPECT_TRUE(post_dt.Dominates(3, 0));
  EXPECT_TRUE(post_dt.Dominates(3, 1));
  EXPECT_TRUE(post_dt.Dominates(3, 2));
}

TEST(DominatorTreeTest, NaturalLoop) {
  // Loop structure:
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

  auto insts = DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;
  ASSERT_EQ(cfg.Blocks().size(), 4u);

  DominatorTree dt = DominatorTree::Compute(cfg);
  EXPECT_THAT(dt.ImmediateDominator(1), Optional(0u));
  EXPECT_THAT(dt.ImmediateDominator(2), Optional(1u));
  EXPECT_THAT(dt.ImmediateDominator(3), Optional(1u));

  // Loop Header (1) dominates Body (2) and Exit (3)
  EXPECT_TRUE(dt.Dominates(1, 2));
  EXPECT_TRUE(dt.Dominates(1, 3));
  // Body (2) does NOT dominate Header (1)
  EXPECT_FALSE(dt.Dominates(2, 1));

  // Latch Block 2 has Header Block 1 in its dominance frontier (back-edge)
  EXPECT_THAT(dt.DominanceFrontier(2), ElementsAre(1u));
}

TEST(DominatorTreeTest, MultipleExits) {
  // Function with two separate returns:
  // Block 0:
  // 0x00: bne $a0, $zero, +3 (to Return2 at 0x10)
  // 0x04: nop
  // Block 1 (Return1):
  // 0x08: jr $ra
  // 0x0C: nop
  // Block 2 (Return2):
  // 0x10: jr $ra
  // 0x14: nop
  std::vector<uint32_t> words = {
      0x14800003,  // 0x00: bne $a0, $zero, +3 (0x10)
      0x00000000,  // 0x04: nop
      0x03E00008,  // 0x08: jr $ra
      0x00000000,  // 0x0C: nop
      0x03E00008,  // 0x10: jr $ra
      0x00000000,  // 0x14: nop
  };

  auto insts = DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;
  ASSERT_EQ(cfg.Blocks().size(), 3u);

  DominatorTree post_dt = DominatorTree::ComputePostDominators(cfg);

  // Both Block 1 and Block 2 are return blocks.
  // Neither of them post-dominates Block 0.
  EXPECT_FALSE(post_dt.Dominates(1, 0));
  EXPECT_FALSE(post_dt.Dominates(2, 0));
  EXPECT_EQ(post_dt.ImmediateDominator(0), std::nullopt);
  EXPECT_EQ(post_dt.ImmediateDominator(1), std::nullopt);
  EXPECT_EQ(post_dt.ImmediateDominator(2), std::nullopt);
}

}  // namespace
}  // namespace rom_nom_nom
