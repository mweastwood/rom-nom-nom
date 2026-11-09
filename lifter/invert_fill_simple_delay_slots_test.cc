#include "lifter/invert_fill_simple_delay_slots.h"

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

class ProductionFillSimpleDelaySlotsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertFillSimpleDelaySlots(
      const RtlControlFlowGraph& dbr_simple_cfg_x) const override {
    return ::rom_nom_nom::lifter::InvertFillSimpleDelaySlots(dbr_simple_cfg_x);
  }
};

class FillSimpleDelaySlotsCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionFillSimpleDelaySlotsProvider();
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
  static ProductionFillSimpleDelaySlotsProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* FillSimpleDelaySlotsCorpusTest::ctx = nullptr;
FunctionHooks* FillSimpleDelaySlotsCorpusTest::hooks = nullptr;
ProductionFillSimpleDelaySlotsProvider* FillSimpleDelaySlotsCorpusTest::provider = nullptr;
RightInverseVerifier* FillSimpleDelaySlotsCorpusTest::verifier = nullptr;

TEST_P(FillSimpleDelaySlotsCorpusTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  bool found_snapshot = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kFillSimpleDelaySlots) {
      continue;
    }
    found_snapshot = true;
    const RightInverseResult res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "kFillSimpleDelaySlots boundary mismatch for " << tc.test_name
                             << ": " << res.diff_detail << "\npre_state:\n"
                             << snap.pre_state.FormatSummary() << "\nlifted:\n"
                             << InvertFillSimpleDelaySlots(snap.post_state).FormatSummary()
                             << "\nexpected:\n"
                             << res.expected_summary << "\nactual:\n"
                             << res.actual_summary;
  }
  EXPECT_TRUE(found_snapshot) << "Missing kFillSimpleDelaySlots snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, FillSimpleDelaySlotsCorpusTest,
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

void EnsureMips2TargetConfigured(FunctionHooks* hooks) {
  RtlControlFlowGraph dummy;
  hooks->ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, dummy, {}, 2,
                                    {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
}

TEST(InvertFillSimpleDelaySlotsUnitTest, UnpacksJumpDelaySlotAndVerifiesRightInverse) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (const_int 7)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) (insn (set "
                 "(reg:SI 2 v0) (const_int 1)))])"},
                {}),
      MakeBlock(2, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(cfg);
  EXPECT_EQ(lifted.pass, RtlPassKind::kJump2);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0], "(set (reg:SI 2 v0) (const_int 7))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0], "(set (reg:SI 2 v0) (const_int 1))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[1], "(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillSimpleDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillSimpleDelaySlotsUnitTest, UnpacksCallAndJumpDelaySlotsAndVerifiesRightInverse) {
  RtlControlFlowGraph call_cfg;
  call_cfg.pass = RtlPassKind::kDbr;
  call_cfg.rpo_blocks = {
      MakeBlock(
          0, false,
          {"(sequence [(call_insn (parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI "
           "(\"helper\"))) (const_int 0))) (clobber (reg:SI 31 ra)) ] )) (insn (set (reg:SI 4 "
           "a0) (const_int 42)))])",
           "(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
          {}),
  };

  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(call_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0], "(set (reg:SI 4 a0) (const_int 42))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"helper\"))) (const_int "
            "0))) (clobber (reg:SI 31 ra)) ] )");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  SubfunctionBoundarySnapshot snap;
  snap.subfunction_id = SubfunctionId::kFillSimpleDelaySlots;
  snap.optimize_level = 2;
  snap.c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  snap.pre_state = lifted;
  snap.post_state = call_cfg;
  const RightInverseResult boundary_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;

  // Also verify via `VerifyFillSimpleDelaySlotsRightInverse` on a jump sequence
  // following a call in the same block.
  RtlControlFlowGraph jump_after_call_cfg;
  jump_after_call_cfg.pass = RtlPassKind::kDbr;
  jump_after_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"helper\"))) "
                 "(const_int 0))) (clobber (reg:SI 31 ra)) ] )",
                 "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) (insn (set "
                 "(reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1))))])"},
                {}),
  };
  const RightInverseResult jump_res =
      verifier.VerifyFillSimpleDelaySlotsRightInverse(jump_after_call_cfg);
  EXPECT_TRUE(jump_res.matches) << jump_res.diff_detail;
}

