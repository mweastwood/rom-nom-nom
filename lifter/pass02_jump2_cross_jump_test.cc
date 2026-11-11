#include "lifter/pass02_jump2_cross_jump.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_do_cross_jump.h"
#include "lifter/invert_fill_eager_delay_slots.h"
#include "lifter/invert_fill_simple_delay_slots.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"
#include "lifter/invert_gas_mips_ip.h"
#include "lifter/invert_make_return_insns.h"
#include "lifter/invert_relax_delay_slots.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::PrologueEpilogueFrameSpec;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionPass02InverseProvider final : public SubfunctionInverseProvider {
 public:
  std::string InvertGasMipsIp(uint32_t word_x) const override {
    return rom_nom_nom::InvertGasMipsIp(word_x);
  }

  std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const override {
    return rom_nom_nom::InvertGasMacro(macro_words_x);
  }

  std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x);
  }

  std::string InvertFinalScanInsn(std::string_view asm_line_x) const override {
    return lifter::InvertFinalScanInsn(asm_line_x);
  }

  PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view asm_text_x) const override {
    const auto spec = lifter::InvertFunctionPrologueEpilogue(asm_text_x);
    return {.var_size = spec.var_size, .saved_gpr_mask = spec.saved_gpr_mask};
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(
      absl::Span<const uint32_t> machine_words_x) const override {
    return lifter::InvertFinalAndGasFunction(machine_words_x);
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) const override {
    return lifter::InvertFinalAndGasFunction(input_x);
  }

  RtlControlFlowGraph InvertMakeReturnInsns(
      const RtlControlFlowGraph& dbr_return_cfg_x) const override {
    return lifter::InvertMakeReturnInsns(dbr_return_cfg_x);
  }

  RtlControlFlowGraph InvertRelaxDelaySlots(
      const RtlControlFlowGraph& dbr_relaxed_cfg_x) const override {
    return lifter::InvertRelaxDelaySlots(dbr_relaxed_cfg_x);
  }

  RtlControlFlowGraph InvertFillEagerDelaySlots(
      const RtlControlFlowGraph& dbr_eager_cfg_x) const override {
    return lifter::InvertFillEagerDelaySlots(dbr_eager_cfg_x);
  }

  RtlControlFlowGraph InvertFillSimpleDelaySlots(
      const RtlControlFlowGraph& dbr_simple_cfg_x) const override {
    return lifter::InvertFillSimpleDelaySlots(dbr_simple_cfg_x);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) const override {
    return lifter::InvertPass01DbrSchedule(dbr_cfg_x);
  }

  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
    return lifter::InvertPass02Jump2CrossJump(jump2_cfg_x);
  }
};

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

