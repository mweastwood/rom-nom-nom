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
  blk.SyncRtlInsnsFromInstructions();
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

TEST(InvertFillEagerDelaySlotsUnitTest, DiamondSubregSplitAndUseBridgeBeforeReturnRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  hooks.ApplyCompilationProfile(
      CompilationProfile{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}});
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. Diamond `try_split` `DImode` `subreg:SI` register-move recombination: `B0` stole the high
  // word `(set (subreg:SI (reg:DI 2 v0) 0) (subreg:SI (reg:DI 6 a2) 0))` from `B2` where `(set
  // (subreg:SI (reg:DI 2 v0) 1) (subreg:SI (reg:DI 6 a2) 1))` remains, while `B1` writes `(set
  // (reg:DI 2 v0) (const_int 1))` and jumps to `B3`. `InvertFillEagerDelaySlots` recombines `B2`'s
  // split halves into `(set (reg:DI 2 v0) (reg:DI 6 a2))` and satisfies
  // `VerifyFillEagerDelaySlotsRightInverse`.
  {
    RtlControlFlowGraph diamond_di_cfg;
    diamond_di_cfg.pass = RtlPassKind::kDbr;
    diamond_di_cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {"(set (reg:DI 2 v0) (reg:DI 4 a0))",
                   "(sequence [(jump_insn/u (set (pc) (if_then_else (eq:SI (reg:SI 8 t0) "
                   "(const_int 0)) (label_ref L0) (pc)))) "
                   "(insn/s (set (subreg:SI (reg:DI 2 v0) 0) (subreg:SI (reg:DI 6 a2) 0)))])"},
                  {2, 1}),
        MakeBlock(1, false,
                  {"(set (mem:DI (reg:SI 9 t1)) (reg:DI 2 v0))", "(set (pc) (label_ref L1))"}, {3}),
        MakeBlock(2, true, {"(set (subreg:SI (reg:DI 2 v0) 1) (subreg:SI (reg:DI 6 a2) 1))"}, {3}),
        MakeBlock(3, true,
                  {"(use (reg/i:DI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
    };

    const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(diamond_di_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
    ASSERT_EQ(lifted.rpo_blocks[2].instructions.size(), 1u);
    EXPECT_EQ(lifted.rpo_blocks[2].instructions[0], "(set (reg:DI 2 v0) (reg:DI 6 a2))");

    const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(diamond_di_cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                             << lifted.FormatSummary() << "\nactual:\n"
                             << res.actual_summary;
  }

  // 2. Shared `(use v0)` bridge in `Im(fill_eager_delay_slots)`: `B0` (`v0 = 0` -> `B4: return`)
  // and `B1` (`v0 = 0` -> `B3: (use v0)`) share `slot_pat = (set (reg/i:SI 2 v0) (const_int 0))`,
  // while `B2` ends with a `fill_simple_delay_slots` unconditional `SEQUENCE` jump over the
  // trampoline to `B3`. `InvertFillEagerDelaySlots` reconstructs `[L0: v0 = 0]` between `B2` and
  // `[L1: (use v0), (return)]` and satisfies `VerifyFillEagerDelaySlotsRightInverse` directly.
  {
    RtlControlFlowGraph bridge_eager_cfg;
    bridge_eager_cfg.pass = RtlPassKind::kDbr;
    bridge_eager_cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {"(set (reg/i:SI 2 v0) (reg:SI 6 a2))",
                   "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg/i:SI 2 v0) "
                   "(reg:SI 4 a0)) (label_ref L1) (pc)))) "
                   "(insn/s (set (reg/i:SI 2 v0) (const_int 0)))])"},
                  {4, 1}),
        MakeBlock(1, false,
                  {"(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 5 a1) "
                   "(const_int 0)) (label_ref L0) (pc)))) "
                   "(insn/s (set (reg/i:SI 2 v0) (const_int 0)))])"},
                  {3, 2}),
        MakeBlock(2, false,
                  {"(set (reg:SI 8 t0) (const_int 1))",
                   "(sequence [(jump_insn (set (pc) (label_ref L0))) "
                   "(insn (set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (reg:SI 8 t0))))])"},
                  {3}),
        MakeBlock(3, true, {"(use (reg/i:SI 2 v0))"}, {4}),
        MakeBlock(4, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
    };

    const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(bridge_eager_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
    EXPECT_EQ(lifted.rpo_blocks[3].instructions,
              std::vector<std::string>{"(set (reg/i:SI 2 v0) (const_int 0))"});
    EXPECT_EQ(lifted.rpo_blocks[3].successors, std::vector<size_t>{4u});

    const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(bridge_eager_cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                             << lifted.FormatSummary() << "\nactual:\n"
                             << res.actual_summary;
  }
}

TEST(InvertFillEagerDelaySlotsUnitTest, FallthroughUseBridgePlacesTrampolineAfterBarrier) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  hooks.ApplyCompilationProfile(
      CompilationProfile{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}});
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // When `B2` genuinely falls through into `B3: L0: (use v0)` -> `B4: L1: return` without a jump
  // barrier, `B0` and `B1` stole from after-barrier `[L_tramp: j L_use; v0 = ...]` blocks and
  // target `B4` (`L1: return`); `InvertFillEagerDelaySlots` places the trampolines after the
  // return barrier and satisfies `VerifyFillEagerDelaySlotsRightInverse` directly on the unmutated
  // input.
  RtlControlFlowGraph after_barrier_eager_cfg;
  after_barrier_eager_cfg.pass = RtlPassKind::kDbr;
  after_barrier_eager_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg/i:SI 2 v0) (reg:SI 6 a2))",
                 "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg/i:SI 2 v0) "
                 "(reg:SI 4 a0)) (label_ref L1) (pc)))) "
                 "(insn/s (set (reg/i:SI 2 v0) (const_int -1)))])"},
                {4, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 5 a1) "
                 "(const_int 0)) (label_ref L1) (pc)))) "
                 "(insn/s (set (reg/i:SI 2 v0) (const_int 0)))])"},
                {4, 2}),
      MakeBlock(2, false,
                {"(set (reg:SI 8 t0) (const_int 1))",
                 "(set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (reg:SI 8 t0))) "
                 "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))"},
                {3}),
      MakeBlock(3, true, {"(use (reg/i:SI 2 v0))"}, {4}),
      MakeBlock(4, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(after_barrier_eager_cfg);
  ASSERT_GE(lifted.rpo_blocks.size(), 6u);
  EXPECT_NE(lifted.rpo_blocks[2].instructions.back(), "(set (pc) (label_ref L1))");

  const RightInverseResult res =
      verifier.VerifyFillEagerDelaySlotsRightInverse(after_barrier_eager_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nactual:\n" << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest, RedundantPrefixTargetStealRedirectsWithoutSplitting) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  hooks.ApplyCompilationProfile(
      CompilationProfile{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}});
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph redundant_prefix_cfg;
  redundant_prefix_cfg.pass = RtlPassKind::kDbr;
  redundant_prefix_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg:SI 8 t0) (reg:SI 4 a0))", "(set (reg:SI 9 t1) (const_int 100))",
                 "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 6 a2) "
                 "(const_int 0)) (label_ref L0) (pc)))) "
                 "(insn/s (set (reg:SI 8 t0) (reg:SI 9 t1)))])"},
                {3, 1}),
      MakeBlock(1, false,
                {"(set (reg:SI 9 t1) (const_int -100))", "(set (reg:SI 10 t2) (reg:SI 8 t0))",
                 "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 7 a3) "
                 "(const_int 0)) (label_ref L1) (pc)))) "
                 "(insn/s (set (reg:SI 11 t3) (plus:SI (reg:SI 10 t2) (reg:SI 5 a1))))])"},
                {4, 2}),
      MakeBlock(2, false, {"(set (reg:SI 8 t0) (reg:SI 9 t1))"}, {3}),
      MakeBlock(
          3, true,
          {"(set (reg:SI 10 t2) (reg:SI 8 t0))",
           "(set (reg:SI 11 t3) (plus:SI (reg:SI 10 t2) (reg:SI 5 a1)))", "(use (reg/i:SI 2 v0))"},
          {4}),
      MakeBlock(4, true,
                {"(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                 "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 11 t3) (const_int 1))))])"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(redundant_prefix_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{2u, 1u}));
  EXPECT_EQ(lifted.rpo_blocks[1].successors, (std::vector<size_t>{3u, 2u}));

  const RightInverseResult res =
      verifier.VerifyFillEagerDelaySlotsRightInverse(redundant_prefix_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
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

TEST(InvertFillEagerDelaySlotsUnitTest, PreservesDiModeLibcallFusageAcrossMergedTargetBlock) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const auto trace = hooks.TraceForwardCompilation(
      "long long DivModCombine(long long a, long long b) {\n"
      "  if (b == 0) return 0;\n"
      "  return ((a / b) << 2) + (a % b);\n"
      "}\n",
      "DivModCombine", {"-O2", "-mips2", "-mcpu=r4000"});

  const SubfunctionBoundarySnapshot* eager_snap = nullptr;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kFillEagerDelaySlots) {
      eager_snap = &snap;
      break;
    }
  }
  ASSERT_NE(eager_snap, nullptr);

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(eager_snap->post_state);
  size_t explicit_fusage_calls = 0;
  for (const RtlBasicBlock& blk : lifted.rpo_blocks) {
    for (const RtlInsn& rinsn : blk.rtl_insns) {
      if (rinsn.has_explicit_call_fusage) {
        ++explicit_fusage_calls;
      }
    }
  }
  EXPECT_EQ(explicit_fusage_calls, 2u);

  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(*eager_snap);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillEagerDelaySlotsUnitTest,
     LoopBegNoteAfterFallthroughPredecessorStaysInPredecessorWithoutExtraBlock) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const std::vector<std::string> flags = {"-O2", "-mips1"};
  hooks.ApplyCompilationProfile(CompilationProfile{.optimize_level = 2, .c_flags = flags});
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // `B0` initializes loop state (`v0 = 0`) and also holds the moved first-iteration
  // `(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))` after `reorg.c` shifted `L0:`
  // past `v0 = v0 + 3` when stealing it into `B1`'s backward branch delay slot (with opposite
  // fallthrough compensation `(-3)` in `B2`, `reorg.c:3528-3586`), while Stage 0 post-`dbr` input
  // has no `NOTE_INSN_LOOP_BEG` in `B0`.
  // `InvertFillEagerDelaySlots` must move `v0 = v0 + 3` back to `B1` and attach
  // `(note NOTE_INSN_LOOP_BEG)` to the end of `B0` rather than inserting a spurious standalone
  // note-only block between `B0` and `B1`!
  RtlControlFlowGraph loop_cfg;
  loop_cfg.pass = RtlPassKind::kDbr;
  loop_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (const_int 0))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))",
                },
                {1}),
      MakeBlock(
          1, true,
          {
              "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))",
              "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) "
              "(label_ref L0) (pc)))) "
              "(insn/s (set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3))))])",
          },
          {1, 2}),
      MakeBlock(2, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -3)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(loop_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(), "(note NOTE_INSN_LOOP_BEG)");

  const auto eager_snap = hooks.ExecuteSubfunctionBoundary(
      rom_nom_nom::gcc_harness::SubfunctionId::kFillEagerDelaySlots, lifted, {}, 2, flags);
  const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(eager_snap);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
  EXPECT_TRUE(
      InvertFillEagerDelaySlots(eager_snap.post_state).CompareAgainst(lifted).MatchesExactly());
}

