#include "lifter/branch_condition_builder.h"

#include <cstdint>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(BranchConditionBuilderTest, FindsAndExtractsFoldedSltComparison) {
  // slti $at, $a0, 10
  // bne $at, $zero, target
  // nop
  std::vector<uint32_t> words = {
      0x2881000A,  // slti $at, $a0, 10
      0x14200002,  // bne $at, $zero, +2
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  EXPECT_TRUE(folded->can_suppress_statement);

  BasicBlock block;
  block.id = 0;
  block.instructions = insts;
  auto cond = BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/false);
  ASSERT_NE(cond, nullptr);
  EXPECT_EQ(cond->ToString(), "(arg0 < 10)");

  auto inv_cond = BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/true);
  ASSERT_NE(inv_cond, nullptr);
  EXPECT_EQ(inv_cond->ToString(), "(arg0 >= 10)");
}

TEST(BranchConditionBuilderTest, DecouplesDelaySlotClobberingConditionRegister) {
  // Block 0:
  //   beq $a0, $zero, 0x80000010
  //   addiu $a0, $zero, 1
  // Block 1:
  //   jr $ra
  //   nop
  // Block 2 (0x80000010):
  //   jr $ra
  //   nop
  std::vector<uint32_t> words = {
      0x10800002,  // beq $a0, $zero, 0x80000010
      0x24040001,  // addiu $a0, $zero, 1 (clobbers $a0 in delay slot)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_TRUE(BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(cfg, 0));
}

}  // namespace
}  // namespace rom_nom_nom
