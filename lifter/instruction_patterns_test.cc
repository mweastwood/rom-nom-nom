#include "lifter/instruction_patterns.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(InstructionPatternsTest, IntToFloatSinglePrecision) {
  // mtc1 $a0, $f4
  // nop
  // cvt.s.w $f0, $f4
  std::vector<uint32_t> words = {
      0x44842000,  // mtc1 $a0, $f4
      0x00000000,  // nop
      0x46802020,  // cvt.s.w $f0, $f4
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto pattern = MatchFloatingPointConversionPattern(insts, 0);
  ASSERT_TRUE(pattern.has_value());
  EXPECT_EQ(pattern->type, FloatingPointConversionType::kIntToFloat);
  EXPECT_TRUE(pattern->is_int_to_float);
  EXPECT_FALSE(pattern->is_rounding);
  EXPECT_FALSE(pattern->is_double);
  EXPECT_EQ(pattern->src_gpr, Register::kA0);
  EXPECT_EQ(pattern->dest_floating_point_reg, FpRegister::kF0);
  EXPECT_EQ(pattern->end_index, 2u);
}

TEST(InstructionPatternsTest, FloatToIntWithTrunc) {
  // trunc.w.s $f6, $f12
  // mfc1 $v0, $f6
  std::vector<uint32_t> words = {
      0x4600618D,  // trunc.w.s $f6, $f12
      0x44023000,  // mfc1 $v0, $f6
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto pattern = MatchFloatingPointConversionPattern(insts, 0);
  ASSERT_TRUE(pattern.has_value());
  EXPECT_EQ(pattern->type, FloatingPointConversionType::kFloatToInt);
  EXPECT_FALSE(pattern->is_int_to_float);
  EXPECT_FALSE(pattern->is_rounding);
  EXPECT_FALSE(pattern->is_double);
  EXPECT_EQ(pattern->src_floating_point_reg, FpRegister::kF12);
  EXPECT_EQ(pattern->dest_gpr, Register::kV0);
  EXPECT_TRUE(pattern->round_func.empty());
  EXPECT_EQ(pattern->end_index, 1u);
}

TEST(InstructionPatternsTest, FloatRoundingPattern) {
  // floor.w.s $f4, $f12
  // cvt.s.w $f0, $f4
  std::vector<uint32_t> words = {
      0x4600610F,  // floor.w.s $f4, $f12
      0x46802020,  // cvt.s.w $f0, $f4
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto pattern = MatchFloatingPointConversionPattern(insts, 0);
  ASSERT_TRUE(pattern.has_value());
  EXPECT_EQ(pattern->type, FloatingPointConversionType::kFloatRounding);
  EXPECT_TRUE(pattern->is_rounding);
  EXPECT_FALSE(pattern->is_double);
  EXPECT_EQ(pattern->round_func, "floorf");
  EXPECT_EQ(pattern->src_floating_point_reg, FpRegister::kF12);
  EXPECT_EQ(pattern->dest_floating_point_reg, FpRegister::kF0);
  EXPECT_EQ(pattern->end_index, 1u);
}

TEST(InstructionPatternsTest, NonMatchingSequenceReturnsNullopt) {
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  EXPECT_FALSE(MatchFloatingPointConversionPattern(insts, 0).has_value());
}

}  // namespace
}  // namespace rom_nom_nom
