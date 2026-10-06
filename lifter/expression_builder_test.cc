#include "lifter/expression_builder.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::SizeIs;

TEST(ExpressionBuilderTest, ArithmeticOperations) {
  std::vector<uint32_t> words = {
      0x00851021,  // 0: addu $v0, $a0, $a1
      0x00851823,  // 4: subu $v1, $a0, $a1
      0x00042080,  // 8: sll  $a0, $a0, 2
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 + arg1;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = arg0 - arg1;\n");
  EXPECT_EQ(statements[2].ToString(), "arg0 = arg0 << 2;\n");
}

TEST(ExpressionBuilderTest, FoldedSymbolAccess) {
  std::string textproto = R"pb(
    entries { name: "g_audio_status" address: 0x801F2340 type: SYMBOL_DATA size: 64 }
  )pb";

  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x3C01801F,  // 0: lui $at, 0x801F
      0x8C222340,  // 4: lw  $v0, 0x2340($at)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts, &(*index_or));

  // lui is folded, so only the loaded symbol statement is emitted
  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "v0 = g_audio_status;\n");
}

TEST(ExpressionBuilderTest, StackVariableAccess) {
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 0: addiu $sp, $sp, -32 (prologue)
      0xAFA40010,  // 4: sw    $a0, 16($sp)
      0x8FA20010,  // 8: lw    $v0, 16($sp)
      0x03E00008,  // C: jr    $ra
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "var_sp_16 = arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "v0 = var_sp_16;\n");
  EXPECT_EQ(statements[2].ToString(), "return v0;\n");
}

TEST(ExpressionBuilderTest, StackVariableAccessNegativeOffset) {
  std::vector<uint32_t> instruction_words = {
      0xAFA4FFF0,  // 0: sw    $a0, -16($sp)
      0x8FA2FFF0,  // 4: lw    $v0, -16($sp)
      0x27A4FFF0,  // 8: addiu $a0, $sp, -16
  };

  auto instructions = *DecodeSequence(instruction_words);
  auto statements = ExpressionBuilder::LiftInstructions(instructions);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "var_sp_neg_16 = arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "v0 = var_sp_neg_16;\n");
  EXPECT_EQ(statements[2].ToString(), "arg0 = &var_sp_neg_16;\n");
}

TEST(ExpressionBuilderTest, FunctionCallAndReturn) {
  std::string textproto = R"pb(
    entries { name: "AudioUpdate" address: 0x80001000 type: SYMBOL_FUNC size: 128 }
  )pb";

  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x24040005,  // 0: addiu $a0, $zero, 5
      0x0C000400,  // 4: jal   0x80001000
      0x00000000,  // 8: nop
      0x03E00008,  // C: jr    $ra
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts, &(*index_or));

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "arg0 = 5;\n");
  EXPECT_EQ(statements[1].ToString(), "AudioUpdate(5);\n");
  EXPECT_EQ(statements[2].ToString(), "return;\n");
}

