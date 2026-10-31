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

TEST(CfgDeReorgTest, SinksTerminatorConflictingDelaySlotIntoShortCircuitFallthroughBranch) {
  // 0x80000000: lw    $v0, 0($a0)            (0x8C820000)
  // 0x80000004: beq   $v0, $zero, 0x8000001C (0x10400005)
  // 0x80000008: lw    $v0, 4($a0)            (0x8C820004)
  // 0x8000000C: beq   $v0, $zero, 0x8000001C (0x10400003)
  // 0x80000010: nop                          (0x00000000)
  // 0x80000014: jr    $ra                    (0x03E00008)
  // 0x80000018: addiu $v0, $zero, 1          (0x24020001)
  // 0x8000001C: jr    $ra                    (0x03E00008)
  // 0x80000020: addiu $v0, $zero, 0          (0x24020000)
  std::vector<uint32_t> words = {
      0x8C820000, 0x10400005, 0x8C820004, 0x10400003, 0x00000000,
      0x03E00008, 0x24020001, 0x03E00008, 0x24020000,
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
  EXPECT_EQ(fallthrough->instructions[0].opcode, Opcode::kLw);
  EXPECT_EQ(fallthrough->instructions[0].rt, Register::kV0);
  EXPECT_EQ(fallthrough->instructions[0].immediate, 4);
  EXPECT_EQ(fallthrough->instructions[1].opcode, Opcode::kBeq);
}

TEST(CfgDeReorgTest, ReconcilesSpeculativeLoopCounterCompensation) {
  // Speculative loop-counter compensation (`reorg.c:fill_slots_from_thread` lines 3527-3585):
  // 0x80000000: bne   $a1, $zero, 0x80000008 (0x14A00001 -> B_head at 0x80000008)
  // 0x80000004: addiu $v0, $zero, 0          (0x24020000)
  // 0x80000008: addiu $v0, $v0, 1            (0x24420001) -- B_head
  // 0x8000000C: lw    $v1, 0($a0)            (0x8C830000) -- B_body
  // 0x80000010: bne   $v0, $a2, 0x8000000C   (0x1446FFFE -> B_body)
  // 0x80000014: addiu $v0, $v0, 1            (0x24420001) -- stolen from B_head
  // 0x80000018: addiu $v0, $v0, -1           (0x2442FFFF) -- compensating adjustment on exit
  // 0x8000001C: jr    $ra                    (0x03E00008)
  // 0x80000020: nop                          (0x00000000)
  std::vector<uint32_t> loop_words = {
      0x14A00001, 0x24020000, 0x24420001, 0x8C830000, 0x1446FFFE,
      0x24420001, 0x2442FFFF, 0x03E00008, 0x00000000,
  };
  auto loop_insts = DecodeSequence(loop_words, 0x80000000);
  ASSERT_TRUE(loop_insts.ok());
  auto loop_cfg_or = ControlFlowGraph::Build(*loop_insts);
  ASSERT_TRUE(loop_cfg_or.ok());
  ControlFlowGraph loop_cfg = *loop_cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&loop_cfg);

  const auto* loop_head = loop_cfg.GetBlock(1);
  const auto* loop_latch = loop_cfg.GetBlock(2);
  const auto* loop_exit = loop_cfg.GetBlock(3);
  ASSERT_NE(loop_head, nullptr);
  ASSERT_NE(loop_latch, nullptr);
  ASSERT_NE(loop_exit, nullptr);
  ASSERT_NE(loop_latch->Terminator(), nullptr);
  EXPECT_TRUE(loop_latch->Terminator()->IsBranchLikely());
  EXPECT_FALSE(loop_latch->instructions.back().IsNop());
  EXPECT_TRUE(loop_exit->instructions.front().IsNop());
}

TEST(CfgDeReorgTest, DuplicatesCrossJumpedTailIntoJumpPredecessor) {
  // Post-reload cross-jumping (`jump.c:find_cross_jump` lines 1921-2034):
  // 0x80000000: beq   $a0, $zero, 0x80000018 (0x10800005 -> else arm at 0x80000018)
  // 0x80000004: nop                          (0x00000000)
  // 0x80000008: addiu $v0, $a1, 1            (0x24A20001) -- then arm
  // 0x8000000C: j     0x8000001C             (0x08000007 -> shared tail B_tail at 0x8000001C)
  // 0x80000010: nop                          (0x00000000)
  // 0x80000014: nop                          (0x00000000)
  // 0x80000018: addiu $v0, $a1, 2            (0x24A20002) -- else arm (falls through to B_tail)
  // 0x8000001C: sw    $v0, 0($a2)            (0xAC820000) -- B_tail: shared store + jump to merge
  // 0x80000020: j     0x8000002C             (0x0800000B -> B_merge at 0x8000002C)
  // 0x80000024: nop                          (0x00000000)
  // 0x80000028: nop                          (0x00000000)
  // 0x8000002C: jr    $ra                    (0x03E00008) -- B_merge
  // 0x80000030: nop                          (0x00000000)
  std::vector<uint32_t> words = {
      0x10800005, 0x00000000, 0x24A20001, 0x08000007, 0x00000000, 0x00000000, 0x24A20002,
      0xAC820000, 0x0800000B, 0x00000000, 0x00000000, 0x03E00008, 0x00000000,
  };
  auto insts = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* then_blk = cfg.FindBlockByVram(0x80000008);
  const auto* merge_blk = cfg.FindBlockByVram(0x8000002C);
  ASSERT_NE(then_blk, nullptr);
  ASSERT_NE(merge_blk, nullptr);
  ASSERT_EQ(then_blk->successors.size(), 1u);
  EXPECT_EQ(then_blk->successors[0], merge_blk->id);
  ASSERT_GE(then_blk->instructions.size(), 4u);
  EXPECT_EQ(then_blk->instructions[1].opcode, Opcode::kSw);
}

TEST(CfgDeReorgTest, HoistsSplitDivideMfloAcrossIndependentInstructionFromDelaySlot) {
  // 0x80000000: div   $zero, $a0, $a1         (0x0085001A)
  // 0x80000004: addiu $t1, $a2, 1             (0x24C90001)
  // 0x80000008: beq   $a2, $zero, 0x80000018  (0x10C00003)
  // 0x8000000C: mflo  $t0                     (0x00004012)
  // 0x80000010: addu  $v0, $t0, $t1           (0x01091021)
  // 0x80000014: jr    $ra                     (0x03E00008)
  // 0x80000018: addu  $v0, $t0, $zero         (0x01001021)
  // 0x8000001C: jr    $ra                     (0x03E00008)
  // 0x80000020: nop                           (0x00000000)
  std::vector<uint32_t> words = {
      0x0085001A, 0x24C90001, 0x10C00003, 0x00004012, 0x01091021,
      0x03E00008, 0x01001021, 0x03E00008, 0x00000000,
  };
  auto insts_or = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts_or.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts_or);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* entry = cfg.GetBlock(0);
  ASSERT_NE(entry, nullptr);
  ASSERT_EQ(entry->instructions.size(), 5u);
  EXPECT_EQ(entry->instructions[0].opcode, Opcode::kDiv);
  EXPECT_EQ(entry->instructions[1].opcode, Opcode::kAddiu);
  EXPECT_EQ(entry->instructions[2].opcode, Opcode::kMflo);
  EXPECT_EQ(entry->instructions[3].opcode, Opcode::kBeq);
  EXPECT_TRUE(entry->instructions[4].IsNop());
}

