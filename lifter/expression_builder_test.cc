#include "lifter/expression_builder.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
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
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  // lui is folded, so only the loaded symbol statement is emitted
  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "v0 = g_audio_status;\n");
}

TEST(ExpressionBuilderTest, FoldedAddressLoadAndConstantLiteral) {
  std::string textproto = R"pb(
    entries { name: "g_target_buf" address: 0x80201000 type: SYMBOL_DATA size: 128 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      // lui + addiu with symbol: &g_target_buf
      0x3C018020,  // 0: lui   $at, 0x8020
      0x24241000,  // 4: addiu $a0, $at, 0x1000
      // lui + ori: 32-bit constant literal 0x12345678
      0x3C081234,  // 8: lui   $t0, 0x1234
      0x35085678,  // C: ori   $t0, $t0, 0x5678
  };

  auto insts = *DecodeSequence(words);
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "arg0 = &g_target_buf;\n");
  EXPECT_EQ(statements[1].ToString(), "temp_t0 = 0x12345678;\n");
}

TEST(ExpressionBuilderTest, FoldedGlobalStoreTypedAndUntyped) {
  std::string textproto = R"pb(
    entries { name: "g_flag" address: 0x80204000 type: SYMBOL_DATA size: 4 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      // 32-bit global store to named symbol
      0x3C018020,  // 0: lui $at, 0x8020
      0xAC244000,  // 4: sw  $a0, 0x4000($at)
      // 8-bit global store to named symbol
      0x3C018020,  // 8: lui $at, 0x8020
      0xA0254000,  // C: sb  $a1, 0x4000($at)
      // 32-bit global store without symbol
      0x3C018030,  // 10: lui $at, 0x8030
      0xAC265000,  // 14: sw  $a2, 0x5000($at)
  };

  auto insts = *DecodeSequence(words);
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "g_flag = arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "*(u8*)&g_flag = arg1;\n");
  EXPECT_EQ(statements[2].ToString(), "D_80305000 = arg2;\n");

  // Without a symbol index, raw hex addresses are emitted for both s32 and sized stores.
  auto untyped_statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(untyped_statements, SizeIs(3));
  EXPECT_EQ(untyped_statements[0].ToString(), "*(0x80204000) = arg0;\n");
  EXPECT_EQ(untyped_statements[1].ToString(), "*(u8*)0x80204000 = arg1;\n");
  EXPECT_EQ(untyped_statements[2].ToString(), "*(0x80305000) = arg2;\n");
}

TEST(ExpressionBuilderTest, FoldedGlobalLoadTypedAndFp) {
  std::string textproto = R"pb(
    entries { name: "g_flag" address: 0x80204000 type: SYMBOL_DATA size: 4 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      // 8-bit typed global load with symbol
      0x3C018020,  // 0: lui   $at, 0x8020
      0x90224000,  // 4: lbu   $v0, 0x4000($at)
      // 32-bit untyped global load without symbol
      0x3C018030,  // 8: lui   $at, 0x8030
      0x8C235000,  // C: lw    $v1, 0x5000($at)
      // FP float load with symbol
      0x3C018020,  // 10: lui  $at, 0x8020
      0xC4204000,  // 14: lwc1 $f0, 0x4000($at)
      // FP float store with symbol
      0x3C018020,  // 18: lui  $at, 0x8020
      0xE42C4000,  // 1C: swc1 $f12, 0x4000($at)
  };

  auto insts = *DecodeSequence(words);
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  ASSERT_THAT(statements, SizeIs(4));
  EXPECT_EQ(statements[0].ToString(), "v0 = *(u8*)&g_flag;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = D_80305000;\n");
  EXPECT_EQ(statements[2].ToString(), "f0 = *(f32*)&g_flag;\n");
  EXPECT_EQ(statements[3].ToString(), "*(f32*)&g_flag = f12;\n");

  // Without a symbol index, raw hex addresses are emitted for loads.
  auto untyped_statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(untyped_statements, SizeIs(4));
  EXPECT_EQ(untyped_statements[0].ToString(), "v0 = *(u8*)0x80204000;\n");
  EXPECT_EQ(untyped_statements[1].ToString(), "v1 = *(s32*)0x80305000;\n");
  EXPECT_EQ(untyped_statements[2].ToString(), "f0 = *(f32*)0x80204000;\n");
  EXPECT_EQ(untyped_statements[3].ToString(), "*(f32*)0x80204000 = f12;\n");
}

