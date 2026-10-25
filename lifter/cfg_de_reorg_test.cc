#include "lifter/cfg_de_reorg.h"

#include <cstdint>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(CfgDeReorgTest, HoistsSplitUnalignedMemoryPairFromDelaySlot) {
  // 0x80000000: lwl $t0, 0($a0)   (0x88880000)
  // 0x80000004: j   0x8000000C    (0x08000003)
  // 0x80000008: lwr $t0, 3($a0)   (0x98880003)
  // 0x8000000C: jr  $ra           (0x03E00008)
  // 0x80000010: addu $v0, $t0, $zero (0x01001021)
  std::vector<uint32_t> words = {
      0x88880000, 0x08000003, 0x98880003, 0x03E00008, 0x01001021,
  };
  auto insts_or = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts_or.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts_or);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* entry = cfg.GetBlock(0);
  ASSERT_NE(entry, nullptr);
  ASSERT_EQ(entry->instructions.size(), 4u);
  EXPECT_EQ(entry->instructions[0].opcode, Opcode::kLwl);
  EXPECT_EQ(entry->instructions[1].opcode, Opcode::kLwr);
  EXPECT_EQ(entry->instructions[2].opcode, Opcode::kJ);
  EXPECT_TRUE(entry->instructions[3].IsNop());
}

TEST(CfgDeReorgTest,
     SinksDeadOnTakenConditionClobberingDelaySlotEnablingSuccessorFoldedComparison) {
  // 0x80000000: beq   $v0, $zero, 0x80000018 (0x10400005)
  // 0x80000004: slti  $v0, $a0, 10           (0x2882000A)
  // 0x80000008: beq   $v0, $zero, 0x80000018 (0x10400003)
  // 0x8000000C: nop                          (0x00000000)
  // 0x80000010: jr    $ra                    (0x03E00008)
  // 0x80000014: addiu $v0, $zero, 1          (0x24020001)
  // 0x80000018: addiu $v0, $zero, 0          (0x24020000)
  // 0x8000001C: jr    $ra                    (0x03E00008)
  // 0x80000020: nop                          (0x00000000)
  std::vector<uint32_t> words = {
      0x10400005, 0x2882000A, 0x10400003, 0x00000000, 0x03E00008,
      0x24020001, 0x24020000, 0x03E00008, 0x00000000,
  };
  auto insts_or = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts_or.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts_or);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* entry = cfg.GetBlock(0);
  const auto* fallthrough = cfg.GetBlock(1);
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(fallthrough, nullptr);
  EXPECT_TRUE(entry->instructions.back().IsNop());
  EXPECT_EQ(entry->instructions.back().unscheduled_clobber_reg, Register::kV0);
  ASSERT_GE(fallthrough->instructions.size(), 2u);
  EXPECT_EQ(fallthrough->instructions[0].opcode, Opcode::kSlti);
  EXPECT_EQ(fallthrough->instructions[0].rt, Register::kV0);
  EXPECT_EQ(fallthrough->instructions[0].immediate, 10);
  EXPECT_EQ(fallthrough->instructions[1].opcode, Opcode::kBeq);
}

TEST(CfgDeReorgTest, SinksDelaySlotBlockingFoldedComparisonIntoFallthrough) {
  // 0x80000000: slti  $at, $v0, 10           (0x2841000A)
  // 0x80000004: beq   $at, $zero, 0x80000018 (0x10200004)
  // 0x80000008: addiu $v0, $zero, 5          (0x24020005)
  // 0x8000000C: sw    $v0, 0($a0)            (0xAC820000)
  // 0x80000010: jr    $ra                    (0x03E00008)
  // 0x80000014: nop                          (0x00000000)
  // 0x80000018: addiu $v0, $zero, 0          (0x24020000)
  // 0x8000001C: jr    $ra                    (0x03E00008)
  // 0x80000020: nop                          (0x00000000)
  std::vector<uint32_t> words = {
      0x2841000A, 0x10200004, 0x24020005, 0xAC820000, 0x03E00008,
      0x00000000, 0x24020000, 0x03E00008, 0x00000000,
  };
  auto insts_or = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts_or.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts_or);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* entry = cfg.GetBlock(0);
  const auto* fallthrough = cfg.GetBlock(1);
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(fallthrough, nullptr);
  EXPECT_TRUE(entry->instructions.back().IsNop());
  ASSERT_GE(fallthrough->instructions.size(), 2u);
  EXPECT_EQ(fallthrough->instructions[0].opcode, Opcode::kAddiu);
  EXPECT_EQ(fallthrough->instructions[0].rt, Register::kV0);
  EXPECT_EQ(fallthrough->instructions[0].immediate, 5);
}

TEST(CfgDeReorgTest, NormalizesBranchLikelyTargetStolenDelaySlotOntoSinglePredecessorTakenBlock) {
  // 0x80000000: beql  $a0, $zero, 0x80000014 (0x50800004)
  // 0x80000004: slti  $v0, $a1, 10           (0x28A2000A)
  // 0x80000008: addiu $v0, $zero, 1          (0x24020001)
  // 0x8000000C: jr    $ra                    (0x03E00008)
  // 0x80000010: nop                          (0x00000000)
  // 0x80000014: beq   $v0, $zero, 0x80000024 (0x10400003)
  // 0x80000018: nop                          (0x00000000)
  // 0x8000001C: jr    $ra                    (0x03E00008)
  // 0x80000020: addiu $v0, $zero, 2          (0x24020002)
  // 0x80000024: jr    $ra                    (0x03E00008)
  // 0x80000028: addiu $v0, $zero, 0          (0x24020000)
  std::vector<uint32_t> words = {
      0x50800004, 0x28A2000A, 0x24020001, 0x03E00008, 0x00000000, 0x10400003,
      0x00000000, 0x03E00008, 0x24020002, 0x03E00008, 0x24020000,
  };
  auto insts_or = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts_or.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts_or);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* entry = cfg.GetBlock(0);
  const auto* taken = cfg.GetBlock(2);
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(entry->instructions[0].opcode, Opcode::kBeq);
  EXPECT_TRUE(entry->instructions[1].IsNop());
  ASSERT_GE(taken->instructions.size(), 2u);
  EXPECT_EQ(taken->instructions[0].opcode, Opcode::kSlti);
  EXPECT_EQ(taken->instructions[1].opcode, Opcode::kBeq);
}

}  // namespace
}  // namespace rom_nom_nom
