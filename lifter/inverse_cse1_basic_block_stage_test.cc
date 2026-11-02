#include "lifter/inverse_cse1_basic_block_stage.h"

#include <vector>

#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

TEST(InverseCse1BasicBlockStageTest, UnthreadsJumpThroughSingleInsnIdenticalBranchBlock) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(5);
  cfg.rpo_blocks[0].successors = {3, 1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[1].AddPatternOnly("addiu $a0, $a0, -1");

  // Single-instruction identical branch block `mid = 2` with `mid.successors[0] == 3`:
  cfg.rpo_blocks[2].successors = {3, 4};
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[3].successors = {4};
  cfg.rpo_blocks[3].AddPatternOnly("addu  $v0, $zero, $zero");

  cfg.rpo_blocks[4].successors = {};
  cfg.rpo_blocks[4].AddPatternOnly("addu  $v0, $a0, $zero");

  InverseCse1BasicBlockLifter::InvertCseToJumpRtlCfg(&cfg);

  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
}

TEST(InverseCse1BasicBlockStageTest, RestoresConstantFoldedPreheaderGuardOnInternalLoop) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");

  // Internal 1-insn zero-init preheader `B1`:
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[1].AddPatternOnly("addu  $v1, $zero, $zero");

  cfg.rpo_blocks[2].successors = {2, 3};
  cfg.rpo_blocks[2].AddPatternOnly("addiu $v1, $v1, 1");
  cfg.rpo_blocks[2].AddPatternOnly("slti  $v0, $v1, 32");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[3].successors = {};
  cfg.rpo_blocks[3].AddPatternOnly("addiu $v0, $zero, -1");

  InverseCse1BasicBlockLifter::InvertCseToJumpRtlCfg(&cfg);

  EXPECT_EQ(cfg.rpo_blocks[1].successors, (std::vector<size_t>{3, 2}));
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.size(), 3u);
}

TEST(InverseCse1BasicBlockStageTest, RestoresConstantFoldedPreheaderGuardOnEntry3BlockForLoop) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $v1, $zero, $zero");

  cfg.rpo_blocks[1].successors = {1, 2};
  cfg.rpo_blocks[1].AddPatternOnly("addiu $v1, $v1, 1");
  cfg.rpo_blocks[1].AddPatternOnly("slti  $v0, $v1, 8");
  cfg.rpo_blocks[1].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[2].successors = {};
  cfg.rpo_blocks[2].AddPatternOnly("addu  $v0, $a0, $zero");

  InverseCse1BasicBlockLifter::InvertCseToJumpRtlCfg(&cfg);

  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u);
}

TEST(InverseCse1BasicBlockStageTest, RestoresExpandMultLeadingCopyBeforeConstantMultiplySll) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("sll   $v0, $a0, 2");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $v0, $v0, $a0");
  cfg.rpo_blocks[0].AddPatternOnly("sll   $v0, $v0, 1");

  InverseCse1BasicBlockLifter::InvertCseToJumpRtlCfg(&cfg);

  // First `sll` is a constant multiply (`* 10`) -> +1 leading copy; second `sll` is chained -> +0.
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 5u);
}

TEST(InverseCse1BasicBlockStageTest, RestoresStoreFlagFoldedXorAndDeadComparison) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("sltiu $v0, $a0, 1");

  InverseCse1BasicBlockLifter::InvertCseToJumpRtlCfg(&cfg);

  // `sltiu $v0, $a0, 1` had `(xor R 0)` folded in `.cse` -> +1 instruction in `.jump`.
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 3u);
}

}  // namespace
}  // namespace rom_nom_nom
