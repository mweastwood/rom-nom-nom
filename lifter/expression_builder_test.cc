#include "lifter/expression_builder.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/abi_analyzer.h"
#include "lifter/branch_condition_builder.h"

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
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

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

TEST(ExpressionBuilderTest, DetermineParameterCountIdentifiesUsedArguments) {
  // 1 arg: addu $v0, $a0, $zero
  std::vector<uint32_t> one_arg_words = {
      0x00801021,  // addu $v0, $a0, $zero
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto one_arg_insts = *DecodeSequence(one_arg_words);
  EXPECT_EQ(AbiAnalyzer::DetermineParameterCount(one_arg_insts), 1);

  // 3 args: addu $v0, $a0, $a1; addu $v0, $v0, $a2
  std::vector<uint32_t> three_args_words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x00461021,  // addu $v0, $v0, $a2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto three_args_insts = *DecodeSequence(three_args_words);
  EXPECT_EQ(AbiAnalyzer::DetermineParameterCount(three_args_insts), 3);

  // 0 args: addiu $v0, $zero, 1
  std::vector<uint32_t> zero_args_words = {
      0x24020001,  // addiu $v0, $zero, 1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto zero_args_insts = *DecodeSequence(zero_args_words);
  EXPECT_EQ(AbiAnalyzer::DetermineParameterCount(zero_args_insts), 0);
}

TEST(ExpressionBuilderTest, DetermineParameterCountIdentifiesArgumentsUsedInCallDelaySlot) {
  // Pattern matching RandomRange:
  // 0: addu $s1, $a0, $zero
  // 4: jal  0x80001000
  // 8: addu $s0, $a1, $zero  (delay slot uses $a1 before call clobbers arguments)
  // C: jr   $ra
  // 10: nop
  std::vector<uint32_t> words = {
      0x00808821,  // 0: addu $s1, $a0, $zero
      0x0C000400,  // 4: jal  0x80001000
      0x00A08021,  // 8: addu $s0, $a1, $zero
      0x03E00008,  // C: jr   $ra
      0x00000000,  // 10: nop
  };
  auto insts = *DecodeSequence(words);
  EXPECT_EQ(AbiAnalyzer::DetermineParameterCount(insts), 2);
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
  auto statements = ExpressionBuilder::LiftInstructions(
      instructions, FunctionLiftContext{.symbol_index = &(*index_or)});

  // The delay-slot assignment arg0 = 42 must execute before TargetFunc, so the call passes 42
  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "arg0 = 42;\n");
  EXPECT_EQ(statements[1].ToString(), "TargetFunc(42);\n");
  EXPECT_EQ(statements[2].ToString(), "return;\n");
}

TEST(ExpressionBuilderTest, JalrDelaySlotReorderingPassesArgumentToCall) {
  // Indirect call via $t9 with delay slot setting $a0:
  // 0: jalr  $t9
  // 4: addiu $a0, $zero, 99
  // 8: jr    $ra
  // C: nop
  std::vector<uint32_t> words = {
      0x0320F809,  // 0: jalr  $ra, $t9
      0x24040063,  // 4: addiu $a0, $zero, 99
      0x03E00008,  // 8: jr    $ra
      0x00000000,  // C: nop
  };

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(instructions);

  // The delay-slot assignment arg0 = 99 must execute before the indirect call
  ASSERT_GE(statements.size(), 3u);
  EXPECT_EQ(statements[0].ToString(), "arg0 = 99;\n");
  EXPECT_EQ(statements[1].ToString(), "((void (*)())temp_t9)(99);\n");
  EXPECT_EQ(statements.back().kind, StatementKind::kReturn);
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
      instructions, FunctionLiftContext{.symbol_index = &(*index_or),
                                        .function_parameter_counts = &parameter_counts});

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
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
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
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0LoopWithBeqlCondition) {
  // MessageInit-like pattern:
  // Loop defines $v0 via andi, then loops back with beql $v0, $zero.
  // On loop exit, falls through to jr $ra. $v0 is only the loop condition.
  std::vector<uint32_t> words = {
      0x00002021,  // 00: addu  $a0, $zero, $zero
      0x3082FFFF,  // 04: andi  $v0, $a0, 0xFFFF
      0x24840001,  // 08: addiu $a0, $a0, 1
      0x5040FFFE,  // 0C: beql  $v0, $zero, 0x80020004
      0x24840001,  // 10: addiu $a0, $a0, 1
      0x03E00008,  // 14: jr    $ra
      0x00000000,  // 18: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0VoidFunctionWithCallBeforeReturn) {
  // Pattern matching GraphicInit:
  // Loop defines $v0, then on loop exit an external call occurs, then the function returns without
  // redefining $v0. The call clobbers $v0, so the function should be detected as returning void
  // (not returning $v0).
  std::vector<uint32_t> words = {
      0x308200FF,  // 00: andi  $v0, $a0, 0xFF
      0x2C420002,  // 04: sltiu $v0, $v0, 2
      0x1440FFFD,  // 08: bnez  $v0, -3 -> 0x80020000
      0x308200FF,  // 0C: andi  $v0, $a0, 0xFF
      0x0C000400,  // 10: jal   0x80001000
      0x00000000,  // 14: nop
      0x03E00008,  // 18: jr    $ra
      0x00000000,  // 1C: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0PartiallyDefinedVoidFunction) {
  // Pattern matching func_800D0318:
  // Block 0:
  //   beqz $a0, .L_exit
  //   nop
  // Block 1:
  //   addiu $v0, $zero, 3
  //   beq $a0, $v0, .L_exit
  //   slti $v0, $a0, 4
  // Block 2 (.L_exit):
  //   jr $ra
  //   nop
  std::vector<uint32_t> words = {
      0x10800004,  // 00: beqz $a0, +4 -> 0x80020014 (.L_exit)
      0x00000000,  // 04: nop
      0x24020003,  // 08: addiu $v0, $zero, 3
      0x10820002,  // 0C: beq $a0, $v0, +2 -> 0x80020014 (.L_exit)
      0x28820004,  // 10: slti $v0, $a0, 4
      0x03E00008,  // 14: jr $ra
      0x00000000,  // 18: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0WithCalleeReturningV0) {
  // Function calls a callee that returns v0 right before return, so v0 is preserved and returned.
  std::vector<uint32_t> words = {
      0x0C000400,  // 00: jal 0x80001000
      0x00000000,  // 04: nop
      0x03E00008,  // 08: jr $ra
      0x00000000,  // 0C: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());

  // Without known callee returning v0, call clobbers v0, so returns false.
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));

  // With known callee returning v0, returns true.
  absl::flat_hash_set<std::string> returning_v0 = {"func_80001000"};
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsV0(*cfg_or, nullptr, &returning_v0));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0EmptyCfg) {
  ControlFlowGraph cfg;
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(cfg));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0InfiniteLoopNoReturn) {
  // Idle loop pattern: loop contains definitions but never returns
  std::vector<uint32_t> words = {
      0x24020001,  // 00: addiu $v0, $zero, 1
      0x1000FFFD,  // 04: b     -3 -> 0x80020000
      0x00000000,  // 08: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0UnreachableReturnIgnored) {
  // Reachable exit returns void, while an unreachable block defines $v0 before returning
  std::vector<uint32_t> words = {
      0x03E00008,  // 00: jr    $ra
      0x00000000,  // 04: nop
      0x24020001,  // 08: addiu $v0, $zero, 1
      0x03E00008,  // 0C: jr    $ra
      0x00000000,  // 10: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0DefinedThenConsumedBeforeReturn) {
  // $v0 is used as a temporary scratch register and read before return:
  std::vector<uint32_t> words = {
      0x24020005,  // 00: addiu $v0, $zero, 5
      0x00402021,  // 04: addu  $a0, $v0, $zero
      0xAFAC0000,  // 08: sw    $a0, 0($sp)
      0x03E00008,  // 0C: jr    $ra
      0x00000000,  // 10: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0DefinedInReturnDelaySlot) {
  // Single block function where $v0 is defined in the return delay slot:
  std::vector<uint32_t> words = {
      0x03E00008,  // 00: jr    $ra
      0x2402002A,  // 04: addiu $v0, $zero, 42
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0DefinedInBranchDelaySlotToReturn) {
  // Block 0 branches to return block, defining $v0 in its branch delay slot:
  std::vector<uint32_t> words = {
      0x10000002,  // 00: b     +2 -> 0x8002000C
      0x24020063,  // 04: addiu $v0, $zero, 99
      0x00000000,  // 08: nop (padding)
      0x03E00008,  // 0C: jr    $ra
      0x00000000,  // 10: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0LoopAccumulatorReturned) {
  // Loop accumulator pattern: $v0 is updated in loop and returned at exit
  std::vector<uint32_t> words = {
      0x00001021,  // 00: addu  $v0, $zero, $zero
      0x2408000A,  // 04: addiu $t0, $zero, 10
      0x00481021,  // 08: addu  $v0, $v0, $t0
      0x2108FFFF,  // 0C: addiu $t0, $t0, -1
      0x1D00FFFD,  // 10: bgtz  $t0, -3 -> 0x80020008
      0x00000000,  // 14: nop
      0x03E00008,  // 18: jr    $ra
      0x00000000,  // 1C: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsV0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsV0WithSymbolIndexLookup) {
  // Function calls 0x80045678, mapped to "ReadHardwareStatus" in SymbolIndex
  std::string textproto = R"pb(
    entries { name: "ReadHardwareStatus" address: 0x80045678 type: SYMBOL_FUNC }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x0C01159E,  // 00: jal 0x80045678
      0x00000000,  // 04: nop
      0x03E00008,  // 08: jr  $ra
      0x00000000,  // 0C: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());

  // Unknown callee -> returns false
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsV0(*cfg_or, &(*index_or), nullptr));

  // Callee known to return v0 -> returns true
  absl::flat_hash_set<std::string> returning_v0 = {"ReadHardwareStatus"};
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsV0(*cfg_or, &(*index_or), &returning_v0));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0DoubleReturnsTrueForDoubleOps) {
  // add.d $f0, $f12, $f14
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x462E6000,  // add.d $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0DoubleReturnsFalseForSinglePrecision) {
  // add.s $f0, $f12, $f14
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x460E6000,  // add.s $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0DoubleAcrossBlocks) {
  // Block 0:
  //   cvt.d.w $f0, $f2
  //   b       .L_exit
  //   nop
  // Block 1 (.L_exit):
  //   jr      $ra
  //   nop
  std::vector<uint32_t> words = {
      0x46801021,  // cvt.d.w $f0, $f2
      0x10000002,  // b       +2 -> 0x80020010
      0x00000000,  // nop
      0x00000000,  // nop (padding)
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));  // Reachable definition in predecessor
  EXPECT_TRUE(
      AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));  // Reachable definition in predecessor
}

TEST(ExpressionBuilderTest, DetermineReturnsF0FalseWhenCallClobbers) {
  // add.s $f0, $f12, $f14
  // jal   func_80020100
  // nop
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x460E6000,  // add.s $f0, $f12, $f14
      0x0C008040,  // jal   0x80020100
      0x00000000,  // nop
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0TrueWhenCallFollowedByF0Def) {
  // jal   func_80020100
  // nop
  // add.s $f0, $f12, $f14
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x0C008040,  // jal   0x80020100
      0x00000000,  // nop
      0x460E6000,  // add.s $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0FalseWhenV0DefinedAfterF0) {
  // add.s $f0, $f12, $f14
  // mfc1  $v0, $f0
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x460E6000,  // add.s $f0, $f12, $f14
      0x44020000,  // mfc1  $v0, $f0
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0BranchDelaySlot) {
  // Block 0:
  //   b     .L_exit
  //   add.s $f0, $f12, $f14  (delay slot)
  // Block 1 (.L_exit):
  //   jr    $ra
  //   nop
  std::vector<uint32_t> words = {
      0x10000002,  // b     +2 -> 0x80020010
      0x460E6000,  // add.s $f0, $f12, $f14
      0x00000000,  // nop
      0x00000000,  // nop
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0ReturnDelaySlot) {
  // jr    $ra
  // add.s $f0, $f12, $f14
  std::vector<uint32_t> words = {
      0x03E00008,  // jr    $ra
      0x460E6000,  // add.s $f0, $f12, $f14
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0LoopWithBeql) {
  // 0x80020000: beql  $a0, $zero, 0x8002000C
  // 0x80020004: add.s $f0, $f12, $f14 (delay slot, annulled on fallthrough)
  // 0x80020008: b     0x80020014
  // 0x8002000C: jr    $ra
  // 0x80020010: nop
  // 0x80020014: jr    $ra
  // 0x80020018: nop
  std::vector<uint32_t> words = {
      0x50800002,  // beql  $a0, $zero, 0x8002000C
      0x460E6000,  // add.s $f0, $f12, $f14
      0x10000002,  // b     0x80020014
      0x00000000,  // nop
      0x03E00008,  // jr    $ra (taken exit)
      0x00000000,  // nop
      0x03E00008,  // jr    $ra (fallthrough exit, $f0 not available)
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0MultipleExitsOneNotReturningF0) {
  // beq   $a0, $zero, 0x80020014
  // nop
  // add.s $f0, $f12, $f14
  // jr    $ra
  // nop
  // jr    $ra (second exit)
  // nop
  std::vector<uint32_t> words = {
      0x10800004,  // beq   $a0, $zero, 0x80020014
      0x00000000,  // nop
      0x460E6000,  // add.s $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
      0x03E00008,  // jr    $ra (second exit without defining $f0)
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0MultipleExitsBothReturningF0) {
  // beq   $a0, $zero, 0x80020014
  // nop
  // add.s $f0, $f12, $f14
  // jr    $ra
  // nop
  // sub.s $f0, $f12, $f14
  // jr    $ra (second exit)
  // nop
  std::vector<uint32_t> words = {
      0x10800004,  // beq   $a0, $zero, 0x80020014
      0x00000000,  // nop
      0x460E6000,  // add.s $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
      0x460E6001,  // sub.s $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0UsedAfterLastDefAsTemporary) {
  // add.s $f0, $f12, $f14
  // add.s $f2, $f0, $f12
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x460E6000,  // add.s $f0, $f12, $f14
      0x460C0080,  // add.s $f2, $f0, $f12
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0DoubleReturnsFalseForVoidOrInteger) {
  // addu $v0, $a0, $a1
  // jr   $ra
  // nop
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0(*cfg_or));
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));
}

TEST(ExpressionBuilderTest, DetermineReturnsF0DoubleVariousOpcodes) {
  // Test ldc1 $f0, 0($a0)
  std::vector<uint32_t> ldc1_words = {
      0xD4800000,  // ldc1 $f0, 0($a0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(ldc1_words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));

  // Test sub.d $f0, $f12, $f14
  std::vector<uint32_t> subd_words = {
      0x462E6001,  // sub.d $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  insts = *DecodeSequence(subd_words, 0x80020000);
  cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));

  // Test mul.d $f0, $f12, $f14
  std::vector<uint32_t> muld_words = {
      0x462E6002,  // mul.d $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  insts = *DecodeSequence(muld_words, 0x80020000);
  cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));

  // Test div.d $f0, $f12, $f14
  std::vector<uint32_t> divd_words = {
      0x462E6003,  // div.d $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  insts = *DecodeSequence(divd_words, 0x80020000);
  cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));

  // Test cvt.d.s $f0, $f2
  std::vector<uint32_t> cvtds_words = {
      0x46001021,  // cvt.d.s $f0, $f2
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  insts = *DecodeSequence(cvtds_words, 0x80020000);
  cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(*cfg_or));
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
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
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
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
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
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
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
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
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
  EXPECT_EQ(statements[5].ToString(), "f16 = N64Sqrtf(f14);\n");
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
  EXPECT_EQ(statements[2].ToString(), "f2 = *(f32*)&arg4;\n");
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
  auto folded = BranchConditionBuilder::FindFoldedComparison(insts);
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
  EXPECT_EQ(statements[0].ToString(), "f0 = (f32)*(s32*)&f2;\n");
  EXPECT_EQ(statements[1].ToString(), "*(s32*)&f4 = (s32)f0;\n");
  EXPECT_EQ(statements[2].ToString(), "f12_d = f14_d + f16_d;\n");
  EXPECT_EQ(statements[3].ToString(), "f20_d = *(f64*)&arg10;\n");
  EXPECT_EQ(statements[4].ToString(), "*(f64*)&var_sp_48 = f20_d;\n");
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
  EXPECT_EQ(AbiAnalyzer::DetermineFpParameterCount(callee_insts), 2);

  // Function that defines f12 before use (takes 0 FP parameters)
  std::vector<uint32_t> non_callee_words = {
      0x44806000,  // mtc1 $zero, $f12
      0x46006005,  // abs.s $f0, $f12
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto non_callee_insts = *DecodeSequence(non_callee_words);
  EXPECT_EQ(AbiAnalyzer::DetermineFpParameterCount(non_callee_insts), 0);

  // Caller that calls func_fp_2 via jal
  std::vector<uint32_t> caller_words = {
      0x0C020000,  // jal func_80080000
      0x00000000,  // nop
  };
  auto caller_insts = *DecodeSequence(caller_words, 0x80010000);
  absl::flat_hash_map<std::string, int> gpr_counts = {{"func_80080000", 0}};
  absl::flat_hash_map<std::string, int> fp_counts = {{"func_80080000", 2}};

  auto caller_stmts = ExpressionBuilder::LiftInstructions(
      caller_insts, FunctionLiftContext{.function_parameter_counts = &gpr_counts,
                                        .function_fp_parameter_counts = &fp_counts});
  ASSERT_GE(caller_stmts.size(), 1u);
  EXPECT_EQ(caller_stmts[0].ToString(), "func_80080000(f12, f14);\n");
}

TEST(ExpressionBuilderTest, FindFoldedComparisonDoesNotFoldSelfMutatingInstructionWithoutClobber) {
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

TEST(ExpressionBuilderTest, FindFoldedComparisonDoesNotFoldWhenDelaySlotClobbersInput) {
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

TEST(ExpressionBuilderTest, FindFoldedComparisonFoldsWhenDelaySlotDoesNotClobberInput) {
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

TEST(ExpressionBuilderTest, LiftsFoldedFloatingPointGlobalLoadAndStore) {
  // lui  $at, 0x8017
  // lwc1 $f2, 1104($at)  # target: 0x80170450
  // lui  $at, 0x8017
  // swc1 $f2, 1104($at)  # target: 0x80170450
  std::vector<uint32_t> words = {
      0x3C018017,  // 0: lui  $at, 0x8017
      0xC4220450,  // 1: lwc1 $f2, 1104($at)
      0x3C018017,  // 2: lui  $at, 0x8017
      0xE4220450,  // 3: swc1 $f2, 1104($at)
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].ToString(), "f2 = *(f32*)0x80170450;\n");
  EXPECT_EQ(stmts[1].ToString(), "*(f32*)0x80170450 = f2;\n");
}

TEST(ExpressionBuilderTest, AssignsV0ReturnWhenUsedAfterSubroutineCall) {
  // jal  func_80110410
  // andi $s0, $s0, 0xFFFF (delay slot)
  // addu $v1, $s1, $zero
  // andi $v0, $v0, 0xFFFF (uses $v0 from call)
  std::vector<uint32_t> words = {
      0x0C044104,  // 0: jal  func_80110410
      0x3210FFFF,  // 4: andi $s0, $s0, 0xFFFF
      0x02201821,  // 8: addu $v1, $s1, $zero
      0x3042FFFF,  // C: andi $v0, $v0, 0xFFFF
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(stmts.size(), 4u);
  EXPECT_EQ(stmts[0].destination_variable, "temp_s0");
  EXPECT_EQ(stmts[1].destination_variable, "v0");
  EXPECT_EQ(stmts[1].kind, StatementKind::kAssignment);
  EXPECT_EQ(stmts[1].ToString(), "v0 = func_80110410();\n");
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternMatchesMtc1CvtAndTruncMfc1) {
  // 1. mtc1 $v0, $f2; nop; cvt.s.w $f2, $f2 -> f2 = (f32)v0;
  // 2. trunc.w.s $f0, $f0; nop; mfc1 $v0, $f0 -> v0 = (s32)f0;
  std::vector<uint32_t> words = {
      0x44821000,  // 0: mtc1 $v0, $f2
      0x00000000,  // 4: nop
      0x468010A0,  // 8: cvt.s.w $f2, $f2
      0x4600000D,  // C: trunc.w.s $f0, $f0
      0x00000000,  // 10: nop
      0x44020000,  // 14: mfc1 $v0, $f0
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].ToString(), "f2 = (f32)v0;\n");
  EXPECT_EQ(stmts[1].ToString(), "v0 = (s32)f0;\n");
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternHandlesZeroAndDouble) {
  // 1. mtc1 $zero, $f4; cvt.s.w $f4, $f4 -> f4 = 0.0f;
  // 2. mtc1 $a0, $f2; cvt.d.w $f2, $f2 -> f2 = (f64)arg0;
  // 3. trunc.w.d $f2, $f12; mfc1 $v1, $f2 -> v1 = (s32)f12;
  std::vector<uint32_t> words = {
      0x44802000,  // 0: mtc1 $zero, $f4
      0x46802120,  // 4: cvt.s.w $f4, $f4
      0x44841000,  // 8: mtc1 $a0, $f2
      0x468010A1,  // C: cvt.d.w $f2, $f2
      0x4620608D,  // 10: trunc.w.d $f2, $f12
      0x44031000,  // 14: mfc1 $v1, $f2
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 3u);
  EXPECT_EQ(stmts[0].ToString(), "f4 = 0.0f;\n");
  EXPECT_EQ(stmts[1].ToString(), "f2_d = (f64)arg0;\n");
  EXPECT_EQ(stmts[2].ToString(), "v1 = (s32)f12_d;\n");
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternHandlesMultipleHazardNops) {
  // mtc1 $a1, $f6; nop; nop; cvt.s.w $f6, $f6 -> f6 = (f32)arg1;
  std::vector<uint32_t> words = {
      0x44853000,  // 0: mtc1 $a1, $f6
      0x00000000,  // 4: nop
      0x00000000,  // 8: nop
      0x468031A0,  // C: cvt.s.w $f6, $f6
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(stmts[0].ToString(), "f6 = (f32)arg1;\n");
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternRejectsMismatchedRegisters) {
  // 1. mtc1 $v0, $f2; cvt.s.w $f4, $f0 ($fs mismatch: 2 vs 0) -> no pattern match
  // 2. trunc.w.s $f2, $f0; mfc1 $v0, $f4 ($fs mismatch: 2 vs 4) -> no pattern match
  std::vector<uint32_t> words = {
      0x44821000,  // 0: mtc1 $v0, $f2
      0x46800120,  // 4: cvt.s.w $f4, $f0
      0x4600008D,  // 8: trunc.w.s $f2, $f0
      0x44022000,  // C: mfc1 $v0, $f4
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 4u);
  EXPECT_EQ(stmts[0].ToString(), "f2 = *(f32*)&v0;\n");
  EXPECT_EQ(stmts[1].ToString(), "f4 = (f32)*(s32*)&f0;\n");
  EXPECT_EQ(stmts[2].ToString(), "*(s32*)&f2 = (s32)f0;\n");
  EXPECT_EQ(stmts[3].ToString(), "v0 = *(s32*)&f4;\n");
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternRejectsNonConversionSuccessors) {
  // 1. mtc1 $v0, $f2; add.s $f0, $f2, $f4 (not cvt) -> no pattern match
  // 2. trunc.w.s $f0, $f0; mfc1 $zero, $f0 (dest $zero) -> no pattern match
  std::vector<uint32_t> words = {
      0x44821000,  // 0: mtc1 $v0, $f2
      0x46041000,  // 4: add.s $f0, $f2, $f4
      0x4600000D,  // 8: trunc.w.s $f0, $f0
      0x44000000,  // C: mfc1 $zero, $f0
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 3u);
  EXPECT_EQ(stmts[0].ToString(), "f2 = *(f32*)&v0;\n");
  EXPECT_EQ(stmts[1].ToString(), "f0 = f2 + f4;\n");
  EXPECT_EQ(stmts[2].ToString(), "*(s32*)&f0 = (s32)f0;\n");
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternMatchesRoundingToMathFunctions) {
  // ceil.w.s  $f2, $f0; nop; cvt.s.w $f2, $f2 -> f2 = ceilf(f0);
  // floor.w.s $f4, $f0; cvt.s.w $f4, $f4      -> f4 = floorf(f0);
  // round.w.s $f6, $f0; cvt.s.w $f6, $f6      -> f6 = roundf(f0);
  // trunc.w.s $f8, $f0; cvt.s.w $f8, $f8      -> f8 = truncf(f0);
  std::vector<uint32_t> words = {
      0x4600008E,  // 0:  ceil.w.s  $f2, $f0
      0x00000000,  // 4:  nop
      0x468010A0,  // 8:  cvt.s.w   $f2, $f2
      0x4600010F,  // C:  floor.w.s $f4, $f0
      0x46802120,  // 10: cvt.s.w   $f4, $f4
      0x4600018C,  // 14: round.w.s $f6, $f0
      0x468031A0,  // 18: cvt.s.w   $f6, $f6
      0x4600020D,  // 1C: trunc.w.s $f8, $f0
      0x46804220,  // 20: cvt.s.w   $f8, $f8
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 4u);
  EXPECT_EQ(stmts[0].ToString(), "f2 = ceilf(f0);\n");
  EXPECT_EQ(stmts[1].ToString(), "f4 = floorf(f0);\n");
  EXPECT_EQ(stmts[2].ToString(), "f6 = roundf(f0);\n");
  EXPECT_EQ(stmts[3].ToString(), "f8 = truncf(f0);\n");
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternMatchesDoubleRounding) {
  // ceil.w.d  $f2, $f12; cvt.d.w $f2, $f2 -> f2_d = ceil(f12_d);
  // floor.w.d $f4, $f12; cvt.d.w $f4, $f4 -> f4_d = floor(f12_d);
  // round.w.d $f6, $f12; cvt.d.w $f6, $f6 -> f6_d = round(f12_d);
  // trunc.w.d $f8, $f12; cvt.d.w $f8, $f8 -> f8_d = trunc(f12_d);
  std::vector<uint32_t> words = {
      0x4620608E,  // 0:  ceil.w.d  $f2, $f12
      0x468010A1,  // 4:  cvt.d.w   $f2, $f2
      0x4620610F,  // 8:  floor.w.d $f4, $f12
      0x46802121,  // C:  cvt.d.w   $f4, $f4
      0x4620618C,  // 10: round.w.d $f6, $f12
      0x468031A1,  // 14: cvt.d.w   $f6, $f6
      0x4620620D,  // 18: trunc.w.d $f8, $f12
      0x46804221,  // 1C: cvt.d.w   $f8, $f8
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 4u);
  EXPECT_EQ(stmts[0].ToString(), "f2_d = ceil(f12_d);\n");
  EXPECT_EQ(stmts[1].ToString(), "f4_d = floor(f12_d);\n");
  EXPECT_EQ(stmts[2].ToString(), "f6_d = round(f12_d);\n");
  EXPECT_EQ(stmts[3].ToString(), "f8_d = trunc(f12_d);\n");
}

TEST(ExpressionBuilderTest, FloatingPointRoundingInReturnDelaySlot) {
  // Pattern matching func_8009D3D0:
  // ceil.w.s $f14, $f12
  // jr       $ra
  // cvt.s.w  $f0, $f14
  // nop
  std::vector<uint32_t> words = {
      0x4600638E,  // 0: ceil.w.s $f14, $f12
      0x03E00008,  // 4: jr       $ra
      0x46807020,  // 8: cvt.s.w  $f0, $f14
      0x00000000,  // C: nop
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].ToString(), "f0 = ceilf(f12);\n");
  EXPECT_EQ(stmts.back().kind, StatementKind::kReturn);
}

TEST(ExpressionBuilderTest, FloatingPointConversionPatternMatchesFloatToIntRounding) {
  // Single precision:
  // ceil.w.s  $f2, $f0;  mfc1 $v0, $f2 -> v0 = (s32)ceilf(f0);
  // floor.w.s $f4, $f0;  mfc1 $v1, $f4 -> v1 = (s32)floorf(f0);
  // Double precision:
  // ceil.w.d  $f2, $f12; mfc1 $a0, $f2 -> arg0 = (s32)ceil(f12_d);
  // floor.w.d $f4, $f12; mfc1 $a1, $f4 -> arg1 = (s32)floor(f12_d);
  // round.w.d $f6, $f12; mfc1 $a2, $f6 -> arg2 = (s32)round(f12_d);
  std::vector<uint32_t> words = {
      0x4600008E,  // 0:  ceil.w.s  $f2, $f0
      0x44021000,  // 4:  mfc1      $v0, $f2
      0x4600010F,  // 8:  floor.w.s $f4, $f0
      0x44032000,  // C:  mfc1      $v1, $f4
      0x4620608E,  // 10: ceil.w.d  $f2, $f12
      0x44041000,  // 14: mfc1      $a0, $f2
      0x4620610F,  // 18: floor.w.d $f4, $f12
      0x44052000,  // 1C: mfc1      $a1, $f4
      0x4620618C,  // 20: round.w.d $f6, $f12
      0x44063000,  // 24: mfc1      $a2, $f6
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 5u);
  EXPECT_EQ(stmts[0].ToString(), "v0 = (s32)ceilf(f0);\n");
  EXPECT_EQ(stmts[1].ToString(), "v1 = (s32)floorf(f0);\n");
  EXPECT_EQ(stmts[2].ToString(), "arg0 = (s32)ceil(f12_d);\n");
  EXPECT_EQ(stmts[3].ToString(), "arg1 = (s32)floor(f12_d);\n");
  EXPECT_EQ(stmts[4].ToString(), "arg2 = (s32)round(f12_d);\n");
}

TEST(ExpressionBuilderTest, AssignsF0ReturnWhenUsedAfterSubroutineCall) {
  // jal   sinf
  // nop (delay slot)
  // add.s $f2, $f0, $f4 (uses $f0 from call)
  std::vector<uint32_t> words = {
      0x0C000000,  // 0: jal  func_00000000
      0x00000000,  // 4: nop
      0x46040080,  // 8: add.s $f2, $f0, $f4
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].destination_variable, "f0");
  EXPECT_EQ(stmts[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(stmts[0].ToString(), "f0 = func_80000000();\n");
}

TEST(ExpressionBuilderTest, AssignsIndirectCallReturnWhenUsed) {
  // jalr $t9
  // nop
  // addu $a0, $v0, $zero (uses $v0)
  std::vector<uint32_t> words = {
      0x0320F809,  // 0: jalr $t9
      0x00000000,  // 4: nop
      0x00402021,  // 8: addu $a0, $v0, $zero
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].destination_variable, "v0");
  EXPECT_EQ(stmts[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(stmts[0].ToString(), "v0 = ((s32 (*)())temp_t9)();\n");
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

TEST(ExpressionBuilderTest, LiftBranchAndLink) {
  // bal target (0x04110002 -> bgezal $zero, +2 instructions)
  std::vector<uint32_t> words = {
      0x04110002,  // 0: bal +2 (target = 0x8002000C)
      0x00000000,  // 4: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_FALSE(statements.empty());
  EXPECT_EQ(statements[0].kind, StatementKind::kCall);
  EXPECT_THAT(statements[0].ToString(), HasSubstr("func_8002000C"));
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

TEST(ExpressionBuilderTest, LiftCop1CvtAndCompare) {
  std::vector<uint32_t> words = {
      0x46001024,  // 0: cvt.w.s $f0, $f2
      0x4604103C,  // 4: c.lt.s  $f2, $f4
      0x46241032,  // 8: c.eq.d  $f2, $f4
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(statements.size(), 3u);
  EXPECT_EQ(statements[0].ToString(), "*(s32*)&f0 = (s32)f2;\n");
  EXPECT_EQ(statements[1].ToString(), "fcond = f2 < f4;\n");
  EXPECT_EQ(statements[2].ToString(), "fcond = f2_d == f4_d;\n");
}

TEST(ExpressionBuilderTest, FoldedGlobalIndexedLoadAndStoreDoublePrecision) {
  std::string textproto = R"pb(
    entries { name: "g_double_arr" address: 0x80188BFC type: SYMBOL_DATA size: 1024 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      // Double indexed load: f0_d = *(f64*)((u8*)&g_double_arr + v1)
      0x3C018019,  // 0: lui   $at, 0x8019
      0x00230821,  // 1: addu  $at, $at, $v1
      0xD4208BFC,  // 2: ldc1  $f0, -29700($at)
      // Double indexed store: *(f64*)((u8*)&g_double_arr + arg0) = f12_d
      0x3C088019,  // 3: lui   $t0, 0x8019
      0x00884021,  // 4: addu  $t0, $a0, $t0
      0xF50C8BFC,  // 5: sdc1  $f12, -29700($t0)
  };

  auto insts = *DecodeSequence(words);
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "f0_d = *(f64*)((u8*)&g_double_arr + v1);\n");
  EXPECT_EQ(statements[1].ToString(), "*(f64*)((u8*)&g_double_arr + arg0) = f12_d;\n");
}

TEST(ExpressionBuilderTest, FprConstBitsInvalidatedForCallerSavedButPreservedForCalleeSaved) {
  // Load constant 0 into caller-saved $f4 and callee-saved $f20 via mtc1 $zero.
  // Execute jal.
  // After jal, load constant 0x3FF00000 (upper half of 1.0) into $f5 and $f21.
  // $f4 was invalidated by jal, so $f5 must NOT combine into f4_d = 1.0.
  // $f20 was preserved across jal, so $f21 MUST combine with $f20 into f20_d = 1.0.
  std::vector<uint32_t> words = {
      0x44802000,  // 0: mtc1 $zero, $f4
      0x4480A000,  // 1: mtc1 $zero, $f20
      0x0C000400,  // 2: jal  0x80001000
      0x00000000,  // 3: nop
      0x3C083FF0,  // 4: lui  $t0, 0x3FF0
      0x44882800,  // 5: mtc1 $t0, $f5
      0x4488A800,  // 6: mtc1 $t0, $f21
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  std::string combined;
  for (const auto& s : statements) {
    combined += s.ToString();
  }

  EXPECT_THAT(combined, ::testing::Not(HasSubstr("f4_d = 1.0;")));
  EXPECT_THAT(combined, HasSubstr("f20_d = 1.0;"));
}

TEST(ExpressionBuilderTest, Ldc1AndSdc1SuppressSavedFprStackSpillsAndRestores) {
  std::vector<uint32_t> words = {
      0xF7B40018,  // 0: sdc1 $f20, 24($sp)  (matches saved_fpr_offsets -> suppressed)
      0xF7B60020,  // 1: sdc1 $f22, 32($sp)  (offset mismatch -> emitted)
      0xD7B40018,  // 2: ldc1 $f20, 24($sp)  (matches saved_fpr_offsets -> suppressed)
      0xD7B60020,  // 3: ldc1 $f22, 32($sp)  (offset mismatch -> emitted)
  };

  absl::flat_hash_map<FpRegister, int32_t> saved_fpr_offsets = {
      {FpRegister::kF20, 24},
      {FpRegister::kF22, 40},
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(
      insts, FunctionLiftContext{.frame_size = 64, .saved_fpr_offsets = &saved_fpr_offsets});

  ASSERT_THAT(statements, SizeIs(2));
  EXPECT_EQ(statements[0].ToString(), "*(f64*)&var_sp_32 = f22_d;\n");
  EXPECT_EQ(statements[1].ToString(), "f22_d = *(f64*)&var_sp_32;\n");
}

TEST(ExpressionBuilderTest, LiftsNewFpuComparisonOpcodesSingleAndDouble) {
  std::vector<uint32_t> words = {
      // Single-precision comparisons ($f2, $f4)
      0x46041031,  // c.un.s   $f2, $f4 -> ==
      0x46041033,  // c.ueq.s  $f2, $f4 -> ==
      0x4604103B,  // c.ngl.s  $f2, $f4 -> ==
      0x46041035,  // c.ult.s  $f2, $f4 -> <
      0x46041037,  // c.ule.s  $f2, $f4 -> <=
      0x46041039,  // c.ngle.s $f2, $f4 -> <=
      // Double-precision comparisons ($f2, $f4)
      0x46241031,  // c.un.d   $f2, $f4 -> ==
      0x46241033,  // c.ueq.d  $f2, $f4 -> ==
      0x4624103B,  // c.ngl.d  $f2, $f4 -> ==
      0x46241035,  // c.ult.d  $f2, $f4 -> <
      0x46241037,  // c.ule.d  $f2, $f4 -> <=
      0x46241039,  // c.ngle.d $f2, $f4 -> <=
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(12));
  EXPECT_EQ(statements[0].ToString(), "fcond = f2 == f4;\n");
  EXPECT_EQ(statements[1].ToString(), "fcond = f2 == f4;\n");
  EXPECT_EQ(statements[2].ToString(), "fcond = f2 == f4;\n");
  EXPECT_EQ(statements[3].ToString(), "fcond = f2 < f4;\n");
  EXPECT_EQ(statements[4].ToString(), "fcond = f2 <= f4;\n");
  EXPECT_EQ(statements[5].ToString(), "fcond = f2 <= f4;\n");

  EXPECT_EQ(statements[6].ToString(), "fcond = f2_d == f4_d;\n");
  EXPECT_EQ(statements[7].ToString(), "fcond = f2_d == f4_d;\n");
  EXPECT_EQ(statements[8].ToString(), "fcond = f2_d == f4_d;\n");
  EXPECT_EQ(statements[9].ToString(), "fcond = f2_d < f4_d;\n");
  EXPECT_EQ(statements[10].ToString(), "fcond = f2_d <= f4_d;\n");
  EXPECT_EQ(statements[11].ToString(), "fcond = f2_d <= f4_d;\n");
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

TEST(ExpressionBuilderTest, PreCallCallerSavedGprsNotPreservedAcrossCall) {
  // 0: addiu $t0, $zero, 42    # caller-saved $t0
  // 1: addiu $s0, $zero, 100   # callee-saved $s0
  // 2: jal func_80100000
  // 3: nop                     # delay slot
  // 4: addu $v0, $t0, $s0      # uses $t0 (clobbered by call) and $s0 (preserved)
  std::vector<uint32_t> words = {
      0x2408002A,  // addiu $t0, $zero, 42
      0x24100064,  // addiu $s0, $zero, 100
      0x0C040000,  // jal func_80100000
      0x00000000,  // nop
      0x01101021,  // addu $v0, $t0, $s0
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  // $s0 should be recognized as constant 100 across the call, but $t0 must NOT be constant 42
  // across the call since the call clobbered it!
  ASSERT_GE(statements.size(), 4u);
  // Last statement: addu $v0, $t0, $s0
  const auto& last_stmt = statements.back();
  EXPECT_EQ(last_stmt.destination_variable, "v0");
  EXPECT_EQ(last_stmt.ToString(), "v0 = temp_t0 + 100;\n");
}

TEST(ExpressionBuilderTest, Mtc1WithFloatParametersInGeneralRegisters) {
  // mtc1 $a1, $f2
  std::vector<uint32_t> words = {
      0x44851000,  // mtc1 $a1, $f2
  };
  auto insts = *DecodeSequence(words);

  // Without float_parameters_in_general_registers: bitcast is emitted
  {
    FunctionLiftContext ctx;
    auto statements = ExpressionBuilder::LiftInstructions(insts, ctx);
    ASSERT_THAT(statements, SizeIs(1));
    EXPECT_EQ(statements[0].destination_variable, "f2");
    EXPECT_EQ(statements[0].ToString(), "f2 = *(f32*)&arg1;\n");
  }

  // With float_parameters_in_general_registers containing kA1: direct variable name is emitted
  {
    absl::flat_hash_set<Register> float_params = {Register::kA1};
    FunctionLiftContext ctx;
    ctx.float_parameters_in_general_registers = &float_params;
    auto statements = ExpressionBuilder::LiftInstructions(insts, ctx);
    ASSERT_THAT(statements, SizeIs(1));
    EXPECT_EQ(statements[0].destination_variable, "f2");
    EXPECT_EQ(statements[0].ToString(), "f2 = arg1;\n");
  }
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

TEST(ExpressionBuilderTest, NonAdjacentCop1IntFloatConversionsScheduledAcrossInstructions) {
  // 00: mtc1      $a0, $f4
  // 04: addiu     $v1, $a1, 8       # independent instruction scheduled in COP1 delay slot
  // 08: cvt.s.w   $f6, $f4          # consumes $f4 from mtc1
  // 0C: trunc.w.s $f8, $f6
  // 10: sw        $v1, 0($a2)       # independent instruction scheduled in COP1 delay slot
  // 14: mfc1      $v0, $f8          # consumes $f8 from trunc.w.s
  std::vector<uint32_t> words = {
      0x44842000,  // mtc1      $a0, $f4
      0x24A30008,  // addiu     $v1, $a1, 8
      0x468021A0,  // cvt.s.w   $f6, $f4
      0x4600320D,  // trunc.w.s $f8, $f6
      0xACC30000,  // sw        $v1, 0($a2)
      0x44024000,  // mfc1      $v0, $f8
  };
  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 4u);
  EXPECT_EQ(stmts[0].ToString(), "v1 = arg1 + 8;\n");
  EXPECT_EQ(stmts[1].ToString(), "f6 = (f32)arg0;\n");
  EXPECT_EQ(stmts[2].ToString(), "*(s32*)arg2 = v1;\n");
  EXPECT_EQ(stmts[3].ToString(), "v0 = (s32)f6;\n");
}

TEST(ExpressionBuilderTest, Lwc1FollowedByCvtSWLiftsAsDirectIntToFloatMemoryLoad) {
  // 00: lwc1    $f0, 4($a0)
  // 04: cvt.s.w $f2, $f0
  std::vector<uint32_t> words = {
      0xC4800004,  // lwc1    $f0, 4($a0)
      0x468000A0,  // cvt.s.w $f2, $f0
  };
  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(stmts[0].ToString(), "f2 = (f32)*(s32*)(arg0 + 4);\n");
}

TEST(ExpressionBuilderTest, InPlaceEntryArg1DetectedWhenArg0DefinedAndArg2SavedInPrologue) {
  // 00: addiu $sp, $sp, -32
  // 04: sw    $s0, 16($sp)
  // 08: addu  $s0, $a0, $zero
  // 0C: sw    $ra, 28($sp)
  // 10: lw    $a0, 12($s0)
  // 14: addu  $s1, $a2, $zero   # $a2 saved in $s1 before call; $a1 passed in-place
  // 18: jal   0x80001000
  // 1C: nop
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // addiu $sp, $sp, -32
      0xAFB00010,  // sw    $s0, 16($sp)
      0x00808021,  // addu  $s0, $a0, $zero
      0xAFBF001C,  // sw    $ra, 28($sp)
      0x8E04000C,  // lw    $a0, 12($s0)
      0x00C08821,  // addu  $s1, $a2, $zero
      0x0C000400,  // jal   0x80001000
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  absl::flat_hash_map<Register, int32_t> saved_regs = {{Register::kS0, 16}};
  FunctionLiftContext ctx;
  ctx.frame_size = 32;
  ctx.has_calls = true;
  ctx.saved_reg_offsets = &saved_regs;
  ctx.cfg = &cfg;
  ctx.current_block_id = cfg.EntryBlockId();

  auto stmts = ExpressionBuilder::LiftInstructions(insts, ctx);
  ASSERT_FALSE(stmts.empty());
  EXPECT_EQ(stmts.back().ToString(), "func_80001000(arg0, arg1);\n");
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

TEST(ExpressionBuilderTest, TruncWSFollowedBySwc1LiftsAsIntegerCastStore) {
  // 00: trunc.w.s $f2, $f12
  // 04: swc1      $f2, 8($a0)
  std::vector<uint32_t> words = {
      0x4600608D,  // trunc.w.s $f2, $f12
      0xE4820008,  // swc1      $f2, 8($a0)
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(stmts[0].ToString(), "*(s32*)(arg0 + 8) = (s32)f12;\n");
}

TEST(ExpressionBuilderTest, Mtc1WithLuiOriNonZeroLoConstantLiftsAsFloatLiteral) {
  // 0.2f = 0x3E4CCCCD:
  //   00: lui  $at, 0x3E4C
  //   04: ori  $at, $at, 0xCCCD
  //   08: mtc1 $at, $f4
  std::vector<uint32_t> words = {
      0x3C013E4C,  // lui  $at, 0x3E4C
      0x3421CCCD,  // ori  $at, $at, 0xCCCD
      0x44812000,  // mtc1 $at, $f4
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 2u);
  EXPECT_EQ(stmts.back().ToString(), "f4 = 0.200000003f;\n");
}

TEST(ExpressionBuilderTest, Mtc1FollowedByGprDecrementBeforeCvtSWLiftsAsIntToFloatCast) {
  // Pattern from libm __atan2f / __logf polynomial loops:
  //   00: mtc1    $a2, $f8
  //   04: addiu   $a2, $a2, -8   # modifies $a2 before cvt.s.w!
  //   08: mtc1    $t2, $f10      # temporary int-to-fp staging using $f10
  //   0C: cvt.s.w $f18, $f10
  //   10: cvt.s.w $f10, $f8
  std::vector<uint32_t> words = {
      0x44864000,  // 00: mtc1    $a2, $f8
      0x24C6FFF8,  // 04: addiu   $a2, $a2, -8
      0x448A5000,  // 08: mtc1    $t2, $f10
      0x468054A0,  // 0C: cvt.s.w $f18, $f10
      0x468042A0,  // 10: cvt.s.w $f10, $f8
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 3u);
  EXPECT_EQ(stmts[0].ToString(), "f10 = (f32)arg2;\n");
  EXPECT_EQ(stmts[1].ToString(), "arg2 = arg2 - 8;\n");
  EXPECT_EQ(stmts[2].ToString(), "f18 = (f32)temp_t2;\n");
}

TEST(ExpressionBuilderTest, PredecessorCallDelaySlotDoesNotLeakIntoSuccessorCallArguments) {
  // Block 0:
  //   00: beq   $a0, $zero, 0x80000018
  //   04: nop
  // Block 1 (fallthrough):
  //   08: jal   0x80001000
  //   0C: addiu $a1, $zero, 10   # delay slot of jal 0x80001000!
  //   10: j     0x80000018
  //   14: nop
  // Block 2 (join at 0x80000018):
  //   18: addiu $a0, $zero, 1
  //   1C: jal   0x80002000       # should take only 1 arg ($a0), NOT $a1 from Block 1's jal!
  //   20: nop
  //   24: jr    $ra
  //   28: nop
  std::vector<uint32_t> words = {
      0x10800005,  // 00: beq   $a0, $zero, 0x80000018
      0x00000000,  // 04: nop
      0x0C000400,  // 08: jal   0x80001000
      0x2405000A,  // 0C: addiu $a1, $zero, 10
      0x08000006,  // 10: j     0x80000018
      0x00000000,  // 14: nop
      0x24040001,  // 18: addiu $a0, $zero, 1
      0x0C000800,  // 1C: jal   0x80002000
      0x00000000,  // 20: nop
      0x03E00008,  // 24: jr    $ra
      0x00000000,  // 28: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());

  const BasicBlock* join_block = nullptr;
  for (const auto& b : cfg_or->Blocks()) {
    if (!b.instructions.empty() && b.instructions.front().vram == 0x80000018) {
      join_block = &b;
      break;
    }
  }
  ASSERT_NE(join_block, nullptr);

  FunctionLiftContext ctx;
  ctx.cfg = &(*cfg_or);
  auto stmts = ExpressionBuilder::LiftBlock(*join_block, ctx, /*omit_last_instruction=*/false);
  ASSERT_GE(stmts.size(), 2u);
  EXPECT_EQ(stmts[1].ToString(), "func_80002000(1);\n");
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

}  // namespace
}  // namespace rom_nom_nom
