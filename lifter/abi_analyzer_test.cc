#include "lifter/abi_analyzer.h"

#include <cstdint>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(AbiAnalyzerTest, RegisterAndFpRegisterVarNames) {
  EXPECT_EQ(AbiAnalyzer::RegisterVarName(Register::kZero), "0");
  EXPECT_EQ(AbiAnalyzer::RegisterVarName(Register::kV0), "v0");
  EXPECT_EQ(AbiAnalyzer::RegisterVarName(Register::kA0), "arg0");
  EXPECT_EQ(AbiAnalyzer::RegisterVarName(Register::kT0), "temp_t0");
  EXPECT_EQ(AbiAnalyzer::FpRegisterVarName(FpRegister::kF0, false), "f0");
  EXPECT_EQ(AbiAnalyzer::FpRegisterVarName(FpRegister::kF12, true), "f12_d");
}

TEST(AbiAnalyzerTest, ParameterCountsAndLiveIn) {
  // addu $v0, $a0, $a1
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_TRUE(AbiAnalyzer::IsRegisterLiveIn(cfg, Register::kA0));
  EXPECT_TRUE(AbiAnalyzer::IsRegisterLiveIn(cfg, Register::kA1));
  EXPECT_FALSE(AbiAnalyzer::IsRegisterLiveIn(cfg, Register::kA2));
  EXPECT_EQ(AbiAnalyzer::DetermineParameterCount(insts), 2);
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsV0(cfg));
  EXPECT_FALSE(AbiAnalyzer::DetermineReturnsF0(cfg));
}

TEST(AbiAnalyzerTest, FpParameterDoubleAndDoubleVars) {
  // add.d $f0, $f12, $f14
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x462E6000,  // add.d $f0, $f12, $f14
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_EQ(AbiAnalyzer::DetermineFpParameterCount(insts), 2);
  EXPECT_TRUE(AbiAnalyzer::IsFpParameterDouble(cfg, FpRegister::kF12));
  EXPECT_TRUE(AbiAnalyzer::IsFpParameterDouble(cfg, FpRegister::kF14));
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0(cfg));
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(cfg));

  auto double_vars = AbiAnalyzer::IdentifyDoubleVariables(cfg);
  EXPECT_TRUE(double_vars.contains("f0_d"));
  EXPECT_TRUE(double_vars.contains("f12_d"));
  EXPECT_TRUE(double_vars.contains("f14_d"));
}

TEST(AbiAnalyzerTest, DetectFloatParametersInGeneralRegistersWithFloatOperation) {
  // mtc1 $a1, $f2
  // add.s $f0, $f2, $f2
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x44851000,  // mtc1 $a1, $f2
      0x46021000,  // add.s $f0, $f2, $f2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_params = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_TRUE(float_params.contains(Register::kA1));
  EXPECT_FALSE(float_params.contains(Register::kA0));
}

TEST(AbiAnalyzerTest, DetectFloatParametersInGeneralRegistersWithConvertFromInteger) {
  // mtc1 $a1, $f2
  // cvt.s.w $f2, $f2
  // add.s $f0, $f2, $f2
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x44851000,  // mtc1 $a1, $f2
      0x468010A0,  // cvt.s.w $f2, $f2
      0x46021000,  // add.s $f0, $f2, $f2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_params = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_FALSE(float_params.contains(Register::kA1));
}

TEST(AbiAnalyzerTest, DetectFloatParametersInGeneralRegistersWithIntegerUse) {
  // addu $v0, $a0, $a1
  // mtc1 $a1, $f2
  // add.s $f0, $f2, $f2
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x44851000,  // mtc1 $a1, $f2
      0x46021000,  // add.s $f0, $f2, $f2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_params = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_FALSE(float_params.contains(Register::kA1));
}

