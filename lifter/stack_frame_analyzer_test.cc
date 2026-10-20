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

TEST(StackFrameAnalyzerTest, RecognizesCalleeSavedRegisterStoresAtLowOffsetsAsOutgoingCallArgs) {
  // 136-byte frame where $s0..$s2 are saved at 96..104($sp) in prologue,
  // loaded with incoming stack args 152..160($sp), and stored at 16..24($sp) as outgoing call args
  // while a local buffer starts at 32($sp).
  std::vector<uint32_t> words = {
      0x27BDFF78,  // addiu $sp, $sp, -136
      0xAFB00060,  // sw $s0, 96($sp)
      0x8FB00098,  // lw $s0, 152($sp)
      0xAFB10064,  // sw $s1, 100($sp)
      0x8FB1009C,  // lw $s1, 156($sp)
      0xAFB20068,  // sw $s2, 104($sp)
      0x8FB200A0,  // lw $s2, 160($sp)
      0xAFBF006C,  // sw $ra, 108($sp)
      0x27A40020,  // addiu $a0, $sp, 32
      0xAFB00010,  // sw $s0, 16($sp)
      0xAFB10014,  // sw $s1, 20($sp)
      0x0C000400,  // jal 0x80001000
      0xAFB20018,  // sw $s2, 24($sp)
      0x8FBF006C,  // lw $ra, 108($sp)
      0x8FB20068,  // lw $s2, 104($sp)
      0x8FB10064,  // lw $s1, 100($sp)
      0x8FB00060,  // lw $s0, 96($sp)
      0x27BD0088,  // addiu $sp, $sp, 136
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  StackFrameAnalysis analysis = StackFrameAnalyzer::Analyze(cfg);
  EXPECT_EQ(analysis.frame_size, 136);
  EXPECT_TRUE(analysis.sp_address_taken);
  EXPECT_EQ(analysis.prologue.min_saved_reg_offset, 96u);
  EXPECT_EQ(analysis.outgoing_args_size, 32);
  EXPECT_EQ(analysis.sp_buffer_size, 64);
}

TEST(StackFrameAnalyzerTest, RecognizesLeafSavedRegisterAtSpZeroAndPredecessorOutgoingStackArgs) {
  // 1) Leaf function saving $s0 at 0($sp) with 8-byte frame:
  std::vector<uint32_t> leaf_words = {
      0x27BDFFF8,  // addiu $sp, $sp, -8
      0xAFB00000,  // sw    $s0, 0($sp)
      0x24900001,  // addiu $s0, $a0, 1
      0x02001021,  // addu  $v0, $s0, $zero
      0x8FB00000,  // lw    $s0, 0($sp)
      0x03E00008,  // jr    $ra
      0x27BD0008,  // addiu $sp, $sp, 8
  };
  auto leaf_insts = *DecodeSequence(leaf_words, 0x80000000);
  auto leaf_cfg = *ControlFlowGraph::Build(leaf_insts);
  StackFrameAnalysis leaf_analysis = StackFrameAnalyzer::Analyze(leaf_cfg);
  EXPECT_EQ(leaf_analysis.frame_size, 8);
  EXPECT_FALSE(leaf_analysis.has_calls);
  ASSERT_TRUE(leaf_analysis.prologue.saved_reg_offsets.contains(Register::kS0));
  EXPECT_EQ(leaf_analysis.prologue.saved_reg_offsets.at(Register::kS0), 0);
  EXPECT_EQ(leaf_analysis.prologue.min_saved_reg_offset, 0u);

  // 2) Non-leaf function where predecessor branches store outgoing args at 16($sp) and 20($sp)
  // before jumping to a shared call block, while a local buffer starts at 24($sp):
  std::vector<uint32_t> branch_call_words = {
      0x27BDFFC8,  // 00: addiu $sp, $sp, -56
      0xAFBF0030,  // 04: sw    $ra, 48($sp)
      0x10800004,  // 08: beqz  $a0, +4 -> 0x1C
      0x27A50018,  // 0C: addiu $a1, $sp, 24
      0xAFA60010,  // 10: sw    $a2, 16($sp)
      0x10000002,  // 14: b     +2 -> 0x20
      0xAFA70014,  // 18: sw    $a3, 20($sp)
      0xAFA70010,  // 1C: sw    $a3, 16($sp)
      0x0C000400,  // 20: jal   0x80001000
      0xAFA60014,  // 24: sw    $a2, 20($sp)
      0x8FBF0030,  // 28: lw    $ra, 48($sp)
      0x03E00008,  // 2C: jr    $ra
      0x27BD0038,  // 30: addiu $sp, $sp, 56
  };
  auto branch_insts = *DecodeSequence(branch_call_words, 0x80000000);
  auto branch_cfg = *ControlFlowGraph::Build(branch_insts);
  StackFrameAnalysis branch_analysis = StackFrameAnalyzer::Analyze(branch_cfg);
  EXPECT_EQ(branch_analysis.outgoing_args_size, 24);
  EXPECT_EQ(branch_analysis.sp_buffer_size, 24);
}

}  // namespace
}  // namespace rom_nom_nom
