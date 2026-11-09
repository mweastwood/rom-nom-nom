#include "lifter/invert_relax_delay_slots.h"

#include <cstddef>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

// Subclasses `SubfunctionInverseProvider` directly (never `ToySubfunctionInverses`)
// so that `RightInverseVerifier` exercises only the production `lifter/`
// implementation of `InvertRelaxDelaySlots`.
class ProductionRelaxDelaySlotsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertRelaxDelaySlots(
      const RtlControlFlowGraph& dbr_relaxed_cfg_x) const override {
    return ::rom_nom_nom::lifter::InvertRelaxDelaySlots(dbr_relaxed_cfg_x);
  }
};

// Deliberately broken inverse provider for negative verification tests.
class WrongRelaxDelaySlotsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertRelaxDelaySlots(
      const RtlControlFlowGraph& dbr_relaxed_cfg_x) const override {
    RtlControlFlowGraph bad = dbr_relaxed_cfg_x;
    if (!bad.rpo_blocks.empty()) {
      bad.rpo_blocks[0].instructions = {"(set (reg/i:SI 2 v0) (const_int 999))", "(return)"};
      bad.rpo_blocks[0].successors.clear();
    }
    return bad;
  }
};

// Subcase I.2 (`reorg.c:3787-3818`): Bare conditional branch inverted around a
// bare unconditional jump.
TEST(InvertRelaxDelaySlotsTest, BareConditionalBranchInvertedAroundUnconditionalJumpRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.sideband.saved_gpr_regnos = {31};
  relaxed_cfg.rpo_blocks.resize(3);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))");
  relaxed_cfg.rpo_blocks[0].successors = {2, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 10))");
  relaxed_cfg.rpo_blocks[1].AddPatternOnly("(return)");
  relaxed_cfg.rpo_blocks[2].has_label = true;
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 20))");
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(return)");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[0].instructions[0], "(eq:SI"));
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(lifted.rpo_blocks[1].instructions,
            std::vector<std::string>{"(set (pc) (label_ref L1))"});
  EXPECT_EQ(lifted.rpo_blocks[1].successors, std::vector<size_t>{3});

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase I.2 (`reorg.c:3787-3818`): Bare conditional branch preceded by
// setup instructions in the same block inverted around a bare unconditional
// jump.
TEST(InvertRelaxDelaySlotsTest,
     BareConditionalBranchWithSetupInvertedAroundUnconditionalJumpRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.sideband.saved_gpr_regnos = {31};
  relaxed_cfg.rpo_blocks.resize(3);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))");
  relaxed_cfg.rpo_blocks[0].successors = {2, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 1))");
  relaxed_cfg.rpo_blocks[1].AddPatternOnly("(return)");
  relaxed_cfg.rpo_blocks[2].has_label = true;
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[0].instructions.back(), "(ne:SI"));
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(lifted.rpo_blocks[1].instructions,
            std::vector<std::string>{"(set (pc) (label_ref L1))"});
  EXPECT_EQ(lifted.rpo_blocks[1].successors, std::vector<size_t>{3});

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase I.3 (`reorg.c:3829-3851`): Bare conditional jump followed by a bare
// unconditional jump where `mostly_true_jump > 0` on the other target caused
// `relax_delay_slots` to invert the conditional jump and swap targets.
TEST(InvertRelaxDelaySlotsTest,
     BareMostlyTrueConditionalBranchSwappedWithUnconditionalJumpRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // In PRE (`lifted`), B1 (`L0`) has the backward conditional jump
  // `(ne:SI $2 0)` to B1 (`L0`, `mostly_true_jump > 0`), and B2 has the bare
  // forward jump to B4 (`L2`). `relax_delay_slots` (`reorg.c:3834`) inverts B1
  // to `(eq:SI $2 0)` targeting B4 (`L2`) and redirects B2 to B1 (`L0`),
  // producing `relaxed_cfg`.
  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.sideband.saved_gpr_regnos = {31};
  relaxed_cfg.rpo_blocks.resize(5);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L1) (pc)))) "
      "(insn (set (reg:SI 2 v0) (reg:SI 4 a0)))])");
  relaxed_cfg.rpo_blocks[0].successors = {3, 1};
  relaxed_cfg.rpo_blocks[1].has_label = true;
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -1)))");
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) "
      "(label_ref L2) (pc)))");
  relaxed_cfg.rpo_blocks[1].successors = {4, 2};
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(set (pc) (label_ref L0))");
  relaxed_cfg.rpo_blocks[2].successors = {1};
  relaxed_cfg.rpo_blocks[3].has_label = true;
  relaxed_cfg.rpo_blocks[3].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int -1))");
  relaxed_cfg.rpo_blocks[3].successors = {4};
  relaxed_cfg.rpo_blocks[4].has_label = true;
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(use (reg/i:SI 2 v0))");
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
  EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[1].instructions.back(), "(ne:SI"));
  EXPECT_EQ(lifted.rpo_blocks[1].successors, (std::vector<size_t>{1, 2}));
  EXPECT_EQ(lifted.rpo_blocks[2].successors, std::vector<size_t>{4});

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase I.1 (`reorg.c:3775`): Bare unconditional jump to `b + 1` after a
// `div`/`mod` parallel instruction restored.
TEST(InvertRelaxDelaySlotsTest, BareUnconditionalJumpAfterDivModParallelRestoredRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.sideband.saved_gpr_regnos = {31};
  relaxed_cfg.rpo_blocks.resize(3);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 5 a1) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn/s (set (reg:SI 2 v0) (const_int 0)))])");
  relaxed_cfg.rpo_blocks[0].successors = {2, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (div:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "(set (reg:SI 3 v1) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "(clobber (reg:SI 65 lo)) (clobber (reg:SI 64 hi)) "
      "(clobber (reg:SI 66 accum)) ] )");
  relaxed_cfg.rpo_blocks[1].successors = {2};
  relaxed_cfg.rpo_blocks[2].has_label = true;
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.back(), "(set (pc) (label_ref L0))");

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase II.6 (`reorg.c:3993-4034`): Non-annulled `SEQUENCE` conditional
// branch inverted around a bare unconditional jump to the shared return
// epilogue block, flipping `insn/s` back to `insn`.
TEST(InvertRelaxDelaySlotsTest,
     NonAnnulledSequenceConditionalBranchInvertedAroundUnconditionalJumpRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.sideband.saved_gpr_regnos = {31};
  relaxed_cfg.rpo_blocks.resize(3);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg/v:SI 4 a0) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn/s (set (reg/i:SI 2 v0) (const_int 1)))])");
  relaxed_cfg.rpo_blocks[0].successors = {2, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashiftrt:SI (reg/v:SI 4 a0) (const_int 31))) "
      "(expr_list:REG_DEAD (reg/v:SI 4 a0) (nil))");
  relaxed_cfg.rpo_blocks[1].successors = {2};
  relaxed_cfg.rpo_blocks[2].has_label = true;
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn (set (pc) (if_then_else (le:SI (reg/v:SI 4 a0) "
            "(const_int 0)) (label_ref L0) (pc)))) "
            "(insn (set (reg/i:SI 2 v0) (const_int 1)))])");
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(lifted.rpo_blocks[1].instructions,
            std::vector<std::string>{"(set (pc) (label_ref L1))"});
  EXPECT_EQ(lifted.rpo_blocks[1].successors, std::vector<size_t>{3});

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase II.6 (`reorg.c:3993-4034`): Non-annulled `SEQUENCE` conditional
// branch inverted around a bare `(return)` (removing the trailing empty
// `end_of_function_label` block and restoring the `(return)` trampoline).
TEST(InvertRelaxDelaySlotsTest,
     NonAnnulledSequenceConditionalBranchInvertedAroundReturnRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.sideband.saved_gpr_regnos = {31};
  relaxed_cfg.rpo_blocks.resize(3);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn/s (set (reg/i:SI 2 v0) (const_int 0)))])");
  relaxed_cfg.rpo_blocks[0].successors = {2, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  relaxed_cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");
  relaxed_cfg.rpo_blocks[1].successors = {2};
  relaxed_cfg.rpo_blocks[2].has_label = true;

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) "
            "(const_int 0)) (label_ref L0) (pc)))) "
            "(insn (set (reg/i:SI 2 v0) (const_int 0)))])");
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(lifted.rpo_blocks[1].instructions, std::vector<std::string>{"(return)"});
  EXPECT_TRUE(lifted.rpo_blocks[1].successors.empty());
  EXPECT_TRUE(lifted.rpo_blocks[2].has_label);

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase II.5 (`reorg.c:3939-3973`): Non-annulled `SEQUENCE` unconditional
// jump to the immediately following block `b + 1` unwrapped by
// `delete_scheduled_jump`.
TEST(InvertRelaxDelaySlotsTest,
     NonAnnulledSequenceUnconditionalJumpToNextBlockUnwrappedRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // Case A: Preceded by another unconditional `SEQUENCE` jump to `b + 1`.
  {
    RtlControlFlowGraph relaxed_cfg;
    relaxed_cfg.pass = RtlPassKind::kDbr;
    relaxed_cfg.rpo_blocks.resize(4);
    relaxed_cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
        "(const_int 0)) (label_ref L0) (pc)))) "
        "(insn (set (reg:SI 2 v0) (const_int 1)))])");
    relaxed_cfg.rpo_blocks[0].successors = {2, 1};
    relaxed_cfg.rpo_blocks[1].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (label_ref L1))) "
        "(insn (set (reg/i:SI 2 v0) (const_int 20)))])");
    relaxed_cfg.rpo_blocks[1].successors = {3};
    relaxed_cfg.rpo_blocks[2].has_label = true;
    relaxed_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 30))");
    relaxed_cfg.rpo_blocks[2].successors = {3};
    relaxed_cfg.rpo_blocks[3].has_label = true;
    relaxed_cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");
    relaxed_cfg.rpo_blocks[3].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
    EXPECT_EQ(lifted.rpo_blocks[2].instructions[0],
              "(sequence [(jump_insn (set (pc) (label_ref L1))) "
              "(insn (set (reg/i:SI 2 v0) (const_int 30)))])");

    const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Case B: Diamond fallthrough arm with `REG_DEAD (const_int 0)`.
  {
    RtlControlFlowGraph relaxed_cfg;
    relaxed_cfg.pass = RtlPassKind::kDbr;
    relaxed_cfg.rpo_blocks.resize(3);
    relaxed_cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 2 v0) "
        "(const_int 0)) (pc) (label_ref L0)))) "
        "(insn/s (set (reg/v:SI 2 v0) (minus:SI (reg/v:SI 4 a0) (reg/v:SI 5 a1))) "
        "(expr_list:REG_DEAD (const_int 0) (nil)))])");
    relaxed_cfg.rpo_blocks[0].successors = {2, 1};
    relaxed_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg/v:SI 2 v0) (plus:SI (reg/v:SI 4 a0) (reg/v:SI 5 a1))) "
        "(expr_list:REG_DEAD (const_int 0) (nil))");
    relaxed_cfg.rpo_blocks[1].successors = {2};
    relaxed_cfg.rpo_blocks[2].has_label = true;
    relaxed_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
    relaxed_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
    EXPECT_EQ(lifted.rpo_blocks[1].instructions[0],
              "(sequence [(jump_insn (set (pc) (label_ref L0))) "
              "(insn (set (reg/v:SI 2 v0) (plus:SI (reg/v:SI 4 a0) (reg/v:SI 5 a1))) "
              "(expr_list:REG_DEAD (const_int 0) (nil)))])");

    const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

