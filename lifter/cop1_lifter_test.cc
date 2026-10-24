#include "lifter/cop1_lifter.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
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

TEST(Cop1LifterTest, LiftsCop1ArithmeticAndUnary) {
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

TEST(Cop1LifterTest, LiftsCop1MemoryLoadsAndStores) {
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

TEST(Cop1LifterTest, LiftsCop1CpuMovesAndControlRegisters) {
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

TEST(Cop1LifterTest, LiftsCop1ConversionsAndDoubleOps) {
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

TEST(Cop1LifterTest, LiftsFoldedFloatingPointGlobalLoadAndStore) {
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

TEST(Cop1LifterTest, FloatingPointConversionPatternMatchesMtc1CvtAndTruncMfc1) {
  // 1. mtc1 $v0, $f2; nop; cvt.s.w $f2, $f2 -> f2 = (f32)v0;
  // 2. trunc.w.s $f0, $f0; nop; mfc1 $v0, $f0 -> v0 = (s32)f0;
  // 3. cvt.w.s $f4, $f12; nop; mfc1 $v1, $f4 -> v1 = (s32)f12;
  // 4. cvt.w.d $f6, $f14; mfc1 $a0, $f6 -> arg0 = (s32)f14_d;
  std::vector<uint32_t> words = {
      0x44821000,  // 0: mtc1 $v0, $f2
      0x00000000,  // 4: nop
      0x468010A0,  // 8: cvt.s.w $f2, $f2
      0x4600000D,  // C: trunc.w.s $f0, $f0
      0x00000000,  // 10: nop
      0x44020000,  // 14: mfc1 $v0, $f0
      0x46006124,  // 18: cvt.w.s $f4, $f12
      0x00000000,  // 1C: nop
      0x44032000,  // 20: mfc1 $v1, $f4
      0x462071A4,  // 24: cvt.w.d $f6, $f14
      0x44043000,  // 28: mfc1 $a0, $f6
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_EQ(stmts.size(), 4u);
  EXPECT_EQ(stmts[0].ToString(), "f2 = (f32)v0;\n");
  EXPECT_EQ(stmts[1].ToString(), "v0 = (s32)f0;\n");
  EXPECT_EQ(stmts[2].ToString(), "v1 = (s32)f12;\n");
  EXPECT_EQ(stmts[3].ToString(), "arg0 = (s32)f14_d;\n");
}

TEST(Cop1LifterTest, FloatingPointConversionPatternMatchesFloatToIntRounding) {
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

TEST(Cop1LifterTest, LiftCop1CvtAndCompare) {
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

TEST(Cop1LifterTest, FoldedGlobalIndexedLoadAndStoreDoublePrecision) {
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

TEST(Cop1LifterTest, FprConstBitsInvalidatedForCallerSavedButPreservedForCalleeSaved) {
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

TEST(Cop1LifterTest, Ldc1AndSdc1SuppressSavedFprStackSpillsAndRestores) {
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

TEST(Cop1LifterTest, LiftsNewFpuComparisonOpcodesSingleAndDouble) {
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

TEST(Cop1LifterTest, Mtc1WithFloatParametersInGeneralRegisters) {
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

TEST(Cop1LifterTest, NonAdjacentCop1IntFloatConversionsScheduledAcrossInstructions) {
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

TEST(Cop1LifterTest, Lwc1FollowedByCvtSWLiftsAsDirectIntToFloatMemoryLoad) {
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

TEST(Cop1LifterTest, TruncWSFollowedBySwc1LiftsAsIntegerCastStore) {
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

TEST(Cop1LifterTest, Mtc1WithLuiOriNonZeroLoConstantLiftsAsFloatLiteral) {
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

TEST(Cop1LifterTest, Mtc1FollowedByGprDecrementBeforeCvtSWLiftsAsIntToFloatCast) {
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

TEST(Cop1LifterTest, FloatingPointConversionPatternHandlesZeroAndDouble) {
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

TEST(Cop1LifterTest, FloatingPointConversionPatternHandlesMultipleHazardNops) {
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

TEST(Cop1LifterTest, FloatingPointConversionPatternRejectsMismatchedRegisters) {
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

TEST(Cop1LifterTest, FloatingPointConversionPatternRejectsNonConversionSuccessors) {
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

TEST(Cop1LifterTest, FloatingPointConversionPatternMatchesRoundingToMathFunctions) {
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

TEST(Cop1LifterTest, FloatingPointConversionPatternMatchesDoubleRounding) {
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

TEST(Cop1LifterTest, FloatingPointRoundingInReturnDelaySlot) {
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

}  // namespace
}  // namespace rom_nom_nom
