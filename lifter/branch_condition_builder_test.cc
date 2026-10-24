#include "lifter/branch_condition_builder.h"

#include <cstdint>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/expression_builder.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

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

TEST(BranchConditionBuilderTest, DecouplesDelaySlotWritingCallArgWhenPredecessorEndsWithCall) {
  // Block 0 (0x80000000):
  //   0x80000000: jal 0x80001000
  //   0x80000004: nop
  // Block 1 (0x80000008, split after jal by ControlFlowGraph::Build):
  //   0x80000008: beq  $v0, $zero, 0x80000018
  //   0x8000000C: addu $a0, $s0, $zero
  // Block 2 (0x80000010):
  //   0x80000010: jal 0x80002000
  //   0x80000014: nop
  // Block 3 (0x80000018):
  //   0x80000018: jr $ra
  //   0x8000001C: nop
  std::vector<uint32_t> words = {
      0x0C000400,  // jal  0x80001000
      0x00000000,  // nop
      0x10400003,  // beq  $v0, $zero, 0x80000018
      0x02002021,  // addu $a0, $s0, $zero
      0x0C000800,  // jal  0x80002000
      0x00000000,  // nop
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_TRUE(BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(cfg, 1));
}

TEST(BranchConditionBuilderTest,
     DecouplesDelaySlotModifingExtractedConditionOrFollowingFoldedSltAfterCall) {
  // 1. bc1f with c.lt.s in delay slot (modifies fcond referenced by extracted condition):
  std::vector<uint32_t> fcond_words = {
      0x45000002,  // 00: bc1f  +2 -> 0x0C
      0x4602603C,  // 04: c.lt.s $f12, $f2 (modifies fcond in delay slot)
      0x03E00008,  // 08: jr    $ra
      0x00000000,  // 0C: nop
      0x03E00008,  // 10: jr    $ra
      0x00000000,  // 14: nop
  };
  auto fcond_cfg = *ControlFlowGraph::Build(*DecodeSequence(fcond_words, 0x80000000));
  EXPECT_TRUE(BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(fcond_cfg, 0));

  // 2. 3-instruction condition block (slti $at, $v0, 5; beq $at, $zero; mov.s $f12, $f20)
  // following a predecessor with a call and leading to a successor with a call:
  std::vector<uint32_t> call_slt_words = {
      0x0C000400,  // 00 (block 0): jal   0x80001000
      0x00000000,  // 04:           nop
      0x28410005,  // 08 (block 1): slti  $at, $v0, 5
      0x10200003,  // 0C:           beq   $at, $zero, +3 -> 0x1C
      0x4600A306,  // 10:           mov.s $f12, $f20 (defines float arg $f12 in delay slot)
      0x0C000800,  // 14 (block 2): jal   0x80002000
      0x00000000,  // 18:           nop
      0x03E00008,  // 1C (block 3): jr    $ra
      0x00000000,  // 20:           nop
  };
  auto call_slt_cfg = *ControlFlowGraph::Build(*DecodeSequence(call_slt_words, 0x80000000));
  EXPECT_TRUE(BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(call_slt_cfg, 1));
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonSltAtSuppressed) {
  // slt $at, $a0, $a1
  // beq $at, $zero, +2
  // nop
  std::vector<uint32_t> words = {
      0x0085082A,  // 0: slt $at, $a0, $a1
      0x10200002,  // 4: beq $at, $zero, +2
      0x00000000,  // 8: nop
  };

  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  EXPECT_TRUE(folded->can_suppress_statement);

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  // temp_at = arg0 < arg1 statement should be suppressed
  EXPECT_THAT(statements, SizeIs(0));
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonV0PreservedWhenNotOverwritten) {
  // sltiu $v0, $a0, 42
  // bne   $v0, $zero, +2
  // nop
  std::vector<uint32_t> words = {
      0x2C82002A,  // 0: sltiu $v0, $a0, 42
      0x14400002,  // 4: bne   $v0, $zero, +2
      0x00000000,  // 8: nop
  };

  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  // $v0 is not $at and not overwritten in delay slot, so statement is preserved
  EXPECT_FALSE(folded->can_suppress_statement);

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].destination_variable, "v0");
  EXPECT_EQ(statements[0].ToString(), "v0 = (u32)arg0 < 42;\n");
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonV0SuppressedWhenOverwrittenInDelaySlot) {
  // Pattern matching MainReset:
  // sltiu $v0, $v0, 57
  // bnel  $v0, $zero, -4
  // andi  $v0, $v1, 0xFF   (delay slot overwrites $v0)
  std::vector<uint32_t> words = {
      0x2C420039,  // 0: sltiu $v0, $v0, 57
      0x5440FFFC,  // 4: bnel  $v0, $zero, -4
      0x306200FF,  // 8: andi  $v0, $v1, 0xFF
  };

  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  // Overwritten in delay slot before any exit, so statement can be safely suppressed
  EXPECT_TRUE(folded->can_suppress_statement);

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  // The sltiu statement is suppressed; only the delay slot andi statement is emitted
  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "v0 = v1 & 255;\n");
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonHazardRejectsFolding) {
  // slt   $at, $a0, $a1
  // addiu $a0, $zero, 5   (hazard: overwrites input $a0!)
  // bne   $at, $zero, +2
  // nop
  std::vector<uint32_t> words = {
      0x0085082A,  // 0: slt   $at, $a0, $a1
      0x24040005,  // 4: addiu $a0, $zero, 5
      0x14200002,  // 8: bne   $at, $zero, +2
      0x00000000,  // C: nop
  };

  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  // Hazard on $a0 prevents folding
  EXPECT_FALSE(folded.has_value());

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].destination_variable, "temp_at");
  EXPECT_EQ(statements[1].destination_variable, "arg0");
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonIntermediateUseRejectsFolding) {
  // slt   $at, $a0, $a1
  // addu  $v0, $at, $zero  (intermediate use of $at!)
  // bne   $at, $zero, +2
  // nop
  std::vector<uint32_t> words = {
      0x0085082A,  // 0: slt  $at, $a0, $a1
      0x00201021,  // 4: addu $v0, $at, $zero
      0x14200002,  // 8: bne  $at, $zero, +2
      0x00000000,  // C: nop
  };

  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  // Intermediate use of $at prevents folding
  EXPECT_FALSE(folded.has_value());

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].destination_variable, "temp_at");
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonXoriAndAndi) {
  // xori $at, $a0, 5
  // bne  $at, $zero, +2
  // nop
  std::vector<uint32_t> xor_words = {
      0x38810005,  // 0: xori $at, $a0, 5
      0x14200002,  // 4: bne  $at, $zero, +2
      0x00000000,  // 8: nop
  };

  auto xor_insts = *DecodeSequence(xor_words);
  auto xor_folded = BranchConditionBuilder::FindFoldedComparison(xor_insts);
  ASSERT_TRUE(xor_folded.has_value());
  EXPECT_EQ(xor_folded->instruction_index, 0u);
  EXPECT_TRUE(xor_folded->can_suppress_statement);

  // andi $at, $a0, 0x8
  // beq  $at, $zero, +2
  // nop
  std::vector<uint32_t> and_words = {
      0x30810008,  // 0: andi $at, $a0, 8
      0x10200002,  // 4: beq  $at, $zero, +2
      0x00000000,  // 8: nop
  };

  auto and_insts = *DecodeSequence(and_words);
  auto and_folded = BranchConditionBuilder::FindFoldedComparison(and_insts);
  ASSERT_TRUE(and_folded.has_value());
  EXPECT_EQ(and_folded->instruction_index, 0u);
  EXPECT_TRUE(and_folded->can_suppress_statement);
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonWithInterveningFpuInstruction) {
  // sltiu $v0, $v0, 176
  // swc1  $f2, 0($a0)   (ft = $f2, whose index 2 must NOT alias with GPR $v0)
  // bnez  $v0, +2
  // andi  $v0, $a0, 0xFFFF (delay slot defines GPR $v0, making comparison dead)
  std::vector<uint32_t> words = {
      0x2C4200B0,  // 0: sltiu $v0, $v0, 176
      0xE4820000,  // 4: swc1  $f2, 0($a0)
      0x14400002,  // 8: bne   $v0, $zero, +2
      0x3082FFFF,  // C: andi  $v0, $a0, 0xFFFF
  };

  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  EXPECT_TRUE(folded->can_suppress_statement);
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonCop1Branch) {
  std::vector<uint32_t> words = {
      0x4602003C,  // 0: c.lt.s $f0, $f2
      0x00000000,  // 4: nop
      0x45010002,  // 8: bc1t +2
      0x00000000,  // C: nop
  };
  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  EXPECT_TRUE(folded->can_suppress_statement);
}

