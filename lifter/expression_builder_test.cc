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

}  // namespace
}  // namespace rom_nom_nom
