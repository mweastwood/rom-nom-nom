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

}  // namespace
}  // namespace rom_nom_nom