class Pass02Jump2CrossJumpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RtlControlFlowGraph dummy;
    hooks_.ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  GccContext ctx_;
  FunctionHooks hooks_{&ctx_};
  ProductionPass02InverseProvider provider_;
  RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_F(Pass02Jump2CrossJumpTest, SyntheticPositiveCrossJumpedJoinBeforeReturn) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 6 a2) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 10))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 6 a2) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 20))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(return)",
                },
                {}),
  };

  const RtlControlFlowGraph lifted = InvertPass02Jump2CrossJump(cfg);
  EXPECT_EQ(lifted.pass, RtlPassKind::kSched2);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[2].instructions.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[3].instructions.size(), 3u);

  const RightInverseResult res = verifier_.VerifyJump2CrossJumpStageRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(Pass02Jump2CrossJumpTest, SyntheticPositiveMultiPredecessorCrossJumpedJoin) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 7 a3) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 11))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(2, true,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                 "(label_ref L1) (pc)))"},
                {4, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg:SI 7 a3) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 22))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 7 a3) (const_int 3))",
                    "(set (reg:SI 3 v1) (const_int 33))",
                },
                {5}),
      MakeBlock(5, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2)))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(return)",
                },
                {}),
  };

  const RtlControlFlowGraph lifted = InvertPass02Jump2CrossJump(cfg);
  EXPECT_EQ(lifted.pass, RtlPassKind::kSched2);
  ASSERT_EQ(lifted.rpo_blocks.size(), 6u);

  const RightInverseResult res = verifier_.VerifyJump2CrossJumpStageRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(Pass02Jump2CrossJumpTest, SyntheticPositiveChainedStage0Stage1Stage2Composition) {
  // Construct a `.12.dbr` CFG with cross-jumped blocks and delay slots,
  // assemble to machine words, chain `InvertFinalAndGasFunction` (Stage 0) ->
  // `InvertPass01DbrSchedule` (Stage 1) -> `InvertPass02Jump2CrossJump` (Stage 2),
  // and replay forward through `kDoCrossJump` -> `RunDbrSchedule` -> `RunFinal` ->
  // `RunGasMdAssemble`.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 6 a2) (const_int 1)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (label_ref L1))) (insn (set (reg:SI 3 v1) "
                 "(const_int 10)))])"},
                {3}),
      MakeBlock(2, true,
                {"(set (reg:SI 6 a2) (const_int 2))", "(set (reg:SI 3 v1) (const_int 20))"}, {3}),
      MakeBlock(3, true,
                {"(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                 "(sequence [(jump_insn (return)) (insn (set (reg:SI 2 v0) (reg:SI 3 v1)))])"},
                {}),
  };

  ctx_.MaterializeFromCfg(dbr_cfg);
  const std::string orig_asm = hooks_.RunFinal();
  const std::vector<uint32_t> orig_words = hooks_.RunGasMdAssemble(orig_asm);
  ASSERT_FALSE(orig_words.empty());

  const RtlControlFlowGraph lifted_dbr = InvertFinalAndGasFunction(orig_words);
  ASSERT_FALSE(lifted_dbr.rpo_blocks.empty());
  EXPECT_EQ(lifted_dbr.pass, RtlPassKind::kDbr);

  const RtlControlFlowGraph lifted_jump2 = InvertPass01DbrSchedule(lifted_dbr);
  ASSERT_FALSE(lifted_jump2.rpo_blocks.empty());
  EXPECT_EQ(lifted_jump2.pass, RtlPassKind::kJump2);

  const RtlControlFlowGraph lifted_sched2 = InvertPass02Jump2CrossJump(lifted_jump2);
  ASSERT_FALSE(lifted_sched2.rpo_blocks.empty());
  EXPECT_EQ(lifted_sched2.pass, RtlPassKind::kSched2);

  const RightInverseResult jump2_res =
      verifier_.VerifyJump2CrossJumpStageRightInverse(lifted_jump2);
  EXPECT_TRUE(jump2_res.matches) << jump2_res.diff_detail;

  const RtlControlFlowGraph replayed_jump2 =
      hooks_
          .ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, lifted_sched2, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"})
          .post_state;
  EXPECT_TRUE(replayed_jump2.CompareAgainst(lifted_jump2).MatchesExactly());

  ctx_.MaterializeFromCfg(replayed_jump2);
  hooks_.RunDbrSchedule();
  const std::string replayed_asm = hooks_.RunFinal();
  const std::vector<uint32_t> replayed_words = hooks_.RunGasMdAssemble(replayed_asm);
  EXPECT_EQ(replayed_words, orig_words);
}

TEST_F(Pass02Jump2CrossJumpTest, SyntheticNegativeMalformedCfgAndCorruptedInverse) {
  // 1. Empty CFG.
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertPass02Jump2CrossJump(empty_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(empty_cfg).matches);

  // 2. Single block with empty instructions.
  RtlControlFlowGraph empty_blk_cfg;
  empty_blk_cfg.rpo_blocks.push_back(MakeBlock(0, false, {}, {}));
  EXPECT_TRUE(InvertPass02Jump2CrossJump(empty_blk_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(empty_blk_cfg).matches);

  // 3. Out-of-bounds successor edge.
  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(return)"}, {4}));
  EXPECT_TRUE(InvertPass02Jump2CrossJump(bad_succ_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(bad_succ_cfg).matches);

  // 4. Malformed S-expression.
  RtlControlFlowGraph bad_sexpr_cfg;
  bad_sexpr_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(set (reg:SI 2 v0)"}, {}));
  EXPECT_TRUE(InvertPass02Jump2CrossJump(bad_sexpr_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyJump2CrossJumpStageRightInverse(bad_sexpr_cfg).matches);

  // 5. Corrupted inverse provider rejected by `VerifyJump2CrossJumpStageRightInverse`.
  class BrokenJump2Provider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
      RtlControlFlowGraph bad = jump2_cfg_x;
      bad.pass = RtlPassKind::kSched2;
      if (!bad.rpo_blocks.empty()) {
        bad.rpo_blocks[0].instructions = {"(set (reg/i:SI 2 v0) (const_int 999))", "(return)"};
        bad.rpo_blocks[0].successors.clear();
        bad.rpo_blocks.resize(1);
        bad.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      }
      return bad;
    }
  };

  RtlControlFlowGraph valid_cfg;
  valid_cfg.pass = RtlPassKind::kJump2;
  valid_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg/i:SI 2 v0) (const_int 42))", "(return)"}, {}),
  };
  BrokenJump2Provider broken_provider;
  RightInverseVerifier broken_verifier(&ctx_, &hooks_, &broken_provider);
  EXPECT_FALSE(broken_verifier.VerifyJump2CrossJumpStageRightInverse(valid_cfg).matches);
}

