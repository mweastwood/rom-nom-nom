#include "lifter/control_flow_structurer.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/control_flow_graph.h"
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
  EXPECT_THAT(s, HasSubstr("if (block_0) {"));
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

}  // namespace
}  // namespace rom_nom_nom