TEST(InvertFillSimpleDelaySlotsUnitTest, PreservesSlotRegDeadNotesAndVerifiesRightInverse) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (reg:SI 5 a1)) "
                 "(expr_list:REG_DEAD (const_int 0) (nil)))])"},
                {2, 1}),
      MakeBlock(1, false, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
      MakeBlock(2, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 2 v0) (reg:SI 5 a1)) (expr_list:REG_DEAD (const_int 0) (nil))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillSimpleDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillSimpleDelaySlotsUnitTest, OptimizeSkipReconstructsFallthroughBlock) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))",
                 "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 1)))])"},
                {1, 1}),
      MakeBlock(1, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(),
            "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  EXPECT_EQ(lifted.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 1u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0], "(set (reg:SI 2 v0) (const_int 1))");
  EXPECT_EQ(lifted.rpo_blocks[1].successors, (std::vector<size_t>{2}));
  EXPECT_TRUE(lifted.rpo_blocks[2].has_label);

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillSimpleDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillSimpleDelaySlotsUnitTest, OptimizeSkipOverUnconditionalJumpToSameTarget) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))",
                 "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L1) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 9)))])"},
                {3, 1}),
      MakeBlock(1, false, {"(note NOTE_INSN_LOOP_CONT)", "(set (pc) (label_ref L1))"}, {3}),
      MakeBlock(2, true,
                {"(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) (insn (set "
                 "(reg:SI 2 v0) (const_int 2)))])"},
                {}),
      MakeBlock(3, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions.back(),
            "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))");
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0], "(note NOTE_INSN_LOOP_CONT)");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[1], "(set (reg:SI 2 v0) (const_int 9))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[2], "(set (pc) (label_ref L1))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillSimpleDelaySlotsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nactual:\n"
                           << res.actual_summary;
}

TEST(InvertFillSimpleDelaySlotsUnitTest, RejectsInvalidAndMalformedCfgs) {
  // 1. Empty CFG.
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertFillSimpleDelaySlots(empty_cfg).rpo_blocks.empty());

  // 2. Single block with empty instructions.
  RtlControlFlowGraph empty_blk_cfg;
  empty_blk_cfg.rpo_blocks.push_back(MakeBlock(0, false, {}, {}));
  EXPECT_TRUE(InvertFillSimpleDelaySlots(empty_blk_cfg).rpo_blocks.empty());

  // 3. Out-of-bounds successor index.
  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(return)"}, {5}));
  EXPECT_TRUE(InvertFillSimpleDelaySlots(bad_succ_cfg).rpo_blocks.empty());

  // 4. Out-of-bounds addr_vec_targets index.
  RtlControlFlowGraph bad_addr_vec_cfg;
  RtlBasicBlock bad_addr_blk = MakeBlock(0, false, {"(return)"}, {});
  bad_addr_blk.addr_vec_targets = {3};
  bad_addr_vec_cfg.rpo_blocks.push_back(bad_addr_blk);
  EXPECT_TRUE(InvertFillSimpleDelaySlots(bad_addr_vec_cfg).rpo_blocks.empty());

  // 5. Unparseable S-expression.
  RtlControlFlowGraph bad_sexpr_cfg;
  bad_sexpr_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(set (reg:SI 2 v0)"}, {}));
  EXPECT_TRUE(InvertFillSimpleDelaySlots(bad_sexpr_cfg).rpo_blocks.empty());

  // 6. Unknown RTL operator.
  RtlControlFlowGraph unknown_op_cfg;
  unknown_op_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(not_an_rtl_op (reg:SI 2 v0))"}, {}));
  EXPECT_TRUE(InvertFillSimpleDelaySlots(unknown_op_cfg).rpo_blocks.empty());

  // 7. Malformed sequence with fewer than 2 children.
  RtlControlFlowGraph bad_seq_cfg;
  bad_seq_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(sequence [(jump_insn (return))])"}, {}));
  EXPECT_TRUE(InvertFillSimpleDelaySlots(bad_seq_cfg).rpo_blocks.empty());
}

TEST(InvertFillSimpleDelaySlotsUnitTest, VerifierRejectsCorruptedInverse) {
  class BrokenFillSimpleDelaySlotsProvider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertFillSimpleDelaySlots(
        const RtlControlFlowGraph& dbr_simple_cfg_x) const override {
      RtlControlFlowGraph corrupted = dbr_simple_cfg_x;
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
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (const_int 7)))])"},
                {2, 1}),
      MakeBlock(
          1, false,
          {"(set (reg:SI 2 v0) (const_int 1))", "(parallel[ (return) (use (reg:SI 31 ra)) ])"}, {}),
      MakeBlock(2, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ])"},
                {}),
  };

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  BrokenFillSimpleDelaySlotsProvider broken_provider;
  RightInverseVerifier verifier(&ctx, &hooks, &broken_provider);
  const RightInverseResult res = verifier.VerifyFillSimpleDelaySlotsRightInverse(cfg);
  EXPECT_FALSE(res.matches);
}

}  // namespace
}  // namespace rom_nom_nom::lifter
