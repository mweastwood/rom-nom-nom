#include "lifter/inverse_flow_life_analysis_stage.h"

#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

TEST(InverseFlowLifeAnalysisStageTest, RestoresSingleBlockStoreFlagSpeculativeReturnDefault) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  // In `.flow`, `BranchCompareLess(s32 a, s32 b)` has 4 instructions (1 `slt` + 3 un-combined
  // entry/exit copies from Stage 7). In `.cse2`, `jump.c` + `cse.c` leave a 5th dead speculative
  // `(set (reg/i:SI 2 v0) (const_int 1))` default that `flow.c:life_analysis` deletes.
  cfg.rpo_blocks[0].AddPatternOnly("slt   $v0, $a0, $a1");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $zero, $zero, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $zero, $zero, $zero");

  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(&cfg);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 5u);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 5u);
}

TEST(InverseFlowLifeAnalysisStageTest, DoesNotAddStoreFlagDefaultToRotateLeftByOne) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  // `ArithmeticRotateLeftOne` ends with `srl $v0, $a0, 31` (combined with `sll` and `or`), which
  // is a bitwise rotate rather than a sign-bit boolean store-flag sequence.
  cfg.rpo_blocks[0].AddPatternOnly("sll   $v1, $a0, 1");
  cfg.rpo_blocks[0].AddPatternOnly("srl   $v0, $a0, 31");
  cfg.rpo_blocks[0].AddPatternOnly("or    $v0, $v1, $v0");

  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(&cfg);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 3u);
}

TEST(InverseFlowLifeAnalysisStageTest, RestoresMultiBlockSignumSpeculativeMinusOneDefault) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[1].successors = {3};
  cfg.rpo_blocks[2].successors = {3};

  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $v0, $zero, 1");
  cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref))");
  cfg.rpo_blocks[2].AddPatternOnly("sra   $v0, $a0, 31");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");

  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 3u);
  EXPECT_EQ(cfg.rpo_blocks[3].rtl_insns.size(), 0u);
}

TEST(InverseFlowLifeAnalysisStageTest, RestoresStrengthReducedLoopDeadBivShift) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].successors = {3, 1};
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[2].successors = {2, 3};

  cfg.rpo_blocks[1].AddPatternOnly("addu  $a0, $a0, $zero");

  // Single-block loop body B2 with pointer GIV `$a0` (stride 4) and unit BIV `$a2` (stride 1).
  // `loop.c:strength_reduce` leaves the initial `(set (reg:SI 76) (ashift:SI (reg/v:SI 74) 2))`
  // in `.cse2`, which `flow.c:life_analysis` deletes as dead.
  cfg.rpo_blocks[2].AddPatternOnly("lw    $v0, 0($a0)");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a0, $a0, 4");
  cfg.rpo_blocks[2].AddPatternOnly("addiu $a2, $a2, 1");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $v1, $v1, $v0");
  cfg.rpo_blocks[2].AddPatternOnly("slt   $v0, $a2, $a1");
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");

  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 7u);
  // Ensure the restored instruction is placed before the terminating conditional branch.
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.back().canonical_pattern,
            "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");
}

TEST(InverseFlowLifeAnalysisStageTest, RestoresReversedDbraLoopDeadGivAndFinalBivValue) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[1].successors = {1, 2};

  // Preheader B0: initializes reversed countdown BIV `$a1 = 3` and GIV `$v0 = &sym + 30`.
  cfg.rpo_blocks[0].AddPatternOnly("addiu $v1, $zero, 4");
  cfg.rpo_blocks[0].AddPatternOnly("addiu $a1, $zero, 3");
  RtlInsn sym_addiu;
  sym_addiu.canonical_pattern = "addiu $v0, $v0, 30";
  sym_addiu.idiom_kind = RtlFusedIdiomKind::kGasAddressHiLo;
  cfg.rpo_blocks[0].AddInsn(sym_addiu);

  // Reversed `check_dbra_loop` body B1: `addiu $v0, $v0, -10; addiu $a1, $a1, -1; bgez $a1`.
  cfg.rpo_blocks[1].AddPatternOnly("sb    $v1, 0($v0)");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $v0, $v0, -10");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $a1, $a1, -1");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (ge:SI (reg:SI 5 a1) (const_int 0)) (label_ref) (pc)))");

  // Loop exit fallthrough B2: `check_dbra_loop` emits dead final BIV assignment `(set reg 4)`.
  cfg.rpo_blocks[2].AddPatternOnly("addu  $v0, $zero, $zero");

  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 7u);  // 3 + 4
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 7u);  // 4 + 3
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 2u);  // 1 + 1
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[3].rtl_sexpr_pattern,
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -10)))");
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[4].rtl_sexpr_pattern,
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -5)))");
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns[5].rtl_sexpr_pattern,
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -4)))");
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns[1].rtl_sexpr_pattern, "(set (reg:SI 2 v0) (const_int 4))");
}

TEST(InverseFlowLifeAnalysisStageTest, SkipsDeadCodeRestorationWhenOptimizerDisabledAtO0) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].AddPatternOnly("slt   $v0, $a0, $a1");

  const ParsedCompilerFlags o0_flags = ParsedCompilerFlags::Parse("-O0");
  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(&cfg, &o0_flags);
  EXPECT_EQ(cfg.pass, RtlPassKind::kCse2);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 1u);
}

TEST(InverseFlowLifeAnalysisStageTest, RestoresCseForwardedZeroExtensionCopiesInEarlyReturnBlock) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[1].successors = {0};

  cfg.rpo_blocks[0].AddPatternOnly("lbu   $v1, 0($a0)");
  cfg.rpo_blocks[0].AddPatternOnly("addiu $a0, $a0, 1");
  cfg.rpo_blocks[0].AddPatternOnly("lbu   $v0, 0($a1)");
  cfg.rpo_blocks[0].AddPatternOnly("addiu $a1, $a1, 1");
  cfg.rpo_blocks[0].AddPatternOnly("andi  $v1, $v1, 0xFF");
  cfg.rpo_blocks[0].AddPatternOnly("andi  $v0, $v0, 0xFF");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 3 v1) (reg:SI 2 v0)) (label_ref) (pc)))");

  cfg.rpo_blocks[2].AddPatternOnly("subu  $v0, $v1, $v0");
  cfg.rpo_blocks[2].AddPatternOnly("addu  $zero, $zero, $zero");

  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[2].rtl_insns.size(), 4u);  // 2 + 2 dead CSE copies
}

}  // namespace
}  // namespace rom_nom_nom
