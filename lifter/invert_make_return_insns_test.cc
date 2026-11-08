#include "lifter/invert_make_return_insns.h"

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
// implementation of `InvertMakeReturnInsns`.
class ProductionMakeReturnInsnsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertMakeReturnInsns(
      const RtlControlFlowGraph& dbr_return_cfg_x) const override {
    return ::rom_nom_nom::lifter::InvertMakeReturnInsns(dbr_return_cfg_x);
  }
};

TEST(InvertMakeReturnInsnsTest, SingleBlockSequenceReturnRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph ret_cfg;
  ret_cfg.pass = RtlPassKind::kDbr;
  ret_cfg.rpo_blocks.resize(1);
  ret_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (const_int 42)))])");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(ret_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 2u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn (set (pc) (label_ref L1))) "
            "(insn (set (reg/i:SI 2 v0) (const_int 42)))])");
  EXPECT_EQ(lifted.rpo_blocks[0].successors, std::vector<size_t>{1});
  EXPECT_TRUE(lifted.rpo_blocks[1].instructions.empty());

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(ret_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, MultiBlockMultipleSequenceReturnsShareEndBlock) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (const_int 1)))])");
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[1].AddPatternOnly(
      "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (const_int 10)))])");
  cfg.rpo_blocks[2].has_label = true;
  cfg.rpo_blocks[2].AddPatternOnly(
      "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (const_int 20)))])");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[1].successors, std::vector<size_t>{3});
  EXPECT_EQ(lifted.rpo_blocks[2].successors, std::vector<size_t>{3});
  EXPECT_TRUE(lifted.rpo_blocks[3].instructions.empty());

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, ReusesExistingTrailingEmptyBlockTargetedByConditionalBranch) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // Block 0 conditionally branches directly to the trailing empty return label
  // block (Block 2, `L0`), while Block 1 returns via a delay-slot-filled
  // `(sequence [(jump_insn (return)) ...])`.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 4 a0))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[1].AddPatternOnly(
      "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (const_int -1)))])");
  cfg.rpo_blocks[2].has_label = true;

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[1].successors, std::vector<size_t>{2});
  EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[1].instructions[0], "(label_ref L0)"));

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest,
     FallthroughConditionalBranchBeforeTrailingEmptyBlockDoesNotStealLoopLabel) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // Case 1: No existing block explicitly jumps to the trailing empty block
  // (Block 2); Block 1 (`b = last_idx - 1`) branches back to Block 0 (`L0`)
  // and only falls through to Block 2 (`last_idx = 2`). `InvertMakeReturnInsns`
  // must reuse Block 2, mark `has_label = true`, and synthesize a fresh label
  // `L2` rather than stealing `L0` from Block 1's back-edge branch.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].has_label = true;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (const_int -1)))])");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 1)) "
        "(label_ref L0) (pc)))) (insn (set (reg/i:SI 2 v0) (reg:SI 4 a0)))])");
    cfg.rpo_blocks[1].successors = {0, 2};

    const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
    EXPECT_EQ(lifted.rpo_blocks[0].successors, std::vector<size_t>{2});
    EXPECT_TRUE(lifted.rpo_blocks[2].has_label);
    EXPECT_FALSE(absl::StrContains(lifted.rpo_blocks[0].instructions[0], "(label_ref L0)"));
    EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[0].instructions[0], "(label_ref L2)"));
  }

  // Case 2: Full right-inverse round-trip where Block 3 (`b = last_idx - 1`)
  // branches back to Block 0 (`L0`) and falls through into the trailing empty
  // Block 4 (`last_idx = 4`), Block 1 returns via `(sequence [(jump_insn (return)) ...])`,
  // and Block 2 has a bare conditional branch to Block 4 (`L2`) so that
  // `end_of_function_label` remains live (`LABEL_NUSES > 0`) after `make_return_insns`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(5);
    cfg.rpo_blocks[0].has_label = true;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) "
        "(label_ref L1) (pc)))) (insn (set (reg/i:SI 2 v0) (const_int 0)))])");
    cfg.rpo_blocks[0].successors = {2, 1};
    cfg.rpo_blocks[1].AddPatternOnly(
        "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (const_int -1)))])");
    cfg.rpo_blocks[2].has_label = true;
    cfg.rpo_blocks[2].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L2) (pc)))");
    cfg.rpo_blocks[2].successors = {4, 3};
    cfg.rpo_blocks[3].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) "
        "(label_ref L0) (pc)))) (insn (set (reg/i:SI 2 v0) (reg:SI 4 a0)))])");
    cfg.rpo_blocks[3].successors = {0, 4};
    cfg.rpo_blocks[4].has_label = true;

    const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
    EXPECT_EQ(lifted.rpo_blocks[1].successors, std::vector<size_t>{4});
    EXPECT_TRUE(lifted.rpo_blocks[4].has_label);
    EXPECT_FALSE(absl::StrContains(lifted.rpo_blocks[1].instructions[0], "(label_ref L0)"));
    EXPECT_TRUE(absl::StrContains(lifted.rpo_blocks[1].instructions[0], "(label_ref L2)"));

    const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST(InvertMakeReturnInsnsTest, PreservesJumpInsnFlagsAndRegNotesInsideSequence) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn/u (return) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))) "
      "(insn (set (reg/i:SI 2 v0) (const_int 7)))])");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 2u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn/u (set (pc) (label_ref L1)) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))) "
            "(insn (set (reg/i:SI 2 v0) (const_int 7)))])");

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, StandaloneReturnAndParallelReturnArePreserved) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. Standalone bare `(return)`:
  {
    RtlControlFlowGraph bare_ret_cfg;
    bare_ret_cfg.pass = RtlPassKind::kDbr;
    bare_ret_cfg.rpo_blocks.resize(1);
    bare_ret_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
    bare_ret_cfg.rpo_blocks[0].AddPatternOnly("(return)");

    const RtlControlFlowGraph lifted = InvertMakeReturnInsns(bare_ret_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(), "(return)");

    const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(bare_ret_cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. Standalone `(parallel[ (return) (use (reg:SI 31 ra)) ] )`:
  {
    RtlControlFlowGraph par_ret_cfg;
    par_ret_cfg.pass = RtlPassKind::kDbr;
    par_ret_cfg.rpo_blocks.resize(1);
    par_ret_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 5))");
    par_ret_cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const RtlControlFlowGraph lifted = InvertMakeReturnInsns(par_ret_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(),
              "(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(par_ret_cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST(InvertMakeReturnInsnsTest, NonLeafFunctionWithEndLabelUnchangedRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph non_leaf_cfg;
  non_leaf_cfg.pass = RtlPassKind::kDbr;
  non_leaf_cfg.sideband.frame_size_bytes = 24;
  non_leaf_cfg.sideband.saved_gpr_regnos = {31};
  non_leaf_cfg.rpo_blocks.resize(3);
  non_leaf_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))) (insn (set (reg/i:SI 2 v0) (const_int 99)))])");
  non_leaf_cfg.rpo_blocks[0].successors = {2, 1};
  non_leaf_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 1))");
  non_leaf_cfg.rpo_blocks[1].successors = {2};
  non_leaf_cfg.rpo_blocks[2].has_label = true;

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(non_leaf_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(non_leaf_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, TrailingUseBlockWithEarlierReturnPreservesTargetsRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))) (insn/s (set (reg/i:SI 2 v0) (const_int 0)))])");
  cfg.rpo_blocks[0].successors = {2, 1};
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 4 a0))");
  cfg.rpo_blocks[1].AddPatternOnly("(return)");
  cfg.rpo_blocks[2].has_label = true;
  cfg.rpo_blocks[2].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_TRUE(lifted.rpo_blocks[3].instructions.empty());
  EXPECT_TRUE(lifted.rpo_blocks[3].has_label);

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, RejectsEmptyAndMalformedControlFlowGraphs) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. Empty `rpo_blocks`:
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertMakeReturnInsns(empty_cfg).rpo_blocks.empty());

  // 2. Single block with zero instructions:
  RtlControlFlowGraph empty_block_cfg;
  empty_block_cfg.pass = RtlPassKind::kDbr;
  empty_block_cfg.rpo_blocks.resize(1);
  EXPECT_TRUE(InvertMakeReturnInsns(empty_block_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier.VerifyMakeReturnInsnsRightInverse(empty_block_cfg).matches);

  // 3. Unbalanced / malformed RTL pattern strings:
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
    EXPECT_TRUE(InvertMakeReturnInsns(bad_cfg).rpo_blocks.empty()) << "bad_pat: " << bad_pat;
    EXPECT_FALSE(verifier.VerifyMakeReturnInsnsRightInverse(bad_cfg).matches)
        << "bad_pat: " << bad_pat;
  }

  // 4. Out-of-range successor index:
  {
    RtlControlFlowGraph bad_succ_cfg;
    bad_succ_cfg.pass = RtlPassKind::kDbr;
    bad_succ_cfg.rpo_blocks.resize(1);
    bad_succ_cfg.rpo_blocks[0].AddPatternOnly("(return)");
    bad_succ_cfg.rpo_blocks[0].successors = {5};
    EXPECT_TRUE(InvertMakeReturnInsns(bad_succ_cfg).rpo_blocks.empty());
    EXPECT_FALSE(verifier.VerifyMakeReturnInsnsRightInverse(bad_succ_cfg).matches);
  }

  // 5. Out-of-range `addr_vec_targets` index:
  {
    RtlControlFlowGraph bad_addr_vec_cfg;
    bad_addr_vec_cfg.pass = RtlPassKind::kDbr;
    bad_addr_vec_cfg.rpo_blocks.resize(1);
    bad_addr_vec_cfg.rpo_blocks[0].AddPatternOnly("(return)");
    bad_addr_vec_cfg.rpo_blocks[0].addr_vec_targets = {3};
    EXPECT_TRUE(InvertMakeReturnInsns(bad_addr_vec_cfg).rpo_blocks.empty());
    EXPECT_FALSE(verifier.VerifyMakeReturnInsnsRightInverse(bad_addr_vec_cfg).matches);
  }

  // 6. Corrupted `SubfunctionBoundarySnapshot` fails `VerifySubfunctionBoundaryRightInverse`:
  {
    SubfunctionBoundarySnapshot bad_snap;
    bad_snap.subfunction_id = SubfunctionId::kMakeReturnInsns;
    bad_snap.optimize_level = 2;
    bad_snap.post_state.pass = RtlPassKind::kDbr;
    bad_snap.post_state.rpo_blocks.resize(1);
    bad_snap.post_state.rpo_blocks[0].instructions.push_back("(unbalanced (return)");
    EXPECT_FALSE(verifier.VerifySubfunctionBoundaryRightInverse(bad_snap).matches);
  }
}

class MakeReturnInsnsCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionMakeReturnInsnsProvider();
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
  static ProductionMakeReturnInsnsProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* MakeReturnInsnsCorpusTest::ctx = nullptr;
FunctionHooks* MakeReturnInsnsCorpusTest::hooks = nullptr;
ProductionMakeReturnInsnsProvider* MakeReturnInsnsCorpusTest::provider = nullptr;
RightInverseVerifier* MakeReturnInsnsCorpusTest::verifier = nullptr;

TEST_P(MakeReturnInsnsCorpusTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  bool found_snapshot = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kMakeReturnInsns) {
      continue;
    }
    found_snapshot = true;
    const RightInverseResult res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "kMakeReturnInsns boundary mismatch for " << tc.test_name << ": "
                             << res.diff_detail << "\nexpected:\n"
                             << res.expected_summary << "\nactual:\n"
                             << res.actual_summary;
  }
  EXPECT_TRUE(found_snapshot) << "Missing kMakeReturnInsns snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, MakeReturnInsnsCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
