#include "lifter/invert_fill_simple_delay_slots.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
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

  RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x,
                                                 const CompilationProfile& profile) const override {
    return ::rom_nom_nom::lifter::InvertFillSimpleDelaySlots(dbr_simple_cfg_x, profile);
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
  blk.SyncRtlInsnsFromInstructions();
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
  call_cfg.rpo_blocks[0].rtl_insns[0].has_explicit_call_fusage = true;
  call_cfg.rpo_blocks[0].rtl_insns[0].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
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
  jump_after_call_cfg.rpo_blocks[0].rtl_insns[0].has_explicit_call_fusage = true;
  jump_after_call_cfg.rpo_blocks[0].rtl_insns[0].call_fusage.clear();
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
            "(set (reg:SI 2 v0) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");

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

TEST(InvertFillSimpleDelaySlotsUnitTest,
     UnpacksDelaySlotBeforeSkippedMips1MemoryLoadWhenFallthroughPredEndsWithSameSet) {
  // On `-mips1`, memory loads cannot occupy a branch delay slot, so `fill_simple_delay_slots`
  // skips `(set (reg:SI 4 a0) (sign_extend:SI (mem:HI ...)))` and pulls `(set (reg:SI 5 a1)
  // (const_int 0))` from BEFORE the load into `j L0`'s delay slot. When `target_b - 1` (`B1`)
  // falls through to `target_b` (`B2`) and also ends with `(set (reg:SI 5 a1) (const_int 0))`,
  // unpacking `(set (reg:SI 5 a1) (const_int 0))` BEFORE the load in `B0` avoids creating a false
  // cross-jump suffix match with `B1` while re-filling identically under `-mips1` `reorg.c`.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg:SI 4 a0) (sign_extend:SI (mem:HI (plus:SI (reg:SI 4 a0) (const_int "
                 "4)))))",
                 "(sequence [(jump_insn (set (pc) (label_ref L1))) "
                 "(insn (set (reg:SI 5 a1) (const_int 0)))])"},
                {2}),
      MakeBlock(1, true,
                {"(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                 "(set (reg:SI 5 a1) (const_int 0))"},
                {2}),
      MakeBlock(2, true,
                {"(use (reg/i:SI 2 v0))",
                 "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                 "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1))))])"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0], "(set (reg:SI 5 a1) (const_int 0))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 4 a0) (sign_extend:SI (mem:HI (plus:SI (reg:SI 4 a0) (const_int 4)))))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2], "(set (pc) (label_ref L1))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RtlControlFlowGraph dummy;
  hooks.ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, dummy, {}, 2,
                                   {"-O2", "-mips1", "-Wa,-O2"});
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
  bad_succ_cfg.rpo_blocks.push_back(
      MakeBlock(0, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {5}));
  EXPECT_TRUE(InvertFillSimpleDelaySlots(bad_succ_cfg).rpo_blocks.empty());

  // 4. Out-of-bounds addr_vec_targets index.
  RtlControlFlowGraph bad_addr_vec_cfg;
  RtlBasicBlock bad_addr_blk =
      MakeBlock(0, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {});
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
  bad_seq_cfg.rpo_blocks.push_back(MakeBlock(
      0, false, {"(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] ))])"}, {}));
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

TEST(InvertFillSimpleDelaySlotsUnitTest,
     PreservesVariadicCallerHomeStoresAndFoldsWitnessedHiLoDiMove) {
  // 1. Pre-`$sp`-decrement variadic caller-home `sw $4, 0($sp); sw $5, 4($sp)` stores must remain
  // distinct `SImode` stores (`mips_expand_prologue` `store_args_on_stack`) even when `DImode` is
  // witnessed in the function, whereas a non-div/mod `mflo`/`mfhi` (`64..65 -> 2..3`) move pair is
  // folded into `(set (reg:DI 2 v0) (reg:DI 64 hi))`.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29, 31};
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (mem:SI (reg:SI 29 sp)) (reg:SI 4 a0))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 5 a1))",
                    "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 16)))",
                    "(set (reg:SI 64 hi) (reg:SI 4 a0)) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg:SI 65 lo) (reg:SI 5 a1)) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(set (reg/i:SI 2 v0) (reg:SI 64 hi))",
                    "(set (reg/i:SI 3 v1) (reg:SI 65 lo))",
                    "(use (reg/i:DI 2 v0))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 16)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph out = InvertFillSimpleDelaySlots(cfg);
  ASSERT_EQ(out.rpo_blocks.size(), 1u);
  ASSERT_GE(out.rpo_blocks[0].instructions.size(), 8u);
  EXPECT_EQ(out.rpo_blocks[0].instructions[0], "(set (mem:SI (reg:SI 29 sp)) (reg:SI 4 a0))");
  EXPECT_EQ(out.rpo_blocks[0].instructions[1],
            "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 5 a1))");

  bool saw_folded_hilo_to_v0 = false;
  for (const std::string& pat_str : out.rpo_blocks[0].instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    ASSERT_TRUE(parsed.ok());
    if (parsed->pattern.ToGccPrintRtlString() == "(set (reg/i:DI 2 v0) (reg:DI 64 hi))") {
      saw_folded_hilo_to_v0 = true;
    }
  }
  EXPECT_TRUE(saw_folded_hilo_to_v0);
}

}  // namespace
}  // namespace rom_nom_nom::lifter
