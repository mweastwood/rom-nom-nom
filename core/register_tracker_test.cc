#include "core/register_tracker.h"

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

TEST(RegisterTrackerTest, DetermineReturnsV0FallthroughRedefinesV0AfterNonV0Instruction) {
  // Block 0 (0x80000000):
  //   0x00: beq   $a0, $zero, +6 (-> 0x8000001C: Block 2 return)
  //   0x04: addiu $v0, $zero, 1  (delay slot defines $v0 for taken branch)
  // Block 1 (0x80000008, fallthrough from Block 0):
  //   0x08: addiu $t0, $zero, 10 (does not touch $v0)
  //   0x0C: addiu $v0, $zero, 2  (redefines $v0 -> ft_redefined_v0 = true, breaks outer loop)
  //   0x10: sw    $v0, 0($a1)    (uses redefined $v0; must not be mistaken for use of delay slot)
  //   0x14: jr    $ra
  //   0x18: nop
  // Block 2 (0x8000001C, target of branch from Block 0):
  //   0x1C: jr    $ra
  //   0x20: nop
  std::vector<uint32_t> words = {
      0x10800006,  // 0x00: beq   $a0, $zero, +6 (0x8000001C)
      0x24020001,  // 0x04: addiu $v0, $zero, 1
      0x2408000A,  // 0x08: addiu $t0, $zero, 10
      0x24020002,  // 0x0C: addiu $v0, $zero, 2
      0xAC620000,  // 0x10: sw    $v0, 0($v1)
      0x03E00008,  // 0x14: jr    $ra
      0x00000000,  // 0x18: nop
      0x03E00008,  // 0x1C: jr    $ra
      0x00000000,  // 0x20: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());

  EXPECT_TRUE(DetermineReturnsV0(*cfg_or));
}

TEST(RegisterTrackerTest, DetermineReturnsV0PreservedAcrossComparisonInstruction) {
  // MaxTwo pattern:
  // 0x00: addu $v0, $a0, $zero
  // 0x04: slt  $v1, $a1, $v0
  // 0x08: beql $v1, $zero, +3 (-> 0x80000018)
  // 0x0C: addu $v0, $a1, $zero
  // 0x10: jr   $ra
  // 0x14: nop
  // 0x18: jr   $ra
  // 0x1C: nop
  std::vector<uint32_t> words = {
      0x00801021,  // 0x00: addu $v0, $a0, $zero
      0x00A2182A,  // 0x04: slt  $v1, $a1, $v0
      0x50600003,  // 0x08: beql $v1, $zero, +3 (0x80000018)
      0x00A01021,  // 0x0C: addu $v0, $a1, $zero
      0x03E00008,  // 0x10: jr   $ra
      0x00000000,  // 0x14: nop
      0x03E00008,  // 0x18: jr   $ra
      0x00000000,  // 0x1C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());

  EXPECT_TRUE(DetermineReturnsV0(*cfg_or));
}

TEST(RegisterTrackerTest, FprConstantBitsAndMergeAndOwnedDefinitionInst) {
  RegisterTracker tracker;

  // Step with temporary Instruction objects to ensure Definition::inst owns the copy
  tracker.Step(*DecodeInstruction(0x3C013F80));  // lui $at, 0x3F80 (1.0f high bits)
  tracker.Step(*DecodeInstruction(0x44816000));  // mtc1 $at, $f12
  tracker.Step(*DecodeInstruction(0x4480A000));  // mtc1 $zero, $f20

  auto def_at = tracker.GetReachingDefinition(Register::kAt);
  ASSERT_TRUE(def_at.has_value());
  ASSERT_TRUE(def_at->inst.has_value());
  EXPECT_EQ(def_at->inst->opcode, Opcode::kLui);

  EXPECT_EQ(tracker.GetFprConstantBits(FpRegister::kF12), 0x3F800000u);
  EXPECT_EQ(tracker.GetFprConstantBits(FpRegister::kF20), 0u);

  // Merge with an identical tracker keeps values
  RegisterTracker other = tracker;
  EXPECT_FALSE(tracker.Merge(other));
  EXPECT_EQ(tracker.GetFprConstantBits(FpRegister::kF12), 0x3F800000u);

  // Call clears caller-saved FPRs ($f0..$f19) but preserves callee-saved ($f20)
  other.Step(*DecodeInstruction(0x0C000400));  // jal 0x80001000
  EXPECT_EQ(other.GetFprConstantBits(FpRegister::kF12), std::nullopt);
  EXPECT_EQ(other.GetFprConstantBits(FpRegister::kF20), 0u);

  // Merging `other` into `tracker` clears diverging $f12 but keeps $f20
  EXPECT_TRUE(tracker.Merge(other));
  EXPECT_EQ(tracker.GetFprConstantBits(FpRegister::kF12), std::nullopt);
  EXPECT_EQ(tracker.GetFprConstantBits(FpRegister::kF20), 0u);
}

TEST(RegisterTrackerTest, CallerSavedGprsClearedAcrossCallWhileCalleeSavedPreserved) {
  RegisterTracker tracker;

  // Set up values in caller-saved registers:
  // $at = SymbolHi(0x80200000)
  tracker.Step(*DecodeInstruction(0x3C018020));  // lui $at, 0x8020
  // $v0 = Constant(10)
  tracker.Step(*DecodeInstruction(0x2402000A));  // addiu $v0, $zero, 10
  // $v1 = Constant(20)
  tracker.Step(*DecodeInstruction(0x24030014));  // addiu $v1, $zero, 20
  // $a0 = Constant(30)
  tracker.Step(*DecodeInstruction(0x2404001E));  // addiu $a0, $zero, 30
  // $a1 = Constant(40)
  tracker.Step(*DecodeInstruction(0x24050028));  // addiu $a1, $zero, 40
  // $t0 = Constant(50)
  tracker.Step(*DecodeInstruction(0x24080032));  // addiu $t0, $zero, 50
  // $t9 = Constant(60)
  tracker.Step(*DecodeInstruction(0x2419003C));  // addiu $t9, $zero, 60

  // Set up values in callee-saved registers:
  // $s0 = SymbolHi(0x80300000)
  tracker.Step(*DecodeInstruction(0x3C108030));  // lui $s0, 0x8030
  // $s1 = Constant(100)
  tracker.Step(*DecodeInstruction(0x24110064));  // addiu $s1, $zero, 100
  // $s7 = Constant(200)
  tracker.Step(*DecodeInstruction(0x241700C8));  // addiu $s7, $zero, 200

  // Verify state before call:
  EXPECT_EQ(tracker.GetRegisterValue(Register::kAt).kind, ValueKind::kSymbolHi);
  EXPECT_EQ(tracker.GetConstant(Register::kV0), 10u);
  EXPECT_EQ(tracker.GetConstant(Register::kV1), 20u);
  EXPECT_EQ(tracker.GetConstant(Register::kA0), 30u);
  EXPECT_EQ(tracker.GetConstant(Register::kA1), 40u);
  EXPECT_EQ(tracker.GetConstant(Register::kT0), 50u);
  EXPECT_EQ(tracker.GetConstant(Register::kT9), 60u);
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kAt).has_value());
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kA0).has_value());

  EXPECT_EQ(tracker.GetRegisterValue(Register::kS0).kind, ValueKind::kSymbolHi);
  EXPECT_EQ(tracker.GetConstant(Register::kS1), 100u);
  EXPECT_EQ(tracker.GetConstant(Register::kS7), 200u);
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kS0).has_value());
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kS1).has_value());
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kS7).has_value());

  // Step a call instruction: jal 0x80001000
  tracker.Step(*DecodeInstruction(0x0C000400));

  // Caller-saved registers must be cleared
  EXPECT_EQ(tracker.GetRegisterValue(Register::kAt).kind, ValueKind::kUnknown);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kV0).kind, ValueKind::kUnknown);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kV1).kind, ValueKind::kUnknown);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kA0).kind, ValueKind::kUnknown);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kA1).kind, ValueKind::kUnknown);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kT0).kind, ValueKind::kUnknown);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kT9).kind, ValueKind::kUnknown);
  EXPECT_FALSE(tracker.IsConstant(Register::kV0));
  EXPECT_FALSE(tracker.IsConstant(Register::kA0));
  EXPECT_FALSE(tracker.IsConstant(Register::kT0));
  EXPECT_FALSE(tracker.GetReachingDefinition(Register::kAt).has_value());
  EXPECT_FALSE(tracker.GetReachingDefinition(Register::kV0).has_value());
  EXPECT_FALSE(tracker.GetReachingDefinition(Register::kA0).has_value());
  EXPECT_FALSE(tracker.GetReachingDefinition(Register::kT0).has_value());
  EXPECT_FALSE(tracker.GetReachingDefinition(Register::kT9).has_value());

  // Callee-saved registers must be preserved
  EXPECT_EQ(tracker.GetRegisterValue(Register::kS0).kind, ValueKind::kSymbolHi);
  EXPECT_EQ(tracker.GetRegisterValue(Register::kS0).symbol_hi, 0x80300000u);
  EXPECT_EQ(tracker.GetConstant(Register::kS1), 100u);
  EXPECT_EQ(tracker.GetConstant(Register::kS7), 200u);
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kS0).has_value());
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kS1).has_value());
  EXPECT_TRUE(tracker.GetReachingDefinition(Register::kS7).has_value());
}

