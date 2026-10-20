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

}  // namespace
}  // namespace rom_nom_nom