TEST(ExpressionBuilderTest, FoldedGlobalIndexedLoadAndStore) {
  std::string textproto = R"pb(
    entries { name: "g_arr" address: 0x80188BFC type: SYMBOL_DATA size: 1024 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      // Indexed load: v0 = *(s32*)((u8*)&g_arr + v1)
      0x3C018019,  // 0: lui   $at, 0x8019
      0x00230821,  // 1: addu  $at, $at, $v1
      0x8C228BFC,  // 2: lw    $v0, -29700($at)
      // Indexed store: *(u8*)((u8*)&g_arr + a0) = 0
      0x3C088019,  // 3: lui   $t0, 0x8019
      0x00884021,  // 4: addu  $t0, $a0, $t0
      0xA1008BFC,  // 5: sb    $zero, -29700($t0)
      // FP Indexed load: f0 = *(f32*)((u8*)&g_arr + v1)
      0x3C018019,  // 6: lui   $at, 0x8019
      0x00230821,  // 7: addu  $at, $at, $v1
      0xC4208BFC,  // 8: lwc1  $f0, -29700($at)
      // FP Indexed store: *(f32*)((u8*)&g_arr + a0) = f12
      0x3C088019,  // 9: lui   $t0, 0x8019
      0x00884021,  // A: addu  $t0, $a0, $t0
      0xE50C8BFC,  // B: swc1  $f12, -29700($t0)
  };

  auto insts = *DecodeSequence(words);
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  ASSERT_THAT(statements, SizeIs(4));
  EXPECT_EQ(statements[0].ToString(), "v0 = *(s32*)((u8*)&g_arr + v1);\n");
  EXPECT_EQ(statements[1].ToString(), "*(u8*)((u8*)&g_arr + arg0) = 0;\n");
  EXPECT_EQ(statements[2].ToString(), "f0 = *(f32*)((u8*)&g_arr + v1);\n");
  EXPECT_EQ(statements[3].ToString(), "*(f32*)((u8*)&g_arr + arg0) = f12;\n");

  // Without symbol index:
  auto untyped_statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_THAT(untyped_statements, SizeIs(4));
  EXPECT_EQ(untyped_statements[0].ToString(), "v0 = *(s32*)((u8*)0x80188BFC + v1);\n");
  EXPECT_EQ(untyped_statements[1].ToString(), "*(u8*)((u8*)0x80188BFC + arg0) = 0;\n");
  EXPECT_EQ(untyped_statements[2].ToString(), "f0 = *(f32*)((u8*)0x80188BFC + v1);\n");
  EXPECT_EQ(untyped_statements[3].ToString(), "*(f32*)((u8*)0x80188BFC + arg0) = f12;\n");
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
  EXPECT_EQ(statements[2].ToString(), "arg0 = (s32)sp - 16;\n");
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

TEST(ExpressionBuilderTest, StackVariableAddressingExpression) {
  // addiu $a0, $sp, 16 -> arg0 = ((s32)sp + 16)
  std::vector<uint32_t> words = {
      0x27A40010,  // addiu $a0, $sp, 16
  };

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(instructions);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].destination_variable, "arg0");
  ASSERT_THAT(statements[0].expression, testing::NotNull());
  EXPECT_EQ(statements[0].expression->kind, ExpressionKind::kBinaryOp);
  EXPECT_EQ(statements[0].expression->op, "+");
  ASSERT_THAT(statements[0].expression->args, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "arg0 = (s32)sp + 16;\n");
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
  EXPECT_EQ(statements[2].ToString(), "v0 = (u32)arg0 < (u32)arg1;\n");
  EXPECT_EQ(statements[3].ToString(), "v1 = (u32)arg0 < 10;\n");
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
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

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
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "g_audio_status = 0;\n");
  EXPECT_EQ(statements[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(statements[0].destination_variable, "g_audio_status");
  ASSERT_NE(statements[0].expression, nullptr);
  EXPECT_EQ(statements[0].expression->kind, ExpressionKind::kIntegerLiteral);
  EXPECT_EQ(statements[0].expression->int_val, 0u);
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

TEST(ExpressionBuilderTest, OrAndOriOperations) {
  // or  $v0, $a0, $a1
  // or  $v1, $a0, $zero
  // ori $t0, $a0, 0x1234
  // or  $zero, $a0, $a1 (should produce no statement)
  // ori $zero, $a0, 0x1234 (should produce no statement)
  std::vector<uint32_t> words = {
      0x00851025,  // or  $v0, $a0, $a1
      0x00801825,  // or  $v1, $a0, $zero
      0x34881234,  // ori $t0, $a0, 0x1234
      0x00850025,  // or  $zero, $a0, $a1
      0x34801234,  // ori $zero, $a0, 0x1234
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].destination_variable, "v0");
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 | arg1;\n");

  EXPECT_EQ(statements[1].destination_variable, "v1");
  EXPECT_EQ(statements[1].ToString(), "v1 = arg0 | 0;\n");

  EXPECT_EQ(statements[2].destination_variable, "temp_t0");
  EXPECT_EQ(statements[2].ToString(), "temp_t0 = arg0 | 4660;\n");
}

TEST(ExpressionBuilderTest, XorAndXoriOperations) {
  // xor  $v0, $a0, $a1
  // xor  $v1, $a0, $zero
  // xori $t0, $a0, 0x5678
  // xor  $zero, $a0, $a1 (should produce no statement)
  // xori $zero, $a0, 0x5678 (should produce no statement)
  std::vector<uint32_t> words = {
      0x00851026,  // xor  $v0, $a0, $a1
      0x00801826,  // xor  $v1, $a0, $zero
      0x38885678,  // xori $t0, $a0, 0x5678
      0x00850026,  // xor  $zero, $a0, $a1
      0x38805678,  // xori $zero, $a0, 0x5678
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].destination_variable, "v0");
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 ^ arg1;\n");

  EXPECT_EQ(statements[1].destination_variable, "v1");
  EXPECT_EQ(statements[1].ToString(), "v1 = arg0 ^ 0;\n");

  EXPECT_EQ(statements[2].destination_variable, "temp_t0");
  EXPECT_EQ(statements[2].ToString(), "temp_t0 = arg0 ^ 22136;\n");
}