TEST(ExpressionBuilderTest, SizedLoadsAndStores) {
  std::vector<uint32_t> words = {
      0x84820004,  // lh  $v0, 4($a0)
      0x94830008,  // lhu $v1, 8($a0)
      0x80880001,  // lb  $t0, 1($a0)
      0x90890002,  // lbu $t1, 2($a0)
      0xA4850004,  // sh  $a1, 4($a0)
      0xA0860001,  // sb  $a2, 1($a0)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(6));
  EXPECT_EQ(statements[0].ToString(), "v0 = *(s16*)(arg0 + 4);\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = *(u16*)(arg0 + 8);\n");
  EXPECT_EQ(statements[2].ToString(), "temp_t0 = *(s8*)(arg0 + 1);\n");
  EXPECT_EQ(statements[3].ToString(), "temp_t1 = *(u8*)(arg0 + 2);\n");
  EXPECT_EQ(statements[4].store_type, "s16");
  EXPECT_EQ(statements[5].store_type, "s8");
}

TEST(ExpressionBuilderTest, DelaySlotExecutionBeforeReturn) {
  // jr $ra followed by delay-slot: sh $a1, 4($a0)
  std::vector<uint32_t> words = {
      0x03E00008,  // 0: jr $ra
      0xA4850004,  // 4: sh $a1, 4($a0)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(2));
  // The delay-slot store must be emitted BEFORE the return statement
  EXPECT_EQ(statements[0].kind, StatementKind::kStore);
  EXPECT_EQ(statements[0].store_type, "s16");
  EXPECT_EQ(statements[1].kind, StatementKind::kReturn);
}

TEST(ExpressionBuilderTest, StackVariableAddressingUnaryExpression) {
  // addiu $a0, $sp, 16 -> arg0 = &var_sp_16
  std::vector<uint32_t> words = {
      0x27A40010,  // addiu $a0, $sp, 16
  };

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(instructions);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].destination_variable, "arg0");
  ASSERT_THAT(statements[0].expression, testing::NotNull());
  EXPECT_EQ(statements[0].expression->kind, ExpressionKind::kUnaryOp);
  EXPECT_EQ(statements[0].expression->op, "&");
  ASSERT_THAT(statements[0].expression->args, SizeIs(1));
  ASSERT_THAT(statements[0].expression->args[0], testing::NotNull());
  EXPECT_EQ(statements[0].expression->args[0]->kind, ExpressionKind::kVariable);
  EXPECT_EQ(statements[0].expression->args[0]->name, "var_sp_16");
  EXPECT_EQ(statements[0].ToString(), "arg0 = &var_sp_16;\n");
}

TEST(ExpressionBuilderTest, AssignmentWithEmptyOrZeroDestinationEmitsBareExpression) {
  LiftedStatement statement;
  statement.kind = StatementKind::kAssignment;
  statement.destination_variable = "0";
  statement.expression = LiftedExpression::Call("DoWork", {});

  EXPECT_EQ(statement.ToString(), "DoWork();\n");

  statement.destination_variable = "";
  EXPECT_EQ(statement.ToString(), "DoWork();\n");
}