TEST(RegisterTrackerTest, DetermineReturnsV0RecognizesCallInLoopTargetRedefinesV0) {
  // Block 0 (entry):
  //   0x80000000: move $s1, $zero
  //   0x80000004: move $s0, $zero
  // Block 1 (do-while loop body, 0x80000008):
  //   0x80000008: jal 0x80001000
  //   0x8000000C: move $a0, $s0
  //   0x80000010: addu $s1, $s1, $v0   <-- reads $v0 from jal, NOT from latch delay slot!
  //   0x80000014: addiu $s0, $s0, 1
  //   0x80000018: bne $s0, $s2, 0x80000008
  //   0x8000001C: move $v0, $s1        <-- sets $v0 for function return when loop exits
  // Block 2 (epilogue, 0x80000020):
  //   0x80000020: jr $ra
  //   0x80000024: nop
  std::vector<uint32_t> words = {
      0x00008821,  // addu $s1, $zero, $zero
      0x00008021,  // addu $s0, $zero, $zero
      0x0C000400,  // jal 0x80001000
      0x02002021,  // addu $a0, $s0, $zero
      0x02228821,  // addu $s1, $s1, $v0
      0x26100001,  // addiu $s0, $s0, 1
      0x1612FFFB,  // bne $s0, $s2, 0x80000008
      0x02201021,  // addu $v0, $s1, $zero
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_TRUE(DetermineReturnsV0(cfg));
}

TEST(RegisterTrackerTest, DetermineReturnsV0RecognizesPreservedV0UsedInStoreComputation) {
  // Pattern from GlobalPointerDereferenceLoadAndStore:
  //   0x80000000: lw $v0, 0($a0)
  //   0x80000004: addu $a1, $v0, $a1
  //   0x80000008: jr $ra
  //   0x8000000C: sw $a1, 0($a0)
  std::vector<uint32_t> words = {
      0x8C820000,  // lw $v0, 0($a0)
      0x00452821,  // addu $a1, $v0, $a1
      0x03E00008,  // jr $ra
      0xAC850000,  // sw $a1, 0($a0)
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_TRUE(DetermineReturnsV0(cfg));
}

TEST(RegisterTrackerTest, PrepareForBlockMarksCrossBlockDefinitionsNonLocal) {
  RegisterTracker tracker;
  tracker.SetEntryParameter(Register::kA0);

  auto entry_def = tracker.GetReachingDefinition(Register::kA0);
  ASSERT_TRUE(entry_def.has_value());
  EXPECT_TRUE(entry_def->is_local);

  tracker.PrepareForBlock();
  auto cross_def = tracker.GetReachingDefinition(Register::kA0);
  ASSERT_TRUE(cross_def.has_value());
  EXPECT_FALSE(cross_def->is_local);

  // A read of $a0 in the successor block should still see the reaching definition
  // without attributing the use to instruction index 0 of the successor block.
  tracker.Step(*DecodeInstruction(0x00801021));  // addu $v0, $a0, $zero
  EXPECT_TRUE(tracker.GetUses(0).empty());
}

TEST(RegisterTrackerTest, DetermineReturnsV0RecognizesPreservedV0LoadedFromAndStoredBackToHead) {
  // Pattern from LinkedListPopFrontReturnNode:
  //   0x80000000: lw $v0, 0($a0)
  //   0x80000004: lw $v1, 0($v0)
  //   0x80000008: jr $ra
  //   0x8000000C: sw $v1, 0($a0)
  std::vector<uint32_t> words = {
      0x8C820000,  // lw $v0, 0($a0)
      0x8C430000,  // lw $v1, 0($v0)
      0x03E00008,  // jr $ra
      0xAC830000,  // sw $v1, 0($a0)
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_TRUE(DetermineReturnsV0(cfg));
}

TEST(RegisterTrackerTest, DetermineReturnsF0RejectsLoopConstantF0ConsumedBeforeFloatStores) {
  // Block 0 (entry):
  //   0x80000000: lui  $at, 0x3780
  //   0x80000004: mtc1 $at, $f0
  //   0x80000008: addiu $t0, $a1, 32
  // Block 1 (loop body, 0x8000000C):
  //   0x8000000C: lw      $v0, 0($a1)
  //   0x80000010: mtc1    $v0, $f2
  //   0x80000014: cvt.s.w $f2, $f2
  //   0x80000018: mul.s   $f2, $f2, $f0
  //   0x8000001C: addiu   $a1, $a1, 4
  //   0x80000020: swc1    $f2, 0($a0)
  //   0x80000024: bne     $a1, $t0, 0x8000000C
  //   0x80000028: addiu   $a0, $a0, 4
  // Block 2 (epilogue, 0x8000002C):
  //   0x8000002C: jr $ra
  //   0x80000030: nop
  std::vector<uint32_t> words = {
      0x3C013780,  // lui $at, 0x3780
      0x44810000,  // mtc1 $at, $f0
      0x24A80020,  // addiu $t0, $a1, 32
      0x8CA20000,  // lw $v0, 0($a1)
      0x44821000,  // mtc1 $v0, $f2
      0x468010A0,  // cvt.s.w $f2, $f2
      0x46001082,  // mul.s $f2, $f2, $f0
      0x24A50004,  // addiu $a1, $a1, 4
      0xE4820000,  // swc1 $f2, 0($a0)
      0x14A8FFF9,  // bne $a1, $t0, 0x8000000C
      0x24840004,  // addiu $a0, $a0, 4
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_FALSE(DetermineReturnsF0(cfg));
}

TEST(RegisterTrackerTest, StepMtc1PropagatesLuiOriSymbolAddressConstantToFpr) {
  // 0.2f = 0x3E4CCCCD -> lui $at, 0x3E4C; ori $at, $at, 0xCCCD; mtc1 $at, $f4
  RegisterTracker tracker;
  tracker.Step(*DecodeInstruction(0x3C013E4C));  // lui  $at, 0x3E4C
  tracker.Step(*DecodeInstruction(0x3421CCCD));  // ori  $at, $at, 0xCCCD
  tracker.Step(*DecodeInstruction(0x44812000));  // mtc1 $at, $f4

  auto f4_bits = tracker.GetFprConstantBits(FpRegister::kF4);
  ASSERT_TRUE(f4_bits.has_value());
  EXPECT_EQ(*f4_bits, 0x3E4CCCCD);
}

TEST(RegisterTrackerTest, DetermineReturnsV0StripsDelaySlotV0DefConsumedAcrossUnconditionalJump) {
  // Pattern from HM64 func_800E1FAC (B97C0.s):
  //   0x80000000: lw    $v0, 0($a0)
  //   0x80000004: bne   $v0, $zero, 0x80000018 (epilogue)
  //   0x80000008: addiu $v0, $zero, 2          (delay slot for fallthrough j -> store tail)
  //   0x8000000C: j     0x80000014             (single-hop unconditional jump to store tail)
  //   0x80000010: nop
  //   0x80000014: sb    $v0, 4($a0)            (consumes $v0 in store tail)
  //   0x80000018: jr    $ra
  //   0x8000001C: nop
  std::vector<uint32_t> words = {
      0x8C820000,  // 00: lw    $v0, 0($a0)
      0x14400004,  // 04: bne   $v0, $zero, 0x80000018
      0x24020002,  // 08: addiu $v0, $zero, 2
      0x08000005,  // 0C: j     0x80000014
      0x00000000,  // 10: nop
      0xA0820004,  // 14: sb    $v0, 4($a0)
      0x03E00008,  // 18: jr    $ra
      0x00000000,  // 1C: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_FALSE(DetermineReturnsV0(cfg));
}

TEST(RegisterTrackerTest, DetermineReturnsV0RejectsVoidFunctionWithIgnoredCalleeReturnOnOneBranch) {
  // Pattern from HM64 func_8005D2B0 / func_8006C628:
  //   0x80000000: beq   $a0, $zero, 0x80000018 (early exit to epilogue without setting $v0)
  //   0x80000004: nop
  //   0x80000008: jal   0x80001000             (callee that returns $v0, ignored by caller)
  //   0x8000000C: nop
  //   0x80000010: j     0x80000018
  //   0x80000014: nop
  //   0x80000018: jr    $ra
  //   0x8000001C: nop
  std::vector<uint32_t> words = {
      0x10800005,  // 00: beq $a0, $zero, 0x80000018
      0x00000000,  // 04: nop
      0x0C000400,  // 08: jal 0x80001000
      0x00000000,  // 0C: nop
      0x08000006,  // 10: j   0x80000018
      0x00000000,  // 14: nop
      0x03E00008,  // 18: jr  $ra
      0x00000000,  // 1C: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  absl::flat_hash_set<std::string> funcs_returning_v0 = {"func_80001000"};

  EXPECT_FALSE(DetermineReturnsV0(cfg, nullptr, &funcs_returning_v0));
}

TEST(RegisterTrackerTest,
     DetermineReturnsV0RejectsVoidFunctionWhenPredecessorConsumesCalleeV0InBranch) {
  // Pattern from HM64 func_8005C00C:
  //   0x00: bne  $a0, $zero, 0x10
  //   0x04: nop
  //   0x08: jal  0x80001000   (callee in funcs_returning_v0, ignored by caller)
  //   0x0C: nop
  //   0x10: jal  0x80002000   (callee in funcs_returning_v0, consumed by branch below)
  //   0x14: nop
  //   0x18: bne  $v0, $zero, 0x28 (branches to shared epilogue after consuming $v0)
  //   0x1C: nop
  //   0x20: jal  0x80003000   (void callee)
  //   0x24: nop
  //   0x28: jr   $ra
  //   0x2C: nop
  std::vector<uint32_t> words = {
      0x14800003,  // 00: bne $a0, $zero, 0x10
      0x00000000,  // 04: nop
      0x0C000400,  // 08: jal 0x80001000
      0x00000000,  // 0C: nop
      0x0C000800,  // 10: jal 0x80002000
      0x00000000,  // 14: nop
      0x14400003,  // 18: bne $v0, $zero, 0x28
      0x00000000,  // 1C: nop
      0x0C000C00,  // 20: jal 0x80003000
      0x00000000,  // 24: nop
      0x03E00008,  // 28: jr  $ra
      0x00000000,  // 2C: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  absl::flat_hash_set<std::string> funcs_returning_v0 = {"func_80001000", "func_80002000"};

  EXPECT_FALSE(DetermineReturnsV0(cfg, nullptr, &funcs_returning_v0));
}

TEST(RegisterTrackerTest, AnalyzeFunctionStructureAndIsomorphicComparison) {
  // Target: 32-byte frame, saves $ra and $s0, has 1 branch, plus 2 trailing alignment NOPs.
  std::vector<uint32_t> target_words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFB00018,  // 08: sw    $s0, 24($sp)
      0x10800002,  // 0C: beq   $a0, $zero, +2 (0x80000018)
      0x00808021,  // 10: addu  $s0, $a0, $zero (move $s0, $a0)
      0x26100001,  // 14: addiu $s0, $s0, 1
      0x02001021,  // 18: addu  $v0, $s0, $zero (move $v0, $s0)
      0x8FBF001C,  // 1C: lw    $ra, 28($sp)
      0x8FB00018,  // 20: lw    $s0, 24($sp)
      0x27BD0020,  // 24: addiu $sp, $sp, 32
      0x03E00008,  // 28: jr    $ra
      0x00000000,  // 2C: nop (return delay slot - preserved)
      0x00000000,  // 30: nop (trailing alignment padding - trimmed)
      0x00000000,  // 34: nop (trailing alignment padding - trimmed)
  };

  // Candidate: same 32-byte frame, saves $ra and $s0 at swapped offsets, uses `or $s0, $a0, $zero`
  // for move, without trailing alignment NOPs.
  std::vector<uint32_t> candidate_words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFB0001C,  // 04: sw    $s0, 28($sp)
      0xAFBF0018,  // 08: sw    $ra, 24($sp)
      0x10800002,  // 0C: beq   $a0, $zero, +2 (0x80001018)
      0x00808025,  // 10: or    $s0, $a0, $zero (move $s0, $a0)
      0x26100005,  // 14: addiu $s0, $s0, 5 (different immediate, same canonical opcode)
      0x02001025,  // 18: or    $v0, $s0, $zero (move $v0, $s0)
      0x8FB0001C,  // 1C: lw    $s0, 28($sp)
      0x8FBF0018,  // 20: lw    $ra, 24($sp)
      0x27BD0020,  // 24: addiu $sp, $sp, 32
      0x03E00008,  // 28: jr    $ra
      0x00000000,  // 2C: nop (return delay slot)
  };

  FunctionStructuralComparison comp =
      CompareFunctionStructure(target_words, 0x80000000, candidate_words, 0x80001000);
  EXPECT_EQ(comp.target.frame_size, 32u);
  EXPECT_EQ(comp.target.instruction_count, 12u);
  EXPECT_TRUE(comp.frame_size_matches);
  EXPECT_TRUE(comp.saved_registers_match);
  EXPECT_TRUE(comp.cfg_topology_matches);
  EXPECT_TRUE(comp.IsStructurallyIsomorphic());
  EXPECT_EQ(comp.normalized_lcs_instructions, 12u);
  EXPECT_DOUBLE_EQ(comp.NormalizedLcsPercentage(), 100.0);
}

TEST(RegisterTrackerTest, CompareFunctionStructureDetectsFrameRegisterAndCfgMismatches) {
  // Target: leaf function with 0 frame size, no saved registers, 1 basic block (0 branches)
  std::vector<uint32_t> target_words = {
      0x24820001,  // addiu $v0, $a0, 1
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Candidate: allocates 16-byte frame, saves $s0, has a conditional branch
  std::vector<uint32_t> candidate_words = {
      0x27BDFFF0,  // addiu $sp, $sp, -16
      0xAFB0000C,  // sw    $s0, 12($sp)
      0x10800001,  // beq   $a0, $zero, +1
      0x24820001,  // addiu $v0, $a0, 1
      0x8FB0000C,  // lw    $s0, 12($sp)
      0x03E00008,  // jr    $ra
      0x27BD0010,  // addiu $sp, $sp, 16
  };

  FunctionStructuralComparison comp =
      CompareFunctionStructure(target_words, 0x80000000, candidate_words, 0x80000000);
  EXPECT_FALSE(comp.frame_size_matches);
  EXPECT_FALSE(comp.saved_registers_match);
  EXPECT_FALSE(comp.cfg_topology_matches);
  EXPECT_FALSE(comp.IsStructurallyIsomorphic());
  EXPECT_EQ(comp.target.instruction_count, 3u);
  EXPECT_EQ(comp.normalized_lcs_instructions, 2u);  // addiu + jr
}

}  // namespace
}  // namespace rom_nom_nom