TEST(CfgDeReorgTest, NormalizesBranchLikelyDelaySlotFeedingSuccessorBranchTerminator) {
  // 0x80000000: beql  $a0, $zero, 0x80000014 (0x50800004)
  // 0x80000004: lw    $v0, 4($a1)            (0x8CA20004)
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
      0x50800004, 0x8CA20004, 0x24020001, 0x03E00008, 0x00000000, 0x10400003,
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
  EXPECT_EQ(taken->instructions[0].opcode, Opcode::kLw);
  EXPECT_EQ(taken->instructions[1].opcode, Opcode::kBeq);
}

TEST(CfgDeReorgTest, DuplicatesPureRegisterCrossJumpedTailAndNonPostDominatingReturnTail) {
  // 1. Pure register-setup cross-jumped tail (`sll $v0, $v0, 2; j 0x8000002C; nop`):
  // 0x80000000: beq   $a0, $zero, 0x80000018 (0x10800005)
  // 0x80000004: nop                          (0x00000000)
  // 0x80000008: addiu $v0, $a1, 1            (0x24A20001)
  // 0x8000000C: j     0x8000001C             (0x08000007 -> B_tail at 0x8000001C)
  // 0x80000010: nop                          (0x00000000)
  // 0x80000014: nop                          (0x00000000)
  // 0x80000018: addiu $v0, $a1, 2            (0x24A20002)
  // 0x8000001C: sll   $v0, $v0, 2            (0x00021080) -- B_tail: pure register op
  // 0x80000020: j     0x8000002C             (0x0800000B -> B_merge at 0x8000002C)
  // 0x80000024: nop                          (0x00000000)
  // 0x80000028: nop                          (0x00000000)
  // 0x8000002C: jr    $ra                    (0x03E00008)
  // 0x80000030: nop                          (0x00000000)
  std::vector<uint32_t> reg_words = {
      0x10800005, 0x00000000, 0x24A20001, 0x08000007, 0x00000000, 0x00000000, 0x24A20002,
      0x00021080, 0x0800000B, 0x00000000, 0x00000000, 0x03E00008, 0x00000000,
  };
  auto reg_insts = DecodeSequence(reg_words, 0x80000000);
  ASSERT_TRUE(reg_insts.ok());
  auto reg_cfg_or = ControlFlowGraph::Build(*reg_insts);
  ASSERT_TRUE(reg_cfg_or.ok());
  ControlFlowGraph reg_cfg = *reg_cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&reg_cfg);

  const auto* then_blk = reg_cfg.FindBlockByVram(0x80000008);
  const auto* merge_blk = reg_cfg.FindBlockByVram(0x8000002C);
  ASSERT_NE(then_blk, nullptr);
  ASSERT_NE(merge_blk, nullptr);
  ASSERT_EQ(then_blk->successors.size(), 1u);
  EXPECT_EQ(then_blk->successors[0], merge_blk->id);
  ASSERT_GE(then_blk->instructions.size(), 4u);
  EXPECT_EQ(then_blk->instructions[1].opcode, Opcode::kSll);

  // 2. Non-post-dominating cross-jumped return tail (`sw $v0, 0($a2); jr $ra; nop`):
  // 0x80000000: beq   $a0, $zero, 0x80000014 (0x10800004 -> B_mid at 0x80000014)
  // 0x80000004: nop                          (0x00000000)
  // 0x80000008: addiu $v0, $zero, 1          (0x24020001) -- B_early_jump
  // 0x8000000C: j     0x80000028             (0x0800000A -> B_ret_tail at 0x80000028)
  // 0x80000010: nop                          (0x00000000)
  // 0x80000014: beq   $a1, $zero, 0x80000024 (0x10A00003 -> B_fallthrough at 0x80000024)
  // 0x80000018: nop                          (0x00000000)
  // 0x8000001C: jr    $ra                    (0x03E00008) -- separate return block
  // 0x80000020: addiu $v0, $zero, -1         (0x2402FFFF)
  // 0x80000024: addiu $v0, $zero, 2          (0x24020002) -- B_fallthrough into B_ret_tail
  // 0x80000028: sw    $v0, 0($a2)            (0xAC820000) -- B_ret_tail
  // 0x8000002C: jr    $ra                    (0x03E00008)
  // 0x80000030: nop                          (0x00000000)
  std::vector<uint32_t> ret_words = {
      0x10800004, 0x00000000, 0x24020001, 0x0800000A, 0x00000000, 0x10A00003, 0x00000000,
      0x03E00008, 0x2402FFFF, 0x24020002, 0xAC820000, 0x03E00008, 0x00000000,
  };
  auto ret_insts = DecodeSequence(ret_words, 0x80000000);
  ASSERT_TRUE(ret_insts.ok());
  auto ret_cfg_or = ControlFlowGraph::Build(*ret_insts);
  ASSERT_TRUE(ret_cfg_or.ok());
  ControlFlowGraph ret_cfg = *ret_cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&ret_cfg);

  const auto* early_jump_blk = ret_cfg.FindBlockByVram(0x80000008);
  ASSERT_NE(early_jump_blk, nullptr);
  EXPECT_TRUE(early_jump_blk->successors.empty());
  ASSERT_GE(early_jump_blk->instructions.size(), 4u);
  EXPECT_EQ(early_jump_blk->instructions[1].opcode, Opcode::kSw);
  EXPECT_NE(early_jump_blk->Terminator(), nullptr);
  EXPECT_TRUE(early_jump_blk->Terminator()->IsReturn());
}

