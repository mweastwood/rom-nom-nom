#include "lifter/invert_fill_eager_delay_slots.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

class ProductionFillEagerDelaySlotsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertFillEagerDelaySlots(
      const RtlControlFlowGraph& dbr_eager_cfg_x) const override {
    return ::rom_nom_nom::lifter::InvertFillEagerDelaySlots(dbr_eager_cfg_x);
  }
};

class FillEagerDelaySlotsCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionFillEagerDelaySlotsProvider();
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
  static ProductionFillEagerDelaySlotsProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* FillEagerDelaySlotsCorpusTest::ctx = nullptr;
FunctionHooks* FillEagerDelaySlotsCorpusTest::hooks = nullptr;
ProductionFillEagerDelaySlotsProvider* FillEagerDelaySlotsCorpusTest::provider = nullptr;
RightInverseVerifier* FillEagerDelaySlotsCorpusTest::verifier = nullptr;

TEST_P(FillEagerDelaySlotsCorpusTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  bool found_snapshot = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kFillEagerDelaySlots) {
      continue;
    }
    found_snapshot = true;
    const RightInverseResult res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "kFillEagerDelaySlots boundary mismatch for " << tc.test_name
                             << ": " << res.diff_detail << "\npre_state:\n"
                             << snap.pre_state.FormatSummary() << "\nlifted:\n"
                             << InvertFillEagerDelaySlots(snap.post_state).FormatSummary()
                             << "\nexpected:\n"
                             << res.expected_summary << "\nactual:\n"
                             << res.actual_summary;
  }
  EXPECT_TRUE(found_snapshot) << "Missing kFillEagerDelaySlots snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, FillEagerDelaySlotsCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

RtlBasicBlock MakeBlock(uint32_t id, bool has_label, std::vector<std::string> instructions,
                        std::vector<size_t> successors) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.has_label = has_label;
  blk.instructions = std::move(instructions);
  blk.successors = std::move(successors);
  return blk;
}

