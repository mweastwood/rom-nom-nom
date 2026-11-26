#include "lifter/invert_fill_simple_delay_slots.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
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
  const absl::StatusOr<const gcc_harness::FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const gcc_harness::FunctionCompilationTrace& trace = **cached_trace;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

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

TEST(InvertFillSimpleDelaySlotsUnitTest, CoalescesSplitAndMultiBatchMovstrsiBlockMoves) {
  // Stage 0/1 reconstructs split `movstrsi_internal2` (`kind=1`, 16B) + `movstrsi_internal3`
  // (`kind=2`, 16B) and adjacent multi-batch `movstrsi_internal` (`kind=0`, 8B at `+16`) back into
  // a single canonical 24B `movstrsi_internal` `PARALLEL` so `fill_simple_delay_slots` / `final`
  // replays identically.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29, 31};
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(parallel[ (set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
                    "(clobber (reg:SI 8 t0)) (clobber (reg:SI 9 t1)) "
                    "(clobber (reg:SI 10 t2)) (clobber (reg:SI 11 t3)) "
                    "(use (const_int 16)) (use (const_int 4)) (use (const_int 1)) ] )",
                    "(parallel[ (set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
                    "(clobber (reg:SI 8 t0)) (clobber (reg:SI 9 t1)) "
                    "(clobber (reg:SI 10 t2)) (clobber (reg:SI 11 t3)) "
                    "(use (const_int 16)) (use (const_int 4)) (use (const_int 2)) ] )",
                    "(parallel[ (set (mem:BLK (plus:SI (reg:SI 4 a0) (const_int 16))) "
                    "(mem:BLK (plus:SI (reg:SI 5 a1) (const_int 16)))) "
                    "(clobber (reg:SI 8 t0)) (clobber (reg:SI 9 t1)) "
                    "(clobber (reg:SI 2 v0)) (clobber (reg:SI 2 v0)) "
                    "(use (const_int 8)) (use (const_int 4)) (use (const_int 0)) ] )",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const CompilationProfile profile{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(cfg, profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 2u) << lifted.FormatSummary();
  const auto merged_pn = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(merged_pn.ok()) << merged_pn.status();
  ASSERT_EQ(merged_pn->pattern.op, RtlOp::kParallel);
  ASSERT_EQ(merged_pn->pattern.children.size(), 8u);
  EXPECT_EQ(merged_pn->pattern.children[5].children[0].IntVal(), 24);
  EXPECT_EQ(merged_pn->pattern.children[6].children[0].IntVal(), 4);
  EXPECT_EQ(merged_pn->pattern.children[7].children[0].IntVal(), 0);

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  hooks.ApplyCompilationProfile(profile);
  ctx.BeginFunction("harness_func");
  ctx.MaterializeFromCfg(lifted);
  std::string bad_insn;
  EXPECT_TRUE(hooks.ValidateMaterializedInsnsRecognizable(&bad_insn)) << bad_insn;

  // Also verify right-inverse replay on a real `kFillSimpleDelaySlots` post-state (`24B` unsplit
  // `movstrsi_internal` at `-O2 -mips2`).
  const SubfunctionBoundarySnapshot simple_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kFillSimpleDelaySlots, lifted, {}, profile.optimize_level, profile.c_flags);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(simple_snap);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

class CorruptedFillSimpleDelaySlotsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertFillSimpleDelaySlots(
      const RtlControlFlowGraph& dbr_simple_cfg_x) const override {
    return Corrupt(dbr_simple_cfg_x);
  }

  RtlControlFlowGraph InvertFillSimpleDelaySlots(
      const RtlControlFlowGraph& dbr_simple_cfg_x,
      const CompilationProfile& /*profile*/) const override {
    return Corrupt(dbr_simple_cfg_x);
  }

 private:
  static RtlControlFlowGraph Corrupt(const RtlControlFlowGraph& cfg) {
    RtlControlFlowGraph out = cfg;
    out.pass = RtlPassKind::kJump2;
    if (!out.rpo_blocks.empty()) {
      out.rpo_blocks[0].instructions = {
          "(set (reg:SI 2 v0) (const_int 777))",
          "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
      };
      out.rpo_blocks[0].successors.clear();
      out.rpo_blocks.resize(1);
      out.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }
    return out;
  }
};

TEST(InvertFillSimpleDelaySlotsUnitTest,
     DirectRightInverseVerifierAcceptsValidAndRejectsCorrupted) {
  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kJump2;
  pre_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const CompilationProfile profile{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  const SubfunctionBoundarySnapshot simple_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kFillSimpleDelaySlots, pre_cfg, {}, profile.optimize_level, profile.c_flags);

  ProductionFillSimpleDelaySlotsProvider good_provider;
  RightInverseVerifier good_verifier(&ctx, &hooks, &good_provider);
  const RightInverseResult good_direct =
      good_verifier.VerifyFillSimpleDelaySlotsRightInverse(simple_snap.post_state);
  EXPECT_TRUE(good_direct.matches) << good_direct.diff_detail;
  const RightInverseResult good_snap =
      good_verifier.VerifySubfunctionBoundaryRightInverse(simple_snap);
  EXPECT_TRUE(good_snap.matches) << good_snap.diff_detail;

  CorruptedFillSimpleDelaySlotsProvider bad_provider;
  RightInverseVerifier bad_verifier(&ctx, &hooks, &bad_provider);
  const RightInverseResult bad_direct =
      bad_verifier.VerifyFillSimpleDelaySlotsRightInverse(simple_snap.post_state);
  EXPECT_FALSE(bad_direct.matches);
  EXPECT_FALSE(bad_direct.diff_detail.empty());
  const RightInverseResult bad_snap =
      bad_verifier.VerifySubfunctionBoundaryRightInverse(simple_snap);
  EXPECT_FALSE(bad_snap.matches);
  EXPECT_FALSE(bad_snap.diff_detail.empty());
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     O1FoldedDelaySlotDiArithNormalizesEarlyclobberAndReturnRmwDeadNotes) {
  // At `-O1 -mips2` (`!RunsSched2()`), when `InvertFillSimpleDelaySlots` unpacks
  // the trailing instruction of a 4-insn `adddi3` out of a `return` `SEQUENCE`
  // and `RecombineReorgSplitIdioms` folds `adddi3`, `NormalizePostReloadHardRegNotes`
  // must:
  //   1. Strip `REG_DEAD (reg:DI 10 t2)` from the Stage-0-folded `"=&d"`
  //      earlyclobber output reload `(set (reg:DI 2 v0) (reg:DI 10 t2))`; and
  //   2. Preserve `REG_DEAD (reg:DI 2 v0)` on the split return-value RMW
  //      `(parallel [(set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 6 a2))) ...])`.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 6, 7, 29, 31};
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                    "(set (reg:SI 2 v0) (and:SI (reg:SI 2 v0) (const_int 63)))",
                    "(parallel[ (set (reg:DI 10 t2) (ashiftrt:DI (reg:DI 4 a0) (reg:SI 2 v0))) "
                    "(clobber (reg:SI 8 t0)) ] ) (expr_list:REG_DEAD (reg:SI 2 v0) "
                    "(expr_list:REG_UNUSED (reg:SI 8 t0) (nil)))",
                    "(set (reg:DI 2 v0) (reg:DI 10 t2)) (expr_list:REG_DEAD (reg:DI 10 t2) (nil))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 7 a3)))",
                    "(set (reg:SI 4 a0) (ltu:SI (reg:SI 3 v1) (reg:SI 7 a3)))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 6 a2)))",
                    "(use (reg/i:DI 2 v0))",
                    "(sequence[ (jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
                    "(expr_list:REG_DEAD (const_int 0) (nil))) ] )",
                },
                {}),
  };

  const CompilationProfile o1_profile{.optimize_level = 1, .c_flags = {"-O1", "-mips2"}};
  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(dbr_cfg, o1_profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 7u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[3], "(set (reg:DI 2 v0) (reg:DI 10 t2))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[4],
            "(parallel[ (set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 6 a2))) "
            "(clobber (reg:SI 4 a0)) ] ) (expr_list:REG_DEAD (reg:DI 2 v0) "
            "(expr_list:REG_DEAD (reg:DI 6 a2) "
            "(expr_list:REG_UNUSED (reg:SI 4 a0) (nil))))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  const SubfunctionBoundarySnapshot simple_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, lifted, {},
                                       o1_profile.optimize_level, o1_profile.c_flags);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(simple_snap);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     O1CallDelaySlotArgSetupHoistsBeforeMultiInsnHigherArgMove) {
  // At `-O1` (`!RunsSched1() && !RunsSched2()`), `expand_call` emits outgoing argument setups in
  // ascending ABI slot order (`$a0`, then 2-insn `DImode` `$a2/$a3`), and `fill_simple_delay_slots`
  // skips the 2-insn `DImode` move (`insn_length > 4`) to pull the 1-insn `$a0` setup into `jal`'s
  // delay slot. Verify `InvertFillSimpleDelaySlots` hoists `$a0` back ahead of `$a2:DI` and
  // satisfies `VerifySubfunctionBoundaryRightInverse`.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.sideband.basic_block_live_at_start[0] = {16, 18, 19, 29, 31};
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:DI 6 a2) (reg:DI 18 s2))",
                    "(sequence[ (call_insn (parallel[ (set (reg:SI 2 v0) "
                    "(call (mem:SI (symbol_ref:SI \"callee\")) (const_int 16))) "
                    "(clobber (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg:SI 4 a0) (reg:SI 16 s0))) ] )",
                    "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  dbr_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  dbr_cfg.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage = true;
  auto u0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  auto u1 = ParsePatternAndNotes("(use (reg:DI 6 a2))");
  ASSERT_TRUE(u0.ok() && u1.ok());
  dbr_cfg.rpo_blocks[0].rtl_insns[1].call_fusage = {u0->pattern, u1->pattern};

  const CompilationProfile o1_profile{.optimize_level = 1, .c_flags = {"-O1", "-mips2"}};
  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(dbr_cfg, o1_profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 6u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0], "(set (reg:SI 4 a0) (reg:SI 16 s0))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1], "(set (reg:DI 6 a2) (reg:DI 18 s2))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  const SubfunctionBoundarySnapshot simple_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, lifted, {},
                                       o1_profile.optimize_level, o1_profile.c_flags);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(simple_snap);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillSimpleDelaySlotsUnitTest, ConditionalEpilogueDelayUnpackingEligibilityRightInverse) {
  const CompilationProfile o2_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  // 1. Eligible ALU `epilogue_delay_pattern` before trailing `(use (reg/i:SI 2 v0))`:
  //    `InvertFillSimpleDelaySlots` unpacks it ahead of `(use ...)`, clears
  //    `sideband.epilogue_delay_pattern`, resolves `REG_DEAD (reg:SI 4 a0)`, and satisfies
  //    `VerifySubfunctionBoundaryRightInverse`.
  RtlControlFlowGraph elig_cfg;
  elig_cfg.pass = RtlPassKind::kDbr;
  elig_cfg.sideband.frame_size_bytes = 24;
  elig_cfg.sideband.saved_gpr_regnos = {31};
  elig_cfg.sideband.epilogue_delay_pattern =
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))";
  elig_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -24)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 31 ra))",
                    "(use (reg/i:SI 2 v0))",
                },
                {}),
  };

  const RtlControlFlowGraph lifted_elig = InvertFillSimpleDelaySlots(elig_cfg, o2_profile);
  EXPECT_FALSE(lifted_elig.sideband.epilogue_delay_pattern.has_value());
  ASSERT_EQ(lifted_elig.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted_elig.rpo_blocks[0].instructions.size(), 4u) << lifted_elig.FormatSummary();
  EXPECT_EQ(lifted_elig.rpo_blocks[0].instructions[2],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  EXPECT_EQ(lifted_elig.rpo_blocks[0].instructions[3], "(use (reg/i:SI 2 v0))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  RtlControlFlowGraph pre_with_ret = lifted_elig;
  pre_with_ret.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  const SubfunctionBoundarySnapshot simple_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, pre_with_ret, {},
                                       o2_profile.optimize_level, o2_profile.c_flags);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  EXPECT_TRUE(verifier.VerifyFillSimpleDelaySlotsRightInverse(simple_snap.post_state).matches);
  EXPECT_TRUE(verifier.VerifySubfunctionBoundaryRightInverse(simple_snap).matches);

  // 2. Ineligible `MEM`-load or saved-register `epilogue_delay_pattern` stays in
  //    `sideband.epilogue_delay_pattern` without being unpacked into `rpo_blocks.back()`.
  RtlControlFlowGraph mem_ep_cfg = elig_cfg;
  mem_ep_cfg.sideband.epilogue_delay_pattern = "(set (reg/i:SI 2 v0) (mem:SI (reg:SI 4 a0)))";
  const RtlControlFlowGraph lifted_mem_ep = InvertFillSimpleDelaySlots(mem_ep_cfg, o2_profile);
  EXPECT_TRUE(lifted_mem_ep.sideband.epilogue_delay_pattern.has_value());
  EXPECT_EQ(lifted_mem_ep.rpo_blocks[0].instructions.size(), 3u);

  RtlControlFlowGraph saved_ep_cfg = elig_cfg;
  saved_ep_cfg.sideband.saved_gpr_regnos = {16, 31};
  saved_ep_cfg.sideband.epilogue_delay_pattern = "(set (reg/i:SI 2 v0) (reg:SI 16 s0))";
  const RtlControlFlowGraph lifted_saved_ep = InvertFillSimpleDelaySlots(saved_ep_cfg, o2_profile);
  EXPECT_TRUE(lifted_saved_ep.sideband.epilogue_delay_pattern.has_value());
  EXPECT_EQ(lifted_saved_ep.rpo_blocks[0].instructions.size(), 3u);
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     SelectsSched1AchievableDelaySlotPositionForLoopLatchBranch) {
  // Post-`reorg` loop latch where `v0 += 1` (multi-set across B0 & B1, `birthing_insn_p == false`)
  // occupies `bne t0`'s delay slot, while `t0 = a0 << 1` (single-set, `birthing_insn_p == true`)
  // feeds the branch condition. Under `-O2`, `[t0 = a0 << 1; v0 += 1; bne]` is outside
  // `Im(F_sched1)`, so Stage 1 selects `[v0 += 1; t0 = a0 << 1; bne]` in
  // `Im(F_sched1) ∩ Im(F_sched2)` while preserving exact `reorg.c:2901-2955` right-inverse
  // replay. Under `-fno-schedule-insns`, the baseline unpack position is preserved untouched.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:SI 2 v0) (const_int 0))"}, {1}),
      MakeBlock(1, true,
                {
                    "(set (reg:SI 8 t0) (ashift:SI (reg:SI 4 a0) (const_int 1)))",
                    "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 8 t0) "
                    "(const_int 0)) (label_ref L0) (pc)))) "
                    "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1))))])",
                },
                {1, 2}),
      MakeBlock(2, false,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const CompilationProfile o2_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_o2 = InvertFillSimpleDelaySlots(dbr_cfg, o2_profile);
  ASSERT_EQ(lifted_o2.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted_o2.rpo_blocks[1].instructions.size(), 3u);
  EXPECT_EQ(lifted_o2.rpo_blocks[1].instructions[0],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
  EXPECT_EQ(lifted_o2.rpo_blocks[1].instructions[1],
            "(set (reg:SI 8 t0) (ashift:SI (reg:SI 4 a0) (const_int 1)))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res_o2 = verifier.VerifyFillSimpleDelaySlotsRightInverse(dbr_cfg);
  EXPECT_TRUE(res_o2.matches) << res_o2.diff_detail;

  const CompilationProfile no_s1_profile{
      .optimize_level = 2, .c_flags = {"-O2", "-fno-schedule-insns", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_no_s1 = InvertFillSimpleDelaySlots(dbr_cfg, no_s1_profile);
  ASSERT_EQ(lifted_no_s1.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted_no_s1.rpo_blocks[1].instructions.size(), 3u);
  EXPECT_EQ(lifted_no_s1.rpo_blocks[1].instructions[0],
            "(set (reg:SI 8 t0) (ashift:SI (reg:SI 4 a0) (const_int 1)))");
  EXPECT_EQ(lifted_no_s1.rpo_blocks[1].instructions[1],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     DoesNotMoveDelaySlotBeforeEligibleIndependentInterveningInsn) {
  // Moving `v0 += 1` to index 0 (before `t1 = a1 + 4`) is the only `sched1`-achievable position
  // (both `t1 = a1 + 4` and `t0 = a0 - 2` are single-set, `birthing_insn_p == true`), but
  // crossing `t1 = a1 + 4` would cause `reorg.c:2901-2955` (`fill_simple_delay_slots`) to pick
  // `t1 = a1 + 4` instead of `v0 += 1` into `bgtz t0`'s delay slot. Gate 1
  // (`CanReorgBackwardScanPickSlotAt`) must reject crossing `t1 = a1 + 4`, keeping `v0 += 1`
  // after `t1 = a1 + 4` and preserving exact `reorg.c` right-inverse replay.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:SI 2 v0) (const_int 0))"}, {1}),
      MakeBlock(1, true,
                {
                    "(set (reg:SI 9 t1) (plus:SI (reg:SI 5 a1) (const_int 4)))",
                    "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int -2)))",
                    "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 8 t0) "
                    "(const_int 0)) (label_ref L0) (pc)))) "
                    "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1))))])",
                },
                {1, 2}),
      MakeBlock(2, false,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 9 t1))))])",
                },
                {}),
  };

  const CompilationProfile o2_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(dbr_cfg, o2_profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0],
            "(set (reg:SI 9 t1) (plus:SI (reg:SI 5 a1) (const_int 4)))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[1],
            "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int -2)))");
  EXPECT_EQ(lifted.rpo_blocks[1].instructions[2],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  EnsureMips2TargetConfigured(&hooks);
  ProductionFillSimpleDelaySlotsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult res = verifier.VerifyFillSimpleDelaySlotsRightInverse(dbr_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     AllowsCrossingSymbolicStoreUnderG0ButBlocksUnderG8SmallDataThreshold) {
  // Under `-G 0` (`small_data_threshold == 0`), a bare `(mem:SI (symbol_ref ...))`
  // store is a 2-instruction `lui`/`sw` macro (`mips.md:79-82, 125-138`), so
  // `reorg.c:fill_simple_delay_slots` skips it and the delay-slot instruction
  // (`v0 += 1`) can unpack ahead of it to match `sched1`; under `-G 8`, the
  // symbolic store may be a 1-instruction `$gp`-relative `sw`, so Gate 1 keeps
  // the delay-slot instruction after it.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:SI 2 v0) (const_int 0))"}, {1}),
      MakeBlock(1, true,
                {
                    "(set (reg:SI 9 t1) (plus:SI (reg:SI 6 a2) (const_int 3)))",
                    "(set (reg:SI 8 t0) (ashift:SI (reg:SI 4 a0) (const_int 1)))",
                    "(set (mem:SI (symbol_ref:SI (\"g_flag\"))) (reg:SI 8 t0))",
                    "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 8 t0) "
                    "(const_int 0)) (label_ref L1) (pc)))) "
                    "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 9 t1))) "
                    "(expr_list:REG_DEAD (reg:SI 9 t1) (nil)))])",
                },
                {3, 2}),
      MakeBlock(2, false,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(3, true,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const CompilationProfile g0_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-G", "0", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_g0 = InvertFillSimpleDelaySlots(dbr_cfg, g0_profile);
  ASSERT_EQ(lifted_g0.rpo_blocks[1].instructions.size(), 5u);
  EXPECT_EQ(lifted_g0.rpo_blocks[1].instructions[1],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 9 t1))) "
            "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))");

  const CompilationProfile g8_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-G", "8", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted_g8 = InvertFillSimpleDelaySlots(dbr_cfg, g8_profile);
  ASSERT_EQ(lifted_g8.rpo_blocks[1].instructions.size(), 5u);
  EXPECT_EQ(lifted_g8.rpo_blocks[1].instructions[2],
            "(set (mem:SI (symbol_ref:SI (\"g_flag\"))) (reg:SI 8 t0))");
  EXPECT_EQ(lifted_g8.rpo_blocks[1].instructions[3],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 9 t1))) "
            "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))");
}