TEST(InvertFillEagerDelaySlotsUnitTest,
     OppositeArithTargetStealWhenBranchReadsSlotRegisterRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const std::vector<std::string> flags = {"-O2", "-mips1"};
  hooks.ApplyCompilationProfile(CompilationProfile{.optimize_level = 2, .c_flags = flags});
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // Backward branch in `B1` both reads `(reg:SI 4 a0)` (`jump_reads_slot_def == true`) and steals
  // `(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))` from `L0` with opposite `+1`
  // compensation in fallthrough exit `B2` (`reorg.c:3528-3586`).
  RtlControlFlowGraph loop_cfg;
  loop_cfg.pass = RtlPassKind::kDbr;
  loop_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (const_int 0))",
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))",
                },
                {1}),
      MakeBlock(
          1, true,
          {
              "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
              "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) "
              "(label_ref L0) (pc)))) "
              "(insn/s (set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1))))])",
          },
          {1, 2}),
      MakeBlock(2, false,
                {
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(loop_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.front(),
            "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))");

  const auto eager_snap = hooks.ExecuteSubfunctionBoundary(
      rom_nom_nom::gcc_harness::SubfunctionId::kFillEagerDelaySlots, lifted, {}, 2, flags);
  const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(eager_snap);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillEagerDelaySlotsUnitTest,
     PostEagerTargetStolenDuplicateAttachesBranchRegDeadRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const std::vector<std::string> flags = {"-O2", "-mips1"};
  const CompilationProfile profile{.optimize_level = 2, .c_flags = flags};
  hooks.ApplyCompilationProfile(profile);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // `B0` sets `(reg:SI 8 t0) (const_int 1)` and jumps to `B2` (`mid_blk`: single bare conditional
  // jump reading `$8` and jumping to `B4: nxt_b`, which also starts with `(set (reg:SI 8 t0) 1)`).
  // `ApplyPostEagerDelaySlotNormalizations` duplicates `(set (reg:SI 8 t0) (const_int 1))` into
  // `B2` AND attaches `REG_DEAD (reg:SI 8 t0)` onto `B0`'s branch at `optimize_level > 0`.
  RtlControlFlowGraph post_eager_cfg;
  post_eager_cfg.pass = RtlPassKind::kDbr;
  post_eager_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 8 t0) (const_int 1))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (reg:SI 8 t0)) "
                    "(label_ref L0) (pc)))",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg/i:SI 2 v0) (const_int 0))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(2, true,
                {
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (reg:SI 8 t0)) "
                    "(label_ref L1) (pc)))",
                },
                {4, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg/i:SI 2 v0) (const_int 2))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 8 t0) (const_int 1))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 6 a2) (reg:SI 8 t0)))",
                },
                {5}),
      MakeBlock(5, true,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillEagerDelaySlots(post_eager_cfg, profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 6u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions.front(), "(set (reg:SI 8 t0) (const_int 1))");
  EXPECT_NE(lifted.rpo_blocks[0].instructions.back().find("REG_DEAD (reg:SI 8 t0)"),
            std::string::npos)
      << lifted.rpo_blocks[0].instructions.back();

  const auto replayed = hooks.ExecuteSubfunctionBoundary(
      rom_nom_nom::gcc_harness::SubfunctionId::kFillEagerDelaySlots, lifted, {}, 2, flags);
  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(replayed);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;
  const RightInverseResult direct_res =
      verifier.VerifyFillEagerDelaySlotsRightInverse(replayed.post_state);
  EXPECT_TRUE(direct_res.matches) << direct_res.diff_detail;
}