TEST(BranchConditionBuilderTest,
     FindFoldedComparisonDoesNotFoldSelfMutatingInstructionWithoutClobber) {
  // Pattern matching func_8002CDB4:
  // andi  $a0, $a0, 0xFFFF
  // beqz  $a0, +6
  // addu  $v1, $zero, $zero
  // Because $a0 is mutated in place and the delay slot does not redefine $a0,
  // the andi instruction must not be folded/suppressed.
  std::vector<uint32_t> words = {
      0x3084FFFF,  // 0: andi $a0, $a0, 0xFFFF
      0x10800006,  // 4: beqz $a0, +6
      0x00001821,  // 8: addu $v1, $zero, $zero
  };
  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  EXPECT_FALSE(folded.has_value());
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonDoesNotFoldWhenDelaySlotClobbersInput) {
  // Pattern matching GraphicSetSpriteDescriptor:
  // andi  $v1, $v0, 0xF
  // beqz  $v1, +5
  // addiu $v0, $zero, 1
  // Because $v0 is an input to andi and the delay slot redefines $v0,
  // the andi instruction must not be folded/suppressed.
  std::vector<uint32_t> words = {
      0x3043000F,  // 0: andi  $v1, $v0, 0xF
      0x10600005,  // 4: beqz  $v1, +5
      0x24020001,  // 8: addiu $v0, $zero, 1
  };
  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  EXPECT_FALSE(folded.has_value());
}

TEST(BranchConditionBuilderTest, FindFoldedComparisonFoldsWhenDelaySlotDoesNotClobberInput) {
  // Same pattern but delay slot does not redefine $v0 or $v1:
  // andi  $v1, $v0, 0xF
  // beqz  $v1, +5
  // addiu $a0, $zero, 1
  std::vector<uint32_t> words = {
      0x3043000F,  // 0: andi  $v1, $v0, 0xF
      0x10600005,  // 4: beqz  $v1, +5
      0x24040001,  // 8: addiu $a0, $zero, 1
  };
  auto insts = *DecodeSequence(words);
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
  EXPECT_TRUE(folded.has_value());
}

}  // namespace
}  // namespace rom_nom_nom