TEST(InvertFillSimpleDelaySlotsUnitTest, UnpacksDelaySlotAndInvertsReorgUpdateRegDeadNotes) {
  const CompilationProfile g0_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-G", "0", "-mips2", "-mcpu=r4000"}};
  const CompilationProfile g8_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-G", "8", "-mips2", "-mcpu=r4000"}};

  // Positive (`-G 0`): `reorg.c:2283-2306` (`update_reg_dead_notes`) moved
  // `REG_DEAD v1` from the ineligible 2-word symbolic store `g_flag` onto the
  // call delay-slot arg setup (`a0 = v1 + t1`) when pulling it past the store.
  // Because `$a0` is also set in B0 (`reg_n_sets[a0] == 2`, `births == 0`),
  // `base_view` has `weight(a0 = v1 + t1) = -2 < weight(sw g_flag) = 0`, so
  // `base_view` is outside `Im(F_sched1)`. `SelectSched1AchievableDelaySlotPositions`
  // moves `a0 = v1 + t1` back ahead of `g_flag` and restores `REG_DEAD v1` onto
  // `g_flag` (giving both `weight = -1`, tied in `Im(F_sched1)`).
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  RtlBasicBlock b0 = MakeBlock(0, false, {"(set (reg:SI 4 a0) (const_int 0))"}, {1});
  RtlBasicBlock b1 =
      MakeBlock(1, true,
                {
                    "(set (reg:SI 9 t1) (plus:SI (reg:SI 6 a2) (const_int 3))) "
                    "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 5))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(set (mem:SI (symbol_ref:SI (\"g_flag\"))) (reg:SI 3 v1))",
                    "(sequence [(call_insn (parallel[ (set (reg:SI 2 v0) (call (mem:SI "
                    "(symbol_ref:SI (\"helper\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg:SI 4 a0) (plus:SI (reg:SI 3 v1) (reg:SI 9 t1))) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) "
                    "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))))])",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {});
  auto use_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  ASSERT_TRUE(use_a0.ok());
  b1.rtl_insns[3].has_explicit_call_fusage = true;
  b1.rtl_insns[3].call_fusage = {std::move(use_a0->pattern)};
  dbr_cfg.rpo_blocks = {std::move(b0), std::move(b1)};

  const RtlControlFlowGraph lifted_g0 = InvertFillSimpleDelaySlots(dbr_cfg, g0_profile);
  ASSERT_EQ(lifted_g0.rpo_blocks[1].instructions.size(), 7u);
  EXPECT_NE(lifted_g0.rpo_blocks[1].instructions[2].find("(set (reg:SI 4 a0)"), std::string::npos)
      << lifted_g0.FormatSummary();
  EXPECT_EQ(lifted_g0.rpo_blocks[1].instructions[2].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos)
      << lifted_g0.FormatSummary();
  EXPECT_NE(lifted_g0.rpo_blocks[1].instructions[3].find("(mem:SI (symbol_ref:SI (\"g_flag\")))"),
            std::string::npos)
      << lifted_g0.FormatSummary();
  EXPECT_NE(lifted_g0.rpo_blocks[1].instructions[3].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos)
      << lifted_g0.FormatSummary();

  // Negative (`-G 8`): under `-G 8`, the symbolic store may be a 1-word `$gp`-relative
  // `sw` eligible for a simple delay slot, so Gate 1 must keep `a0 = v1 + t1` after
  // the store without transferring `REG_DEAD v1`.
  const RtlControlFlowGraph lifted_g8 = InvertFillSimpleDelaySlots(dbr_cfg, g8_profile);
  ASSERT_EQ(lifted_g8.rpo_blocks[1].instructions.size(), 7u);
  EXPECT_NE(lifted_g8.rpo_blocks[1].instructions[2].find("(mem:SI (symbol_ref:SI (\"g_flag\")))"),
            std::string::npos)
      << lifted_g8.FormatSummary();
  EXPECT_EQ(lifted_g8.rpo_blocks[1].instructions[2].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos)
      << lifted_g8.FormatSummary();
  EXPECT_NE(lifted_g8.rpo_blocks[1].instructions[3].find("(set (reg:SI 4 a0)"), std::string::npos)
      << lifted_g8.FormatSummary();
  EXPECT_NE(lifted_g8.rpo_blocks[1].instructions[3].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos)
      << lifted_g8.FormatSummary();
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     RestoresSched2AchievabilityWhenDefaultUnpackViolatesLoadDelay) {
  const CompilationProfile g0_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-G", "0", "-mips2", "-mcpu=r4000"}};
  const CompilationProfile g8_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-G", "8", "-mips2", "-mcpu=r4000"}};

  // Default unpack places `a0 = a1 + 3` at index 3 (after `lw v0; v0 += 7; sw g_flag, v0`),
  // which is outside `Im(F_sched2)` because `sched2` always schedules the independent ALU
  // `a0 = a1 + 3` into the 1-cycle load-use delay slot after `lw v0`.
  // Under `-G 0`, `sw g_flag, v0` is ineligible (2-word symbolic store) and `v0 += 7`
  // feeds `sw g_flag, v0`, so `reorg.c:2901-2955` uniquely picks `a0 = a1 + 3` from index 1,
  // restoring `IsAchievable(kSched2)` via `SelectSched1AchievableDelaySlotPositions`.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  RtlBasicBlock b0 =
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 7)))",
                    "(set (mem:SI (symbol_ref:SI (\"g_flag\"))) (reg:SI 2 v0)) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                    "(sequence [(call_insn (parallel[ (set (reg:SI 2 v0) (call (mem:SI "
                    "(symbol_ref:SI (\"helper\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg:SI 4 a0) (plus:SI (reg:SI 5 a1) (const_int 3))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil)))])",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {});
  auto use_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  ASSERT_TRUE(use_a0.ok());
  b0.rtl_insns[3].has_explicit_call_fusage = true;
  b0.rtl_insns[3].call_fusage = {std::move(use_a0->pattern)};
  dbr_cfg.rpo_blocks = {std::move(b0)};

  // Positive (`-G 0`): `a0 = a1 + 3` moves to index 1 (between `lw v0` and `v0 += 7`),
  // restoring `Im(F_sched2)`.
  const RtlControlFlowGraph lifted_g0 = InvertFillSimpleDelaySlots(dbr_cfg, g0_profile);
  ASSERT_EQ(lifted_g0.rpo_blocks[0].instructions.size(), 7u);
  EXPECT_NE(lifted_g0.rpo_blocks[0].instructions[1].find("(set (reg:SI 4 a0)"), std::string::npos)
      << lifted_g0.FormatSummary();

  // Negative (`-G 8`): `sw g_flag, v0` is a 1-word `$gp`-relative store eligible for
  // the call delay slot, so Gate 1 (`CanReorgBackwardScanPickSlotAt`) must NOT move
  // `a0 = a1 + 3` ahead of `v0 += 7` into the load-delay slot at index 1.
  const RtlControlFlowGraph lifted_g8 = InvertFillSimpleDelaySlots(dbr_cfg, g8_profile);
  ASSERT_EQ(lifted_g8.rpo_blocks[0].instructions.size(), 7u);
  EXPECT_NE(lifted_g8.rpo_blocks[0].instructions[1].find("(plus:SI (reg:SI 2 v0) (const_int 7))"),
            std::string::npos)
      << lifted_g8.FormatSummary();
  EXPECT_NE(lifted_g8.rpo_blocks[0].instructions[2].find("(set (reg:SI 4 a0)"), std::string::npos)
      << lifted_g8.FormatSummary();
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     InvertsJumpTargetStolenDelaySlotsOnlyWhenUniqueAndNotCrossJumped) {
  const CompilationProfile o2_profile{.optimize_level = 2,
                                      .c_flags = {"-O2", "-G", "0", "-mips2", "-mcpu=r4000"}};

  // Build a 4-block diamond (`B0 -> {B1, B2}`, `B1` jumps to `B3` with a delay-slot
  // copy of `S`, `B2` falls through to `B3` ending with `S`, `reorg.c:3097-3139`).
  auto make_diamond = [](const std::string& b1_prev_insn, const std::string& b2_prev_insn) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    RtlBasicBlock b0 = MakeBlock(0, false,
                                 {
                                     "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                                     "(label_ref L2) (pc)))",
                                 },
                                 {1, 2});
    RtlBasicBlock b1 =
        MakeBlock(1, false,
                  {
                      b1_prev_insn,
                      "(sequence [(jump_insn (set (pc) (label_ref L3))) "
                      "(insn (set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))))])",
                  },
                  {3});
    RtlBasicBlock b2 = MakeBlock(2, true,
                                 {
                                     b2_prev_insn,
                                     "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1)))",
                                 },
                                 {3});
    RtlBasicBlock b3 = MakeBlock(3, true,
                                 {
                                     "(use (reg/i:SI 2 v0))",
                                     "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                                 },
                                 {});
    cfg.rpo_blocks = {std::move(b0), std::move(b1), std::move(b2), std::move(b3)};
    return cfg;
  };

  // Positive (`reorg.c:3097-3139`): B1's preceding insn is an ineligible 2-word
  // `-G 0` symbolic store (`g_left`) distinct from B2's preceding store (`g_right`).
  // `InvertSimpleJumpTargetStolenDelaySlots` restores `v0 = a1 + 1` at the head of
  // B3 and removes the duplicated copies from B1's delay slot and B2's tail.
  const RtlControlFlowGraph pos_lifted = InvertFillSimpleDelaySlots(
      make_diamond("(set (mem:SI (symbol_ref:SI (\"g_left\"))) (reg:SI 6 a2))",
                   "(set (mem:SI (symbol_ref:SI (\"g_right\"))) (reg:SI 6 a2))"),
      o2_profile);
  ASSERT_EQ(pos_lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(pos_lifted.rpo_blocks[1].instructions.size(), 2u) << pos_lifted.FormatSummary();
  EXPECT_EQ(pos_lifted.rpo_blocks[2].instructions.size(), 1u) << pos_lifted.FormatSummary();
  ASSERT_GE(pos_lifted.rpo_blocks[3].instructions.size(), 3u) << pos_lifted.FormatSummary();
  EXPECT_NE(pos_lifted.rpo_blocks[3].instructions[0].find("(plus:SI (reg:SI 5 a1) (const_int 1))"),
            std::string::npos)
      << pos_lifted.FormatSummary();

  // Negative 1: B1 has an eligible local delay-slot candidate (`t0 = a2 + 3`), so
  // `reorg.c:fill_simple_delay_slots` would not have stolen `v0 = a1 + 1` from B3.
  const RtlControlFlowGraph neg_elig = InvertFillSimpleDelaySlots(
      make_diamond("(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (const_int 3)))",
                   "(set (mem:SI (symbol_ref:SI (\"g_right\"))) (reg:SI 6 a2))"),
      o2_profile);
  EXPECT_EQ(neg_elig.rpo_blocks[3].instructions.size(), 2u) << neg_elig.FormatSummary();
  EXPECT_EQ(neg_elig.rpo_blocks[3].instructions[0].find("(plus:SI (reg:SI 5 a1) (const_int 1))"),
            std::string::npos)
      << neg_elig.FormatSummary();

  // Negative 2 (`toplev.c:3142` `jump_optimize (insns, 1, 1, 0)` cross-jump guard):
  // B1 and B2 share an identical preceding store (`g_same`), so `v0 = a1 + 1` could
  // not have been at the head of B3 before `reorg.c` without `do_cross_jump` also
  // merging `g_same`.
  const RtlControlFlowGraph neg_xjump = InvertFillSimpleDelaySlots(
      make_diamond("(set (mem:SI (symbol_ref:SI (\"g_same\"))) (reg:SI 6 a2))",
                   "(set (mem:SI (symbol_ref:SI (\"g_same\"))) (reg:SI 6 a2))"),
      o2_profile);
  EXPECT_EQ(neg_xjump.rpo_blocks[3].instructions.size(), 2u) << neg_xjump.FormatSummary();
  EXPECT_EQ(neg_xjump.rpo_blocks[3].instructions[0].find("(plus:SI (reg:SI 5 a1) (const_int 1))"),
            std::string::npos)
      << neg_xjump.FormatSummary();
}

TEST(InvertFillSimpleDelaySlotsUnitTest, TreatsFloatFromIntConversionAsDelaySlotIneligible) {
  const CompilationProfile mips1_o2{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-mcpu=r3000"}};

  // `config/mips/mips.md:2595-2660` (`floatsidf2`/`floatsisf2`) emits
  // `mtc1`/`l.s` + `cvt.*` (`length = 3..4 > 1`), so `mips.md:125-138`
  // (`define_delay`) never accepts it in a delay slot. When an unpacked return
  // delay-slot store `s.s $f0, 0($5)` must slide back before `floatsidf2` to
  // restore `kSched1` achievability, `IsIneligibleForMipsDelaySlot` allows the
  // backward slide across `floatsidf2` (`kFloat`), whereas a 1-insn eligible FP
  // op (`float_truncate`) blocks the backward slide ahead of it.
  auto make_cfg = [](const std::string& intervening_fp_op) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    RtlBasicBlock b0 = MakeBlock(
        0, false,
        {
            "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 4)))",
            intervening_fp_op,
            "(sequence [(jump_insn (set (pc) (label_ref L1))) "
            "(insn (set (mem:SF (reg:SI 5 a1)) (reg:SF 44 $f12)) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SF 44 $f12) (nil))))])",
        },
        {1});
    RtlBasicBlock b1 = MakeBlock(1, true,
                                 {
                                     "(use (reg/i:DF 32 $f0))",
                                     "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                                 },
                                 {});
    cfg.rpo_blocks = {std::move(b0), std::move(b1)};
    return cfg;
  };

  auto idx_of = [](const std::vector<std::string>& insns, std::string_view needle) -> int {
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].find(needle) != std::string::npos) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  // Positive: `floatsidf2` (`(float:DF (reg:SI 7 a3))`) is ineligible for a
  // delay slot, so `SelectSched1AchievableDelaySlotPositions` slides
  // `s.s $f12, 0($5)` back before `floatsidf2` (index 1) to restore `kSched1`
  // achievability (`s.s` kills 2 regs and beats birthing `floatsidf2`).
  const RtlControlFlowGraph pos_lifted =
      InvertFillSimpleDelaySlots(make_cfg("(set (reg:DF 32 $f0) (float:DF (reg:SI 7 a3))) "
                                          "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))"),
                                 mips1_o2);
  ASSERT_EQ(pos_lifted.rpo_blocks.size(), 2u);
  const auto& pos_insns = pos_lifted.rpo_blocks[0].instructions;
  EXPECT_LT(idx_of(pos_insns, "(set (mem:SF (reg:SI 5 a1)) (reg:SF 44 $f12))"),
            idx_of(pos_insns, "(float:DF (reg:SI 7 a3))"))
      << pos_lifted.FormatSummary();

  // Negative contrast: `extendsfdf2` (`float_extend:DF`, 1-insn `cvt.d.s`
  // with `length = 1`) IS eligible for the jump delay slot, so
  // `CanPlaceUnpackedSlotBeforeIntervening` must NOT slide `s.s $f12, 0($5)`
  // ahead of it.
  const RtlControlFlowGraph neg_lifted = InvertFillSimpleDelaySlots(
      make_cfg("(set (reg:DF 32 $f0) (float_extend:DF (reg:SF 46 $f14))) "
               "(expr_list:REG_DEAD (reg:SF 46 $f14) (nil))"),
      mips1_o2);
  ASSERT_EQ(neg_lifted.rpo_blocks.size(), 2u);
  const auto& neg_insns = neg_lifted.rpo_blocks[0].instructions;
  EXPECT_GT(idx_of(neg_insns, "(set (mem:SF (reg:SI 5 a1)) (reg:SF 44 $f12))"),
            idx_of(neg_insns, "(float_extend:DF (reg:SF 46 $f14))"))
      << neg_lifted.FormatSummary();
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     RecombineReorgSplitIdiomsReattributesSched2NotesOnlyWhenSched2Runs) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000"}};

  // Build a block with (a) a pure-reader store `sw $t0, 0($a2)` that `sched2`
  // hoisted past an earlier consumer `t1 = t0 + 1` carrying `(REG_DEAD t0)`,
  // and (b) a delay-slot-split 32-bit `mulsidi3` (`mult` + `mfhi $v0` + return
  // delay-slot `mflo $v1`) so `RecombineReorgSplitIdioms` triggers
  // `folded_multi_insn == true` and rebuilds liveness. Under `-O2`
  // (`toplev.c:3028-3034, 3105-3118`, `sched.c:2779-3018, 3415-3611`),
  // `RecombineReorgSplitIdioms` must re-run `AttributeSched2PureReaderRegDeadNotes`
  // and transfer `(REG_DEAD t0)` back from `t1 = t0 + 1` onto `sw $t0, 0($a2)`
  // (or restore `sched` note ordering); under `-O1` (`!RunsSched2()`), it must
  // leave pure-liveness notes untouched.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  RtlBasicBlock b0 =
      MakeBlock(0, false,
                {
                    "(set (reg:SI 8 t0) (mem:SI (reg:SI 6 a2)))",
                    "(set (reg:SI 9 t1) (mem:SI (reg:SI 7 a3))) "
                    "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 6 a2) (const_int 4))) (reg:SI 8 t0))",
                    "(set (reg:SI 9 t1) (plus:SI (reg:SI 9 t1) (reg:SI 8 t0))) "
                    "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 9 t1)) "
                    "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))",
                    "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI "
                    "(sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1))) "
                    "(const_int 32)))) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 hilo)) ] )",
                    "(set (reg:SI 2 v0) (reg:SI 64 hi))",
                    "(use (reg/i:DI 2 v0))",
                    "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg:SI 3 v1) (reg:SI 65 lo)))])",
                },
                {});
  dbr_cfg.rpo_blocks = {std::move(b0)};

  // Positive (`-O2`): `mulsidi3` folds (`mult:DI` into `reg:DI 64 hi`), AND
  // `CanonicalizeCfgLivenessNoteOrder(/*sched_note_order=*/true)` reorders
  // multi-death notes on `[4]` (`REG_DEAD a2` before `REG_DEAD t1`, matching
  // `sched.c:2779-3018` `attach_deaths` prepending RHS then LHS).
  const RtlControlFlowGraph lifted_o2 = InvertFillSimpleDelaySlots(dbr_cfg, o2_prof);
  ASSERT_EQ(lifted_o2.rpo_blocks.size(), 1u);
  bool folded_o2 = false;
  bool found_store_o2 = false;
  for (const std::string& insn : lifted_o2.rpo_blocks[0].instructions) {
    if (insn.find("(set (reg:DI 64 hi) (mult:DI") != std::string::npos) {
      folded_o2 = true;
    }
    if (insn.find("(set (mem:SI (reg:SI 6 a2)) (reg:SI 9 t1))") != std::string::npos) {
      found_store_o2 = true;
      const size_t pos_t1 = insn.find("REG_DEAD (reg:SI 9 t1)");
      const size_t pos_a2 = insn.find("REG_DEAD (reg:SI 6 a2)");
      ASSERT_NE(pos_t1, std::string::npos) << insn;
      ASSERT_NE(pos_a2, std::string::npos) << insn;
      EXPECT_LT(pos_a2, pos_t1) << insn;
    }
  }
  EXPECT_TRUE(folded_o2) << lifted_o2.FormatSummary();
  EXPECT_TRUE(found_store_o2) << lifted_o2.FormatSummary();

  // Negative (`-O1`): `mulsidi3` still folds, but `!RunsSched1()` keeps flow.c
  // note order (`REG_DEAD t1` before `REG_DEAD a2`) without re-running `sched`
  // note canonicalization.
  const RtlControlFlowGraph lifted_o1 = InvertFillSimpleDelaySlots(dbr_cfg, o1_prof);
  ASSERT_EQ(lifted_o1.rpo_blocks.size(), 1u);
  bool folded_o1 = false;
  bool found_store_o1 = false;
  for (const std::string& insn : lifted_o1.rpo_blocks[0].instructions) {
    if (insn.find("(set (reg:DI 64 hi) (mult:DI") != std::string::npos) {
      folded_o1 = true;
    }
    if (insn.find("(set (mem:SI (reg:SI 6 a2)) (reg:SI 9 t1))") != std::string::npos) {
      found_store_o1 = true;
      const size_t pos_t1 = insn.find("REG_DEAD (reg:SI 9 t1)");
      const size_t pos_a2 = insn.find("REG_DEAD (reg:SI 6 a2)");
      ASSERT_NE(pos_t1, std::string::npos) << insn;
      ASSERT_NE(pos_a2, std::string::npos) << insn;
      EXPECT_LT(pos_t1, pos_a2) << insn;
    }
  }
  EXPECT_TRUE(folded_o1) << lifted_o1.FormatSummary();
  EXPECT_TRUE(found_store_o1) << lifted_o1.FormatSummary();
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     ConditionChainUnpacksCrossJumpedFpConsumerAndCoveredXferSlot) {
  CompilationProfile mips1_prof;
  mips1_prof.optimize_level = 2;
  mips1_prof.c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"};

  CompilationProfile r4k_prof;
  r4k_prof.optimize_level = 2;
  r4k_prof.c_flags = {"-O2", "-mcpu=r4000", "-Wa,-g"};

  // Positive 1 (`-mips1`): `li.s $f2` (`xfer` latency 2) at index 0 is already
  // covered by `addu v1, v0, 17` at index 1, so `add.s $f4, $f0, $f2` steps
  // before the `li v0, 131072 ; sltu v0, v0, a1` condition chain (index 2).
  auto make_join_cfg = [](bool include_li_s, bool include_intervening_addu) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> b3_insns;
    if (include_li_s) {
      b3_insns.push_back("(set (reg:SF 34 $f2) (const_double:SF (cc0) 0 1072693248))");
    }
    if (include_intervening_addu) {
      b3_insns.push_back("(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 17)))");
    }
    b3_insns.push_back("(set (reg:SI 2 v0) (const_int 131072))");
    b3_insns.push_back("(set (reg:SI 2 v0) (ltu:SI (reg:SI 2 v0) (reg:SI 5 a1)))");
    b3_insns.push_back(
        "(sequence[ "
        "(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) "
        "(label_ref L4) (pc)))) "
        "(insn (set (reg:SF 36 $f4) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
        "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))) ] )");
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {"(set (pc) (if_then_else (ge:SI (reg:SI 4 a0) (const_int 0)) "
                   "(label_ref L2) (pc)))"},
                  {2, 1}),
        MakeBlock(1, false,
                  {"(set (reg:SF 32 $f0) (mult:SF (reg:SF 34 $f2) (reg:SF 32 $f0)))",
                   "(set (reg:SI 2 v0) (neg:SI (reg:SI 4 a0)))", "(set (pc) (label_ref L3))"},
                  {3}),
        MakeBlock(2, true,
                  {"(set (reg:SF 32 $f0) (mult:SF (reg:SF 34 $f2) (reg:SF 32 $f0)))",
                   "(set (reg:SI 2 v0) (xor:SI (reg:SI 4 a0) (const_int 1)))"},
                  {3}),
        MakeBlock(3, true, std::move(b3_insns), {5, 4}),
        MakeBlock(4, false, {"(set (reg:SI 2 v0) (const_int 1))", "(set (pc) (label_ref L5))"},
                  {5}),
        MakeBlock(5, true,
                  {"(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                   "(set (reg/i:SF 32 $f0) (reg:SF 36 $f4))", "(use (reg/i:SF 32 $f0))",
                   "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                  {}),
    };
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 5, 32, 34}}, {1, {4, 5, 32, 34}}, {2, {4, 5, 32, 34}},
        {3, {2, 5, 32, 34}}, {4, {3, 36}},        {5, {3, 36}},
    };
    return cfg;
  };

  {
    const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(
        make_join_cfg(/*include_li_s=*/true, /*include_intervening_addu=*/true), mips1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 6u);
    ASSERT_EQ(lifted.rpo_blocks[3].instructions.size(), 6u);
    EXPECT_NE(lifted.rpo_blocks[3].instructions[2].find("(plus:SF (reg:SF 32 $f0)"),
              std::string::npos)
        << lifted.FormatSummary();
  }

  // Positive 2 (R4000): `IsCrossJumpedFpConsumerDelaySlot` preserves the
  // pre-condition-chain placement (`B3[1]`) instead of letting
  // `SelectSched1AchievableDelaySlotPositions` push `add.s` back to `B3[3]`.
  {
    const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(
        make_join_cfg(/*include_li_s=*/false, /*include_intervening_addu=*/true), r4k_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 6u);
    ASSERT_EQ(lifted.rpo_blocks[3].instructions.size(), 5u);
    EXPECT_NE(lifted.rpo_blocks[3].instructions[1].find("(plus:SF (reg:SF 32 $f0)"),
              std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative: when `li.s $f2` is immediately adjacent to the condition chain
  // (no intervening instruction covering the 2-cycle `xfer` latency), `add.s`
  // must NOT step back immediately next to `li.s $f2`.
  {
    const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(
        make_join_cfg(/*include_li_s=*/true, /*include_intervening_addu=*/false), mips1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 6u);
    ASSERT_EQ(lifted.rpo_blocks[3].instructions.size(), 5u);
    EXPECT_NE(lifted.rpo_blocks[3].instructions[3].find("(plus:SF (reg:SF 32 $f0)"),
              std::string::npos)
        << lifted.FormatSummary();
  }
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     ReordersO1DelaySlotAheadOfJumpInputReloadAndSkipsConflictingSlot) {
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  auto make_o1_cfg = [](const std::string& slot_insn) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.basic_block_live_at_start = {{1, {2}}, {2, {2}}};
    RtlBasicBlock b0 = MakeBlock(
        0, false,
        {
            "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
            "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 8 t0) (const_int 0)) "
            "(label_ref L2) (pc))) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))) "
            "(insn " +
                slot_insn + ")])",
        },
        {1, 2});
    RtlBasicBlock b1 = MakeBlock(1, false,
                                 {
                                     "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
                                     "(use (reg/i:SI 2 v0))",
                                     "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                                 },
                                 {});
    RtlBasicBlock b2 = MakeBlock(2, true,
                                 {
                                     "(use (reg/i:SI 2 v0))",
                                     "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                                 },
                                 {});
    cfg.rpo_blocks = {std::move(b0), std::move(b1), std::move(b2)};
    return cfg;
  };

  // Positive (`reload1.c:6583` + `reorg.c:2901-2955`): at `-O1`, an independent
  // constant load `(set (reg:SI 2 v0) (const_int 7))` unpacked from a conditional
  // jump's delay slot swaps ahead of the jump's stack-slot input reload
  // `(set (reg:SI 8 t0) (mem:SI (+ sp 16)))` so the input reload stays adjacent
  // to the jump.
  {
    const RtlControlFlowGraph lifted =
        InvertFillSimpleDelaySlots(make_o1_cfg("(set (reg:SI 2 v0) (const_int 7))"), o1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 3u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(const_int 7)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(mem:SI (plus:SI (reg:SI 29 sp)"),
              std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative: when the delay-slot instruction reads the reloaded register `$8`,
  // swapping ahead of the reload would violate the data dependence, so it stays
  // after the reload.
  {
    const RtlControlFlowGraph lifted =
        InvertFillSimpleDelaySlots(make_o1_cfg("(set (reg:SI 2 v0) (reg:SI 8 t0))"), o1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 3u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(mem:SI (plus:SI (reg:SI 29 sp)"),
              std::string::npos)
        << lifted.FormatSummary();
  }
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     O1ConditionChainDoesNotSplitStackReloadFromComparisonButStopsAtNonStackLoad) {
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  auto make_cfg = [](const std::string& load_insn) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.basic_block_live_at_start = {{1, {3}}, {2, {3}}};
    RtlBasicBlock b0 = MakeBlock(
        0, false,
        {
            load_insn,
            "(set (reg:SI 2 v0) (lt:SI (reg:SI 8 t0) (reg:SI 5 a1))) "
            "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
            "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) "
            "(label_ref L2) (pc))) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))) "
            "(insn (set (reg:SI 3 v1) (const_int 9)))])",
        },
        {1, 2});
    RtlBasicBlock b1 = MakeBlock(1, false,
                                 {
                                     "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                                     "(use (reg/i:SI 2 v0))",
                                     "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                                 },
                                 {});
    RtlBasicBlock b2 = MakeBlock(2, true,
                                 {
                                     "(set (reg/i:SI 2 v0) (reg:SI 3 v1))",
                                     "(use (reg/i:SI 2 v0))",
                                     "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                                 },
                                 {});
    cfg.rpo_blocks = {std::move(b0), std::move(b1), std::move(b2)};
    return cfg;
  };

  // Positive (`-O1`, `!runs_sched2`, `reload1.c:6239`): unpacked delay-slot
  // `(set v1 9)` steps ahead of BOTH `lt:SI` and its adjacent stack-slot input
  // reload `(set t0 (mem:SI (+ sp 16)))` so the reload stays glued to `lt:SI`.
  {
    const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(
        make_cfg("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"), o1_prof);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(const_int 9)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(mem:SI (plus:SI (reg:SI 29 sp)"),
              std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative 1 (non-stack memory load at `-O1`): stops at `lt:SI`, leaving
  // `(mem:SI (reg:SI 4 a0))` at index 0.
  {
    const RtlControlFlowGraph lifted =
        InvertFillSimpleDelaySlots(make_cfg("(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))"), o1_prof);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(mem:SI (reg:SI 4 a0))"),
              std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(const_int 9)"), std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative 2 (`-O2`, `runs_sched2 == true`): stops at `lt:SI` without
  // stepping across the stack load.
  {
    const RtlControlFlowGraph lifted = InvertFillSimpleDelaySlots(
        make_cfg("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"), o2_prof);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(mem:SI (plus:SI (reg:SI 29 sp)"),
              std::string::npos)
        << lifted.FormatSummary();
  }
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     O1CallDelaySlotDoesNotSplitAdjacentFprOutputReloadIntoArgPair) {
  // Mirrors `reload1.c:6237-6246` (`emit_reload_insns`): without `sched2`
  // (`-O1`), an unpacked `a0` call-delay-slot insn must NOT step ahead of an
  // adjacent `DFmode` FPR->GPR output reload `(set (reg:DF 6 a2) (reg:DF 32 $f0))`
  // (which would split it from its producer `plus:DF`), whereas a standalone
  // `DFmode` arg move `(set (reg:DF 6 a2) (reg:DF 52 $f20))` with `REG_DEAD`
  // still reorders to argument order.
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  auto make_call_cfg = [](bool is_output_reload) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> insns;
    if (is_output_reload) {
      insns = {
          "(set (reg:DF 32 $f0) (plus:DF (reg:DF 36 $f4) (reg:DF 52 $f20))) "
          "(expr_list:REG_DEAD (reg:DF 36 $f4) (nil))",
          "(set (reg:DF 6 a2) (reg:DF 32 $f0))",
          "(sequence [(call_insn (parallel[ (set (reg:SI 2 v0) (call (mem:SI "
          "(symbol_ref:SI (\"Helper\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
          "(insn (set (reg:SI 4 a0) (reg:SI 16 s0)))])",
          "(use (reg/i:SI 2 v0))",
          "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
      };
    } else {
      insns = {
          "(set (reg:DF 6 a2) (reg:DF 52 $f20)) (expr_list:REG_DEAD (reg:DF 52 $f20) (nil))",
          "(sequence [(call_insn (parallel[ (set (reg:SI 2 v0) (call (mem:SI "
          "(symbol_ref:SI (\"Helper\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
          "(insn (set (reg:SI 4 a0) (reg:SI 16 s0)))])",
          "(use (reg/i:SI 2 v0))",
          "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
      };
    }
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    return cfg;
  };

  {
    const RtlControlFlowGraph pos =
        InvertFillSimpleDelaySlots(make_call_cfg(/*is_output_reload=*/true), o1_prof);
    ASSERT_GE(pos.rpo_blocks[0].instructions.size(), 4u) << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[0].find("(plus:DF "), std::string::npos)
        << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[1].find("(set (reg:DF 6 a2) (reg:DF 32 $f0))"),
              std::string::npos)
        << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[2].find("(set (reg:SI 4 a0) (reg:SI 16 s0))"),
              std::string::npos)
        << pos.FormatSummary();
  }
  {
    const RtlControlFlowGraph neg =
        InvertFillSimpleDelaySlots(make_call_cfg(/*is_output_reload=*/false), o1_prof);
    ASSERT_GE(neg.rpo_blocks[0].instructions.size(), 3u) << neg.FormatSummary();
    EXPECT_NE(neg.rpo_blocks[0].instructions[0].find("(set (reg:SI 4 a0) (reg:SI 16 s0))"),
              std::string::npos)
        << neg.FormatSummary();
    EXPECT_NE(neg.rpo_blocks[0].instructions[1].find("(set (reg:DF 6 a2) (reg:DF 52 $f20))"),
              std::string::npos)
        << neg.FormatSummary();
  }
}

TEST(InvertFillSimpleDelaySlotsUnitTest,
     HoistsDelaySlotSplitDiAddTailBehindLengthGt1InsnsAndSkipsEligibleIntervening) {
  // Mirrors `reorg.c:2901-2955` (`fill_simple_delay_slots`) + `mips.md:453-480`
  // (`adddi3_internal_1`): when `reorg` splits a 4-word big-endian `DImode` add
  // behind a delay-slot-ineligible `length > 1` `xor:DI` insn and moves its 4th
  // word (`addu dH, dH, tmp`) into a `CALL_INSN` delay slot, hoisting the 4th
  // word back behind `xor:DI` lets `RecombineReorgSplitIdioms` fold all 4 words
  // back into the `adddi3_internal_1` `PARALLEL`.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  auto make_cfg = [](bool include_eligible_intervening) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> insns = {
        "(set (reg:SI 5 a1) (plus:SI (reg:SI 21 s5) (reg:SI 19 s3)))",
        "(set (reg:SI 2 v0) (ltu:SI (reg:SI 5 a1) (reg:SI 21 s5)))",
        "(set (reg:SI 4 a0) (plus:SI (reg:SI 20 s4) (reg:SI 18 s2)))",
        "(set (reg:DI 16 s0) (xor:DI (reg:DI 16 s0) (reg:DI 18 s2)))",
    };
    if (include_eligible_intervening) {
      // Single-word independent `length == 1` insn eligible for the call delay
      // slot: `reorg`'s backward scan would have picked this insn before ever
      // splitting `adddi3_internal_1`, so hoisting must NOT cross it.
      insns.push_back("(set (reg:SI 22 s6) (plus:SI (reg:SI 22 s6) (const_int 1)))");
    }
    insns.push_back(
        "(sequence [(call_insn (parallel[ (set (reg:DI 2 v0) (call (mem:SI "
        "(symbol_ref:SI (\"SinkDi\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
        "(insn (set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (reg:SI 2 v0))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil)))])");
    insns.push_back("(use (reg/i:DI 2 v0))");
    insns.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    return cfg;
  };

  // Positive: `xor:DI` has `length > 1` (ineligible for delay slot), so the 4th
  // word hoists behind `xor:DI` and folds into the `adddi3_internal_1` PARALLEL.
  {
    const RtlControlFlowGraph pos =
        InvertFillSimpleDelaySlots(make_cfg(/*include_eligible_intervening=*/false), o2_prof);
    ASSERT_EQ(pos.rpo_blocks.size(), 1u);
    EXPECT_NE(pos.rpo_blocks[0].instructions[0].find("(plus:DI (reg:DI 18 s2) (reg:DI 20 s4))"),
              std::string::npos)
        << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[1].find("(xor:DI "), std::string::npos)
        << pos.FormatSummary();
  }

  // Negative: an intervening delay-slot-eligible `length == 1` insn blocks
  // hoisting, so the 3-word prefix is NOT folded into `plus:DI`.
  {
    const RtlControlFlowGraph neg =
        InvertFillSimpleDelaySlots(make_cfg(/*include_eligible_intervening=*/true), o2_prof);
    ASSERT_EQ(neg.rpo_blocks.size(), 1u);
    for (const std::string& insn : neg.rpo_blocks[0].instructions) {
      EXPECT_EQ(insn.find("(plus:DI "), std::string::npos) << neg.FormatSummary();
    }
  }
}

}  // namespace
}  // namespace rom_nom_nom::lifter