TEST(InvertFillEagerDelaySlotsUnitTest, TargetThreadOwnTargetPrependAndRightInverse) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 7)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (mem:SI (reg:SI 5 a1)) (const_int 0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
      MakeBlock(2, true,
                {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))",
                 "(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(),
            "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  ASSERT_EQ(lifted.rpo_blocks[2].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[0], "(set (reg:SI 2 v0) (const_int 7))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillEagerDelaySlotsUnitTest, TargetThreadMultipleJumpersShareOwnTarget) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (mem:SI (reg:SI 6 a2)) (const_int 0)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int "
                 "0)) (label_ref L1) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 9)))])"},
                {4, 2}),
      MakeBlock(2, true,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int "
                 "0)) (label_ref L1) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 9)))])"},
                {4, 3}),
      MakeBlock(3, false,
                {"(set (mem:SI (reg:SI 6 a2)) (const_int 0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
      MakeBlock(4, true,
                {"(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.back(),
            "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");
  EXPECT_EQ(lifted.rpo_blocks[2].instructions.back(),
            "(set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int 0)) (label_ref L1) (pc)))");
  EXPECT_EQ(lifted.rpo_blocks[4].instructions.front(), "(set (reg:SI 2 v0) (const_int 9))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, TargetThreadSharedTargetMergePredBlock) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 5)))])"},
                {3, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int "
                 "0)) (label_ref L1) (pc)))) (insn (set (mem:SI (reg:SI 6 a2)) (const_int 0)))])"},
                {4, 2}),
      MakeBlock(2, false, {"(set (reg:SI 2 v0) (const_int 5))"}, {3}),
      MakeBlock(3, true,
                {"(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
      MakeBlock(4, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_TRUE(lifted.rpo_blocks[2].has_label);
  ASSERT_EQ(lifted.rpo_blocks[2].instructions.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[0], "(set (reg:SI 2 v0) (const_int 5))");
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[1], "(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))");
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[2], "(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, TargetThreadSharedTargetMoveFromPredBlockWithLoopNote) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 5)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (mem:SI (reg:SI 5 a1)) (const_int 0))", "(note NOTE_INSN_LOOP_CONT)",
                 "(set (reg:SI 2 v0) (const_int 5))"},
                {2}),
      MakeBlock(2, true,
                {"(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[2].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[0], "(note NOTE_INSN_LOOP_CONT)");
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[1], "(set (reg:SI 2 v0) (const_int 5))");
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[2], "(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))");
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[3], "(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, TargetThreadSharedTargetSplitFallthroughBlock) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 5)))])"},
                {3, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (const_int 9)))])"},
                {3, 2}),
      MakeBlock(2, false,
                {"(set (mem:SI (reg:SI 6 a2)) (const_int 0))", "(set (reg:SI 2 v0) (const_int 5))"},
                {3}),
      MakeBlock(3, true,
                {"(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
  EXPECT_EQ(lifted.rpo_blocks[0].successors[0], 3u);
  EXPECT_EQ(lifted.rpo_blocks[1].successors[0], 4u);
  EXPECT_EQ(lifted.rpo_blocks[3].instructions.front(), "(set (reg:SI 2 v0) (const_int 5))");
  EXPECT_EQ(lifted.rpo_blocks[3].successors, std::vector<size_t>{4u});

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, TargetThreadSharedTargetNoFallthroughInsertTrampoline) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 5)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (mem:SI (reg:SI 5 a1)) (const_int 0))",
                 "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1)))",
                 "(sequence [(jump_insn (set (pc) (label_ref L0))) (insn (set (mem:SI (reg:SI 6 "
                 "a2)) (const_int 0)))])"},
                {2}),
      MakeBlock(2, true,
                {"(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].successors[0], 2u);
  EXPECT_EQ(lifted.rpo_blocks[1].successors[0], 3u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions.front(), "(set (reg:SI 2 v0) (const_int 5))");
  EXPECT_EQ(lifted.rpo_blocks[2].successors, std::vector<size_t>{3u});

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, TargetThreadSharedTargetWithFallthroughStealTrampoline) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 5)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (mem:SI (reg:SI 5 a1)) (const_int 0))",
                 "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1)))"},
                {2}),
      MakeBlock(2, true,
                {"(set (mem:SI (reg:SI 7 a3)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].successors[0], 3u);
  EXPECT_EQ(lifted.rpo_blocks[3].successors, std::vector<size_t>{2u});
  EXPECT_EQ(lifted.rpo_blocks[3].instructions.front(),
            "(sequence [(jump_insn (set (pc) (label_ref L0))) (insn (set (reg:SI 2 v0) (const_int "
            "5)))])");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, FallthroughThreadOwnAndSharedFallthrough) {
  RtlControlFlowGraph own_ft_cfg;
  own_ft_cfg.pass = RtlPassKind::kDbr;
  own_ft_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (const_int 11)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (mem:SI (reg:SI 5 a1)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
      MakeBlock(2, true,
                {"(set (mem:SI (reg:SI 6 a2)) (const_int 0))",
                 "(set (reg:SI 2 v0) (mem:SI (reg:SI 6 a2)))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted_own = InvertFillEagerDelaySlots(own_ft_cfg);
  ASSERT_EQ(lifted_own.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted_own.rpo_blocks[1].instructions.size(), 3u);
  EXPECT_EQ(lifted_own.rpo_blocks[1].instructions[0], "(set (reg:SI 2 v0) (const_int 11))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res_own = verifier.VerifyFillEagerDelaySlotsRightInverse(own_ft_cfg);
  EXPECT_TRUE(res_own.matches) << res_own.diff_detail << "\nlifted:\n"
                               << lifted_own.FormatSummary() << "\nactual:\n"
                               << res_own.actual_summary;

  RtlControlFlowGraph shared_ft_cfg;
  shared_ft_cfg.pass = RtlPassKind::kDbr;
  shared_ft_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int "
                 "0)) (label_ref L1) (pc)))) (insn (set (reg:SI 2 v0) (const_int 11)))])"},
                {2, 1}),
      MakeBlock(1, true,
                {"(set (mem:SI (reg:SI 5 a1)) (reg:SI 2 v0))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
      MakeBlock(2, true,
                {"(set (mem:SI (reg:SI 6 a2)) (const_int 0))",
                 "(set (reg:SI 2 v0) (mem:SI (reg:SI 6 a2)))",
                 "(sequence [(jump_insn (set (pc) (label_ref L0))) (insn (set (mem:SI (reg:SI 7 "
                 "a3)) (const_int 0)))])"},
                {1}),
  };
  const RtlControlFlowGraph lifted_shared = InvertFillEagerDelaySlots(shared_ft_cfg);
  ASSERT_EQ(lifted_shared.rpo_blocks.size(), 4u);
  EXPECT_FALSE(lifted_shared.rpo_blocks[1].has_label);
  EXPECT_EQ(lifted_shared.rpo_blocks[1].instructions,
            std::vector<std::string>{"(set (reg:SI 2 v0) (const_int 11))"});
  EXPECT_EQ(lifted_shared.rpo_blocks[1].successors, std::vector<size_t>{2u});
  const RightInverseResult res_shared =
      verifier.VerifyFillEagerDelaySlotsRightInverse(shared_ft_cfg);
  EXPECT_TRUE(res_shared.matches) << res_shared.diff_detail << "\nlifted:\n"
                                  << lifted_shared.FormatSummary() << "\nactual:\n"
                                  << res_shared.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, FallthroughThreadStealDelayListFromFallthrough) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) "
                 "(const_int 1))))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (mem:QI (reg:SI 5 a1)) (const_int 0))", "(set (pc) (label_ref L0))"}, {2}),
      MakeBlock(2, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(),
            "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.back(),
            "(sequence [(jump_insn (set (pc) (label_ref L0))) (insn (set (reg:SI 6 a2) (plus:SI "
            "(reg:SI 6 a2) (const_int 1))))])");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillEagerDelaySlotsUnitTest, PreservesOptimizeSkipAndSimpleDelaySlots) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn/u (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 1)))])"},
                {1, 1}),
      MakeBlock(1, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 2u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(), cfg.rpo_blocks[0].instructions.back());

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillEagerDelaySlotsUnitTest, RejectsInvalidAndMalformedCfgs) {
  // 1. Empty CFG.
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertFillEagerDelaySlots(empty_cfg).rpo_blocks.empty());

  // 2. Single block with empty instructions.
  RtlControlFlowGraph empty_blk_cfg;
  empty_blk_cfg.rpo_blocks.push_back(MakeBlock(0, false, {}, {}));
  EXPECT_TRUE(InvertFillEagerDelaySlots(empty_blk_cfg).rpo_blocks.empty());

  // 3. Out-of-bounds successor index.
  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.rpo_blocks.push_back(
      MakeBlock(0, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {5}));
  EXPECT_TRUE(InvertFillEagerDelaySlots(bad_succ_cfg).rpo_blocks.empty());

  // 4. Out-of-bounds addr_vec_targets index.
  RtlControlFlowGraph bad_addr_vec_cfg;
  RtlBasicBlock bad_addr_blk =
      MakeBlock(0, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {});
  bad_addr_blk.addr_vec_targets = {3};
  bad_addr_vec_cfg.rpo_blocks.push_back(bad_addr_blk);
  EXPECT_TRUE(InvertFillEagerDelaySlots(bad_addr_vec_cfg).rpo_blocks.empty());

  // 5. Unparseable S-expression.
  RtlControlFlowGraph bad_sexpr_cfg;
  bad_sexpr_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(set (reg:SI 2 v0)"}, {}));
  EXPECT_TRUE(InvertFillEagerDelaySlots(bad_sexpr_cfg).rpo_blocks.empty());

  // 6. Unknown RTL operator.
  RtlControlFlowGraph unknown_op_cfg;
  unknown_op_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(not_an_rtl_op (reg:SI 2 v0))"}, {}));
  EXPECT_TRUE(InvertFillEagerDelaySlots(unknown_op_cfg).rpo_blocks.empty());

  // 7. Malformed sequence with fewer than 2 children.
  RtlControlFlowGraph bad_seq_cfg;
  bad_seq_cfg.rpo_blocks.push_back(MakeBlock(
      0, false, {"(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] ))])"}, {}));
  EXPECT_TRUE(InvertFillEagerDelaySlots(bad_seq_cfg).rpo_blocks.empty());
}

TEST(InvertFillEagerDelaySlotsUnitTest, VerifierRejectsCorruptedInverse) {
  class BrokenFillEagerDelaySlotsProvider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertFillEagerDelaySlots(
        const RtlControlFlowGraph& dbr_eager_cfg_x) const override {
      RtlControlFlowGraph corrupted = dbr_eager_cfg_x;
      if (!corrupted.rpo_blocks.empty()) {
        corrupted.rpo_blocks[0].instructions = {"(parallel[ (return) (use (reg:SI 31 ra)) ])"};
        corrupted.rpo_blocks[0].successors.clear();
      }
      return corrupted;
    }
  };

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 7)))])"},
                {2, 1}),
      MakeBlock(
          1, false,
          {"(set (reg:SI 2 v0) (const_int 1))", "(parallel[ (return) (use (reg:SI 31 ra)) ])"}, {}),
      MakeBlock(2, true,
                {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ])"},
                {}),
  };

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  BrokenFillEagerDelaySlotsProvider broken_provider;
  RightInverseVerifier verifier(&ctx, &hooks, &broken_provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_FALSE(res.matches);
}

}  // namespace
}  // namespace rom_nom_nom::lifter