// Subcase II.7 (`reorg.c:4038-4040` & `1866-1884`): `try_merge_delay_insns`
// merging a duplicate delay slot from a fallthrough `SEQUENCE` conditional jump.
TEST(InvertRelaxDelaySlotsTest, TryMergeDelayInsnsFallthroughSequenceBranchMergedRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.rpo_blocks.resize(4);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (lt:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn (set (reg:SI 2 v0) (const_int -1)))])");
  relaxed_cfg.rpo_blocks[0].successors = {3, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))");
  relaxed_cfg.rpo_blocks[1].successors = {3, 2};
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 4 a0))");
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  relaxed_cfg.rpo_blocks[3].has_label = true;
  relaxed_cfg.rpo_blocks[3].AddPatternOnly("(use (reg:SI 2 v0))");
  relaxed_cfg.rpo_blocks[3].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn/u (set (pc) (if_then_else (lt:SI (reg:SI 4 a0) "
            "(const_int 0)) (label_ref L0) (pc)))) "
            "(insn/s (set (reg:SI 2 v0) (const_int -1)))])");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0],
            "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
            "(const_int 0)) (label_ref L0) (pc)))) "
            "(insn/s (set (reg:SI 2 v0) (const_int -1)))])");

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase II.7 (`reorg.c:4041-4043` & `1866-1884`): `try_merge_delay_insns`
// merging a duplicate delay slot from the target block's `SEQUENCE` conditional
// jump when `own_thread_p` holds.
TEST(InvertRelaxDelaySlotsTest, TryMergeDelayInsnsTargetSequenceBranchMergedRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.rpo_blocks.resize(5);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn (set (reg/i:SI 2 v0) (const_int 1)))])");
  relaxed_cfg.rpo_blocks[0].successors = {2, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (label_ref L1))) "
      "(insn (set (reg:SI 3 v1) (const_int 10)))])");
  relaxed_cfg.rpo_blocks[1].successors = {4};
  relaxed_cfg.rpo_blocks[2].has_label = true;
  relaxed_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (lt:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L1) (pc)))");
  relaxed_cfg.rpo_blocks[2].successors = {4, 3};
  relaxed_cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 3 v1) (const_int 20))");
  relaxed_cfg.rpo_blocks[3].successors = {4};
  relaxed_cfg.rpo_blocks[4].has_label = true;
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(use (reg/i:SI 2 v0))");
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[0],
            "(sequence [(jump_insn (set (pc) (if_then_else (lt:SI (reg:SI 4 a0) "
            "(const_int 0)) (label_ref L1) (pc)))) "
            "(insn (set (reg/i:SI 2 v0) (const_int 1)))])");

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase II.7 (`reorg.c:4038-4040` & `1816-1853`): `try_merge_delay_insns`
// merging a normal instruction in the fallthrough block with an annulled
// `/u` + `/s` (or `/s`) delay slot in `b`.
TEST(InvertRelaxDelaySlotsTest, TryMergeDelayInsnsFallthroughInstructionMergedRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.sideband.saved_gpr_regnos = {31};
  relaxed_cfg.rpo_blocks.resize(3);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 2 v0) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn (set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 1))))])");
  relaxed_cfg.rpo_blocks[0].successors = {2, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 7 a3) (plus:SI (reg:SI 7 a3) (const_int 1)))");
  relaxed_cfg.rpo_blocks[1].AddPatternOnly("(note NOTE_INSN_LOOP_CONT)");
  relaxed_cfg.rpo_blocks[1].successors = {2};
  relaxed_cfg.rpo_blocks[2].has_label = true;
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(use (reg:SI 6 a2))");
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 2 v0) "
            "(const_int 0)) (label_ref L0) (pc)))) "
            "(insn/s (set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 1))))])");
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.back(),
            "(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 1)))");

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase II.2 (`reorg.c:3768-3782` & `3883-3898`, `follow_jumps`): Jump
// threading through a trampoline block.
TEST(InvertRelaxDelaySlotsTest, FollowJumpsThroughOrphanedLabeledTrampolineBlockRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.rpo_blocks.resize(5);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L1) (pc)))) "
      "(insn (set (reg:SI 2 v0) (const_int 1)))])");
  relaxed_cfg.rpo_blocks[0].successors = {4, 1};
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(sequence [(jump_insn/u (set (pc) (if_then_else (eq:SI (reg:SI 5 a1) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn/s (set (reg:SI 3 v1) (const_int 2)))])");
  relaxed_cfg.rpo_blocks[1].successors = {3, 2};
  // Block 2 (`L0` in pre-image) is the trampoline jumping over Block 3 (`L0` in
  // post-image) to Block 4 (`L1` in post-image).
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(set (pc) (label_ref L1))");
  relaxed_cfg.rpo_blocks[2].successors = {4};
  relaxed_cfg.rpo_blocks[3].has_label = true;
  relaxed_cfg.rpo_blocks[3].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 3))");
  relaxed_cfg.rpo_blocks[3].successors = {4};
  relaxed_cfg.rpo_blocks[4].has_label = true;
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(use (reg/i:SI 2 v0))");
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_TRUE(lifted.rpo_blocks[2].has_label);
  EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[0].instructions[0], "(label_ref L0)"));

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

