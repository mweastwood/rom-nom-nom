#include "lifter/stack_frame_analyzer.h"

#include <cstdint>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(StackFrameAnalyzerTest, DetectsFrameSizeSavedRegsAndCalls) {
  // addiu $sp, $sp, -32
  // sw $ra, 28($sp)
  // sw $s0, 24($sp)
  // sw $a0, 16($sp)
  // jal 0x80001000
  // nop
  // lw $ra, 28($sp)
  // lw $s0, 24($sp)
  // addiu $sp, $sp, 32
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // addiu $sp, $sp, -32
      0xAFBF001C,  // sw $ra, 28($sp)
      0xAFB00018,  // sw $s0, 24($sp)
      0xAFA40010,  // sw $a0, 16($sp)
      0x0C000400,  // jal 0x80001000
      0x00000000,  // nop
      0x8FBF001C,  // lw $ra, 28($sp)
      0x8FB00018,  // lw $s0, 24($sp)
      0x27BD0020,  // addiu $sp, $sp, 32
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  StackFrameAnalysis analysis = StackFrameAnalyzer::Analyze(cfg);
  EXPECT_EQ(analysis.frame_size, 32);
  EXPECT_TRUE(analysis.has_calls);
  EXPECT_FALSE(analysis.has_frame_pointer);
  EXPECT_FALSE(analysis.sp_address_taken);
  EXPECT_EQ(analysis.prologue.min_saved_reg_offset, 24u);
  ASSERT_TRUE(analysis.prologue.saved_reg_offsets.contains(Register::kRa));
  EXPECT_EQ(analysis.prologue.saved_reg_offsets.at(Register::kRa), 28);
  ASSERT_TRUE(analysis.prologue.saved_reg_offsets.contains(Register::kS0));
  EXPECT_EQ(analysis.prologue.saved_reg_offsets.at(Register::kS0), 24);
  EXPECT_TRUE(analysis.stack_stores.contains(16));
}

TEST(StackFrameAnalyzerTest, DetectsSpAddressTakenAndComputesBufferSize) {
  // addiu $sp, $sp, -48
  // sw $ra, 44($sp)
  // addiu $a0, $sp, 16
  // jal 0x80001000
  // nop
  // lw $ra, 44($sp)
  // addiu $sp, $sp, 48
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x27BDFFD0,  // addiu $sp, $sp, -48
      0xAFBF002C,  // sw $ra, 44($sp)
      0x27A40010,  // addiu $a0, $sp, 16
      0x0C000400,  // jal 0x80001000
      0x00000000,  // nop
      0x8FBF002C,  // lw $ra, 44($sp)
      0x27BD0030,  // addiu $sp, $sp, 48
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  StackFrameAnalysis analysis = StackFrameAnalyzer::Analyze(cfg);
  EXPECT_EQ(analysis.frame_size, 48);
  EXPECT_TRUE(analysis.sp_address_taken);
  EXPECT_EQ(analysis.sp_buffer_size, 28);
}

TEST(StackFrameAnalyzerTest, LargeFrameWithCalleeSavedFpAndOutgoingArgs) {
  // 128-byte frame like func_800305CC:
  // $s0-$s8 + $ra saved at 88..124($sp)
  // 7-arg call storing args up to 24($sp)
  // addiu $a3, $sp, 48 taking address of stack local
  std::vector<uint32_t> words = {
      0x27BDFF80,  // addiu $sp, $sp, -128
      0xAFB00058,  // sw $s0, 88($sp)
      0xAFB1005C,  // sw $s1, 92($sp)
      0xAFB20060,  // sw $s2, 96($sp)
      0xAFB30064,  // sw $s3, 100($sp)
      0xAFB40068,  // sw $s4, 104($sp)
      0xAFB5006C,  // sw $s5, 108($sp)
      0xAFB60070,  // sw $s6, 112($sp)
      0xAFB70074,  // sw $s7, 116($sp)
      0xAFBE0078,  // sw $fp, 120($sp)
      0xAFBF007C,  // sw $ra, 124($sp)
      0x27A70030,  // addiu $a3, $sp, 48
      0xAFA20010,  // sw $v0, 16($sp)
      0xAFA30014,  // sw $v1, 20($sp)
      0xAFA40018,  // sw $a0, 24($sp)
      0x0C009E54,  // jal 0x80027950
      0x00000000,  // nop
      0x8FB00058,  // lw $s0, 88($sp)
      0x8FB1005C,  // lw $s1, 92($sp)
      0x8FB20060,  // lw $s2, 96($sp)
      0x8FB30064,  // lw $s3, 100($sp)
      0x8FB40068,  // lw $s4, 104($sp)
      0x8FB5006C,  // lw $s5, 108($sp)
      0x8FB60070,  // lw $s6, 112($sp)
      0x8FB70074,  // lw $s7, 116($sp)
      0x8FBE0078,  // lw $fp, 120($sp)
      0x8FBF007C,  // lw $ra, 124($sp)
      0x27BD0080,  // addiu $sp, $sp, 128
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  StackFrameAnalysis analysis = StackFrameAnalyzer::Analyze(cfg);
  EXPECT_EQ(analysis.frame_size, 128);
  EXPECT_FALSE(analysis.has_frame_pointer);
  EXPECT_TRUE(analysis.sp_address_taken);
  EXPECT_EQ(analysis.prologue.min_saved_reg_offset, 88u);
  EXPECT_EQ(analysis.outgoing_args_size, 32);
  EXPECT_EQ(analysis.sp_buffer_size, 56);
}

}  // namespace
}  // namespace rom_nom_nom
