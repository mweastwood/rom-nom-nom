#include "lifter/register_tracker.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Optional;

TEST(RegisterTrackerTest, InstructionUseDefSets) {
  // 1. addu $t0, $a0, $a1
  auto inst1_or = DecodeInstruction(0x00854021);
  ASSERT_TRUE(inst1_or.ok());
  auto ud1 = GetInstructionUseDef(*inst1_or);
  EXPECT_THAT(ud1.gpr_uses, ElementsAre(Register::kA0, Register::kA1));
  EXPECT_THAT(ud1.gpr_defs, ElementsAre(Register::kT0));

  // 2. nop (sll $zero, $zero, 0)
  auto inst2_or = DecodeInstruction(0x00000000);
  ASSERT_TRUE(inst2_or.ok());
  auto ud2 = GetInstructionUseDef(*inst2_or);
  EXPECT_THAT(ud2.gpr_uses, IsEmpty());
  EXPECT_THAT(ud2.gpr_defs, IsEmpty());

  // 3. lw $v0, 4($sp)
  auto inst3_or = DecodeInstruction(0x8FA20004);
  ASSERT_TRUE(inst3_or.ok());
  auto ud3 = GetInstructionUseDef(*inst3_or);
  EXPECT_THAT(ud3.gpr_uses, ElementsAre(Register::kSp));
  EXPECT_THAT(ud3.gpr_defs, ElementsAre(Register::kV0));

  // 4. sw $ra, 28($sp)
  auto inst4_or = DecodeInstruction(0xAFA3001C);
  // Note: 0xAFBF001C is sw $ra, 28($sp)
  auto inst4_real = DecodeInstruction(0xAFBF001C);
  ASSERT_TRUE(inst4_real.ok());
  auto ud4 = GetInstructionUseDef(*inst4_real);
  EXPECT_THAT(ud4.gpr_uses, ElementsAre(Register::kSp, Register::kRa));
  EXPECT_THAT(ud4.gpr_defs, IsEmpty());

  // 5. jal 0x80001000
  auto inst5_or = DecodeInstruction(0x0C000400);
  ASSERT_TRUE(inst5_or.ok());
  auto ud5 = GetInstructionUseDef(*inst5_or);
  EXPECT_THAT(ud5.gpr_uses, IsEmpty());
  EXPECT_THAT(ud5.gpr_defs, ElementsAre(Register::kRa));
}

TEST(RegisterTrackerTest, ConstantTrackingAndPropagation) {
  std::vector<uint32_t> words = {
      0x2408002A,  // 0x00: addiu $t0, $zero, 42
      0x2509000A,  // 0x04: addiu $t1, $t0, 10
      0x3C018005,  // 0x08: lui $at, 0x8005
      0x34221234,  // 0x0C: ori $v0, $at, 0x1234
      0x24241234,  // 0x10: addiu $a0, $at, 0x1234
  };

  auto insts = *DecodeSequence(words);
  RegisterTracker tracker;
  tracker.Analyze(insts);

  // $zero is always 0
  EXPECT_TRUE(tracker.IsConstant(Register::kZero));
  EXPECT_THAT(tracker.GetConstant(Register::kZero), Optional(0u));

  // $t0 = 42
  EXPECT_TRUE(tracker.IsConstant(Register::kT0));
  EXPECT_THAT(tracker.GetConstant(Register::kT0), Optional(42u));

  // $t1 = 42 + 10 = 52
  EXPECT_TRUE(tracker.IsConstant(Register::kT1));
  EXPECT_THAT(tracker.GetConstant(Register::kT1), Optional(52u));

  // $v0 = 0x80050000 | 0x1234 = 0x80051234
  EXPECT_TRUE(tracker.IsConstant(Register::kV0));
  EXPECT_THAT(tracker.GetConstant(Register::kV0), Optional(0x80051234u));

  // $a0 = 0x80050000 + 0x1234 = 0x80051234
  EXPECT_TRUE(tracker.IsConstant(Register::kA0));
  EXPECT_THAT(tracker.GetConstant(Register::kA0), Optional(0x80051234u));
}

TEST(RegisterTrackerTest, StackFrameDiscovery) {
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 0x00: addiu $sp, $sp, -32
      0xAFBF001C,  // 0x04: sw $ra, 28($sp)
      0xAFB00018,  // 0x08: sw $s0, 24($sp)
      0x0C000010,  // 0x0C: jal 0x80000040 (makes call)
      0x00000000,  // 0x10: nop
  };

  auto insts = *DecodeSequence(words);
  RegisterTracker tracker;
  tracker.Analyze(insts);

  const auto& frame = tracker.FrameInfo();
  EXPECT_EQ(frame.frame_size, 32u);
  EXPECT_THAT(frame.saved_ra_offset, Optional(28));
  EXPECT_FALSE(frame.is_leaf);

  auto it = frame.saved_gpr_offsets.find(Register::kS0);
  ASSERT_NE(it, frame.saved_gpr_offsets.end());
  EXPECT_EQ(it->second, 24);
}

TEST(RegisterTrackerTest, DefUseChains) {
  std::vector<uint32_t> words = {
      0x24040001,  // 0: addiu $a0, $zero, 1 (defines $a0)
      0x24850002,  // 1: addiu $a1, $a0, 2    (uses $a0, defines $a1)
      0x00851021,  // 2: addu  $v0, $a0, $a1   (uses $a0 and $a1, defines $v0)
  };

  auto insts = *DecodeSequence(words);
  RegisterTracker tracker;
  tracker.Analyze(insts);

  // Inst 0 defines $a0, used by inst 1 and inst 2
  EXPECT_THAT(tracker.GetUses(0), ElementsAre(1u, 2u));

  // Inst 1 defines $a1, used by inst 2
  EXPECT_THAT(tracker.GetUses(1), ElementsAre(2u));

  // Inst 2 defines $v0, not used
  EXPECT_THAT(tracker.GetUses(2), IsEmpty());

  // Reaching definitions at the end
  auto def_a0 = tracker.GetReachingDefinition(Register::kA0);
  ASSERT_TRUE(def_a0.has_value());
  EXPECT_EQ(def_a0->instruction_index, 0u);

  auto def_v0 = tracker.GetReachingDefinition(Register::kV0);
  ASSERT_TRUE(def_v0.has_value());
  EXPECT_EQ(def_v0->instruction_index, 2u);
}

}  // namespace
}  // namespace rom_nom_nom