class Pass02Jump2CrossJumpRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionPass02InverseProvider();
    verifier = new RightInverseVerifier(ctx, hooks, provider);

    RtlControlFlowGraph dummy;
    hooks->ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
  static ProductionPass02InverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* Pass02Jump2CrossJumpRoundTripTest::ctx = nullptr;
FunctionHooks* Pass02Jump2CrossJumpRoundTripTest::hooks = nullptr;
ProductionPass02InverseProvider* Pass02Jump2CrossJumpRoundTripTest::provider = nullptr;
RightInverseVerifier* Pass02Jump2CrossJumpRoundTripTest::verifier = nullptr;

TEST_P(Pass02Jump2CrossJumpRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  auto jump2_it = trace.pass_snapshots.find(RtlPassKind::kJump2);
  ASSERT_NE(jump2_it, trace.pass_snapshots.end()) << "Missing .11.jump2 for " << tc.test_name;
  const RtlControlFlowGraph& post_jump2 = jump2_it->second;

  // 1. Verify Stage 2 (`SubfunctionId::kDoCrossJump`) subfunction boundary
  // snapshot via `ProductionPass02InverseProvider`.
  bool found_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kDoCrossJump) {
      continue;
    }
    found_snap = true;
    const RightInverseResult sub_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(sub_res.matches) << "Stage 2 subfunction boundary mismatch for " << tc.test_name
                                 << ": " << sub_res.diff_detail << "\nexpected:\n"
                                 << sub_res.expected_summary << "\nactual:\n"
                                 << sub_res.actual_summary;
  }
  EXPECT_TRUE(found_snap) << "Missing kDoCrossJump snapshot for " << tc.test_name;

  // 2. Whole-stage `InvertPass02Jump2CrossJump` (`provider->InvertDoCrossJump`)
  // composition right-inverse on `.11.jump2`.
  const RtlControlFlowGraph pre_jump2 = InvertPass02Jump2CrossJump(post_jump2);
  ASSERT_FALSE(pre_jump2.rpo_blocks.empty())
      << "Empty InvertPass02Jump2CrossJump for " << tc.test_name;
  EXPECT_EQ(pre_jump2.pass, RtlPassKind::kSched2);

  const RightInverseResult stage2_res = verifier->VerifyJump2CrossJumpStageRightInverse(post_jump2);
  EXPECT_TRUE(stage2_res.matches) << "Stage 2 (.11.jump2) right-inverse mismatch for "
                                  << tc.test_name << ":\n"
                                  << stage2_res.diff_detail << "\nexpected (.11.jump2):\n"
                                  << stage2_res.expected_summary << "\nactual (.11.jump2):\n"
                                  << stage2_res.actual_summary;

  // 3. Chained Stage 1 + Stage 2 composition on `.12.dbr` and Stage 0 + Stage 1 + Stage 2
  // composition across the 2,480 corpus cases.
  auto dbr_it = trace.pass_snapshots.find(RtlPassKind::kDbr);
  ASSERT_NE(dbr_it, trace.pass_snapshots.end()) << "Missing .12.dbr for " << tc.test_name;
  const RtlControlFlowGraph& post_dbr = dbr_it->second;

  const RtlControlFlowGraph stage1_from_dbr = InvertPass01DbrSchedule(post_dbr);
  ASSERT_FALSE(stage1_from_dbr.rpo_blocks.empty()) << "Test case: " << tc.test_name;
  EXPECT_EQ(stage1_from_dbr.pass, RtlPassKind::kJump2);
  const RtlControlFlowGraph stage2_from_dbr = InvertPass02Jump2CrossJump(stage1_from_dbr);
  ASSERT_FALSE(stage2_from_dbr.rpo_blocks.empty()) << "Test case: " << tc.test_name;
  EXPECT_EQ(stage2_from_dbr.pass, RtlPassKind::kSched2);

  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << "Test case: " << tc.test_name;

  const RtlControlFlowGraph stage0_lifted_dbr = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(stage0_lifted_dbr.rpo_blocks.empty()) << "Test case: " << tc.test_name;
  const RtlControlFlowGraph stage1_lifted_jump2 = InvertPass01DbrSchedule(stage0_lifted_dbr);
  ASSERT_FALSE(stage1_lifted_jump2.rpo_blocks.empty()) << "Test case: " << tc.test_name;
  const RtlControlFlowGraph stage2_lifted_sched2 = InvertPass02Jump2CrossJump(stage1_lifted_jump2);
  ASSERT_FALSE(stage2_lifted_sched2.rpo_blocks.empty()) << "Test case: " << tc.test_name;
  EXPECT_EQ(stage2_lifted_sched2.pass, RtlPassKind::kSched2);
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass02Jump2CrossJumpRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
