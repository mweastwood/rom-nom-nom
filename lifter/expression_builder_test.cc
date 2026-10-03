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

}  // namespace
}  // namespace rom_nom_nom