TEST(AbiAnalyzerTest, DetermineParameterCountBoundsStackParametersAndRejectsBogusOffsets) {
  // Valid 6-parameter function (reads $a0..$a3 and 16($sp), 20($sp)):
  // addu $v0, $a0, $a1
  // addu $v0, $v0, $a2
  // addu $v0, $v0, $a3
  // lw   $t0, 16($sp)
  // lw   $t1, 20($sp)
  // addu $v0, $v0, $t0
  // jr   $ra
  // addu $v0, $v0, $t1
  std::vector<uint32_t> valid_six_param_words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x00461021,  // addu $v0, $v0, $a2
      0x00471021,  // addu $v0, $v0, $a3
      0x8FA80010,  // lw $t0, 16($sp)
      0x8FA90014,  // lw $t1, 20($sp)
      0x00481021,  // addu $v0, $v0, $t0
      0x03E00008,  // jr $ra
      0x00491021,  // addu $v0, $v0, $t1
  };
  auto valid_insts = *DecodeSequence(valid_six_param_words, 0x80000000);
  EXPECT_EQ(AbiAnalyzer::DetermineParameterCount(valid_insts), 6);

  // Bogus large stack offset (e.g. lw $t0, 24884($sp)) or unaligned offset without register args
  // must not inflate parameter count into thousands of parameters.
  std::vector<uint32_t> bogus_offset_words = {
      0x8FA86134,  // lw $t0, 24884($sp)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto bogus_insts = *DecodeSequence(bogus_offset_words, 0x80000000);
  EXPECT_EQ(AbiAnalyzer::DetermineParameterCount(bogus_insts), 0);
}

