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

TEST(RegisterTrackerTest, LogicalImmediateTracking) {
  std::vector<uint32_t> words = {
      0x3408FFFF,  // 0x00: ori  $t0, $zero, 0xFFFF
      0x3109F000,  // 0x04: andi $t1, $t0, 0xF000
      0x390AF0F0,  // 0x08: xori $t2, $t0, 0xF0F0
  };

  auto insts = *DecodeSequence(words);
  RegisterTracker tracker;
  tracker.Analyze(insts);

  // $t0 = 0 | 0xFFFF = 0x0000FFFF (zero-extended)
  EXPECT_TRUE(tracker.IsConstant(Register::kT0));
  EXPECT_THAT(tracker.GetConstant(Register::kT0), Optional(0x0000FFFFu));

  // $t1 = 0x0000FFFF & 0xF000 = 0x0000F000
  EXPECT_TRUE(tracker.IsConstant(Register::kT1));
  EXPECT_THAT(tracker.GetConstant(Register::kT1), Optional(0x0000F000u));

  // $t2 = 0x0000FFFF ^ 0xF0F0 = 0x00000F0F
  EXPECT_TRUE(tracker.IsConstant(Register::kT2));
  EXPECT_THAT(tracker.GetConstant(Register::kT2), Optional(0x00000F0Fu));
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

TEST(RegisterTrackerTest, StepExecution) {
  std::vector<uint32_t> words = {
      0x24020001,  // 0: addiu $v0, $zero, 1
      0x3C018020,  // 1: lui   $at, 0x8020
      0xA0224B38,  // 2: sb    $v0, 19256($at)
  };

  auto insts = *DecodeSequence(words);
  RegisterTracker tracker;

  tracker.Step(insts[0]);
  auto def_v0 = tracker.GetReachingDefinition(Register::kV0);
  ASSERT_TRUE(def_v0.has_value());
  EXPECT_EQ(def_v0->instruction_index, 0u);
  EXPECT_TRUE(tracker.IsConstant(Register::kV0));
  EXPECT_THAT(tracker.GetConstant(Register::kV0), Optional(1u));

  tracker.Step(insts[1]);
  auto def_at = tracker.GetReachingDefinition(Register::kAt);
  ASSERT_TRUE(def_at.has_value());
  EXPECT_EQ(def_at->instruction_index, 1u);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kAt).kind, ValueKind::kSymbolHi);

  tracker.Step(insts[2]);
  EXPECT_THAT(tracker.GetUses(0), ElementsAre(2u));
  EXPECT_THAT(tracker.GetUses(1), ElementsAre(2u));
}

TEST(RegisterTrackerTest, SymbolHiIndexTracking) {
  std::vector<uint32_t> words = {
      0x3C088019,  // 0: lui  $t0, 0x8019
      0x01044821,  // 1: addu $t1, $t0, $a0
  };

  auto insts = *DecodeSequence(words);
  RegisterTracker tracker;
  tracker.Analyze(insts);

  const auto& val_t1 = tracker.GetRegisterValue(Register::kT1);
  EXPECT_EQ(val_t1.kind, ValueKind::kSymbolHiIndex);
  EXPECT_EQ(val_t1.symbol_hi, 0x80190000u);
  EXPECT_EQ(val_t1.index_reg, Register::kA0);
  EXPECT_EQ(val_t1.hi_inst_index, 0u);
  EXPECT_EQ(val_t1.mid_inst_index, 1u);
}

TEST(RegisterTrackerTest, InvalidateRegister) {
  std::vector<uint32_t> words = {
      0x2402002A,  // 0: addiu $v0, $zero, 42
  };

  auto insts = *DecodeSequence(words);
  RegisterTracker tracker;
  tracker.Analyze(insts);

  EXPECT_TRUE(tracker.IsConstant(Register::kV0));
  EXPECT_THAT(tracker.GetConstant(Register::kV0), Optional(42u));

  tracker.Invalidate(Register::kV0);
  EXPECT_FALSE(tracker.IsConstant(Register::kV0));
  EXPECT_EQ(tracker.GetRegisterValue(Register::kV0).kind, ValueKind::kUnknown);
  EXPECT_FALSE(tracker.GetReachingDefinition(Register::kV0).has_value());
}

TEST(RegisterTrackerTest, Cop0AndCop1InstructionUseDef) {
  // mfc0 $v0, $12 (Status) - 0x40026000
  auto mfc0_inst = DecodeInstruction(0x40026000);
  ASSERT_TRUE(mfc0_inst.ok());
  auto ud_mfc0 = GetInstructionUseDef(*mfc0_inst);
  EXPECT_THAT(ud_mfc0.gpr_defs, ElementsAre(Register::kV0));

  // mtc0 $v0, $12 - 0x40826000
  auto mtc0_inst = DecodeInstruction(0x40826000);
  ASSERT_TRUE(mtc0_inst.ok());
  auto ud_mtc0 = GetInstructionUseDef(*mtc0_inst);
  EXPECT_THAT(ud_mtc0.gpr_uses, ElementsAre(Register::kV0));

  // round.w.s $f0, $f12 - 0x4600600C
  auto round_inst = DecodeInstruction(0x4600600C);
  ASSERT_TRUE(round_inst.ok());
  auto ud_round = GetInstructionUseDef(*round_inst);
  EXPECT_THAT(ud_round.fpr_uses, ElementsAre(FpRegister::kF12));
  EXPECT_THAT(ud_round.fpr_defs, ElementsAre(FpRegister::kF0));

  // cvt.w.s $f0, $f12 - 0x46006024
  auto cvtws_inst = DecodeInstruction(0x46006024);
  ASSERT_TRUE(cvtws_inst.ok());
  auto ud_cvtws = GetInstructionUseDef(*cvtws_inst);
  EXPECT_THAT(ud_cvtws.fpr_uses, ElementsAre(FpRegister::kF12));
  EXPECT_THAT(ud_cvtws.fpr_defs, ElementsAre(FpRegister::kF0));

  // c.eq.s $f0, $f2 - 0x46001032
  auto ceqs_inst = DecodeInstruction(0x46001032);
  ASSERT_TRUE(ceqs_inst.ok());
  auto ud_ceqs = GetInstructionUseDef(*ceqs_inst);
  EXPECT_THAT(ud_ceqs.fpr_uses, ::testing::UnorderedElementsAre(FpRegister::kF0, FpRegister::kF2));

  // daddiu $t0, $a0, 16 - 0x64880010
  auto daddiu_inst = DecodeInstruction(0x64880010);
  ASSERT_TRUE(daddiu_inst.ok());
  auto ud_daddiu = GetInstructionUseDef(*daddiu_inst);
  EXPECT_THAT(ud_daddiu.gpr_uses, ElementsAre(Register::kA0));
  EXPECT_THAT(ud_daddiu.gpr_defs, ElementsAre(Register::kT0));

  // sd $s0, 16($sp) - 0xFFB00010
  auto sd_inst = DecodeInstruction(0xFFB00010);
  ASSERT_TRUE(sd_inst.ok());
  auto ud_sd = GetInstructionUseDef(*sd_inst);
  EXPECT_THAT(ud_sd.gpr_uses, ElementsAre(Register::kSp, Register::kS0));

  // bgezall $zero, target
  auto bgezall_inst = DecodeInstruction(0x04130004);
  ASSERT_TRUE(bgezall_inst.ok());
  auto ud_bgezall = GetInstructionUseDef(*bgezall_inst);
  EXPECT_THAT(ud_bgezall.gpr_defs, ElementsAre(Register::kRa));
}

}  // namespace
}  // namespace rom_nom_nom