TEST(InvertFillEagerDelaySlotsUnitTest,
     RelocatesEagerStolenFallthroughSlotPastOppositeLiveInsnForSched1) {
  // Mirrors `reorg.c:3258-3460` (`fill_slots_from_thread`, `ANNUL_IFTRUE == (nil)` in
  // `mips.md:125-128`): `B0` branches to `B2` (where `$5` / `a1` is live) and falls through
  // to `B1`. `reorg.c` skipped `(set a1 (plus a1 25))` because `a1` is live at `B2`, and
  // stole `(set v0 -67174400)` into `B0`'s delay slot. Under `-O2` (`RunsSched1`), restoring
  // `(set v0 -67174400)` at index 0 of `B1` is NOT `sched1`-achievable (single-set `v0` gets
  // `birthing_insn_p` priority boost over multi-set `a1`), so `InvertFillEagerDelaySlots` must
  // place it after `(set a1 (plus a1 25))`; under `-O1` (`!RunsSched1`), index 0 is preserved.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 5 a1) (const_int 10))",
                    "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))",
                    "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 2 v0) "
                    "(const_int 0)) (label_ref L0) (pc)))) "
                    "(insn (set (reg:SI 2 v0) (const_int -67174400)))])",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 25)))",
                    "(set (reg:SI 2 v0) (ior:SI (reg:SI 2 v0) (const_int 65535))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                    "(set (reg:SI 2 v0) (and:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))",
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 2 v0)) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                },
                {2}),
      MakeBlock(2, true,
                {
                    "(set (reg/i:SI 2 v0) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_o2 = InvertFillEagerDelaySlots(cfg, o2_prof);
  ASSERT_EQ(lifted_o2.rpo_blocks.size(), 3u);
  ASSERT_GE(lifted_o2.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(lifted_o2.rpo_blocks[1].instructions[0],
            "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 25)))");
  EXPECT_EQ(lifted_o2.rpo_blocks[1].instructions[1], "(set (reg:SI 2 v0) (const_int -67174400))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  hooks.ApplyCompilationProfile(o2_prof);
  ProductionFillEagerDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillEagerDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted_o2.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;

  // Negative check: under `-O1` (`!RunsSched1`), relocation does not fire.
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_o1 = InvertFillEagerDelaySlots(cfg, o1_prof);
  ASSERT_EQ(lifted_o1.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted_o1.rpo_blocks[1].instructions[0], "(set (reg:SI 2 v0) (const_int -67174400))");
}

TEST(InvertFillEagerDelaySlotsTest, RestoreCombineDeadUseForUnfilledEagerBranchClearsStaleRegDead) {
  // Mirrors `combine.c:10831-10846` + `reorg.c:2415-2430, 2613-2654, 3375-3377`:
  // an unfilled conditional branch `B1 -> (target=B3, fall=B2)` whose fallthrough
  // starts with `(set (reg:SI 3 v1) (const_int 1))` can only remain unfilled if
  // `(use (reg:SI 3 v1)) (REG_DEAD v1)` precedes `li $v1, 1` and a def-free path
  // from `B3` back to `B2` propagates `$v1` live at `B3` entry; any stale
  // `(REG_DEAD (reg:SI 3 v1))` on `B2`'s `slt` must also be stripped so
  // `mark_target_live_regs` does not kill `$v1` at `B3`'s `CODE_LABEL`.
  auto make_loop_cfg = [&](bool target_kills_v1) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> b3_insns;
    if (target_kills_v1) {
      b3_insns.push_back("(set (reg:SI 3 v1) (const_int 9))");
    }
    b3_insns.push_back("(set (reg:SI 7 a3) (plus:SI (reg:SI 7 a3) (const_int 1)))");
    b3_insns.push_back(
        "(set (pc) (if_then_else (ne:SI (reg:SI 7 a3) (const_int 0)) (label_ref L0) (pc)))");
    cfg.rpo_blocks = {
        MakeBlock(0, true,
                  {
                      "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 3 v1) (const_int 1))",
                      "(set (reg:SI 2 v0) (lt:SI (reg:SI 3 v1) (reg:SI 6 a2))) "
                      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) "
                      "(label_ref L0) (pc))) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                  },
                  {0, 2}),
        MakeBlock(2, true, std::move(b3_insns), {0, 3}),
        MakeBlock(
            3, false,
            {
                "(set (reg/i:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(use (reg/i:SI 2 v0))",
                "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
            },
            {}),
    };
    return cfg;
  };

  const CompilationProfile mips1_o2{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-mcpu=r3000"}};

  // Positive: def-free path `B2 -> B0 -> B1` allows `(use (reg:SI 3 v1))` to
  // propagate `$v1` live at `B2` entry and strips stale `REG_DEAD v1` from `slt`.
  const RtlControlFlowGraph pos_cfg = make_loop_cfg(/*target_kills_v1=*/false);
  const RtlControlFlowGraph lifted_pos = InvertFillEagerDelaySlots(pos_cfg, mips1_o2);
  ASSERT_EQ(lifted_pos.rpo_blocks.size(), 4u);
  ASSERT_GE(lifted_pos.rpo_blocks[1].instructions.size(), 4u) << lifted_pos.FormatSummary();
  EXPECT_EQ(lifted_pos.rpo_blocks[1].instructions[0],
            "(use (reg:SI 3 v1)) (expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  EXPECT_EQ(lifted_pos.rpo_blocks[1].instructions[2].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos)
      << lifted_pos.FormatSummary();
  ASSERT_TRUE(lifted_pos.sideband.basic_block_live_at_start.contains(2));
  EXPECT_TRUE(lifted_pos.sideband.basic_block_live_at_start.at(2).contains(3));

  // Negative: when target block `B2` kills `$v1`, no def-free path reaches `B1`,
  // so no `(use (reg:SI 3 v1))` is inserted and `slt` retains `(REG_DEAD v1)`.
  const RtlControlFlowGraph neg_cfg = make_loop_cfg(/*target_kills_v1=*/true);
  const RtlControlFlowGraph lifted_neg = InvertFillEagerDelaySlots(neg_cfg, mips1_o2);
  ASSERT_EQ(lifted_neg.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted_neg.rpo_blocks[1].instructions[0], "(set (reg:SI 3 v1) (const_int 1))");
  EXPECT_NE(lifted_neg.rpo_blocks[1].instructions[1].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos)
      << lifted_neg.FormatSummary();
}

}  // namespace
}  // namespace rom_nom_nom::lifter