TEST(AbiAnalyzerTest, DetectFloatParametersCopiedToCalleeSavedAndFloatCarrierRegisters) {
  // Rule A: $a1 copied to $s0 in entry block, then $s0 used only by mtc1 $s0, $f12
  // Rule B: $s1 defined only by mfc1 $s1, $f0 (where $f0 is produced by add.s) and used only by
  //         mtc1 $s1, $f2 (used by add.s)
  // 00: addu  $s0, $a1, $zero
  // 04: add.s $f0, $f4, $f6
  // 08: mfc1  $s1, $f0
  // 0C: mtc1  $s0, $f12
  // 10: mtc1  $s1, $f2
  // 14: add.s $f0, $f12, $f2
  // 18: jr    $ra
  // 1C: nop
  std::vector<uint32_t> words = {
      0x00A08021,  // addu  $s0, $a1, $zero
      0x46062000,  // add.s $f0, $f4, $f6
      0x44028800,  // wait: mfc1 rt=$s1(17), fs=$f0(0) -> 0x44000000 | (17 << 16) = 0x44110000
      0x44906000,  // mtc1  $s0, $f12
      0x44911000,  // mtc1  $s1, $f2
      0x46026000,  // add.s $f0, $f12, $f2
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  words[2] = 0x44110000;  // mfc1 $s1, $f0
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_regs = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_TRUE(float_regs.contains(Register::kA1));
  EXPECT_TRUE(float_regs.contains(Register::kS1));
}

TEST(AbiAnalyzerTest, DetectFloatParametersSavedToCalleeSavedFprAcrossCalls) {
  // Incoming $a1 and $a2 are moved to $f20 and $f22 via mtc1 before the first jal,
  // $a1 is redefined in the delay slot of the first jal for an outgoing integer arg,
  // and $a1/$a2 are later loaded via mfc1 $a1, $f20 and mfc1 $a2, $f22 for a second jal.
  std::vector<uint32_t> words = {
      0x4485A000,  // mtc1 $a1, $f20
      0x4486B000,  // mtc1 $a2, $f22
      0x0C000400,  // jal 0x80001000
      0x00802821,  // addu $a1, $a0, $zero
      0x4405A000,  // mfc1 $a1, $f20
      0x4406B000,  // mfc1 $a2, $f22
      0x0C000410,  // jal 0x80001040
      0x00000000,  // nop
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_regs = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_TRUE(float_regs.contains(Register::kA1));
  EXPECT_TRUE(float_regs.contains(Register::kA2));
}

TEST(AbiAnalyzerTest, DetectFloatParametersMovedToCallerSavedFprBeforeCall) {
  // Incoming $a2 and $a3 are moved via mtc1 into caller-saved $f2/$f4 and stored before a call,
  // while $a2 is later overwritten for an outgoing call argument.
  std::vector<uint32_t> words = {
      0x44861000,  // mtc1 $a2, $f2
      0x44872000,  // mtc1 $a3, $f4
      0x46041080,  // add.s $f2, $f2, $f4
      0xE4820000,  // swc1 $f2, 0($a0)
      0x0C000400,  // jal 0x80001000
      0x24060001,  // addiu $a2, $zero, 1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_regs = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_TRUE(float_regs.contains(Register::kA2));
  EXPECT_TRUE(float_regs.contains(Register::kA3));
}

TEST(AbiAnalyzerTest, DetectFloatParametersWhenEntryParamReusedForIntegerReturn) {
  // Pattern from func_8002BD0C:
  //   00: sll   $v0, $a0, 16
  //   04: srl   $a0, $v0, 16
  //   08: mtc1  $a1, $f0       # $a1 read as float parameter
  //   0C: sltiu $v0, $a0, 192
  //   10: beq   $v0, $zero, 0x80000024
  //   14: addu  $a1, $zero, $zero  # $a1 reused as integer return accumulator
  //   18: swc1  $f0, 0($a2)
  //   1C: addiu $a1, $zero, 1
  //   20: jr    $ra
  //   24: addu  $v0, $a1, $zero
  std::vector<uint32_t> words = {
      0x00041400,  // sll   $v0, $a0, 16
      0x00022402,  // srl   $a0, $v0, 16
      0x44850000,  // mtc1  $a1, $f0
      0x2C8200C0,  // sltiu $v0, $a0, 192
      0x10400003,  // beq   $v0, $zero, 0x80000020
      0x00002821,  // addu  $a1, $zero, $zero
      0xE4C00000,  // swc1  $f0, 0($a2)
      0x24050001,  // addiu $a1, $zero, 1
      0x03E00008,  // jr    $ra
      0x00A01021,  // addu  $v0, $a1, $zero
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_regs = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_TRUE(float_regs.contains(Register::kA1));
}

TEST(AbiAnalyzerTest, DetectFloatStackParameterSlotsAndLocalOffsets) {
  // Frame size = 32:
  //   00: addiu $sp, $sp, -32
  //   04: lwc1  $f2, 48($sp)   # caller stack slot 4 (32 + 16 = 48)
  //   08: lw    $s0, 52($sp)   # caller stack slot 5 loaded into float carrier $s0
  //   0C: mtc1  $s0, $f4
  //   10: add.s $f6, $f2, $f4
  //   14: swc1  $f6, 20($sp)   # local float stack slot at offset 20
  //   18: swc1  $f6, 48($sp)   # caller stack slot 4 updated via swc1
  //   1C: lwc1  $f0, 20($sp)
  //   20: jr    $ra
  //   24: addiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // addiu $sp, $sp, -32
      0xC7A20030,  // lwc1  $f2, 48($sp)
      0x8FB00034,  // lw    $s0, 52($sp)
      0x44902000,  // mtc1  $s0, $f4
      0x46041180,  // add.s $f6, $f2, $f4
      0xE7A60014,  // swc1  $f6, 20($sp)
      0xE7A60030,  // swc1  $f6, 48($sp)
      0xC7A00014,  // lwc1  $f0, 20($sp)
      0x03E00008,  // jr    $ra
      0x27BD0020,  // addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_regs = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_TRUE(float_regs.contains(Register::kS0));

  auto float_slots = AbiAnalyzer::DetectFloatStackParameterSlots(cfg);
  EXPECT_TRUE(float_slots.contains(4));
  EXPECT_TRUE(float_slots.contains(5));

  auto float_locals = AbiAnalyzer::DetectFloatLocalStackOffsets(cfg);
  EXPECT_TRUE(float_locals.contains(20));
}

TEST(AbiAnalyzerTest, DetectsCalleeSavedFloatCarriersWithPrologueStackSavesAndHomeSlotSpills) {
  // Frame size = 48:
  //   00: addiu $sp, $sp, -48
  //   04: sw    $s4, 24($sp)   # prologue save of $s4 before definition!
  //   08: sw    $s7, 28($sp)   # prologue save of $s7 before definition!
  //   0C: sw    $fp, 32($sp)   # prologue save of $fp ($s8) before definition!
  //   10: addu  $s4, $a2, $zero # $s4 copies incoming f32 arg2 ($a2)
  //   14: lw    $s7, 64($sp)   # $s7 loads incoming f32 stack slot 4 (48 + 16 = 64)
  //   18: swc1  $f12, 48($sp)  # caller argument home slot spill at frame_size + 0
  //   1C: jal   0x80001000
  //   20: nop
  //   24: mfc1  $fp, $f0       # $fp receives f32 return from helper call
  //   28: mtc1  $s4, $f2       # $s4 used as f32 after call
  //   2C: mtc1  $s7, $f4       # $s7 used as f32 after call
  //   30: mtc1  $fp, $f6       # $fp used as f32
  //   34: lwc1  $f8, 48($sp)   # reload from caller argument home slot 48($sp)
  //   38: add.s $f0, $f2, $f4
  //   3C: add.s $f0, $f0, $f6
  //   40: add.s $f0, $f0, $f8
  //   44: lw    $s4, 24($sp)
  //   48: lw    $s7, 28($sp)
  //   4C: lw    $fp, 32($sp)
  //   50: jr    $ra
  //   54: addiu $sp, $sp, 48
  std::vector<uint32_t> words = {
      0x27BDFFD0,  // 00: addiu $sp, $sp, -48
      0xAFB40018,  // 04: sw    $s4, 24($sp)
      0xAFB7001C,  // 08: sw    $s7, 28($sp)
      0xAFBE0020,  // 0C: sw    $fp, 32($sp)
      0x00C0A021,  // 10: addu  $s4, $a2, $zero
      0x8FB70040,  // 14: lw    $s7, 64($sp)
      0xE7AC0030,  // 18: swc1  $f12, 48($sp)
      0x0C000400,  // 1C: jal   0x80001000
      0x00000000,  // 20: nop
      0x441E0000,  // 24: mfc1  $fp, $f0
      0x44941000,  // 28: mtc1  $s4, $f2
      0x44972000,  // 2C: mtc1  $s7, $f4
      0x449E3000,  // 30: mtc1  $fp, $f6
      0xC7A80030,  // 34: lwc1  $f8, 48($sp)
      0x46041000,  // 38: add.s $f0, $f2, $f4
      0x46060000,  // 3C: add.s $f0, $f0, $f6
      0x46080000,  // 40: add.s $f0, $f0, $f8
      0x8FB40018,  // 44: lw    $s4, 24($sp)
      0x8FB7001C,  // 48: lw    $s7, 28($sp)
      0x8FBE0020,  // 4C: lw    $fp, 32($sp)
      0x03E00008,  // 50: jr    $ra
      0x27BD0030,  // 54: addiu $sp, $sp, 48
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto float_regs = AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  EXPECT_TRUE(float_regs.contains(Register::kS4));
  EXPECT_TRUE(float_regs.contains(Register::kS7));
  EXPECT_TRUE(float_regs.contains(Register::kFp));
  EXPECT_TRUE(float_regs.contains(Register::kA2));

  auto float_slots = AbiAnalyzer::DetectFloatStackParameterSlots(cfg);
  EXPECT_TRUE(float_slots.contains(4));

  auto float_locals = AbiAnalyzer::DetectFloatLocalStackOffsets(cfg);
  EXPECT_TRUE(float_locals.contains(48));
}

TEST(AbiAnalyzerTest, ScansPostCallReturnRegisterUsage) {
  std::vector<uint32_t> words = {
      0x0C000400,  // 00: jal   0x80001000
      0x00000000,  // 04: nop
      0x46200080,  // 08: add.d $f2, $f0, $f0
      0x03E00008,  // 0C: jr    $ra
      0x00401821,  // 10: addu  $v1, $v0, $zero (in jr delay slot)
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  CallReturnInfo info = AbiAnalyzer::ScanCallReturnUsage(insts, 2, /*stop_at_branches=*/true);
  EXPECT_TRUE(info.returns_f0);
  EXPECT_TRUE(info.returns_f0_double);
  EXPECT_TRUE(info.returns_v0);
}

TEST(AbiAnalyzerTest, CompleteDoubleComparisonAndConversionOpcodeCoverage) {
  // Verify IsDoublePrecisionUseOp covers all 16 c.*.d comparison opcodes and all .d read
  // conversions
  const std::vector<Opcode> double_use_ops = {
      Opcode::kCFD,     Opcode::kCUnD,   Opcode::kCEqD,    Opcode::kCUeqD, Opcode::kCOltD,
      Opcode::kCUltD,   Opcode::kCOleD,  Opcode::kCUleD,   Opcode::kCSfD,  Opcode::kCNgleD,
      Opcode::kCSeqD,   Opcode::kCNglD,  Opcode::kCLtD,    Opcode::kCNgeD, Opcode::kCLeD,
      Opcode::kCNgtD,   Opcode::kCvtSD,  Opcode::kCvtWD,   Opcode::kCvtLD, Opcode::kTruncWD,
      Opcode::kRoundWD, Opcode::kCeilWD, Opcode::kFloorWD,
  };
  for (Opcode op : double_use_ops) {
    EXPECT_TRUE(AbiAnalyzer::IsDoublePrecisionUseOp(op))
        << "Expected IsDoublePrecisionUseOp true for opcode " << static_cast<int>(op);
  }

  // CFG exercising c.ult.d, cvt.w.d, round.w.d, ceil.w.d, floor.w.d, cvt.l.d, and cvt.d.l:
  //   00: c.ult.d   $f12, $f14
  //   04: cvt.w.d   $f2, $f16
  //   08: round.w.d $f4, $f18
  //   0C: ceil.w.d  $f6, $f20
  //   10: floor.w.d $f8, $f22
  //   14: cvt.l.d   $f10, $f24
  //   18: cvt.d.l   $f0, $f26
  //   1C: jr        $ra
  //   20: nop
  std::vector<uint32_t> words = {
      0x462E6035,  // c.ult.d   $f12, $f14
      0x462080A4,  // cvt.w.d   $f2, $f16
      0x4620910C,  // round.w.d $f4, $f18
      0x4620A18E,  // ceil.w.d  $f6, $f20
      0x4620B20F,  // floor.w.d $f8, $f22
      0x4620C2A5,  // cvt.l.d   $f10, $f24
      0x46A0D021,  // cvt.d.l   $f0, $f26
      0x03E00008,  // jr        $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  EXPECT_TRUE(AbiAnalyzer::IsFpParameterDouble(cfg, FpRegister::kF12));
  EXPECT_TRUE(AbiAnalyzer::IsFpParameterDouble(cfg, FpRegister::kF14));
  EXPECT_TRUE(AbiAnalyzer::DetermineReturnsF0Double(cfg));

  auto double_vars = AbiAnalyzer::IdentifyDoubleVariables(cfg);
  EXPECT_TRUE(double_vars.contains("f12_d"));
  EXPECT_TRUE(double_vars.contains("f14_d"));
  EXPECT_TRUE(double_vars.contains("f16_d"));
  EXPECT_TRUE(double_vars.contains("f18_d"));
  EXPECT_TRUE(double_vars.contains("f20_d"));
  EXPECT_TRUE(double_vars.contains("f22_d"));
  EXPECT_TRUE(double_vars.contains("f24_d"));
  EXPECT_TRUE(double_vars.contains("f26_d"));
  EXPECT_TRUE(double_vars.contains("f0_d"));
}

}  // namespace
}  // namespace rom_nom_nom
