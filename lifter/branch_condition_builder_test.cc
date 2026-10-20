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

TEST(BranchConditionBuilderTest,
     DoesNotSuppressTemporaryDataRegistersWhileSuppressingEphemeralComparisonRegisters) {
  // 1. andi $t0, $v1, 0xFF feeding beqz $t0:
  // Even though it is folded into the condition, $t0 is a temporary data register that may be live
  // out and read later, so its statement emission must NOT be suppressed.
  std::vector<uint32_t> t0_words = {
      0x304800FF,  // andi $t0, $v1, 0xFF
      0x11000002,  // beq  $t0, $zero, +2
      0x00000000,  // nop
  };
  auto t0_insts = *DecodeSequence(t0_words, 0x80000000);
  auto t0_folded = BranchConditionBuilder::FindFoldedComparison(t0_insts);
  ASSERT_TRUE(t0_folded.has_value());
  EXPECT_EQ(t0_folded->instruction_index, 0u);
  EXPECT_FALSE(t0_folded->can_suppress_statement);

  // 2. slt $v1, $a1, $v0 feeding beq $v1, $zero:
  // $v1 from slt/sltu is an ephemeral branch condition temporary and can be safely suppressed.
  std::vector<uint32_t> v1_words = {
      0x00A2182A,  // slt $v1, $a1, $v0
      0x10600002,  // beq $v1, $zero, +2
      0x00000000,  // nop
  };
  auto v1_insts = *DecodeSequence(v1_words, 0x80000000);
  auto v1_folded = BranchConditionBuilder::FindFoldedComparison(v1_insts);
  ASSERT_TRUE(v1_folded.has_value());
  EXPECT_EQ(v1_folded->instruction_index, 0u);
  EXPECT_TRUE(v1_folded->can_suppress_statement);

  // 3. slti $at, $a0, 10 feeding bne $at, $zero:
  // $at is assembler temporary and can always be suppressed.
  std::vector<uint32_t> at_words = {
      0x2881000A,  // slti $at, $a0, 10
      0x14200002,  // bne  $at, $zero, +2
      0x00000000,  // nop
  };
  auto at_insts = *DecodeSequence(at_words, 0x80000000);
  auto at_folded = BranchConditionBuilder::FindFoldedComparison(at_insts);
  ASSERT_TRUE(at_folded.has_value());
  EXPECT_EQ(at_folded->instruction_index, 0u);
  EXPECT_TRUE(at_folded->can_suppress_statement);
}

TEST(BranchConditionBuilderTest, DoesNotDecoupleLoopLatchDelaySlotWhenLoopBodyCallRedefinesV0) {
  // Block 0 (entry):
  //   0x80000000: move $s1, $zero
  //   0x80000004: move $s0, $zero
  // Block 1 (do-while loop body, 0x80000008):
  //   0x80000008: jal 0x80001000
  //   0x8000000C: move $a0, $s0
  //   0x80000010: addu $s1, $s1, $v0
  //   0x80000014: addiu $s0, $s0, 1
  //   0x80000018: bne $s0, $s2, 0x80000008
  //   0x8000001C: move $v0, $s1
  // Block 2 (epilogue, 0x80000020):
  //   0x80000020: jr $ra
  //   0x80000024: nop
  std::vector<uint32_t> words = {
      0x00008821,  // addu $s1, $zero, $zero
      0x00008021,  // addu $s0, $zero, $zero
      0x0C000400,  // jal 0x80001000
      0x02002021,  // addu $a0, $s0, $zero
      0x02228821,  // addu $s1, $s1, $v0
      0x26100001,  // addiu $s0, $s0, 1
      0x1612FFFB,  // bne $s0, $s2, 0x80000008
      0x02201021,  // addu $v0, $s1, $zero
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_FALSE(BranchConditionBuilder::ShouldDecoupleLoopConditionDelaySlot(cfg, 1, 1));
}

}  // namespace
}  // namespace rom_nom_nom
