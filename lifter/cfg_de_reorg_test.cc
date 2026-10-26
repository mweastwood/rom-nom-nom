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

TEST(CfgDeReorgTest, DefaultPipelineContainsExpectedModularPasses) {
  CfgPassPipeline pipeline = CfgDeReorg::BuildDefaultPipeline();
  ASSERT_EQ(pipeline.PassCount(), 5u);
  EXPECT_EQ(pipeline.Passes()[0]->Name(), "SimpleDelaySlotUnschedulerPass");
  EXPECT_EQ(pipeline.Passes()[1]->Name(), "TargetHeaderDelaySlotReconcilerPass");
  EXPECT_EQ(pipeline.Passes()[2]->Name(), "RelaxDelaySlotUnschedulerPass");
  EXPECT_EQ(pipeline.Passes()[3]->Name(), "EagerThreadDelaySlotUnschedulerPass");
  EXPECT_EQ(pipeline.Passes()[4]->Name(), "CrossJumpTailDuplicatorPass");
}

}  // namespace
}  // namespace rom_nom_nom