// Subcase II.3 (`reorg.c:3902-3914`, `redundant_insn(trial, insn, 0)`): Target
// block redundant instruction re-vectoring.
TEST(InvertRelaxDelaySlotsTest, RedundantTargetInstructionReVectoringRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph relaxed_cfg;
  relaxed_cfg.pass = RtlPassKind::kDbr;
  relaxed_cfg.rpo_blocks.resize(5);
  relaxed_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (const_int 42))");
  relaxed_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L1) (pc)))) "
      "(insn (set (reg:SI 2 v0) (const_int 1)))])");
  relaxed_cfg.rpo_blocks[0].successors = {4, 1};
  // Block 1 also jumps to Block 3 (`L0`), but modifies `$3` in its delay slot
  // so `(set (reg:SI 3 v1) (const_int 42))` is not redundant along Block 1.
  relaxed_cfg.rpo_blocks[1].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 5 a1) "
      "(const_int 0)) (label_ref L0) (pc)))) "
      "(insn (set (reg:SI 3 v1) (const_int 99)))])");
  relaxed_cfg.rpo_blocks[1].successors = {3, 2};
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  relaxed_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  // Block 3 (`L0`) has the redundant `(set (reg:SI 3 v1) (const_int 42))`
  // falling through to Block 4 (`L1`), which `relax_delay_slots` skipped over
  // for Block 0.
  relaxed_cfg.rpo_blocks[3].has_label = true;
  relaxed_cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 3 v1) (const_int 42))");
  relaxed_cfg.rpo_blocks[3].successors = {4};
  relaxed_cfg.rpo_blocks[4].has_label = true;
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 3 v1))");
  relaxed_cfg.rpo_blocks[4].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(relaxed_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{3, 1}));
  EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[0].instructions[1], "(label_ref L0)"));

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(relaxed_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertRelaxDelaySlotsTest, AlreadyUnrelaxedCfgPreservedRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (return)) "
      "(insn (set (reg/i:SI 2 v0) (reg:SI 4 a0)))])");

  const RtlControlFlowGraph lifted = InvertRelaxDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.FormatSummary(), cfg.FormatSummary());

  const RightInverseResult res = verifier.VerifyRelaxDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertRelaxDelaySlotsTest, RejectsEmptyAndMalformedControlFlowGraphsAndBadInverses) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionRelaxDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. Empty `rpo_blocks`:
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertRelaxDelaySlots(empty_cfg).rpo_blocks.empty());

  // 2. Single block with zero instructions:
  RtlControlFlowGraph empty_block_cfg;
  empty_block_cfg.pass = RtlPassKind::kDbr;
  empty_block_cfg.rpo_blocks.resize(1);
  EXPECT_TRUE(InvertRelaxDelaySlots(empty_block_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier.VerifyRelaxDelaySlotsRightInverse(empty_block_cfg).matches);

  // 3. Non-terminal empty block in a multi-block CFG:
  {
    RtlControlFlowGraph mid_empty_cfg;
    mid_empty_cfg.pass = RtlPassKind::kDbr;
    mid_empty_cfg.rpo_blocks.resize(2);
    mid_empty_cfg.rpo_blocks[1].AddPatternOnly("(return)");
    EXPECT_TRUE(InvertRelaxDelaySlots(mid_empty_cfg).rpo_blocks.empty());
  }

  // 4. Unbalanced / malformed RTL pattern strings:
  for (const std::string& bad_pat : std::vector<std::string>{
           "",
           "   ",
           "not_an_sexpr",
           "(unknown_op (const_int 1))",
           "(set (reg:SI 2 v0) (const_int 1)",
           "(set (reg:SI 2 v0) (const_int 1)))",
           "(sequence [(jump_insn (return))])",
           "(sequence (jump_insn (return)) (insn (const_int 0)))",
           "(set (reg:SI 2 v0) (const_int 1)) trailing_garbage",
       }) {
    RtlControlFlowGraph bad_cfg;
    bad_cfg.pass = RtlPassKind::kDbr;
    bad_cfg.rpo_blocks.resize(1);
    bad_cfg.rpo_blocks[0].instructions.push_back(bad_pat);
    EXPECT_TRUE(InvertRelaxDelaySlots(bad_cfg).rpo_blocks.empty()) << "bad_pat: " << bad_pat;
    EXPECT_FALSE(verifier.VerifyRelaxDelaySlotsRightInverse(bad_cfg).matches)
        << "bad_pat: " << bad_pat;
  }

  // 5. Out-of-range successor index:
  {
    RtlControlFlowGraph bad_succ_cfg;
    bad_succ_cfg.pass = RtlPassKind::kDbr;
    bad_succ_cfg.rpo_blocks.resize(1);
    bad_succ_cfg.rpo_blocks[0].AddPatternOnly("(return)");
    bad_succ_cfg.rpo_blocks[0].successors = {5};
    EXPECT_TRUE(InvertRelaxDelaySlots(bad_succ_cfg).rpo_blocks.empty());
    EXPECT_FALSE(verifier.VerifyRelaxDelaySlotsRightInverse(bad_succ_cfg).matches);
  }

  // 6. Out-of-range `addr_vec_targets` index:
  {
    RtlControlFlowGraph bad_addr_vec_cfg;
    bad_addr_vec_cfg.pass = RtlPassKind::kDbr;
    bad_addr_vec_cfg.rpo_blocks.resize(1);
    bad_addr_vec_cfg.rpo_blocks[0].AddPatternOnly("(return)");
    bad_addr_vec_cfg.rpo_blocks[0].addr_vec_targets = {3};
    EXPECT_TRUE(InvertRelaxDelaySlots(bad_addr_vec_cfg).rpo_blocks.empty());
    EXPECT_FALSE(verifier.VerifyRelaxDelaySlotsRightInverse(bad_addr_vec_cfg).matches);
  }

  // 7. Corrupted `SubfunctionBoundarySnapshot` fails
  //    `VerifySubfunctionBoundaryRightInverse`:
  {
    SubfunctionBoundarySnapshot bad_snap;
    bad_snap.subfunction_id = SubfunctionId::kRelaxDelaySlots;
    bad_snap.optimize_level = 2;
    bad_snap.post_state.pass = RtlPassKind::kDbr;
    bad_snap.post_state.rpo_blocks.resize(1);
    bad_snap.post_state.rpo_blocks[0].instructions.push_back("(unbalanced (return)");
    EXPECT_FALSE(verifier.VerifySubfunctionBoundaryRightInverse(bad_snap).matches);
  }

  // 8. Wrong inverse provider is rejected by both
  //    `VerifyRelaxDelaySlotsRightInverse` and
  //    `VerifySubfunctionBoundaryRightInverse`:
  {
    WrongRelaxDelaySlotsProvider wrong_provider;
    RightInverseVerifier wrong_verifier(&ctx, &hooks, &wrong_provider);

    RtlControlFlowGraph valid_cfg;
    valid_cfg.pass = RtlPassKind::kDbr;
    valid_cfg.rpo_blocks.resize(3);
    valid_cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) "
        "(const_int 0)) (label_ref L0) (pc)))) "
        "(insn/s (set (reg/i:SI 2 v0) (const_int 1)))])");
    valid_cfg.rpo_blocks[0].successors = {2, 1};
    valid_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
    valid_cfg.rpo_blocks[1].successors = {2};
    valid_cfg.rpo_blocks[2].has_label = true;
    valid_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
    valid_cfg.rpo_blocks[2].AddPatternOnly("(return)");

    EXPECT_FALSE(wrong_verifier.VerifyRelaxDelaySlotsRightInverse(valid_cfg).matches);

    SubfunctionBoundarySnapshot snap;
    snap.subfunction_id = SubfunctionId::kRelaxDelaySlots;
    snap.optimize_level = 2;
    snap.post_state = valid_cfg;
    EXPECT_FALSE(wrong_verifier.VerifySubfunctionBoundaryRightInverse(snap).matches);
  }
}

class RelaxDelaySlotsCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionRelaxDelaySlotsProvider();
    verifier = new RightInverseVerifier(ctx, hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
  static ProductionRelaxDelaySlotsProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* RelaxDelaySlotsCorpusTest::ctx = nullptr;
FunctionHooks* RelaxDelaySlotsCorpusTest::hooks = nullptr;
ProductionRelaxDelaySlotsProvider* RelaxDelaySlotsCorpusTest::provider = nullptr;
RightInverseVerifier* RelaxDelaySlotsCorpusTest::verifier = nullptr;

TEST_P(RelaxDelaySlotsCorpusTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  bool found_snapshot = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kRelaxDelaySlots) {
      continue;
    }
    found_snapshot = true;
    const RightInverseResult res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "kRelaxDelaySlots boundary mismatch for " << tc.test_name << ": "
                             << res.diff_detail << "\nexpected:\n"
                             << res.expected_summary << "\nactual:\n"
                             << res.actual_summary;
  }
  EXPECT_TRUE(found_snapshot) << "Missing kRelaxDelaySlots snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, RelaxDelaySlotsCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
