#include "lifter/inverse_loop_opt_stage.h"

#include <vector>

#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

TEST(InverseLoopOptStageTest, ContractsSingleBlockLoopGivPreheaderAndRemovesExtraInsn) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].successors = {3, 1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a1, $a1, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a2, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $v1, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 5 a1) (const_int 0)) (label_ref) (pc)))");

  // Synthesized preheader B1 from `loop.c:strength_reduce`:
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[1].AddPatternOnly("addu  $a0, $a0, $zero");

  // Loop body B2 (8 insns in `.loop` -> 7 insns in `.cse`):
  cfg.rpo_blocks[2].successors = {2, 3};
  cfg.rpo_blocks[2].AddPatternOnly("lw    $v0, 0($a0)");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a0, $a0, 4");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a2, $a2, 1");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $v1, $v1, $v0");
  cfg.rpo_blocks[2].AddPatternOnly("slt   $v0, $a2, $a1");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[3].successors = {};
  cfg.rpo_blocks[3].AddPatternOnly("addu  $v0, $v1, $zero");

  InverseLoopOptLifter::InvertLoopToCseRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 3u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[1].successors, (std::vector<size_t>{1, 2}));
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 7u);
  EXPECT_EQ(cfg.rpo_blocks[2].successors, std::vector<size_t>{});
}

TEST(InverseLoopOptStageTest, UnHoistsMovableConstantsFromPreheaderAndContractsWhenEmpty) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(6);
  cfg.rpo_blocks[0].successors = {5, 1};
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 5 a1) (const_int 0)) (label_ref) (pc)))");

  // Preheader B1 created by `move_movables` (`0x08114795`) with hoisted constant `5`:
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[1].AddPatternOnly("addiu $t0, $zero, 5");

  cfg.rpo_blocks[2].successors = {4, 3};
  cfg.rpo_blocks[2].AddPatternOnly("lhu   $v1, 0($a0)");
  cfg.rpo_blocks[2].AddPatternOnly("andi  $v0, $v1, 0x7");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (reg:SI 8 t0)) (label_ref) (pc)))");

  cfg.rpo_blocks[3].successors = {4};
  cfg.rpo_blocks[3].AddPatternOnly("ori   $v0, $v1, 0x8");
  cfg.rpo_blocks[3].AddPatternOnly("sh    $v0, 0($a0)");
  cfg.rpo_blocks[3].AddPatternOnly("addiu $a3, $a3, 1");

  cfg.rpo_blocks[4].successors = {2, 5};
  cfg.rpo_blocks[4].AddPatternOnly("addiu $a0, $a0, 2");
  cfg.rpo_blocks[4].AddPatternOnly("addiu $a2, $a2, 1");
  cfg.rpo_blocks[4].AddPatternOnly("slt   $v0, $a2, $a1");
  cfg.rpo_blocks[4].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[5].successors = {};
  cfg.rpo_blocks[5].AddPatternOnly("addu  $v0, $a3, $zero");

  InverseLoopOptLifter::InvertLoopToCseRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 5u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{4, 1}));
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 5u);
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.front(), "addiu $t0, $zero, 5");
}

TEST(InverseLoopOptStageTest, UnHoistsMovablesFromEntryBlockB0IntoSwitchAndTablejumpBlocks) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(12);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a1, $a1, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a2, $a2, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a3, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("sltiu $t0, $a1, 6");
  cfg.rpo_blocks[0].AddPatternOnly("addiu $v0, $v0, 0");
  cfg.rpo_blocks[0].AddPatternOnly("sll   $a1, $a1, 2");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a1, $a1, $v0");

  cfg.rpo_blocks[1].successors = {9, 2};
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 8 t0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[2].successors = {8, 7, 6, 5, 4, 3};
  cfg.rpo_blocks[2].AddPatternOnly("lw    $v0, 0($a1)");
  cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (set (pc) (reg:SI 2 v0)) (use (label_ref)) ] )");

  InverseLoopOptLifter::InvertLoopToCseRtlCfg(&cfg);

  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 5u);
}

TEST(InverseLoopOptStageTest, ReversesFindAndVerifyLoopsExitBlockRelocation) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(10);
  cfg.rpo_blocks[4].successors = {5};
  cfg.rpo_blocks[4].AddPatternOnly("addu  $v1, $zero, $zero");
  cfg.rpo_blocks[4].AddPatternOnly("addiu $a1, $a1, 0");
  cfg.rpo_blocks[4].AddPatternOnly("addu  $zero, $zero, $zero");

  cfg.rpo_blocks[5].successors = {8, 6};
  cfg.rpo_blocks[5].AddPatternOnly("lw    $v0, 0($a1)");
  cfg.rpo_blocks[5].AddPatternOnly("nor   $v0, $zero, $v0");
  cfg.rpo_blocks[5].AddPatternOnly("and   $v0, $a0, $v0");
  cfg.rpo_blocks[5].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[5].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[5].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[6].successors = {5, 7};
  cfg.rpo_blocks[6].AddPatternOnly("addiu $a1, $a1, 4");
  cfg.rpo_blocks[6].AddPatternOnly("addiu $v1, $v1, 1");
  cfg.rpo_blocks[6].AddPatternOnly("slti  $v0, $v1, 32");
  cfg.rpo_blocks[6].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[7].successors = {9};
  cfg.rpo_blocks[7].AddPatternOnly("addiu $v0, $zero, -1");

  cfg.rpo_blocks[8].successors = {9};
  cfg.rpo_blocks[8].AddPatternOnly("addu  $v0, $v1, $zero");
  cfg.rpo_blocks[8].AddPatternOnly("(set (pc) (label_ref))");

  InverseLoopOptLifter::InvertLoopToCseRtlCfg(&cfg);

  EXPECT_EQ(cfg.rpo_blocks[4].instructions.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[5].successors, (std::vector<size_t>{7, 6}));
  EXPECT_EQ(cfg.rpo_blocks[5].instructions.size(), 7u);
  EXPECT_EQ(cfg.rpo_blocks[6].successors, std::vector<size_t>{9});
  EXPECT_EQ(cfg.rpo_blocks[6].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[7].successors, (std::vector<size_t>{5, 8}));
  EXPECT_EQ(cfg.rpo_blocks[7].instructions.size(), 3u);
  EXPECT_EQ(cfg.rpo_blocks[8].successors, std::vector<size_t>{9});
  EXPECT_EQ(cfg.rpo_blocks[8].instructions.size(), 1u);
}

TEST(InverseLoopOptStageTest, LeavesNonLoopCfgUntouched) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].AddPatternOnly("addu  $v0, $a0, $a1");

  InverseLoopOptLifter::InvertLoopToCseRtlCfg(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 1u);
}

}  // namespace
}  // namespace rom_nom_nom
