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

}  // namespace
}  // namespace rom_nom_nom
