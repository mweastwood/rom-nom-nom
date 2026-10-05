#include "lifter/division_pattern.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::Eq;
using ::testing::Ne;
using ::testing::Optional;

TEST(DivisionPatternTest, MatchesSignedModuloWithZeroAndOverflowTrap) {
  // Pattern from InterpolateStep:
  // div   $zero, $v1, $v0
  // bnez  $v0, +2
  // nop
  // break 7
  // addiu $at, $zero, -1
  // bne   $v0, $at, +4
  // lui   $at, 0x8000
  // bne   $v1, $at, +2
  // nop
  // break 6
  // mfhi  $a0
  std::vector<uint32_t> words = {
      0x0062001A,  // div   $zero, $v1, $v0
      0x14400002,  // bnez  $v0, +2
      0x00000000,  // nop
      0x0007000D,  // break 7
      0x2401FFFF,  // addiu $at, $zero, -1
      0x14410004,  // bne   $v0, $at, +4
      0x3C018000,  // lui   $at, 0x8000
      0x14610002,  // bne   $v1, $at, +2
      0x00000000,  // nop
      0x0006000D,  // break 6
      0x00002010,  // mfhi  $a0
  };

  auto insts = *DecodeSequence(words);
  auto pattern = MatchDivisionPattern(insts, 0);

  ASSERT_TRUE(pattern.has_value());
  EXPECT_FALSE(pattern->is_unsigned);
  EXPECT_EQ(pattern->num_reg, Register::kV1);
  EXPECT_EQ(pattern->den_reg, Register::kV0);
  EXPECT_THAT(pattern->mod_dest_reg, Optional(Register::kA0));
  EXPECT_FALSE(pattern->div_dest_reg.has_value());
  EXPECT_EQ(pattern->start_index, 0u);
  EXPECT_EQ(pattern->end_index, 10u);
}

TEST(DivisionPatternTest, MatchesSignedDivisionWithZeroAndOverflowTrap) {
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
  std::vector<uint32_t> words = {
      0x0085001A,  // div   $zero, $a0, $a1
      0x14A00002,  // bnez  $a1, +2
      0x00000000,  // nop
      0x0007000D,  // break 7
      0x2401FFFF,  // addiu $at, $zero, -1
      0x14A10004,  // bne   $a1, $at, +4
      0x3C018000,  // lui   $at, 0x8000
      0x14810002,  // bne   $a0, $at, +2
      0x00000000,  // nop
      0x0006000D,  // break 6
      0x00001012,  // mflo  $v0
  };

  auto insts = *DecodeSequence(words);
  auto pattern = MatchDivisionPattern(insts, 0);

  ASSERT_TRUE(pattern.has_value());
  EXPECT_FALSE(pattern->is_unsigned);
  EXPECT_EQ(pattern->num_reg, Register::kA0);
  EXPECT_EQ(pattern->den_reg, Register::kA1);
  EXPECT_THAT(pattern->div_dest_reg, Optional(Register::kV0));
  EXPECT_FALSE(pattern->mod_dest_reg.has_value());
  EXPECT_EQ(pattern->start_index, 0u);
  EXPECT_EQ(pattern->end_index, 10u);
}

TEST(DivisionPatternTest, MatchesUnsignedDivisionWithZeroTrap) {
  // divu  $zero, $t0, $t1
  // bnez  $t1, +2
  // nop
  // break 7
  // mflo  $s0
  std::vector<uint32_t> words = {
      0x0109001B,  // divu  $zero, $t0, $t1
      0x15200002,  // bnez  $t1, +2
      0x00000000,  // nop
      0x0007000D,  // break 7
      0x00008012,  // mflo  $s0
  };

  auto insts = *DecodeSequence(words);
  auto pattern = MatchDivisionPattern(insts, 0);

  ASSERT_TRUE(pattern.has_value());
  EXPECT_TRUE(pattern->is_unsigned);
  EXPECT_EQ(pattern->num_reg, Register::kT0);
  EXPECT_EQ(pattern->den_reg, Register::kT1);
  EXPECT_THAT(pattern->div_dest_reg, Optional(Register::kS0));
  EXPECT_FALSE(pattern->mod_dest_reg.has_value());
  EXPECT_EQ(pattern->start_index, 0u);
  EXPECT_EQ(pattern->end_index, 4u);
}

TEST(DivisionPatternTest, RejectsNonDivisionInstruction) {
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
  };

  auto insts = *DecodeSequence(words);
  EXPECT_EQ(MatchDivisionPattern(insts, 0), std::nullopt);
}

TEST(DivisionPatternTest, RejectsDivisionWithoutMfhiOrMflo) {
  std::vector<uint32_t> words = {
      0x0062001A,  // div   $zero, $v1, $v0
      0x14400002,  // bnez  $v0, +2
      0x00000000,  // nop
      0x0007000D,  // break 7
      0x00851021,  // addu $v0, $a0, $a1 (unrelated instruction, no mfhi/mflo)
  };

  auto insts = *DecodeSequence(words);
  EXPECT_EQ(MatchDivisionPattern(insts, 0), std::nullopt);
}

}  // namespace
}  // namespace rom_nom_nom