TEST(CfgDeReorgTest, DuplicatesCrossJumpedTailWithNonNopDelaySlotsAndSinksFprDelaySlot) {
  // 1. Cross-jumped return tail where both `pred_blk`'s `j` and `tail_blk`'s `jr $ra` have
  // non-nop delay slots filled by `reorg.c`:
  // 0x80000000: beq   $a0, $zero, 0x80000010 (0x10800003 -> B_mid at 0x80000010)
  // 0x80000004: nop                          (0x00000000)
  // 0x80000008: j     0x80000024             (0x08000009 -> B_ret_tail at 0x80000024)
  // 0x8000000C: addiu $v0, $zero, 10         (0x2402000A) -- non-nop delay slot of `j`!
  // 0x80000010: beq   $a1, $zero, 0x80000020 (0x10A00003 -> B_fallthrough at 0x80000020)
  // 0x80000014: nop                          (0x00000000)
  // 0x80000018: jr    $ra                    (0x03E00008) -- separate return block
  // 0x8000001C: addiu $v0, $zero, -1         (0x2402FFFF)
  // 0x80000020: addiu $v0, $zero, 20         (0x24020014) -- B_fallthrough into B_ret_tail
  // 0x80000024: jr    $ra                    (0x03E00008) -- B_ret_tail (body in delay slot!)
  // 0x80000028: sw    $v0, 0($a2)            (0xAC820000) -- non-nop delay slot of `jr $ra`!
  std::vector<uint32_t> cj_words = {
      0x10800003, 0x00000000, 0x08000009, 0x2402000A, 0x10A00003, 0x00000000,
      0x03E00008, 0x2402FFFF, 0x24020014, 0x03E00008, 0xAC820000,
  };
  auto cj_insts = DecodeSequence(cj_words, 0x80000000);
  ASSERT_TRUE(cj_insts.ok());
  auto cj_cfg_or = ControlFlowGraph::Build(*cj_insts);
  ASSERT_TRUE(cj_cfg_or.ok());
  ControlFlowGraph cj_cfg = *cj_cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cj_cfg);

  const auto* pred_blk = cj_cfg.FindBlockByVram(0x80000008);
  ASSERT_NE(pred_blk, nullptr);
  EXPECT_TRUE(pred_blk->successors.empty());
  ASSERT_EQ(pred_blk->instructions.size(), 3u);
  EXPECT_EQ(pred_blk->instructions[0].opcode, Opcode::kAddiu);
  EXPECT_TRUE(pred_blk->instructions[1].IsReturn());
  EXPECT_EQ(pred_blk->instructions[2].opcode, Opcode::kSw);

  // 2. FPR delay slot (`lwc1 $f4, 0($a0)`) dead on taken return path and feeding `c.lt.s` on
  // fallthrough:
  // 0x80000000: beq   $a1, $zero, 0x8000001C (0x10A00006 -> B_ret at 0x8000001C)
  // 0x80000004: lwc1  $f4, 0($a0)            (0xC4840000) -- FPR delay slot!
  // 0x80000008: c.lt.s $f4, $f12             (0x460C203C)
  // 0x8000000C: bc1t  0x8000001C             (0x45010003)
  // 0x80000010: nop                          (0x00000000)
  // 0x80000014: swc1  $f4, 4($a0)            (0xE4840004)
  // 0x80000018: nop                          (0x00000000)
  // 0x8000001C: jr    $ra                    (0x03E00008)
  // 0x80000020: nop                          (0x00000000)
  std::vector<uint32_t> fpr_words = {
      0x10A00006, 0xC4840000, 0x460C203C, 0x45010003, 0x00000000,
      0xE4840004, 0x00000000, 0x03E00008, 0x00000000,
  };
  auto fpr_insts = DecodeSequence(fpr_words, 0x80000000);
  ASSERT_TRUE(fpr_insts.ok());
  auto fpr_cfg_or = ControlFlowGraph::Build(*fpr_insts);
  ASSERT_TRUE(fpr_cfg_or.ok());
  ControlFlowGraph fpr_cfg = *fpr_cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&fpr_cfg);

  const auto* entry_blk = fpr_cfg.FindBlockByVram(0x80000000);
  const auto* ft_blk = fpr_cfg.FindBlockByVram(0x80000008);
  ASSERT_NE(entry_blk, nullptr);
  ASSERT_NE(ft_blk, nullptr);
  EXPECT_TRUE(entry_blk->instructions.back().IsNop());
  EXPECT_EQ(ft_blk->instructions.front().opcode, Opcode::kLwc1);
}

TEST(CfgDeReorgTest, RelaxDelaySlotUnschedulerRetargetsIntraLoopAndAcyclicJoinBlocks) {
  // 0x80000000: bne   $a0, $zero, 0x80000010 (0x14800003 -> B_else at 0x80000010)
  // 0x80000004: nop                          (0x00000000)
  // 0x80000008: j     0x80000024             (0x08000009 -> B_join at 0x80000024)
  // 0x8000000C: sw    $a1, 0($a2)            (0xAC850000)
  // 0x80000010: bnel  $a1, $zero, 0x80000028 (0x54A00005 -> B_after at 0x80000028, bypassing
  // B_join) 0x80000014: addiu $a2, $a2, 4            (0x24C60004) -- stolen from B_join!
  // 0x80000018: beq   $a3, $zero, 0x80000024 (0x10E00002 -> B_join at 0x80000024)
  // 0x8000001C: nop                          (0x00000000)
  // 0x80000020: sw    $zero, 0($a2)          (0xAC800000)
  // 0x80000024: addiu $a2, $a2, 4            (0x24C60004) -- B_join (1-insn fallthrough into
  // B_after) 0x80000028: jr    $ra                    (0x03E00008) -- B_after 0x8000002C: addu $v0,
  // $a2, $zero        (0x00C01021)
  std::vector<uint32_t> words = {
      0x14800003, 0x00000000, 0x08000009, 0xAC850000, 0x54A00005, 0x24C60004,
      0x10E00002, 0x00000000, 0xAC800000, 0x24C60004, 0x03E00008, 0x00C01021,
  };
  auto insts = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* else_blk = cfg.FindBlockByVram(0x80000010);
  const auto* join_blk = cfg.FindBlockByVram(0x80000024);
  ASSERT_NE(else_blk, nullptr);
  ASSERT_NE(join_blk, nullptr);
  ASSERT_NE(else_blk->Terminator(), nullptr);
  EXPECT_EQ(else_blk->Terminator()->opcode, Opcode::kBne);
  EXPECT_TRUE(else_blk->instructions.back().IsNop());
  EXPECT_NE(std::find(else_blk->successors.begin(), else_blk->successors.end(), join_blk->id),
            else_blk->successors.end());
}

TEST(CfgDeReorgTest, DefaultPipelineContainsExpectedModularPasses) {
  CfgPassPipeline pipeline = CfgDeReorg::BuildDefaultPipeline();
  ASSERT_EQ(pipeline.PassCount(), 7u);
  EXPECT_EQ(pipeline.Passes()[0]->Name(), "SimpleDelaySlotUnschedulerPass");
  EXPECT_EQ(pipeline.Passes()[1]->Name(), "TargetHeaderDelaySlotReconcilerPass");
  EXPECT_EQ(pipeline.Passes()[2]->Name(), "RelaxDelaySlotUnschedulerPass");
  EXPECT_EQ(pipeline.Passes()[3]->Name(), "EagerThreadDelaySlotUnschedulerPass");
  EXPECT_EQ(pipeline.Passes()[4]->Name(), "CrossJumpTailDuplicatorPass");
  EXPECT_EQ(pipeline.Passes()[5]->Name(), "SimpleDelaySlotUnschedulerPass");
  EXPECT_EQ(pipeline.Passes()[6]->Name(), "EagerThreadDelaySlotUnschedulerPass");
}