TEST(ExpressionBuilderTest, DetermineParameterCountIdentifiesUsedArguments) {
  // 1 arg: addu $v0, $a0, $zero
  std::vector<uint32_t> one_arg_words = {
      0x00801021,  // addu $v0, $a0, $zero
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto one_arg_insts = *DecodeSequence(one_arg_words);
  EXPECT_EQ(ExpressionBuilder::DetermineParameterCount(one_arg_insts), 1);

  // 3 args: addu $v0, $a0, $a1; addu $v0, $v0, $a2
  std::vector<uint32_t> three_args_words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x00461021,  // addu $v0, $v0, $a2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto three_args_insts = *DecodeSequence(three_args_words);
  EXPECT_EQ(ExpressionBuilder::DetermineParameterCount(three_args_insts), 3);

  // 0 args: addiu $v0, $zero, 1
  std::vector<uint32_t> zero_args_words = {
      0x24020001,  // addiu $v0, $zero, 1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto zero_args_insts = *DecodeSequence(zero_args_words);
  EXPECT_EQ(ExpressionBuilder::DetermineParameterCount(zero_args_insts), 0);
}

TEST(ExpressionBuilderTest, JalDelaySlotReorderingPassesArgumentToCall) {
  // Call target: 0x80001000
  // 0: jal   0x80001000
  // 4: addiu $a0, $zero, 42  (delay-slot executes BEFORE call!)
  // 8: jr    $ra
  // C: nop
  std::vector<uint32_t> words = {
      0x0C000400,  // 0: jal   0x80001000
      0x2404002A,  // 4: addiu $a0, $zero, 42
      0x03E00008,  // 8: jr    $ra
      0x00000000,  // C: nop
  };

  std::string textproto = R"pb(
    entries { name: "TargetFunc" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(instructions, &(*index_or));

  // The delay-slot assignment arg0 = 42 must execute before TargetFunc, so the call passes 42
  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "arg0 = 42;\n");
  EXPECT_EQ(statements[1].ToString(), "TargetFunc(42);\n");
  EXPECT_EQ(statements[2].ToString(), "return;\n");
}

TEST(ExpressionBuilderTest, JalParameterArityUsesKnownCount) {
  // Setup a call with reaching definitions for $a0, $a1, $a2
  std::vector<uint32_t> words = {
      0x24040001,  // 0: addiu $a0, $zero, 1
      0x24050002,  // 4: addiu $a1, $zero, 2
      0x24060003,  // 8: addiu $a2, $zero, 3
      0x0C000400,  // C: jal   0x80001000
      0x00000000,  // 10: nop
  };

  std::string textproto = R"pb(
    entries { name: "BinaryFunc" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // Tell expression builder that BinaryFunc takes only 2 parameters
  absl::flat_hash_map<std::string, int> parameter_counts;
  parameter_counts["BinaryFunc"] = 2;

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(
      instructions, &(*index_or), /*split_config=*/nullptr, &parameter_counts);

  ASSERT_THAT(statements, SizeIs(4));
  EXPECT_EQ(statements[3].ToString(), "BinaryFunc(1, 2);\n");
}

TEST(ExpressionBuilderTest, LogicalAndiOperation) {
  std::vector<uint32_t> words = {
      0x00851024,  // 0: and  $v0, $a0, $a1
      0x3083000F,  // 4: andi $v1, $a0, 15
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 & arg1;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = arg0 & 15;\n");
}

TEST(ExpressionBuilderTest, LogicalImmediateZeroExtension) {
  // Test that logical immediate instructions (andi, ori, xori) with bit 15 set
  // (e.g. 0xFFFF) are zero-extended to 65535, NOT sign-extended to 4294967295 (-1).
  std::vector<uint32_t> words = {
      0x3082FFFF,  // 0: andi $v0, $a0, 0xFFFF
      0x3403FFFF,  // 4: ori  $v1, $zero, 0xFFFF
      0x38848000,  // 8: xori $a0, $a0, 0x8000
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 & 65535;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = 0 | 65535;\n");
  EXPECT_EQ(statements[2].ToString(), "arg0 = arg0 ^ 32768;\n");
}

TEST(ExpressionBuilderTest, ComparisonOperations) {
  std::vector<uint32_t> words = {
      0x0085102A,  // 0: slt   $v0, $a0, $a1
      0x28830005,  // 4: slti  $v1, $a0, 5
      0x0085102B,  // 8: sltu  $v0, $a0, $a1
      0x2C83000A,  // C: sltiu $v1, $a0, 10
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(4));
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 < arg1;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = arg0 < 5;\n");
  EXPECT_EQ(statements[2].ToString(), "v0 = arg0 < arg1;\n");
  EXPECT_EQ(statements[3].ToString(), "v1 = arg0 < 10;\n");
}

TEST(ExpressionBuilderTest, StandaloneLuiOperation) {
  std::vector<uint32_t> words = {
      0x3C02801F,  // 0: lui $v0, 0x801F
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "v0 = 0x801F0000;\n");
}

TEST(ExpressionBuilderTest, FoldedSymbolAddressLoad) {
  std::string textproto = R"pb(
    entries { name: "g_audio_status" address: 0x801F2340 type: SYMBOL_DATA size: 64 }
  )pb";

  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x3C01801F,  // 0: lui   $at, 0x801F
      0x24242340,  // 4: addiu $a0, $at, 0x2340
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts, &(*index_or));

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "arg0 = &g_audio_status;\n");
  EXPECT_EQ(statements[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(statements[0].destination_variable, "arg0");
  ASSERT_NE(statements[0].expression, nullptr);
  EXPECT_EQ(statements[0].expression->kind, ExpressionKind::kUnaryOp);
  EXPECT_EQ(statements[0].expression->op, "&");
  ASSERT_THAT(statements[0].expression->args, SizeIs(1));
  ASSERT_NE(statements[0].expression->args[0], nullptr);
  EXPECT_EQ(statements[0].expression->args[0]->kind, ExpressionKind::kGlobalRef);
  EXPECT_EQ(statements[0].expression->args[0]->name, "g_audio_status");
}

TEST(ExpressionBuilderTest, FoldedSymbolStoreZero) {
  std::string textproto = R"pb(
    entries { name: "g_audio_status" address: 0x801F2340 type: SYMBOL_DATA size: 64 }
  )pb";

  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x3C01801F,  // 0: lui $at, 0x801F
      0xAC202340,  // 4: sw  $zero, 0x2340($at)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts, &(*index_or));

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "g_audio_status = 0;\n");
  EXPECT_EQ(statements[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(statements[0].destination_variable, "g_audio_status");
  ASSERT_NE(statements[0].expression, nullptr);
  EXPECT_EQ(statements[0].expression->kind, ExpressionKind::kIntegerLiteral);
  EXPECT_EQ(statements[0].expression->int_val, 0u);
}

TEST(ExpressionBuilderTest, DetermineReturnsV0MultiBlock) {
  // Abs16-like pattern:
  // Block 0:
  //   sll   $v0, $a0, 16
  //   sra   $v0, $v0, 16
  //   bgez  $v0, .L_exit (+4 instructions)
  //   nop
  // Block 1:
  //   subu  $v0, $zero, $a0
  //   sll   $v0, $v0, 16
  //   sra   $v0, $v0, 16
  // Block 2 (.L_exit):
  //   jr    $ra
  //   nop
  std::vector<uint32_t> words = {
      0x00041400,  // sll   $v0, $a0, 16
      0x00021403,  // sra   $v0, $v0, 16
      0x04410004,  // bgez  $v0, +4 -> .L_exit
      0x00000000,  // nop
      0x00041023,  // subu  $v0, $zero, $a0
      0x00021400,  // sll   $v0, $v0, 16
      0x00021403,  // sra   $v0, $v0, 16
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(ExpressionBuilder::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0LoopVoidFunction) {
  // DmaInit-like pattern:
  // Block 0:
  //   addu  $v1, $zero, $zero
  //   andi  $v0, $v1, 0xFFFF
  // Block 1 (loop body):
  //   sll   $v0, $v0, 4
  //   addiu $v1, $v1, 1
  //   sltiu $v0, $v0, 40
  //   bnel  $v0, $zero, Block 1
  //   andi  $v0, $v1, 0xFFFF
  // Block 2 (exit):
  //   jr    $ra
  //   nop
  std::vector<uint32_t> words = {
      0x00001821,  // 00: addu  $v1, $zero, $zero
      0x3062FFFF,  // 04: andi  $v0, $v1, 0xFFFF
      0x00021100,  // 08: sll   $v0, $v0, 4
      0x24630001,  // 0C: addiu $v1, $v1, 1
      0x2C420028,  // 10: sltiu $v0, $v0, 40
      0x5440FFFD,  // 14: bnel  $v0, $zero, -3 (0x80020008)
      0x3062FFFF,  // 18: andi  $v0, $v1, 0xFFFF
      0x03E00008,  // 1C: jr    $ra
      0x00000000,  // 20: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(ExpressionBuilder::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, NorBitwiseNot) {
  // nor $v0, $zero, $a0 (bitwise NOT / unary ~)
  std::vector<uint32_t> words = {
      0x00041027,  // nor $v0, $zero, $a0
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(statements[0].destination_variable, "v0");
  ASSERT_NE(statements[0].expression, nullptr);
  EXPECT_EQ(statements[0].expression->kind, ExpressionKind::kUnaryOp);
  EXPECT_EQ(statements[0].expression->op, "~");
  EXPECT_EQ(statements[0].expression->ToString(), "~arg0");
}

TEST(ExpressionBuilderTest, NorGeneral) {
  // nor $v0, $a0, $a1 (bitwise NOR: ~(arg0 | arg1))
  std::vector<uint32_t> words = {
      0x00851027,  // nor $v0, $a0, $a1
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(statements[0].destination_variable, "v0");
  ASSERT_NE(statements[0].expression, nullptr);
  EXPECT_EQ(statements[0].expression->kind, ExpressionKind::kUnaryOp);
  EXPECT_EQ(statements[0].expression->op, "~");
  EXPECT_EQ(statements[0].expression->ToString(), "~(arg0 | arg1)");
}

TEST(ExpressionBuilderTest, VariableShiftOperations) {
  // sllv $v0, $a0, $a1
  // srlv $v1, $a0, $a1
  // srav $t0, $a0, $a1
  std::vector<uint32_t> words = {
      0x00A41004,  // sllv $v0, $a0, $a1
      0x00A41806,  // srlv $v1, $a0, $a1
      0x00A44007,  // srav $t0, $a0, $a1
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 << arg1;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = arg0 >> arg1;\n");
  EXPECT_EQ(statements[2].ToString(), "temp_t0 = arg0 >> arg1;\n");
}

TEST(ExpressionBuilderTest, MultMfloAndMfhi) {
  // mult $a0, $a1
  // mflo $v0
  // mfhi $v1
  std::vector<uint32_t> words = {
      0x00850018,  // mult $a0, $a1
      0x00001012,  // mflo $v0
      0x00850018,  // mult $a0, $a1
      0x00001810,  // mfhi $v1
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 * arg1;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = (s64)arg0 * (s64)arg1 >> 32;\n");
}

TEST(ExpressionBuilderTest, UnalignedLoadStore) {
  // lwl $v0, 0($a0)
  // lwr $v0, 3($a0)
  // swl $a1, 0($a0)
  // swr $a1, 3($a0)
  std::vector<uint32_t> words = {
      0x88820000,  // lwl $v0, 0($a0)
      0x98820003,  // lwr $v0, 3($a0)
      0xA8850000,  // swl $a1, 0($a0)
      0xB8850003,  // swr $a1, 3($a0)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "v0 = *(s32*)arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "*(s32*)arg0 = arg1;\n");
}

TEST(ExpressionBuilderTest, IndirectCallJalr) {
  // jalr $t9
  // nop
  std::vector<uint32_t> words = {
      0x0320F809,  // jalr $t9
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].kind, StatementKind::kCall);
  ASSERT_NE(statements[0].expression, nullptr);
  EXPECT_EQ(statements[0].expression->name, "((void (*)())temp_t9)");
  EXPECT_EQ(statements[0].ToString(), "((void (*)())temp_t9)();\n");
}

TEST(ExpressionBuilderTest, SizedGlobalLoadAndStore) {
  std::string textproto = R"pb(
    entries { name: "g_flag" address: 0x80204B38 type: SYMBOL_DATA size: 1 }
    entries { name: "g_count" address: 0x80182BA0 type: SYMBOL_DATA size: 2 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // lui $at, 0x8020
  // sb  $zero, 0x4B38($at)
  // lui $t0, 0x8018
  // sh  $a0, 0x2BA0($t0)
  // lui $t1, 0x8020
  // lbu $v0, 0x4B38($t1)
  std::vector<uint32_t> words = {
      0x3C018020,  // 0: lui $at, 0x8020
      0xA0204B38,  // 1: sb  $zero, 0x4B38($at)
      0x3C088018,  // 2: lui $t0, 0x8018
      0xA5042BA0,  // 3: sh  $a0, 0x2BA0($t0)
      0x3C098020,  // 4: lui $t1, 0x8020
      0x91224B38,  // 5: lbu $v0, 0x4B38($t1)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts, &(*index_or));

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].kind, StatementKind::kStore);
  EXPECT_EQ(statements[0].store_type, "u8");
  EXPECT_EQ(statements[0].ToString(), "*(u8*)&g_flag = 0;\n");

  EXPECT_EQ(statements[1].kind, StatementKind::kStore);
  EXPECT_EQ(statements[1].store_type, "u16");
  EXPECT_EQ(statements[1].ToString(), "*(u16*)&g_count = arg0;\n");

  EXPECT_EQ(statements[2].kind, StatementKind::kAssignment);
  EXPECT_EQ(statements[2].ToString(), "v0 = *(u8*)&g_flag;\n");
}

TEST(ExpressionBuilderTest, FindFoldedComparisonSltAtSuppressed) {
  // slt $at, $a0, $a1
  // beq $at, $zero, +2
  // nop
  std::vector<uint32_t> words = {
      0x0085082A,  // 0: slt $at, $a0, $a1
      0x10200002,  // 4: beq $at, $zero, +2
      0x00000000,  // 8: nop
  };

  auto insts = *DecodeSequence(words);
  auto folded = ExpressionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  EXPECT_TRUE(folded->can_suppress_statement);

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  // temp_at = arg0 < arg1 statement should be suppressed
  EXPECT_THAT(statements, SizeIs(0));
}

TEST(ExpressionBuilderTest, FindFoldedComparisonV0PreservedWhenNotOverwritten) {
  // sltiu $v0, $a0, 42
  // bne   $v0, $zero, +2
  // nop
  std::vector<uint32_t> words = {
      0x2C82002A,  // 0: sltiu $v0, $a0, 42
      0x14400002,  // 4: bne   $v0, $zero, +2
      0x00000000,  // 8: nop
  };

  auto insts = *DecodeSequence(words);
  auto folded = ExpressionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  // $v0 is not $at and not overwritten in delay slot, so statement is preserved
  EXPECT_FALSE(folded->can_suppress_statement);

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].destination_variable, "v0");
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 < 42;\n");
}

TEST(ExpressionBuilderTest, FindFoldedComparisonV0SuppressedWhenOverwrittenInDelaySlot) {
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
  auto folded = ExpressionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  // Overwritten in delay slot before any exit, so statement can be safely suppressed
  EXPECT_TRUE(folded->can_suppress_statement);

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  // The sltiu statement is suppressed; only the delay slot andi statement is emitted
  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "v0 = v1 & 255;\n");
}

TEST(ExpressionBuilderTest, FindFoldedComparisonHazardRejectsFolding) {
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
  auto folded = ExpressionBuilder::FindFoldedComparison(insts);
  // Hazard on $a0 prevents folding
  EXPECT_FALSE(folded.has_value());

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].destination_variable, "temp_at");
  EXPECT_EQ(statements[1].destination_variable, "arg0");
}

TEST(ExpressionBuilderTest, FindFoldedComparisonIntermediateUseRejectsFolding) {
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
  auto folded = ExpressionBuilder::FindFoldedComparison(insts);
  // Intermediate use of $at prevents folding
  EXPECT_FALSE(folded.has_value());

  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].destination_variable, "temp_at");
}

TEST(ExpressionBuilderTest, FindFoldedComparisonXoriAndAndi) {
  // xori $at, $a0, 5
  // bne  $at, $zero, +2
  // nop
  std::vector<uint32_t> xor_words = {
      0x38810005,  // 0: xori $at, $a0, 5
      0x14200002,  // 4: bne  $at, $zero, +2
      0x00000000,  // 8: nop
  };

  auto xor_insts = *DecodeSequence(xor_words);
  auto xor_folded = ExpressionBuilder::FindFoldedComparison(xor_insts);
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
  auto and_folded = ExpressionBuilder::FindFoldedComparison(and_insts);
  ASSERT_TRUE(and_folded.has_value());
  EXPECT_EQ(and_folded->instruction_index, 0u);
  EXPECT_TRUE(and_folded->can_suppress_statement);
}

TEST(ExpressionBuilderTest, FindFoldedComparisonWithInterveningFpuInstruction) {
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
  auto folded = ExpressionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  EXPECT_TRUE(folded->can_suppress_statement);
}

TEST(ExpressionBuilderTest, LiftsStackPointerArithmeticAsCast) {
  // addu $v0, $v1, $sp
  // addu $a3, $sp, $zero
  std::vector<uint32_t> words = {
      0x007D1021,  // 0: addu $v0, $v1, $sp
      0x03A03821,  // 4: addu $a3, $sp, $zero
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(statements.size(), 2u);
  EXPECT_EQ(statements[0].ToString(), "v0 = v1 + (s32)sp;\n");
  EXPECT_EQ(statements[1].ToString(), "arg3 = (s32)sp;\n");
}

TEST(ExpressionBuilderTest, SuppressesSpDestinationInAdduSubuAndLoads) {
  // Epilogue stack pointer adjustments or OS context switches restoring $sp
  // should not emit assignments to 'sp'.
  std::vector<uint32_t> words = {
      0x03C0E821,  // 0: addu $sp, $fp, $zero
      0x03A2E823,  // 4: subu $sp, $sp, $v0
      0x8D3D0004,  // 8: lw   $sp, 4($t1)
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  EXPECT_TRUE(statements.empty());
}

TEST(ExpressionBuilderTest, LiftsStackPointerBitwiseOperations) {
  // or $v0, $sp, $a0
  std::vector<uint32_t> words = {
      0x03A41025,  // 0: or $v0, $sp, $a0
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(statements.size(), 1u);
  EXPECT_EQ(statements[0].ToString(), "v0 = (s32)sp | arg0;\n");
}

TEST(ExpressionBuilderTest, LiftsCop1ArithmeticAndUnary) {
  std::vector<uint32_t> words = {
      0x46041000,  // 0: add.s $f0, $f2, $f4
      0x46080182,  // 4: mul.s $f6, $f0, $f8
      0x46023281,  // 8: sub.s $f10, $f6, $f2
      0x46045303,  // C: div.s $f12, $f10, $f4
      0x46006387,  // 10: neg.s $f14, $f12
      0x46007404,  // 14: sqrt.s $f16, $f14
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(statements.size(), 6u);
  EXPECT_EQ(statements[0].ToString(), "f0 = f2 + f4;\n");
  EXPECT_EQ(statements[1].ToString(), "f6 = f0 * f8;\n");
  EXPECT_EQ(statements[2].ToString(), "f10 = f6 - f2;\n");
  EXPECT_EQ(statements[3].ToString(), "f12 = f10 / f4;\n");
  EXPECT_EQ(statements[4].ToString(), "f14 = -f12;\n");
  EXPECT_EQ(statements[5].ToString(), "f16 = sqrtf(f14);\n");
}

TEST(ExpressionBuilderTest, LiftsCop1MemoryLoadsAndStores) {
  std::vector<uint32_t> words = {
      0xC4800000,  // 0: lwc1 $f0, 0($a0)
      0xE4A00004,  // 4: swc1 $f0, 4($a1)
      0xC7A20010,  // 8: lwc1 $f2, 16($sp)
      0xE7A20014,  // C: swc1 $f2, 20($sp)
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(statements.size(), 4u);
  EXPECT_EQ(statements[0].ToString(), "f0 = *(f32*)arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "*(f32*)(arg1 + 4) = f0;\n");
  EXPECT_EQ(statements[2].ToString(), "f2 = *(f32*)&var_sp_16;\n");
  EXPECT_EQ(statements[3].ToString(), "*(f32*)&var_sp_20 = f2;\n");
}

TEST(ExpressionBuilderTest, FindFoldedComparisonCop1Branch) {
  std::vector<uint32_t> words = {
      0x4602003C,  // 0: c.lt.s $f0, $f2
      0x00000000,  // 4: nop
      0x45010002,  // 8: bc1t +2
      0x00000000,  // C: nop
  };
  auto insts = *DecodeSequence(words);
  auto folded = ExpressionBuilder::FindFoldedComparison(insts);
  ASSERT_TRUE(folded.has_value());
  EXPECT_EQ(folded->instruction_index, 0u);
  EXPECT_TRUE(folded->can_suppress_statement);
}

TEST(ExpressionBuilderTest, LiftsCop1CpuMovesAndControlRegisters) {
  std::vector<uint32_t> words = {
      0x44020000,  // 0: mfc1 $v0, $f0
      0x44801000,  // 4: mtc1 $zero, $f2
      0x44842000,  // 8: mtc1 $a0, $f4
      0x4443F800,  // C: cfc1 $v1, $31
      0x44C5F800,  // 10: ctc1 $a1, $31
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(statements.size(), 5u);
  EXPECT_EQ(statements[0].ToString(), "v0 = *(s32*)&f0;\n");
  EXPECT_EQ(statements[1].ToString(), "f2 = 0.0f;\n");
  EXPECT_EQ(statements[2].ToString(), "f4 = *(f32*)&arg0;\n");
  EXPECT_EQ(statements[3].ToString(), "v1 = fcr31;\n");
  EXPECT_EQ(statements[4].ToString(), "fcr31 = arg1;\n");
}

TEST(ExpressionBuilderTest, LiftsCop1ConversionsAndDoubleOps) {
  std::vector<uint32_t> words = {
      0x46801020,  // 0: cvt.s.w $f0, $f2
      0x4600010D,  // 4: trunc.w.s $f4, $f0
      0x46307300,  // 8: add.d $f12, $f14, $f16
      0xD7B40028,  // C: ldc1 $f20, 40($sp)
      0xF7B40030,  // 10: sdc1 $f20, 48($sp)
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(statements.size(), 5u);
  EXPECT_EQ(statements[0].ToString(), "f0 = (f32)f2;\n");
  EXPECT_EQ(statements[1].ToString(), "f4 = (s32)f0;\n");
  EXPECT_EQ(statements[2].ToString(), "f12 = f14 + f16;\n");
  EXPECT_EQ(statements[3].ToString(), "f20 = *(f64*)&var_sp_40;\n");
  EXPECT_EQ(statements[4].ToString(), "*(f64*)&var_sp_48 = f20;\n");
}

TEST(ExpressionBuilderTest, DetermineFpParameterCountAndPassesFpArgsToCalls) {
  // Function that takes f12 and f14 (uses both before definition)
  std::vector<uint32_t> callee_words = {
      0x462E6032,  // c.eq.d $f12, $f14
      0x45010002,  // bc1t +2
      0x00000000,  // nop
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto callee_insts = *DecodeSequence(callee_words);
  EXPECT_EQ(ExpressionBuilder::DetermineFpParameterCount(callee_insts), 2);

  // Function that defines f12 before use (takes 0 FP parameters)
  std::vector<uint32_t> non_callee_words = {
      0x44806000,  // mtc1 $zero, $f12
      0x46006005,  // abs.s $f0, $f12
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto non_callee_insts = *DecodeSequence(non_callee_words);
  EXPECT_EQ(ExpressionBuilder::DetermineFpParameterCount(non_callee_insts), 0);

  // Caller that calls func_fp_2 via jal
  std::vector<uint32_t> caller_words = {
      0x0C020000,  // jal func_80080000
      0x00000000,  // nop
  };
  auto caller_insts = *DecodeSequence(caller_words, 0x80010000);
  absl::flat_hash_map<std::string, int> gpr_counts = {{"func_80080000", 0}};
  absl::flat_hash_map<std::string, int> fp_counts = {{"func_80080000", 2}};

  auto caller_stmts =
      ExpressionBuilder::LiftInstructions(caller_insts, nullptr, nullptr, &gpr_counts, &fp_counts);
  ASSERT_GE(caller_stmts.size(), 1u);
  EXPECT_EQ(caller_stmts[0].ToString(), "func_80080000(f12, f14);\n");
}

}  // namespace
}  // namespace rom_nom_nom
