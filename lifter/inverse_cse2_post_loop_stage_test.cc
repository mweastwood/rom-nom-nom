#include "lifter/inverse_cse2_post_loop_stage.h"

#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

TEST(InverseCse2PostLoopStageTest, RestoresSinglePointerGivDeadAddressPlusInSelfLoop) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].successors = {3, 1};
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[2].successors = {2, 3};

  // In `.cse2`, `LoopArraySum` block B2 has 7 instructions (including the initial `i << 2` shift
  // restored by Stage 8). In `.loop`, `loop.c:strength_reduce` also left the single-def unused
  // address addition `(set (reg:SI 79) (plus:SI (reg:SI 78) (reg/v:SI 72)))`, which
  // `jump_optimize(..., after_regscan=1)` (`0x080f6817`) deleted before `.cse2`.
  cfg.rpo_blocks[2].AddPatternOnly("lw    $v0, 0($a0)");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a0, $a0, 4");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a2, $a2, 1");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $v1, $v1, $v0");
  cfg.rpo_blocks[2].AddPatternOnly("slt   $v0, $a2, $a1");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 8u);
  EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 8u);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.back().canonical_pattern,
            "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");
}

TEST(InverseCse2PostLoopStageTest, RestoresMultiplePointerGivDeadAddressPlusesSharingShift) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].successors = {3, 1};
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[2].successors = {2, 3};

  // `LoopCopyArray` has two distinct stride-4 pointer GIVs (`$a0` and `$a1`) sharing the same
  // `i << 2` shift, so `jump_optimize(..., after_regscan=1)` deletes 2 dead address `plus`
  // instructions between `.loop` and `.cse2`.
  cfg.rpo_blocks[2].AddPatternOnly("lw    $v0, 0($a1)");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a1, $a1, 4");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $v1, $v1, 1");
  cfg.rpo_blocks[2].AddPatternOnly("sw    $v0, 0($a0)");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a0, $a0, 4");
  cfg.rpo_blocks[2].AddPatternOnly("slt   $v0, $v1, $a2");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 10u);  // 8 + 2
}

TEST(InverseCse2PostLoopStageTest, RestoresMultiBlockLoopHeaderDeadAddressPlus) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(5);
  cfg.rpo_blocks[0].successors = {4, 1};
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[2].successors = {4, 3};
  cfg.rpo_blocks[3].successors = {2, 4};

  // Multi-block loop (`BranchLikelyLoop`): header B2 loads from pointer GIV `$a0` and branches on
  // early exit; latch B3 increments `$a0` by 4 and unit BIV `$a2` by 1 and jumps back to B2.
  cfg.rpo_blocks[2].AddPatternOnly("lw    $v0, 0($a0)");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  cfg.rpo_blocks[3].AddPatternOnly("addu  $v1, $v1, $v0");
  cfg.rpo_blocks[3].AddPatternOnly("addiu $a0, $a0, 4");
  cfg.rpo_blocks[3].AddPatternOnly("addiu $a2, $a2, 1");
  cfg.rpo_blocks[3].AddPatternOnly("slt   $v0, $a2, $a1");
  cfg.rpo_blocks[3].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 4u);  // 3 + 1
  EXPECT_EQ(cfg.rpo_blocks[3].rtl_insns.size(), 5u);
}

TEST(InverseCse2PostLoopStageTest, RestoresCompoundNonPowerOfTwoGivDeadInstructions) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[1].successors = {1, 2};

  // Stride-10 GIV loop (`DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf` Loop 2):
  // `jump_optimize(..., after_regscan=1)` deletes 3 single-def intermediate GIV instructions
  // (`plus R_shift R_biv`, `ashift R_giv5 1`, `plus R_giv10 R_base`).
  cfg.rpo_blocks[1].AddPatternOnly("lbu   $v0, 0($v1)");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $a1, $a1, 1");
  cfg.rpo_blocks[1].AddPatternOnly("andi  $v0, $v0, 0x7F");
  cfg.rpo_blocks[1].AddPatternOnly("sb    $v0, 0($v1)");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $v1, $v1, 10");
  cfg.rpo_blocks[1].AddPatternOnly("slti  $v0, $a1, 4");
  cfg.rpo_blocks[1].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[1].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[1].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 13u);  // 10 + 3
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 1u);   // 0 + 1 post-loop final BIV assignment
}

TEST(InverseCse2PostLoopStageTest, RestoresReversedDbraLoopPreheaderAndBodyDeadGivs) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[1].successors = {1, 2};

  // Preheader B0 of reversed `check_dbra_loop`:
  cfg.rpo_blocks[0].AddPatternOnly("addiu $v1, $zero, 4");
  cfg.rpo_blocks[0].AddPatternOnly("addiu $a1, $zero, 3");
  cfg.rpo_blocks[0].AddPatternOnly("addiu $v0, $v0, 30");

  // Loop body B1 with stride -10 (`addiu $v0, $v0, -10`) and stride -1 (`addiu $a1, $a1, -1`):
  cfg.rpo_blocks[1].AddPatternOnly("sb    $v1, 0($v0)");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $v0, $v0, -10");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $a1, $a1, -1");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (ge:SI (reg:SI 5 a1) (const_int 0)) (label_ref) (pc)))");

  InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 4u);  // 3 + 1
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 8u);  // 4 + 4
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns[3].rtl_sexpr_pattern, "(set (reg:SI 2 v0) (reg:SI 2 v0))");
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[3].rtl_sexpr_pattern,
            "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[4].rtl_sexpr_pattern,
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[5].rtl_sexpr_pattern,
            "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))");
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[6].rtl_sexpr_pattern,
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
}

TEST(InverseCse2PostLoopStageTest, LeavesLoopWithoutPointerGivUnchanged) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(2);
  cfg.rpo_blocks[0].successors = {0, 1};

  // Simple integer accumulation loop (`for (i = 0; i < n; i++) sum += i;`) has no strength-reduced
  // pointer GIV, so `.loop` and `.cse2` have the exact same instruction count.
  cfg.rpo_blocks[0].AddPatternOnly("addu  $v0, $v0, $a0");
  cfg.rpo_blocks[0].AddPatternOnly("addiu $a0, $a0, 1");
  cfg.rpo_blocks[0].AddPatternOnly("slt   $v1, $a0, $a1");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 3 v1) (const_int 0)) (label_ref) (pc)))");

  InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 4u);
}

}  // namespace
}  // namespace rom_nom_nom