TEST(CfgDeReorgTest, CrossJumpTailDuplicatorDuplicatesFallthroughTailIntoJumpPredecessors) {
  // Entry (0x80000000) branches on $a0 == 0 to B_right (0x80000018).
  // Left subtree:
  //   0x80000008: beq $a1, $zero, 0x80000028 (B_ret, bypassing B_tail)
  //   0x80000010: j   0x80000020 (B_tail) / sw $a2, 0($a3)
  // Right subtree:
  //   0x80000018: sw  $a1, 0($a3) (falls through into B_tail at 0x80000020)
  // Shared fallthrough tail B_tail (0x80000020):
  //   0x80000020: sw  $zero, 4($a3)
  //   0x80000024: ori $v0, $zero, 1 (falls through into B_ret at 0x80000028)
  // B_ret (0x80000028):
  //   0x80000028: lw  $ra, 20($sp)
  //   0x8000002C: jr  $ra
  //   0x80000030: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x10800005, 0x00000000,  // 0x80000000: beq $a0, $zero, 0x80000018 / nop
      0x10A00007, 0x00000000,  // 0x80000008: beq $a1, $zero, 0x80000028 / nop
      0x08000008, 0xAC660000,  // 0x80000010: j 0x80000020 / sw $a2, 0($a3)
      0xAC650000, 0x00000000,  // 0x80000018: sw $a1, 0($a3) / nop (falls through to 0x80000020)
      0xAC600004, 0x34020001,  // 0x80000020: sw $zero, 4($a3) / ori $v0, $zero, 1
      0x8FBF0014, 0x03E00008,  // 0x80000028: lw $ra, 20($sp) / jr $ra
      0x27BD0018,              // 0x80000030: addiu $sp, $sp, 24
  };
  auto insts = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* left_jump_blk = cfg.FindBlockByVram(0x80000010);
  const auto* tail_blk = cfg.FindBlockByVram(0x80000020);
  const auto* ret_blk = cfg.FindBlockByVram(0x80000028);
  ASSERT_NE(left_jump_blk, nullptr);
  ASSERT_NE(tail_blk, nullptr);
  ASSERT_NE(ret_blk, nullptr);

  // Left jump block should now jump directly to B_ret (0x80000028) and contain the duplicated
  // fallthrough tail instructions (`sw $zero, 4($a3)` and `ori $v0, $zero, 1`).
  ASSERT_EQ(left_jump_blk->successors.size(), 1u);
  EXPECT_EQ(left_jump_blk->successors[0], ret_blk->id);
  EXPECT_EQ(tail_blk->predecessors.size(), 1u);

  bool has_duplicated_sw = false;
  bool has_duplicated_ori = false;
  for (const auto& inst : left_jump_blk->instructions) {
    if (inst.opcode == Opcode::kSw && inst.rt == Register::kZero && inst.immediate == 4) {
      has_duplicated_sw = true;
    }
    if (inst.opcode == Opcode::kOri && inst.rt == Register::kV0 && inst.immediate == 1) {
      has_duplicated_ori = true;
    }
  }
  EXPECT_TRUE(has_duplicated_sw);
  EXPECT_TRUE(has_duplicated_ori);
}