TEST(ExpressionBuilderTest, ImmediateShiftOperations) {
  // sll $v0, $a0, 4
  // srl $v1, $a0, 4
  // sra $t0, $a0, 4
  std::vector<uint32_t> words = {
      0x00041100,  // sll $v0, $a0, 4
      0x00041902,  // srl $v1, $a0, 4
      0x00044103,  // sra $t0, $a0, 4
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "v0 = arg0 << 4;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = (u32)arg0 >> 4;\n");
  EXPECT_EQ(statements[2].ToString(), "temp_t0 = arg0 >> 4;\n");
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
  EXPECT_EQ(statements[1].ToString(), "v1 = (u32)arg0 >> arg1;\n");
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

  ASSERT_THAT(statements, SizeIs(5));
  EXPECT_EQ(statements[0].ToString(), "v0 = *(s32*)arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "*(u8*)arg0 = arg1 >> 24;\n");
  EXPECT_EQ(statements[2].ToString(), "*(u8*)(arg0 + 1) = arg1 >> 16;\n");
  EXPECT_EQ(statements[3].ToString(), "*(u8*)(arg0 + 2) = arg1 >> 8;\n");
  EXPECT_EQ(statements[4].ToString(), "*(u8*)(arg0 + 3) = arg1;\n");
}

TEST(ExpressionBuilderTest, UnalignedStoreWithOffset) {
  // swl $a1, 4($a0)
  // swr $a1, 7($a0)
  std::vector<uint32_t> words = {
      0xA8850004,  // swl $a1, 4($a0)
      0xB8850007,  // swr $a1, 7($a0)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(4));
  EXPECT_EQ(statements[0].ToString(), "*(u8*)(arg0 + 4) = arg1 >> 24;\n");
  EXPECT_EQ(statements[1].ToString(), "*(u8*)(arg0 + 5) = arg1 >> 16;\n");
  EXPECT_EQ(statements[2].ToString(), "*(u8*)(arg0 + 6) = arg1 >> 8;\n");
  EXPECT_EQ(statements[3].ToString(), "*(u8*)(arg0 + 7) = arg1;\n");
}

TEST(ExpressionBuilderTest, StandaloneUnalignedStoreFlushedBeforeOtherInstruction) {
  // swl $a1, 0($a0)
  // addu $v0, $zero, $zero
  std::vector<uint32_t> words = {
      0xA8850000,  // swl $a1, 0($a0)
      0x00001021,  // addu $v0, $zero, $zero
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "*(u8*)arg0 = arg1;\n");
  EXPECT_EQ(statements[1].ToString(), "v0 = 0;\n");
}

TEST(ExpressionBuilderTest, StandaloneUnalignedStoreFlushedAtFunctionEnd) {
  // swl $a1, 4($a0)
  std::vector<uint32_t> words = {
      0xA8850004,  // swl $a1, 4($a0)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "*(u8*)(arg0 + 4) = arg1;\n");
}

TEST(ExpressionBuilderTest, UnalignedLoadStoreRightFirstOrder) {
  // lwr $v0, 7($a0)
  // lwl $v0, 4($a0)
  // swr $a1, 7($a0)
  // swl $a1, 4($a0)
  std::vector<uint32_t> words = {
      0x98820007,  // lwr $v0, 7($a0)
      0x88820004,  // lwl $v0, 4($a0)
      0xB8850007,  // swr $a1, 7($a0)
      0xA8850004,  // swl $a1, 4($a0)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(5));
  EXPECT_EQ(statements[0].ToString(), "v0 = *(s32*)(arg0 + 4);\n");
  EXPECT_EQ(statements[1].ToString(), "*(u8*)(arg0 + 4) = arg1 >> 24;\n");
  EXPECT_EQ(statements[2].ToString(), "*(u8*)(arg0 + 5) = arg1 >> 16;\n");
  EXPECT_EQ(statements[3].ToString(), "*(u8*)(arg0 + 6) = arg1 >> 8;\n");
  EXPECT_EQ(statements[4].ToString(), "*(u8*)(arg0 + 7) = arg1;\n");
}

TEST(ExpressionBuilderTest, Unaligned64BitLoadStoreBothOrders) {
  // ldl $v0, 0($a0)
  // ldr $v0, 7($a0)
  // ldr $v1, 15($a0)
  // ldl $v1, 8($a0)
  // sdl $a1, 0($a0)
  // sdr $a1, 7($a0)
  std::vector<uint32_t> words = {
      0x68820000,  // ldl $v0, 0($a0)
      0x6C820007,  // ldr $v0, 7($a0)
      0x6C83000F,  // ldr $v1, 15($a0)
      0x68830008,  // ldl $v1, 8($a0)
      0xB0850000,  // sdl $a1, 0($a0)
      0xB4850007,  // sdr $a1, 7($a0)
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(10));
  EXPECT_EQ(statements[0].ToString(), "v0 = *(s64*)arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "v1 = *(s64*)(arg0 + 8);\n");
  EXPECT_EQ(statements[2].ToString(), "*(u8*)arg0 = arg1 >> 56;\n");
  EXPECT_EQ(statements[3].ToString(), "*(u8*)(arg0 + 1) = arg1 >> 48;\n");
  EXPECT_EQ(statements[4].ToString(), "*(u8*)(arg0 + 2) = arg1 >> 40;\n");
  EXPECT_EQ(statements[5].ToString(), "*(u8*)(arg0 + 3) = arg1 >> 32;\n");
  EXPECT_EQ(statements[6].ToString(), "*(u8*)(arg0 + 4) = arg1 >> 24;\n");
  EXPECT_EQ(statements[7].ToString(), "*(u8*)(arg0 + 5) = arg1 >> 16;\n");
  EXPECT_EQ(statements[8].ToString(), "*(u8*)(arg0 + 6) = arg1 >> 8;\n");
  EXPECT_EQ(statements[9].ToString(), "*(u8*)(arg0 + 7) = arg1;\n");
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
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

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

TEST(ExpressionBuilderTest, MatchesSignedAndUnsignedDivisionAndModuloPatterns) {
  // 1. Signed division and modulo:
  // div   $zero, $a0, $a1
  // bnez  $a1, +2
  // nop
  // break 7
  // addiu $at, $zero, -1
  // bne   $a1, $at, +4
  // lui   $at, 0x8000
  // bne   $a0, $at, +2
  // nop
  // break 6
  // mflo  $v0
  // mfhi  $v1
  std::vector<uint32_t> signed_words = {
      0x0085001A,  // 0:  div   $zero, $a0, $a1
      0x14A00002,  // 4:  bnez  $a1, +2
      0x00000000,  // 8:  nop
      0x0007000D,  // C:  break 7
      0x2401FFFF,  // 10: addiu $at, $zero, -1
      0x14A10004,  // 14: bne   $a1, $at, +4
      0x3C018000,  // 18: lui   $at, 0x8000
      0x14810002,  // 1C: bne   $a0, $at, +2
      0x00000000,  // 20: nop
      0x0006000D,  // 24: break 6
      0x00001012,  // 28: mflo  $v0
      0x00001810,  // 2C: mfhi  $v1
  };

  auto signed_insts = *DecodeSequence(signed_words);
  auto signed_stmts = ExpressionBuilder::LiftInstructions(signed_insts);
  ASSERT_EQ(signed_stmts.size(), 2u);
  EXPECT_EQ(signed_stmts[0].destination_variable, "v0");
  EXPECT_EQ(signed_stmts[0].ToString(), "v0 = arg0 / arg1;\n");
  EXPECT_EQ(signed_stmts[1].destination_variable, "v1");
  EXPECT_EQ(signed_stmts[1].ToString(), "v1 = arg0 % arg1;\n");

  // 2. Unsigned division and modulo:
  // divu  $zero, $a0, $a1
  // bnez  $a1, +2
  // nop
  // break 7
  // mflo  $v0
  // mfhi  $v1
  std::vector<uint32_t> unsigned_words = {
      0x0085001B,  // 0: divu  $zero, $a0, $a1
      0x14A00002,  // 4: bnez  $a1, +2
      0x00000000,  // 8: nop
      0x0007000D,  // C: break 7
      0x00001012,  // 10: mflo  $v0
      0x00001810,  // 14: mfhi  $v1
  };

  auto unsigned_insts = *DecodeSequence(unsigned_words);
  auto unsigned_stmts = ExpressionBuilder::LiftInstructions(unsigned_insts);
  ASSERT_EQ(unsigned_stmts.size(), 2u);
  EXPECT_EQ(unsigned_stmts[0].destination_variable, "v0");
  EXPECT_EQ(unsigned_stmts[0].ToString(), "v0 = (u32)arg0 / (u32)arg1;\n");
  EXPECT_EQ(unsigned_stmts[1].destination_variable, "v1");
  EXPECT_EQ(unsigned_stmts[1].ToString(), "v1 = (u32)arg0 % (u32)arg1;\n");
}

TEST(ExpressionBuilderTest, LiftedExpressionCloneCreatesIndependentDeepCopy) {
  auto orig = LiftedExpression::Binary(
      "+", LiftedExpression::Variable("arg0"),
      LiftedExpression::Unary("(u32)", LiftedExpression::Integer(42, /*hex=*/true)));
  auto clone = orig->Clone();
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(clone->ToString(), orig->ToString());

  // Mutating clone does not mutate original
  clone->args[0]->name = "modified";
  EXPECT_EQ(orig->args[0]->name, "arg0");
  EXPECT_EQ(clone->args[0]->name, "modified");
}

TEST(ExpressionBuilderTest, SignedAndUnsignedMultWithBothMfloAndMfhi) {
  // 1. Signed multiplication:
  // mult $a0, $a1
  // mflo $v0
  // mfhi $v1
  std::vector<uint32_t> signed_words = {
      0x00850018,  // 0: mult $a0, $a1
      0x00001012,  // 4: mflo $v0
      0x00001810,  // 8: mfhi $v1
  };
  auto signed_insts = *DecodeSequence(signed_words);
  auto signed_stmts = ExpressionBuilder::LiftInstructions(signed_insts);
  ASSERT_EQ(signed_stmts.size(), 2u);
  EXPECT_EQ(signed_stmts[0].destination_variable, "v0");
  EXPECT_EQ(signed_stmts[0].ToString(), "v0 = arg0 * arg1;\n");
  EXPECT_EQ(signed_stmts[1].destination_variable, "v1");
  EXPECT_EQ(signed_stmts[1].ToString(), "v1 = (s64)arg0 * (s64)arg1 >> 32;\n");

  // 2. Unsigned multiplication with (u64)(u32) casting:
  // multu $a0, $a1
  // mflo  $v0
  // mfhi  $v1
  std::vector<uint32_t> unsigned_words = {
      0x00850019,  // 0: multu $a0, $a1
      0x00001012,  // 4: mflo  $v0
      0x00001810,  // 8: mfhi  $v1
  };
  auto unsigned_insts = *DecodeSequence(unsigned_words);
  auto unsigned_stmts = ExpressionBuilder::LiftInstructions(unsigned_insts);
  ASSERT_EQ(unsigned_stmts.size(), 2u);
  EXPECT_EQ(unsigned_stmts[0].destination_variable, "v0");
  EXPECT_EQ(unsigned_stmts[0].ToString(), "v0 = arg0 * arg1;\n");
  EXPECT_EQ(unsigned_stmts[1].destination_variable, "v1");
  EXPECT_EQ(unsigned_stmts[1].ToString(), "v1 = (u64)(u32)arg0 * (u64)(u32)arg1 >> 32;\n");
}

TEST(ExpressionBuilderTest, MultWhereMfloClobbersSourceOperandBeforeMfhi) {
  // multu $a0, $a1
  // mflo  $a0  <- clobbers $a0 before mfhi reads it!
  // mfhi  $v0
  std::vector<uint32_t> words = {
      0x00850019,  // 0: multu $a0, $a1
      0x00002012,  // 4: mflo  $a0
      0x00001010,  // 8: mfhi  $v0
  };
  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].destination_variable, "v0");
  EXPECT_EQ(stmts[0].ToString(), "v0 = (u64)(u32)arg0 * (u64)(u32)arg1 >> 32;\n");
  EXPECT_EQ(stmts[1].destination_variable, "arg0");
  EXPECT_EQ(stmts[1].ToString(), "arg0 = arg0 * arg1;\n");
}

TEST(ExpressionBuilderTest, MultWhereIntermediateInstructionClobbersSourceOperand) {
  // mult $a0, $a1
  // addu $a0, $zero, $zero  <- clobbers $a0 before mflo/mfhi!
  // mflo $v0
  // mfhi $v1
  std::vector<uint32_t> words = {
      0x00850018,  // 0: mult $a0, $a1
      0x00002021,  // 4: addu $a0, $zero, $zero
      0x00001012,  // 8: mflo $v0
      0x00001810,  // C: mfhi $v1
  };
  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 3u);
  EXPECT_EQ(stmts[0].destination_variable, "v0");
  EXPECT_EQ(stmts[0].ToString(), "v0 = arg0 * arg1;\n");
  EXPECT_EQ(stmts[1].destination_variable, "v1");
  EXPECT_EQ(stmts[1].ToString(), "v1 = (s64)arg0 * (s64)arg1 >> 32;\n");
  EXPECT_EQ(stmts[2].destination_variable, "arg0");
  EXPECT_EQ(stmts[2].ToString(), "arg0 = 0;\n");
}

TEST(ExpressionBuilderTest, LiftedExpressionFormatsLargeIntegersAsHex) {
  // Values with bit 31 set should automatically format as hex even when is_hex is false.
  EXPECT_TRUE(LiftedExpression::RequiresHex(0x80000000, /*explicit_hex=*/false));
  EXPECT_TRUE(LiftedExpression::RequiresHex(0x800266C0, /*explicit_hex=*/false));
  EXPECT_TRUE(LiftedExpression::RequiresHex(0xFFFFFFFF, /*explicit_hex=*/false));

  EXPECT_EQ(LiftedExpression::Integer(0x80000000, /*hex=*/false)->ToString(), "0x80000000");
  EXPECT_EQ(LiftedExpression::Integer(0x800266C0, /*hex=*/false)->ToString(), "0x800266C0");
  EXPECT_EQ(LiftedExpression::Integer(0xFFFFFFFF, /*hex=*/false)->ToString(), "0xFFFFFFFF");

  // Standard values without bit 31 set format as decimal unless explicit_hex is true.
  EXPECT_FALSE(LiftedExpression::RequiresHex(0, /*explicit_hex=*/false));
  EXPECT_FALSE(LiftedExpression::RequiresHex(42, /*explicit_hex=*/false));
  EXPECT_FALSE(LiftedExpression::RequiresHex(0x7FFFFFFF, /*explicit_hex=*/false));

  EXPECT_EQ(LiftedExpression::Integer(0, /*hex=*/false)->ToString(), "0");
  EXPECT_EQ(LiftedExpression::Integer(42, /*hex=*/false)->ToString(), "42");
  EXPECT_EQ(LiftedExpression::Integer(0x7FFFFFFF, /*hex=*/false)->ToString(), "2147483647");

  // Explicit hex flag formatting: multi-digit (> 9) uses hex, single digit (0-9) stays decimal.
  EXPECT_TRUE(LiftedExpression::RequiresHex(0x2A, /*explicit_hex=*/true));
  EXPECT_FALSE(LiftedExpression::RequiresHex(5, /*explicit_hex=*/true));

  EXPECT_EQ(LiftedExpression::Integer(0x2A, /*hex=*/true)->ToString(), "0x2A");
  EXPECT_EQ(LiftedExpression::Integer(5, /*hex=*/true)->ToString(), "5");
}

TEST(ExpressionBuilderTest, LiftRegisterOrConstantExcludesKsegPointerAddresses) {
  // 1. Loading a KSEG0 pointer address into $t0 (0x80051234) should NOT be constant-folded
  // into a raw integer literal in subsequent expressions.
  std::vector<uint32_t> kseg0_words = {
      0x3C088005,  // lui   $t0, 0x8005
      0x25081234,  // addiu $t0, $t0, 0x1234
      0x01001021,  // addu  $v0, $t0, $zero  (reads $t0)
  };
  auto kseg0_insts = *DecodeSequence(kseg0_words);
  auto kseg0_stmts = ExpressionBuilder::LiftInstructions(kseg0_insts);
  ASSERT_EQ(kseg0_stmts.size(), 2u);
  EXPECT_EQ(kseg0_stmts[0].ToString(), "temp_t0 = 0x80051234;\n");
  // $t0 in the second statement should remain variable temp_t0, not 0x80051234
  EXPECT_EQ(kseg0_stmts[1].ToString(), "v0 = temp_t0;\n");

  // 2. In contrast, standard non-pointer constants (e.g. 0x1234) are constant-folded.
  std::vector<uint32_t> const_words = {
      0x24081234,  // li   $t0, 0x1234
      0x01001021,  // addu $v0, $t0, $zero
  };
  auto const_insts = *DecodeSequence(const_words);
  auto const_stmts = ExpressionBuilder::LiftInstructions(const_insts);
  ASSERT_EQ(const_stmts.size(), 2u);
  EXPECT_EQ(const_stmts[0].ToString(), "temp_t0 = 4660;\n");
  EXPECT_EQ(const_stmts[1].ToString(), "v0 = 4660;\n");
}

TEST(ExpressionBuilderTest, LoudUnhandledInstructionMarker) {
  // Test that an unsupported instruction emits an unhandled marker statement
  Instruction unhandled_inst;
  unhandled_inst.opcode = Opcode::kUnknown;
  unhandled_inst.raw_word = 0x01234567;
  unhandled_inst.vram = 0x80020000;

  auto statements = ExpressionBuilder::LiftInstructions(absl::MakeSpan(&unhandled_inst, 1));
  ASSERT_EQ(statements.size(), 1u);
  EXPECT_EQ(statements[0].kind, StatementKind::kUnhandled);
  EXPECT_EQ(statements[0].ToString(), "/* UNHANDLED_INSTRUCTION: 0x01234567 */\n");
}

TEST(ExpressionBuilderTest, Lift64BitAluAndShifts) {
  std::vector<uint32_t> words = {
      0x67BDFFE0,  // 0: daddiu $sp, $sp, -32
      0x00A4102D,  // 4: daddu  $v0, $a1, $a0
      0x00041138,  // 8: dsll   $v0, $a0, 4
      0x0004113F,  // C: dsra32 $v0, $a0, 4
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(statements.size(), 3u);
  EXPECT_EQ(statements[0].ToString(), "v0 = arg1 + arg0;\n");
  EXPECT_EQ(statements[1].ToString(), "v0 = arg0 << 4;\n");
  EXPECT_EQ(statements[2].ToString(), "v0 = (s64)arg0 >> 36;\n");
}

TEST(ExpressionBuilderTest, LiftsStandaloneDivDdivAndDmultInstructions) {
  std::vector<uint32_t> words = {
      0x0085001A,  // 0: div    $zero, $a0, $a1
      0x0085001B,  // 1: divu   $zero, $a0, $a1
      0x0085001E,  // 2: ddiv   $zero, $a0, $a1
      0x0085001F,  // 3: ddivu  $zero, $a0, $a1
      0x0085001C,  // 4: dmult  $a0, $a1
      0x00001012,  // 5: mflo   $v0
      0x00001810,  // 6: mfhi   $v1
      0x0085001D,  // 7: dmultu $a0, $a1
      0x00001012,  // 8: mflo   $v0
      0x00001810,  // 9: mfhi   $v1
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  for (const auto& stmt : statements) {
    EXPECT_NE(stmt.kind, StatementKind::kUnhandled);
  }

  ASSERT_THAT(statements, SizeIs(12));
  // div
  EXPECT_EQ(statements[0].ToString(), "lo = arg0 / arg1;\n");
  EXPECT_EQ(statements[1].ToString(), "hi = arg0 % arg1;\n");
  // divu
  EXPECT_EQ(statements[2].ToString(), "lo = (u32)arg0 / (u32)arg1;\n");
  EXPECT_EQ(statements[3].ToString(), "hi = (u32)arg0 % (u32)arg1;\n");
  // ddiv
  EXPECT_EQ(statements[4].ToString(), "lo = arg0 / arg1;\n");
  EXPECT_EQ(statements[5].ToString(), "hi = arg0 % arg1;\n");
  // ddivu
  EXPECT_EQ(statements[6].ToString(), "lo = (u32)arg0 / (u32)arg1;\n");
  EXPECT_EQ(statements[7].ToString(), "hi = (u32)arg0 % (u32)arg1;\n");
  // dmult + mflo/mfhi
  EXPECT_EQ(statements[8].ToString(), "v0 = arg0 * arg1;\n");
  EXPECT_EQ(statements[9].ToString(), "v1 = (s64)arg0 * (s64)arg1 >> 32;\n");
  // dmultu + mflo/mfhi
  EXPECT_EQ(statements[10].ToString(), "v0 = arg0 * arg1;\n");
  EXPECT_EQ(statements[11].ToString(), "v1 = (u64)(u32)arg0 * (u64)(u32)arg1 >> 32;\n");
}

TEST(ExpressionBuilderTest, CrossBlockDominatorHiSymbolFoldedInBlock) {
  RegisterTracker initial_tracker;
  initial_tracker.SetRegisterValue(Register::kS0, TrackedValue::SymbolHi(0x80200000));

  std::string textproto = R"pb(
    entries { name: "g_player_pos" address: 0x80201234 type: SYMBOL_DATA size: 4 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // Block in dominating block's successor:
  // lw $v0, 0x1234($s0)
  std::vector<uint32_t> words = {
      0x8E021234,  // lw $v0, 0x1234($s0)
  };
  auto insts = *DecodeSequence(words);

  FunctionLiftContext ctx;
  ctx.symbol_index = &(*index_or);
  ctx.initial_tracker = &initial_tracker;

  auto statements = ExpressionBuilder::LiftInstructions(insts, ctx);
  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].ToString(), "v0 = g_player_pos;\n");
}

TEST(ExpressionBuilderTest, SpIndexingWithOutgoingArgsSize) {
  // 0: addiu $a0, $sp, 48
  // 1: lw    $v0, 48($sp)
  std::vector<uint32_t> words = {
      0x27A40030,  // addiu $a0, $sp, 48
      0x8FA20030,  // lw    $v0, 48($sp)
  };
  auto insts = *DecodeSequence(words);

  // When outgoing_args_size == 32 and sp_address_taken == true:
  // effective offset is 48 - 32 = 16.
  FunctionLiftContext ctx;
  ctx.frame_size = 128;
  ctx.has_calls = true;
  ctx.sp_address_taken = true;
  ctx.outgoing_args_size = 32;

  auto statements = ExpressionBuilder::LiftInstructions(insts, ctx);
  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "arg0 = (s32)sp + 16;\n");
  EXPECT_EQ(statements[1].ToString(), "v0 = *(s32*)((s32)sp + 16);\n");
}

TEST(ExpressionBuilderTest, LoadsBeforeStackFrameAllocationLiftWithConstQualification) {
  // 00: lwc1  $f0, 8($a1)       # scheduled before addiu $sp, $sp, -40
  // 04: lw    $a2, 0($a1)       # scheduled before addiu $sp, $sp, -40
  // 08: addiu $sp, $sp, -40
  // 0C: lw    $a3, 4($a1)       # scheduled after addiu $sp, $sp, -40
  std::vector<uint32_t> words = {
      0xC4A00008,  // lwc1  $f0, 8($a1)
      0x8CA60000,  // lw    $a2, 0($a1)
      0x27BDFFD8,  // addiu $sp, $sp, -40
      0x8CA70004,  // lw    $a3, 4($a1)
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 3u);
  EXPECT_EQ(stmts[0].ToString(), "f0 = *(const f32*)(arg1 + 8);\n");
  EXPECT_EQ(stmts[1].ToString(), "arg2 = *(const s32*)arg1;\n");
  EXPECT_EQ(stmts[2].ToString(), "arg3 = *(s32*)(arg1 + 4);\n");
}

TEST(ExpressionBuilderTest, CalleeSavedRomSymbolAddressNotReplacedByHexConstant) {
  std::string textproto = R"pb(
    entries { name: "D_00DBA100" address: 0x00DBA100 type: SYMBOL_DATA }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x3C1200DC,  // 00: lui   $s2, 0x00DC
      0x2652A100,  // 04: addiu $s2, $s2, -24320  # &D_00DBA100
      0x02402021,  // 08: addu  $a0, $s2, $zero
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  FunctionLiftContext ctx;
  ctx.symbol_index = &(*index_or);
  auto stmts = ExpressionBuilder::LiftInstructions(insts, ctx);
  ASSERT_EQ(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].ToString(), "temp_s2 = &D_00DBA100;\n");
  EXPECT_EQ(stmts[1].ToString(), "arg0 = temp_s2;\n");
}

TEST(ExpressionBuilderTest, RomSymbolAddressInArgAndScratchRegistersNotReplacedByHexConstant) {
  std::string textproto = R"pb(
    entries { name: "D_00D52670" address: 0x00D52670 type: SYMBOL_DATA }
    entries { name: "D_00D5A670" address: 0x00D5A670 type: SYMBOL_DATA }
    entries { name: "DmaCopy" address: 0x80040000 type: SYMBOL_FUNC }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x3C0500D5,  // 00: lui   $a1, 0x00D5
      0x24A52670,  // 04: addiu $a1, $a1, 0x2670   # &D_00D52670
      0x3C0200D6,  // 08: lui   $v0, 0x00D6
      0x2442A670,  // 0C: addiu $v0, $v0, -0x5990  # &D_00D5A670
      0xAC450000,  // 10: sw    $a1, 0($v0)
      0x0C010000,  // 14: jal   0x80040000         # DmaCopy
      0x00403021,  // 18: addu  $a2, $v0, $zero
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  FunctionLiftContext ctx;
  ctx.symbol_index = &(*index_or);
  auto stmts = ExpressionBuilder::LiftInstructions(insts, ctx);
  ASSERT_EQ(stmts.size(), 5u);
  EXPECT_EQ(stmts[0].ToString(), "arg1 = &D_00D52670;\n");
  EXPECT_EQ(stmts[1].ToString(), "v0 = &D_00D5A670;\n");
  EXPECT_EQ(stmts[2].ToString(), "*(s32*)v0 = arg1;\n");
  EXPECT_EQ(stmts[3].ToString(), "arg2 = v0;\n");
  EXPECT_EQ(stmts[4].ToString(), "DmaCopy(arg0, arg1, arg2);\n");
}

TEST(ExpressionBuilderTest, SubwordStackReadsFrom32BitWordStoreIncludeHighHalfSignCast) {
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFA40010,  // 04: sw    $a0, 16($sp)
      0x87A20010,  // 08: lh    $v0, 16($sp)   (high halfword, signed)
      0x97A30010,  // 0C: lhu   $v1, 16($sp)   (high halfword, unsigned)
      0x87A80012,  // 10: lh    $t0, 18($sp)   (low halfword, signed)
      0x97A90012,  // 14: lhu   $t1, 18($sp)   (low halfword, unsigned)
      0x83AA0010,  // 18: lb    $t2, 16($sp)   (byte 0, signed)
      0x93AB0011,  // 1C: lbu   $t3, 17($sp)   (byte 1, unsigned)
      0x83AC0012,  // 20: lb    $t4, 18($sp)   (byte 2, signed)
      0x93AD0013,  // 24: lbu   $t5, 19($sp)   (byte 3, unsigned)
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 9u);
  EXPECT_EQ(stmts[0].ToString(), "var_sp_16 = arg0;\n");
  EXPECT_EQ(stmts[1].ToString(), "v0 = (s16)((u32)var_sp_16 >> 16);\n");
  EXPECT_EQ(stmts[2].ToString(), "v1 = (u16)((u32)var_sp_16 >> 16);\n");
  EXPECT_EQ(stmts[3].ToString(), "temp_t0 = (s16)var_sp_16;\n");
  EXPECT_EQ(stmts[4].ToString(), "temp_t1 = (u16)var_sp_16;\n");
  EXPECT_EQ(stmts[5].ToString(), "temp_t2 = (s8)((u32)var_sp_16 >> 24);\n");
  EXPECT_EQ(stmts[6].ToString(), "temp_t3 = (u8)((u32)var_sp_16 >> 16);\n");
  EXPECT_EQ(stmts[7].ToString(), "temp_t4 = (s8)((u32)var_sp_16 >> 8);\n");
  EXPECT_EQ(stmts[8].ToString(), "temp_t5 = (u8)var_sp_16;\n");
}

TEST(ExpressionBuilderTest, WordAndSubwordStackReadsExtractedFrom64BitSdAndSdc1Stores) {
  // 1. 64-bit integer store (sd $a0, 16($sp)):
  std::vector<uint32_t> sd_words = {
      0x27BDFFE0,  // 00: addiu   $sp, $sp, -32
      0xFFA40010,  // 04: sd      $a0, 16($sp)
      0x8FA20010,  // 08: lw      $v0, 16($sp)   (high 32-bit word)
      0x9FA30014,  // 0C: lwu     $v1, 20($sp)   (low 32-bit word, unsigned)
      0x87A80010,  // 10: lh      $t0, 16($sp)   (halfword 0)
      0x97A90012,  // 14: lhu     $t1, 18($sp)   (halfword 1)
      0x87AA0014,  // 18: lh      $t2, 20($sp)   (halfword 2)
      0x97AB0016,  // 1C: lhu     $t3, 22($sp)   (halfword 3)
      0x83AC0010,  // 20: lb      $t4, 16($sp)   (byte 0)
      0x93AD0015,  // 24: lbu     $t5, 21($sp)   (byte 5)
      0x83AE0017,  // 28: lb      $t6, 23($sp)   (byte 7)
      0xC7A00010,  // 2C: lwc1    $f0, 16($sp)   (high float word)
      0xC7A20014,  // 30: lwc1    $f2, 20($sp)   (low float word)
      0xC7A40010,  // 34: lwc1    $f4, 16($sp)   (followed by cvt.s.w)
      0x468021A0,  // 38: cvt.s.w $f6, $f4
  };
  auto sd_insts = *DecodeSequence(sd_words, 0x80000000);
  auto sd_stmts = ExpressionBuilder::LiftInstructions(sd_insts);
  ASSERT_EQ(sd_stmts.size(), 13u);
  EXPECT_EQ(sd_stmts[0].ToString(), "var_sp_16 = arg0;\n");
  EXPECT_EQ(sd_stmts[1].ToString(), "v0 = (s32)((u64)var_sp_16 >> 32);\n");
  EXPECT_EQ(sd_stmts[2].ToString(), "v1 = (u32)var_sp_16;\n");
  EXPECT_EQ(sd_stmts[3].ToString(), "temp_t0 = (s16)((u64)var_sp_16 >> 48);\n");
  EXPECT_EQ(sd_stmts[4].ToString(), "temp_t1 = (u16)((u64)var_sp_16 >> 32);\n");
  EXPECT_EQ(sd_stmts[5].ToString(), "temp_t2 = (s16)((u64)var_sp_16 >> 16);\n");
  EXPECT_EQ(sd_stmts[6].ToString(), "temp_t3 = (u16)var_sp_16;\n");
  EXPECT_EQ(sd_stmts[7].ToString(), "temp_t4 = (s8)((u64)var_sp_16 >> 56);\n");
  EXPECT_EQ(sd_stmts[8].ToString(), "temp_t5 = (u8)((u64)var_sp_16 >> 16);\n");
  EXPECT_EQ(sd_stmts[9].ToString(), "temp_t6 = (s8)var_sp_16;\n");
  EXPECT_EQ(sd_stmts[10].ToString(), "f0 = *(f32*)&var_sp_16;\n");
  EXPECT_EQ(sd_stmts[11].ToString(), "f2 = *(f32*)((u8*)&var_sp_16 + 4);\n");
  EXPECT_EQ(sd_stmts[12].ToString(), "f6 = (f32)(s32)((u64)var_sp_16 >> 32);\n");

  // 2. 64-bit floating-point store (sdc1 $f12, 16($sp)):
  std::vector<uint32_t> sdc1_words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xF7AC0010,  // 04: sdc1  $f12, 16($sp)
      0x8FA20010,  // 08: lw    $v0, 16($sp)   (high 32-bit word)
      0x8FA30014,  // 0C: lw    $v1, 20($sp)   (low 32-bit word)
      0x87A80012,  // 10: lh    $t0, 18($sp)   (halfword 1)
      0x97A90016,  // 14: lhu   $t1, 22($sp)   (halfword 3)
      0x83AA0011,  // 18: lb    $t2, 17($sp)   (byte 1)
      0x93AB0017,  // 1C: lbu   $t3, 23($sp)   (byte 7)
      0xC7A00010,  // 20: lwc1  $f0, 16($sp)   (high float word)
      0xC7A20014,  // 24: lwc1  $f2, 20($sp)   (low float word)
  };
  auto sdc1_insts = *DecodeSequence(sdc1_words, 0x80000000);
  auto sdc1_stmts = ExpressionBuilder::LiftInstructions(sdc1_insts);
  ASSERT_EQ(sdc1_stmts.size(), 9u);
  EXPECT_EQ(sdc1_stmts[0].ToString(), "*(f64*)&var_sp_16 = f12_d;\n");
  EXPECT_EQ(sdc1_stmts[1].ToString(), "v0 = (s32)(*(u64*)&var_sp_16 >> 32);\n");
  EXPECT_EQ(sdc1_stmts[2].ToString(), "v1 = (s32)*(u64*)&var_sp_16;\n");
  EXPECT_EQ(sdc1_stmts[3].ToString(), "temp_t0 = (s16)(*(u64*)&var_sp_16 >> 32);\n");
  EXPECT_EQ(sdc1_stmts[4].ToString(), "temp_t1 = (u16)*(u64*)&var_sp_16;\n");
  EXPECT_EQ(sdc1_stmts[5].ToString(), "temp_t2 = (s8)(*(u64*)&var_sp_16 >> 48);\n");
  EXPECT_EQ(sdc1_stmts[6].ToString(), "temp_t3 = (u8)*(u64*)&var_sp_16;\n");
  EXPECT_EQ(sdc1_stmts[7].ToString(), "f0 = *(f32*)&var_sp_16;\n");
  EXPECT_EQ(sdc1_stmts[8].ToString(), "f2 = *(f32*)((u8*)&var_sp_16 + 4);\n");
}

}  // namespace
}  // namespace rom_nom_nom