TEST(CfgDeReorgTest,
     CrossJumpTailDuplicatorClonesTailForConditionalBranchSubtreeAndUnschedulesDelaySlot) {
  // Entry (0x80000000) branches on $a0 == 0 to B_right (0x80000018).
  // Left subtree (0x80000008):
  //   0x80000008: beql  $a1, $zero, 0x80000024 (targets B_tail+4, having stolen `addiu $a1, $a1,
  //   1`) 0x8000000C: addiu $a1, $a1, 1 0x80000010: jr    $ra / addiu $v0, $zero, -1
  // Right subtree (0x80000018):
  //   0x80000018: bne   $a2, $zero, 0x80000020 (targets B_tail) / nop
  //   (falls through into B_tail at 0x80000020)
  // Shared return tail B_tail (0x80000020):
  //   0x80000020: addiu $a1, $a1, 1
  //   0x80000024: sw    $a1, 0($a3)
  //   0x80000028: jr    $ra
  //   0x8000002C: ori   $v0, $zero, 1
  std::vector<uint32_t> words = {
      0x10800005, 0x00000000,  // 0x80000000: beq   $a0, $zero, 0x80000018 / nop
      0x50A00005, 0x24A50001,  // 0x80000008: beql  $a1, $zero, 0x80000024 / addiu $a1, $a1, 1
      0x03E00008, 0x2402FFFF,  // 0x80000010: jr    $ra / addiu $v0, $zero, -1
      0x14C00001, 0x00000000,  // 0x80000018: bne   $a2, $zero, 0x80000020 / nop
      0x24A50001, 0xAC650000,  // 0x80000020: addiu $a1, $a1, 1 / sw $a1, 0($a3)
      0x03E00008, 0x34020001,  // 0x80000028: jr    $ra / ori $v0, $zero, 1
  };
  auto insts = DecodeSequence(words, 0x80000000);
  ASSERT_TRUE(insts.ok());
  auto cfg_or = ControlFlowGraph::Build(*insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const auto* left_branch_blk = cfg.FindBlockByVram(0x80000008);
  const auto* tail_blk = cfg.FindBlockByVram(0x80000024);
  ASSERT_NE(left_branch_blk, nullptr);
  ASSERT_NE(tail_blk, nullptr);

  // Left conditional branch should have been retargeted to a newly cloned tail block, and the
  // trailing EagerThreadDelaySlotUnschedulerPass should have demoted `beql` -> `beq` + `nop` and
  // sunk `addiu $a1, $a1, 1` into the cloned block.
  ASSERT_NE(left_branch_blk->Terminator(), nullptr);
  EXPECT_EQ(left_branch_blk->Terminator()->opcode, Opcode::kBeq);
  EXPECT_TRUE(left_branch_blk->instructions.back().IsNop());

  int cloned_target_id = -1;
  for (const auto& edge : left_branch_blk->outgoing_edges) {
    if (edge.type == EdgeType::kBranch) {
      cloned_target_id = static_cast<int>(edge.to_block_id);
    }
  }
  ASSERT_GE(cloned_target_id, 0);
  EXPECT_NE(static_cast<uint32_t>(cloned_target_id), tail_blk->id);

  const BasicBlock& cloned_blk = cfg.Blocks()[cloned_target_id];
  EXPECT_EQ(cloned_blk.predecessors.size(), 1u);
  EXPECT_TRUE(cloned_blk.HasReturn());
  ASSERT_FALSE(cloned_blk.instructions.empty());
  EXPECT_EQ(cloned_blk.instructions.front().opcode, Opcode::kAddiu);
  EXPECT_EQ(cloned_blk.instructions.front().rt, Register::kA1);
}

TEST(CfgDeReorgTest,
     RelaxDelaySlotUnschedulerRetargetsMultipleBranchesToOneInstructionPredecessor) {
  // Models a decision tree where 0x04 (`blez $v1, 0x1C`) splits into two arms: the left arm at
  // 0x0C (`beq $v1, $v0, 0x2C / addu $a1, $zero, $zero`) stole the 1-instruction initialization
  // block at 0x28 (`addu $a1, $zero, $zero`), while the right arm at 0x1C (`beq $v1, $a2, 0x28 /
  // nop`) branches directly to 0x28.
  std::vector<uint32_t> words = {
      0x24020002,  // 0x00: addiu $v0, $zero, 2
      0x18600005,  // 0x04: blez  $v1, +5 -> 0x1C
      0x00000000,  // 0x08: nop
      0x10620007,  // 0x0C: beq   $v1, $v0, +7 -> 0x2C (taken_block)
      0x00002821,  // 0x10: addu  $a1, $zero, $zero (stolen from 0x28)
      0x03E00008,  // 0x14: jr    $ra
      0x00001021,  // 0x18: addu  $v0, $zero, $zero
      0x10600002,  // 0x1C: beq   $v1, $zero, +2 -> 0x28 (pred_blk)
      0x00000000,  // 0x20: nop
      0x03E00008,  // 0x24: jr    $ra
      0x00002821,  // 0x28 (pred_blk): addu $a1, $zero, $zero (falls through to 0x2C)
      0x00A41021,  // 0x2C (taken_block): addu $v0, $a1, $a0
      0x03E00008,  // 0x30: jr    $ra
      0x00000000,  // 0x34: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  RelaxDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b_left = cfg.FindBlockByVram(0x8000000C);
  const BasicBlock* pred_blk = cfg.FindBlockByVram(0x80000028);
  ASSERT_NE(b_left, nullptr);
  ASSERT_NE(pred_blk, nullptr);
  EXPECT_TRUE(b_left->instructions.back().IsNop());
  EXPECT_EQ(pred_blk->predecessors.size(), 2u);
}

TEST(CfgDeReorgTest,
     RelaxDelaySlotUnschedulerUnschedulesAcrossShortCircuitChainWithoutPostDominance) {
  // Models a 3-branch short-circuit condition where:
  // - Block 0 (0x00): `beq $a0, $zero, 0x2C (taken_block)` stole `addiu $v0, $a3, 4` from `0x28`
  //   (`pred_blk`) into its non-likely delay slot (`$v0` is dead on fallthrough 0x08).
  // - Block 1 (0x08): `beq $a1, $zero, 0x20 (then_ret)` branches to an early return (so `0x28`
  //   does NOT post-dominate `0x08`), or falls through to Block 2 (0x10).
  // - Block 2 (0x10): `bne $a2, $zero, 0x28 (pred_blk)` branches to `0x28` (`pred_blk`) or falls
  //   through to `0x20` (`then_ret`).
  // Because `0x08 -> 0x10 -> 0x28` can reach `pred_blk` (`0x28`) and preserves `$a3`,
  // `RelaxDelaySlotUnschedulerPass` should retarget Block 0 from `0x2C` to `0x28` and replace its
  // delay slot with `nop`.
  std::vector<uint32_t> words = {
      0x1080000A,  // 0x00: beq   $a0, $zero, +10 -> 0x2C (taken_block)
      0x24E20004,  // 0x04: addiu $v0, $a3, 4 (stolen from 0x28)
      0x10A00005,  // 0x08: beq   $a1, $zero, +5 -> 0x20 (then_ret)
      0x00000000,  // 0x0C: nop
      0x14C00005,  // 0x10: bne   $a2, $zero, +5 -> 0x28 (pred_blk)
      0x00000000,  // 0x14: nop
      0x08000008,  // 0x18: j     0x80000020 (then_ret)
      0x00000000,  // 0x1C: nop
      0x03E00008,  // 0x20 (then_ret): jr $ra
      0x24020001,  // 0x24: addiu $v0, $zero, 1
      0x24E20004,  // 0x28 (pred_blk): addiu $v0, $a3, 4 (falls through to 0x2C)
      0x8C420000,  // 0x2C (taken_block): lw $v0, 0($v0)
      0x03E00008,  // 0x30: jr    $ra
      0x00000000,  // 0x34: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  RelaxDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b0 = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* pred_blk = cfg.FindBlockByVram(0x80000028);
  ASSERT_NE(b0, nullptr);
  ASSERT_NE(pred_blk, nullptr);
  EXPECT_TRUE(b0->instructions.back().IsNop());
  EXPECT_NE(std::find(b0->successors.begin(), b0->successors.end(), pred_blk->id),
            b0->successors.end());
}

TEST(CfgDeReorgTest,
     EagerThreadDelaySlotUnschedulerSinksCommonDelaySlotIntoMultiPredecessorJoinBlock) {
  // Models a join block `0x20` (`bne $v0, $zero, 0x2C`) where every incoming predecessor ends by
  // executing `andi $v0, $s1, 0xFF`:
  // - Block 0 (0x00): `bne $a0, $zero, 0x20` with delay slot `andi $v0, $s1, 0xFF` (dead on
  //   fallthrough 0x08 because 0x08 overwrites `$v0`).
  // - Block 1 (0x08): `addiu $v0, $a1, -1; bne $v0, $zero, 0x20` with delay slot
  //   `andi $v0, $s1, 0xFF` (dead on fallthrough 0x14).
  // - Block 2 (0x14): fallthrough body `sw $zero, 0($a2); addiu $s1, $zero, 1; andi $v0, $s1, 0xFF`
  //   falling through into `0x20`.
  std::vector<uint32_t> words = {
      0x14800007,  // 0x00: bne   $a0, $zero, +7 -> 0x20
      0x322200FF,  // 0x04: andi  $v0, $s1, 0xFF
      0x24A2FFFF,  // 0x08: addiu $v0, $a1, -1
      0x14400004,  // 0x0C: bne   $v0, $zero, +4 -> 0x20
      0x322200FF,  // 0x10: andi  $v0, $s1, 0xFF
      0xAC800000,  // 0x14: sw    $zero, 0($a0)
      0x24110001,  // 0x18: addiu $s1, $zero, 1
      0x322200FF,  // 0x1C: andi  $v0, $s1, 0xFF (falls through to 0x20)
      0x14400002,  // 0x20: bne   $v0, $zero, +2 -> 0x2C
      0x00000000,  // 0x24: nop
      0xAC820004,  // 0x28: sw    $v0, 4($a0)
      0x03E00008,  // 0x2C: jr    $ra
      0x00000000,  // 0x30: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  EagerThreadDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b0 = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* b1 = cfg.FindBlockByVram(0x80000008);
  const BasicBlock* b2 = cfg.FindBlockByVram(0x80000014);
  const BasicBlock* join_blk = cfg.FindBlockByVram(0x80000020);
  ASSERT_NE(b0, nullptr);
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(b2, nullptr);
  ASSERT_NE(join_blk, nullptr);
  EXPECT_TRUE(b0->instructions.back().IsNop());
  EXPECT_TRUE(b1->instructions.back().IsNop());
  EXPECT_TRUE(b2->instructions.back().IsNop());
  ASSERT_FALSE(join_blk->instructions.empty());
  EXPECT_EQ(join_blk->instructions.front().opcode, Opcode::kAndi);
  EXPECT_EQ(join_blk->instructions.front().rt, Register::kV0);
  EXPECT_EQ(join_blk->instructions.front().rs, Register::kS1);
}

TEST(CfgDeReorgTest, RelaxDelaySlotUnschedulerRetargetsChainedFallthroughBranchToSamePredecessor) {
  // Models a 2-branch decision-tree arm where:
  // - Block B1 (0x00): `beq $a0, $a1, 0x28` stole `addu $v0, $zero, $zero` from `pred_blk` (0x24).
  //   `$v0` is live into fallthrough B2 (0x08) only because B2 also branches to `taken_block`
  //   (0x28) where `$v0` is returned.
  // - Block B2 (0x08): `beq $a0, $a2, 0x28 / nop` branches to `0x28` without reading/writing `$v0`,
  //   and falls through to B3 (0x10) where `$v0` is dead.
  std::vector<uint32_t> words = {
      0x10850009,  // 0x00 (B1): beq   $a0, $a1, +9 -> 0x28 (taken_block)
      0x00001021,  // 0x04:      addu  $v0, $zero, $zero (stolen from 0x24)
      0x10860007,  // 0x08 (B2): beq   $a0, $a2, +7 -> 0x28 (taken_block)
      0x00000000,  // 0x0C:      nop
      0x10870004,  // 0x10 (B3): beq   $a0, $a3, +4 -> 0x24 (pred_blk)
      0x00000000,  // 0x14:      nop
      0x24020001,  // 0x18 (B4): addiu $v0, $zero, 1
      0x0800000A,  // 0x1C:      j 0x80000028 (taken_block)
      0x00000000,  // 0x20:      nop
      0x00001021,  // 0x24 (pred_blk): addu $v0, $zero, $zero (falls through to 0x28)
      0x8FBF0014,  // 0x28 (taken_block): lw $ra, 20($sp)
      0x03E00008,  // 0x2C:      jr $ra
      0x27BD0018,  // 0x30:      addiu $sp, $sp, 24
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  RelaxDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b1 = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* b2 = cfg.FindBlockByVram(0x80000008);
  const BasicBlock* pred_blk = cfg.FindBlockByVram(0x80000024);
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(b2, nullptr);
  ASSERT_NE(pred_blk, nullptr);
  EXPECT_TRUE(b1->instructions.back().IsNop());
  EXPECT_NE(std::find(b1->successors.begin(), b1->successors.end(), pred_blk->id),
            b1->successors.end());
  EXPECT_NE(std::find(b2->successors.begin(), b2->successors.end(), pred_blk->id),
            b2->successors.end());
}

TEST(CfgDeReorgTest, RelaxDelaySlotUnschedulerUnmergesHammockDelaySlotIntoPostDominator) {
  // Models GCC 2.7.2 `reorg.c:try_merge_delay_insns` on a nested `if (a && b)` before a loop latch:
  // - Block 0 (0x00): `beq $v0, $zero, 0x18` with delay slot `addiu $a2, $a2, 1` (stolen from 0x18
  //   and deleted from 0x18 by `try_merge_delay_insns` because `$a2` is not modified inside the
  //   hammock).
  // - Block 1 (0x08): `bne $a1, $zero, 0x18 / nop` (inner branch to 0x18 whose delay slot was left
  //   unfilled because 0x18 originally started with `addiu $a2, $a2, 1`).
  // - Block 2 (0x10): `sw $zero, 0($a0)` falling through to 0x18.
  // - Block 3 (0x18): `slti $v0, $a2, 16; jr $ra / nop`.
  std::vector<uint32_t> words = {
      0x10400005,  // 0x00: beq   $v0, $zero, +5 -> 0x18
      0x24C60001,  // 0x04: addiu $a2, $a2, 1
      0x14A00003,  // 0x08: bne   $a1, $zero, +3 -> 0x18
      0x00000000,  // 0x0C: nop
      0xAC800000,  // 0x10: sw    $zero, 0($a0)
      0x00000000,  // 0x14: nop (falls through to 0x18)
      0x28C20010,  // 0x18: slti  $v0, $a2, 16
      0x03E00008,  // 0x1C: jr    $ra
      0x00000000,  // 0x20: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  RelaxDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b0 = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* join_blk = cfg.FindBlockByVram(0x80000018);
  ASSERT_NE(b0, nullptr);
  ASSERT_NE(join_blk, nullptr);
  EXPECT_TRUE(b0->instructions.back().IsNop());
  ASSERT_FALSE(join_blk->instructions.empty());
  EXPECT_EQ(join_blk->instructions.front().opcode, Opcode::kAddiu);
  EXPECT_EQ(join_blk->instructions.front().rt, Register::kA2);
  EXPECT_EQ(join_blk->instructions.front().rs, Register::kA2);
  EXPECT_EQ(join_blk->instructions.front().immediate, 1);
}

TEST(CfgDeReorgTest, RelaxDelaySlotUnschedulerRetargetsFallthroughJumpTrampolineToPredecessor) {
  // Models GCC 2.7.2 `reorg.c:fill_slots_from_thread` stealing `addu $v0, $s0, $zero` from
  // `pred_blk` (0x20) across a 1-predecessor fallthrough `j 0x24 / nop` trampoline (0x08) into the
  // delay slot of `beq $a1, $v0, 0x10` (0x00), where `$v0` is dead on the taken arm (0x10).
  std::vector<uint32_t> words = {
      0x24060001,  // 0x00: addiu $a2, $zero, 1
      0x10A60003,  // 0x04: beq   $a1, $a2, +3 -> 0x14 (case_blk)
      0x02002021,  // 0x08: addu  $a0, $s0, $zero (stolen from 0x24 via fallthrough 0x0C)
      0x0800000A,  // 0x0C: j     0x80000028 (epilogue)
      0x00000000,  // 0x10: nop
      0x24040001,  // 0x14 (case_blk): addiu $a0, $zero, 1 (kills $a0)
      0x10E00002,  // 0x18: beq   $a3, $zero, +2 -> 0x24 (pred_blk)
      0x00000000,  // 0x1C: nop
      0x26100001,  // 0x20: addiu $s0, $s0, 1 (falls through to 0x24)
      0x02002021,  // 0x24 (pred_blk): addu $a0, $s0, $zero (falls through to 0x28)
      0x00801021,  // 0x28 (epilogue): addu $v0, $a0, $zero
      0x03E00008,  // 0x2C: jr    $ra
      0x00000000,  // 0x30: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  RelaxDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b0 = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* ft_jump = cfg.FindBlockByVram(0x8000000C);
  const BasicBlock* pred_blk = cfg.FindBlockByVram(0x80000024);
  ASSERT_NE(b0, nullptr);
  ASSERT_NE(ft_jump, nullptr);
  ASSERT_NE(pred_blk, nullptr);
  EXPECT_TRUE(b0->instructions.back().IsNop());
  ASSERT_EQ(ft_jump->successors.size(), 1u);
  EXPECT_EQ(ft_jump->successors[0], pred_blk->id);
}

TEST(CfgDeReorgTest, RelaxDelaySlotUnschedulerAllowsRetargetingToJumpTableDefaultExitBlock) {
  // Models a switch case block (0x00) where `beq $a1, $zero, 0x18` stole `lw $v0, 20($sp)` from the
  // switch exit block `0x14` (`jt.default_target_vram`). Because `0x14` is the switch exit (not a
  // distinct case body), `RelaxDelaySlotUnschedulerPass` must retarget the branch to `0x14`.
  std::vector<uint32_t> words = {
      0x10A00005,  // 0x00: beq   $a1, $zero, +5 -> 0x18 (taken_block)
      0x8FA20014,  // 0x04: lw    $v0, 20($sp) (stolen from 0x14)
      0xAC800000,  // 0x08: sw    $zero, 0($a0)
      0x00000000,  // 0x0C: nop
      0x24030001,  // 0x10: addiu $v1, $zero, 1 (falls through to 0x14)
      0x8FA20014,  // 0x14 (pred_blk / default_target_vram): lw $v0, 20($sp)
      0x03E00008,  // 0x18 (taken_block): jr $ra
      0x27BD0018,  // 0x1C: addiu $sp, $sp, 24
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  JumpTable jt;
  jt.table_vram = 0x80010000;
  jt.default_target_vram = 0x80000014;
  jt.entries.push_back(JumpTableEntry{.case_value = 0, .target_vram = 0x80000000});
  jt.entries.push_back(JumpTableEntry{.case_value = 1, .target_vram = 0x80000014});
  auto cfg_or = ControlFlowGraph::Build(insts, {jt});
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  RelaxDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b0 = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* pred_blk = cfg.FindBlockByVram(0x80000014);
  ASSERT_NE(b0, nullptr);
  ASSERT_NE(pred_blk, nullptr);
  EXPECT_TRUE(b0->instructions.back().IsNop());
  EXPECT_NE(std::find(b0->successors.begin(), b0->successors.end(), pred_blk->id),
            b0->successors.end());
}

TEST(CfgDeReorgTest, EagerThreadDelaySlotUnschedulerSinksFallthroughStolenCalleeSavedMove) {
  // Models a branch `bnez $a0, 0x14` whose delay slot `addu $s4, $t0, $zero` is dead on the taken
  // path (0x14 restores `$s4` in the epilogue) and live on the fallthrough path (0x08).
  std::vector<uint32_t> words = {
      0x14800004,  // 0x00: bne   $a0, $zero, +4 -> 0x14 (epilogue)
      0x0100A021,  // 0x04: addu  $s4, $t0, $zero (dead on taken, live on fallthrough)
      0xAE940000,  // 0x08: sw    $s4, 0($s4)
      0x08000005,  // 0x0C: j     0x80000014
      0x00000000,  // 0x10: nop
      0x8FB40010,  // 0x14: lw    $s4, 16($sp) (kills $s4 on taken path)
      0x03E00008,  // 0x18: jr    $ra
      0x27BD0018,  // 0x1C: addiu $sp, $sp, 24
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  EagerThreadDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* b0 = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* ft_blk = cfg.FindBlockByVram(0x80000008);
  ASSERT_NE(b0, nullptr);
  ASSERT_NE(ft_blk, nullptr);
  EXPECT_TRUE(b0->instructions.back().IsNop());
  ASSERT_FALSE(ft_blk->instructions.empty());
  EXPECT_EQ(ft_blk->instructions.front().opcode, Opcode::kAddu);
  EXPECT_EQ(ft_blk->instructions.front().rd, Register::kS4);
  EXPECT_EQ(ft_blk->instructions.front().rs, Register::kT0);
}

TEST(CfgDeReorgTest, CrossJumpTailDuplicatorInlinesConditionalBranchTailIntoJumpPredecessor) {
  // Models a cross-jumped conditional branch tail (`0x18: bnez $a1, 0x24; nop`)
  // reached via fallthrough from `0x10` (which can also branch directly to `0x24`) and via
  // `j 0x18` from `0x08` (`func_80065AA0` pattern).
  // `CrossJumpTailDuplicatorPass` should inline the conditional branch into `0x08`.
  std::vector<uint32_t> words = {
      0x10800003,  // 0x00: beqz  $a0, +3 -> 0x10
      0x00000000,  // 0x04: nop
      0x08000006,  // 0x08: j     0x80000018
      0x24840001,  // 0x0C: addiu $a0, $a0, 1
      0x10C00004,  // 0x10: beqz  $a2, +4 -> 0x24 (falls through to 0x18)
      0x24840002,  // 0x14: addiu $a0, $a0, 2
      0x14A00002,  // 0x18 (tail): bne $a1, $zero, +2 -> 0x24
      0x00000000,  // 0x1C: nop
      0x24020002,  // 0x20: addiu $v0, $zero, 2
      0x03E00008,  // 0x24: jr    $ra
      0x00000000,  // 0x28: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  CrossJumpTailDuplicatorPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* jmp_pred = cfg.FindBlockByVram(0x80000008);
  ASSERT_NE(jmp_pred, nullptr);
  EXPECT_EQ(jmp_pred->successors.size(), 2u);
  ASSERT_GE(jmp_pred->instructions.size(), 3u);
  EXPECT_EQ(jmp_pred->instructions[jmp_pred->instructions.size() - 2].opcode, Opcode::kBeq);
}

TEST(CfgDeReorgTest, EagerThreadDelaySlotUnschedulerPreservesPreLoopConstCalleeSavedInit) {
  // Models a loop guard `blez $v1, 0x20` whose delay slot initializes a loop induction counter
  // `addu $s0, $zero, $zero` before falling through into a single-successor loop preheader `0x08`
  // (`func_800F8840` pattern). The constant zero init must remain before the guard so GCC fills the
  // `blez` delay slot with `move $s0, $zero`.
  std::vector<uint32_t> words = {
      0x18600007,  // 0x00: blez  $v1, +7 -> 0x20 (post-loop epilogue)
      0x00008021,  // 0x04: addu  $s0, $zero, $zero (constant zero init of loop counter $s0)
      0x00408821,  // 0x08: addu  $s1, $v0, $zero (loop preheader falls through to 0x0C)
      0x26100001,  // 0x0C: addiu $s0, $s0, 1 (loop header/latch)
      0x0203102A,  // 0x10: slt   $v0, $s0, $v1
      0x1440FFFD,  // 0x14: bne   $v0, $zero, -3 -> 0x0C
      0x2631001C,  // 0x18: addiu $s1, $s1, 28
      0x00000000,  // 0x1C: nop
      0x8FB00010,  // 0x20: lw    $s0, 16($sp) (epilogue restores $s0)
      0x8FB10014,  // 0x24: lw    $s1, 20($sp)
      0x03E00008,  // 0x28: jr    $ra
      0x27BD0018,  // 0x2C: addiu $sp, $sp, 24
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgDeReorg::UnscheduleDelaySlots(&cfg);

  const BasicBlock* guard_blk = cfg.FindBlockByVram(0x80000000);
  const BasicBlock* preheader_blk = cfg.FindBlockByVram(0x80000008);
  ASSERT_NE(guard_blk, nullptr);
  ASSERT_NE(preheader_blk, nullptr);
  ASSERT_GE(guard_blk->instructions.size(), 2u);
  EXPECT_EQ(guard_blk->instructions.back().opcode, Opcode::kAddu);
  EXPECT_EQ(guard_blk->instructions.back().rd, Register::kS0);
  EXPECT_EQ(guard_blk->instructions.back().rs, Register::kZero);
  ASSERT_FALSE(preheader_blk->instructions.empty());
  EXPECT_EQ(preheader_blk->instructions.front().rd, Register::kS1);
}

TEST(CfgDeReorgTest, SimpleDelaySlotUnschedulerUnschedulesPrologueSaveInEntryBranchDelaySlot) {
  // Models GCC 2.7.2 `reorg.c:fill_simple_delay_slots` pulling `sw $ra, 16($sp)` into the delay
  // slot of an entry-block `bc1t` branch (`ShortCircuitIfElseWithStructScaleAndFloatCompare`
  // pattern). `SimpleDelaySlotUnschedulerPass` must unschedule `sw $ra, 16($sp)` before `bc1t`
  // and replace the delay slot with a `nop`.
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 0x00: addiu  $sp, $sp, -24
      0x44860000,  // 0x04: mtc1   $a2, $f0
      0x44871000,  // 0x08: mtc1   $a3, $f2
      0x4602003C,  // 0x0C: c.lt.s $f0, $f2
      0x00000000,  // 0x10: nop
      0x45010002,  // 0x14: bc1t   +2 -> 0x20
      0xAFBF0010,  // 0x18: sw     $ra, 16($sp) (prologue save in entry branch delay slot)
      0x00001021,  // 0x1C: addu   $v0, $zero, $zero
      0x8FBF0010,  // 0x20: lw     $ra, 16($sp)
      0x03E00008,  // 0x24: jr     $ra
      0x27BD0018,  // 0x28: addiu  $sp, $sp, 24
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  SimpleDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, nullptr));

  const BasicBlock* entry_blk = cfg.GetBlock(cfg.EntryBlockId());
  ASSERT_NE(entry_blk, nullptr);
  ASSERT_GE(entry_blk->instructions.size(), 3u);
  EXPECT_EQ(entry_blk->instructions[entry_blk->instructions.size() - 3].opcode, Opcode::kSw);
  EXPECT_EQ(entry_blk->instructions[entry_blk->instructions.size() - 3].rt, Register::kRa);
  EXPECT_EQ(entry_blk->instructions[entry_blk->instructions.size() - 2].opcode, Opcode::kBc1t);
  EXPECT_TRUE(entry_blk->instructions.back().IsNop());
}

TEST(CfgDeReorgTest, RelaxDelaySlotUnschedulerSplitsMultiInstFallthroughPredecessorToTakenBlock) {
  // Models GCC 2.7.2 `reorg.c` stealing the first instruction (`addiu $v0, $a1, 1`) of a target
  // block into a `bnezl` delay slot and advancing the branch target by +4 when the fallthrough
  // predecessor into the target block has multiple instructions (`addu $a1, $a0, $a2; addiu $v0,
  // $a1, 1`) and was therefore not split by `BasicBlockBuilder`.
  std::vector<uint32_t> words = {
      0x24820005,  // 0x00: addiu $v0, $a0, 5
      0x54400003,  // 0x04: bnezl $v0, +3 -> 0x14
      0x24A20001,  // 0x08: addiu $v0, $a1, 1 (stolen from target header; clobbers $v0)
      0x00862821,  // 0x0C: addu  $a1, $a0, $a2 (fallthrough block, instruction 0)
      0x24A20001,  // 0x10: addiu $v0, $a1, 1   (fallthrough block, instruction 1)
      0x03E00008,  // 0x14: jr    $ra           (taken block)
      0x00000000,  // 0x18: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  RelaxDelaySlotUnschedulerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, nullptr));

  const BasicBlock* entry_blk = cfg.GetBlock(cfg.EntryBlockId());
  const BasicBlock* taken_blk = cfg.GetBlock(2);
  ASSERT_NE(entry_blk, nullptr);
  ASSERT_NE(taken_blk, nullptr);
  EXPECT_EQ(entry_blk->instructions[entry_blk->instructions.size() - 2].opcode, Opcode::kBne);
  EXPECT_TRUE(entry_blk->instructions.back().IsNop());
  EXPECT_EQ(taken_blk->start_vram, 0x80000010u);
  EXPECT_EQ(taken_blk->instructions.front().opcode, Opcode::kAddiu);
  EXPECT_EQ(taken_blk->instructions.front().rt, Register::kV0);
}

TEST(CfgDeReorgTest,
     TargetHeaderDelaySlotReconcilerRetargetsUnfilledBranchWhenRedundantInsnPrecedesBranch) {
  // Models GCC 2.7.2 `reorg.c:fill_slots_from_thread` lines 3466-3490:
  // When `redundant_insn` finds that the first instruction of a branch/jump target (`0x18: addiu
  // $a2, $zero, 1`) is redundant with an instruction prior to the jump in the `then` block (`0x08:
  // addiu $a2, $zero, 1`), `reorg.c` advances the jump target from `0x18` (`pred_blk`) to `0x1C`
  // (`taken_blk`) and leaves the jump delay slot (`0x10`) as `nop`.
  // `TargetHeaderDelaySlotReconcilerPass` must retarget the `then` block's jump back to `0x18`.
  std::vector<uint32_t> words = {
      0x10800005,  // 0x00: beq   $a0, $zero, +5 -> 0x18 (pred_blk)
      0x00000000,  // 0x04: nop
      0x24060001,  // 0x08 (then_blk): addiu $a2, $zero, 1
      0x08000007,  // 0x0C: j     0x8000001C (taken_blk, skipped 0x18)
      0x00000000,  // 0x10: nop
      0x00000000,  // 0x14: nop
      0x24060001,  // 0x18 (pred_blk): addiu $a2, $zero, 1 (1-inst fallthrough pred of 0x1C)
      0xACA60004,  // 0x1C (taken_blk): sw    $a2, 4($a1)
      0x03E00008,  // 0x20: jr    $ra
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  ControlFlowGraph cfg = *cfg_or;

  CfgAnalysisManager analyses(&cfg);
  TargetHeaderDelaySlotReconcilerPass pass;
  EXPECT_TRUE(pass.Run(&cfg, &analyses));

  const BasicBlock* then_blk = cfg.FindBlockByVram(0x80000008);
  const BasicBlock* pred_blk = cfg.FindBlockByVram(0x80000018);
  ASSERT_NE(then_blk, nullptr);
  ASSERT_NE(pred_blk, nullptr);
  EXPECT_NE(std::find(then_blk->successors.begin(), then_blk->successors.end(), pred_blk->id),
            then_blk->successors.end());
}

}  // namespace
}  // namespace rom_nom_nom
