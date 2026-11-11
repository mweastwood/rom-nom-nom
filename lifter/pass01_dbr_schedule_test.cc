#include "lifter/pass01_dbr_schedule.h"

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

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::PrologueEpilogueFrameSpec;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionPass01InverseProvider final : public SubfunctionInverseProvider {
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
};

RtlBasicBlock MakeBlock(uint32_t id, bool has_label, std::vector<std::string> instructions,
                        std::vector<size_t> successors) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.has_label = has_label;
  blk.instructions = std::move(instructions);
  blk.successors = std::move(successors);
  return blk;
}

class Pass01DbrScheduleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RtlControlFlowGraph dummy;
    hooks_.ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  GccContext ctx_;
  FunctionHooks hooks_{&ctx_};
  ProductionPass01InverseProvider provider_;
  RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

// Exercises all four Stage 1 subfunctions in composition:
//   - `make_return_insns`: delay-slot-filled `(sequence [(jump_insn (return)) ...])`
//   - `relax_delay_slots`: branch inverted around unconditional jump (`insn/s`)
//   - `fill_eager_delay_slots`: target-thread delay-slot stealing (`insn/s`)
//   - `fill_simple_delay_slots`: same-block delay slot and `optimize_skip` (`jump_insn/u` +
//   `insn/s`)
TEST_F(Pass01DbrScheduleTest, SyntheticPositiveMultiSubfunctionComposition) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      // Block 0: Eager target-thread delay slot (`insn/s` sets `$v1 = 7` from L0)
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 3 v1) (const_int 7)))])"},
                {2, 1}),
      // Block 1: `make_return_insns` return with same-block simple delay slot
      MakeBlock(1, false,
                {"(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 "
                 "a0) (reg:SI 5 a1))))])"},
                {}),
      // Block 2 (`L0`): `optimize_skip` (`jump_insn/u` + `insn/s` with both edges to L1)
      MakeBlock(2, true,
                {"(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (reg:SI 3 v1)))",
                 "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int "
                 "0)) (label_ref L1) (pc)))) (insn/s (set (reg/i:SI 2 v0) (const_int 99)))])"},
                {3, 3}),
      // Block 3 (`L1`): shared return block
      MakeBlock(3, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertPass01DbrSchedule(cfg);
  EXPECT_EQ(lifted.pass, RtlPassKind::kJump2);
  ASSERT_FALSE(lifted.rpo_blocks.empty());

  const RightInverseResult res = verifier_.VerifyDbrStageRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(Pass01DbrScheduleTest, SyntheticPositiveRelaxAndReturnComposition) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg/i:SI 2 v0) (const_int 1)))])"},
                {2, 1}),
      MakeBlock(1, false, {"(set (reg/i:SI 2 v0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))"},
                {2}),
      MakeBlock(2, true,
                {"(use (reg/i:SI 2 v0))",
                 "(sequence [(jump_insn (return)) (insn (set (reg:SI 3 v1) (const_int 0)))])"},
                {}),
  };

  const RtlControlFlowGraph lifted = InvertPass01DbrSchedule(cfg);
  EXPECT_EQ(lifted.pass, RtlPassKind::kJump2);
  ASSERT_FALSE(lifted.rpo_blocks.empty());

  const RightInverseResult res = verifier_.VerifyDbrStageRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nlifted:\n"
                           << lifted.FormatSummary() << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(Pass01DbrScheduleTest, SyntheticPositiveChainedStage0AndStage1Composition) {
  // Compile a multi-block function with delay slots through Stage 1 + Stage 0,
  // then chain `InvertFinalAndGasFunction` (Stage 0) -> `InvertPass01DbrSchedule`
  // (Stage 1) and replay `RunDbrSchedule` + `RunFinal` + `RunGasMdAssemble`.
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) "
                 "(const_int 7))))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
                 "(sequence [(jump_insn (return)) (insn (set (reg:SI 2 v0) (ashift:SI (reg:SI 3 "
                 "v1) (const_int 2))))])"},
                {}),
      MakeBlock(2, true,
                {"(set (reg:SI 3 v1) (minus:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
                 "(sequence [(jump_insn (return)) (insn (set (reg:SI 2 v0) (ashift:SI (reg:SI 3 "
                 "v1) (const_int 1))))])"},
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

  const RightInverseResult dbr_res = verifier_.VerifyDbrStageRightInverse(dbr_cfg);
  EXPECT_TRUE(dbr_res.matches) << dbr_res.diff_detail;

  const RightInverseResult stage0_res = verifier_.VerifyFinalAndGasStageRightInverse(orig_words);
  EXPECT_TRUE(stage0_res.matches) << stage0_res.diff_detail;

  ctx_.MaterializeFromCfg(lifted_jump2);
  hooks_.RunDbrSchedule();
  const std::string replayed_asm = hooks_.RunFinal();
  const std::vector<uint32_t> replayed_words = hooks_.RunGasMdAssemble(replayed_asm);
  EXPECT_EQ(replayed_words, orig_words);
}

TEST_F(Pass01DbrScheduleTest, SyntheticNegativeMalformedCfgAndCorruptedInverse) {
  // 1. Empty CFG.
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertPass01DbrSchedule(empty_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyDbrStageRightInverse(empty_cfg).matches);

  // 2. Single block with empty instructions.
  RtlControlFlowGraph empty_blk_cfg;
  empty_blk_cfg.rpo_blocks.push_back(MakeBlock(0, false, {}, {}));
  EXPECT_TRUE(InvertPass01DbrSchedule(empty_blk_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyDbrStageRightInverse(empty_blk_cfg).matches);

  // 3. Out-of-bounds successor edge.
  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(return)"}, {4}));
  EXPECT_TRUE(InvertPass01DbrSchedule(bad_succ_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyDbrStageRightInverse(bad_succ_cfg).matches);

  // 4. Malformed S-expression.
  RtlControlFlowGraph bad_sexpr_cfg;
  bad_sexpr_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(set (reg:SI 2 v0)"}, {}));
  EXPECT_TRUE(InvertPass01DbrSchedule(bad_sexpr_cfg).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyDbrStageRightInverse(bad_sexpr_cfg).matches);

  // 5. Corrupted inverse provider rejected by `VerifyDbrStageRightInverse`.
  class BrokenDbrProvider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) const override {
      RtlControlFlowGraph bad = dbr_cfg_x;
      if (!bad.rpo_blocks.empty()) {
        bad.rpo_blocks[0].instructions = {"(set (reg/i:SI 2 v0) (const_int 999))", "(return)"};
        bad.rpo_blocks[0].successors.clear();
      }
      return bad;
    }
  };

  RtlControlFlowGraph valid_cfg;
  valid_cfg.pass = RtlPassKind::kDbr;
  valid_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (const_int 42)))])"},
                {}),
  };
  BrokenDbrProvider broken_provider;
  RightInverseVerifier broken_verifier(&ctx_, &hooks_, &broken_provider);
  EXPECT_FALSE(broken_verifier.VerifyDbrStageRightInverse(valid_cfg).matches);
}

TEST_F(Pass01DbrScheduleTest,
       OptimizeSkipForm2WithLeadingUseAtSharedTargetAndShortenBranchesReset) {
  // Regression test for:
  // 1. `reorg.c:optimize_skip` Form 2 (`jump_insn/u` + `insn/s` followed by a
  //    bare unconditional jump to a shared target block beginning with `(use ...)`),
  //    ensuring `InvertFillEagerDelaySlots` preserves it for `InvertFillSimpleDelaySlots`.
  // 2. Resetting `final.c:insn_lengths` (`init_insn_lengths()`) before replaying
  //    `reorg.c` after `shorten_branches` / `RunFinal` has populated `insn_lengths`.
  constexpr std::string_view kSource = R"(
extern float g_min_val;
extern float g_clamp_val;
extern float g_inner_val;
extern float g_fallback_table[16];
float clamp_or_lookup(int bypass, int idx, float x) {
  float out = 0.0f;
  if (!bypass) {
    if (x >= g_min_val) {
      if (x < g_clamp_val) {
        if (x < g_inner_val) {
          out = g_clamp_val;
        }
      } else {
        out = g_clamp_val;
      }
    } else {
      out = g_fallback_table[idx];
    }
  }
  return out;
}
)";
  const auto trace = hooks_.TraceForwardCompilation(kSource, "clamp_or_lookup",
                                                    {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                                                    "optimize_skip_form2_regression");
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kFillSimpleDelaySlots &&
        snap.subfunction_id != SubfunctionId::kFillEagerDelaySlots &&
        snap.subfunction_id != SubfunctionId::kRelaxDelaySlots &&
        snap.subfunction_id != SubfunctionId::kMakeReturnInsns) {
      continue;
    }
    const RightInverseResult sub_res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(sub_res.matches) << SubfunctionIdName(snap.subfunction_id) << ": "
                                 << sub_res.diff_detail;
  }
  const RightInverseResult stage0_res =
      verifier_.VerifyFinalAndGasStageRightInverse(trace.stage0_input);
  EXPECT_TRUE(stage0_res.matches) << stage0_res.diff_detail;
}

class Pass01DbrScheduleRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionPass01InverseProvider();
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
  static ProductionPass01InverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* Pass01DbrScheduleRoundTripTest::ctx = nullptr;
FunctionHooks* Pass01DbrScheduleRoundTripTest::hooks = nullptr;
ProductionPass01InverseProvider* Pass01DbrScheduleRoundTripTest::provider = nullptr;
RightInverseVerifier* Pass01DbrScheduleRoundTripTest::verifier = nullptr;

TEST_P(Pass01DbrScheduleRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  auto dbr_it = trace.pass_snapshots.find(RtlPassKind::kDbr);
  ASSERT_NE(dbr_it, trace.pass_snapshots.end()) << "Missing .12.dbr for " << tc.test_name;
  const RtlControlFlowGraph& post_dbr = dbr_it->second;

  // 1. Verify each of the 4 Stage 1 subfunction boundary snapshots via
  // `ProductionPass01InverseProvider`.
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kFillSimpleDelaySlots &&
        snap.subfunction_id != SubfunctionId::kFillEagerDelaySlots &&
        snap.subfunction_id != SubfunctionId::kRelaxDelaySlots &&
        snap.subfunction_id != SubfunctionId::kMakeReturnInsns) {
      continue;
    }
    const RightInverseResult sub_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(sub_res.matches) << "Stage 1 subfunction boundary mismatch ("
                                 << gcc_harness::SubfunctionIdName(snap.subfunction_id) << ") for "
                                 << tc.test_name << ": " << sub_res.diff_detail
                                 << "\nsnap.pre_state:\n"
                                 << snap.pre_state.FormatSummary() << "\nexpected:\n"
                                 << sub_res.expected_summary << "\nactual:\n"
                                 << sub_res.actual_summary;
  }

  // 2. Whole-stage `InvertPass01DbrSchedule` (`provider->InvertDbrSchedule`)
  // composition right-inverse on `.12.dbr`.
  const RtlControlFlowGraph pre_dbr = provider->InvertDbrSchedule(post_dbr);
  ASSERT_FALSE(pre_dbr.rpo_blocks.empty()) << "Empty InvertPass01DbrSchedule for " << tc.test_name;
  EXPECT_EQ(pre_dbr.pass, RtlPassKind::kJump2);

  RtlControlFlowGraph replayed_dbr = pre_dbr;
  for (SubfunctionId sub_id : {
           SubfunctionId::kFillSimpleDelaySlots,
           SubfunctionId::kFillEagerDelaySlots,
           SubfunctionId::kRelaxDelaySlots,
           SubfunctionId::kMakeReturnInsns,
       }) {
    replayed_dbr = hooks
                       ->ExecuteSubfunctionBoundary(sub_id, replayed_dbr, {}, trace.optimize_level,
                                                    trace.c_flags)
                       .post_state;
  }
  const auto cmp = replayed_dbr.CompareAgainst(post_dbr);
  EXPECT_TRUE(cmp.MatchesExactly())
      << "Stage 1 (.12.dbr) right-inverse mismatch for " << tc.test_name << ":\n"
      << cmp.mismatch_detail << "\nexpected (.12.dbr):\n"
      << post_dbr.FormatSummary() << "\nactual (.12.dbr):\n"
      << replayed_dbr.FormatSummary();

  // 3. Chained Stage 0 + Stage 1 composition across the 2,480 corpus cases.
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << "Test case: " << tc.test_name;

  const RightInverseResult stage0_res =
      verifier->VerifyFinalAndGasStageRightInverse(trace.stage0_input);
  EXPECT_TRUE(stage0_res.matches) << "Stage 0 right-inverse mismatch for " << tc.test_name << ":\n"
                                  << stage0_res.diff_detail;

  const RtlControlFlowGraph stage0_lifted_dbr = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(stage0_lifted_dbr.rpo_blocks.empty()) << "Test case: " << tc.test_name;
  const RtlControlFlowGraph stage1_lifted_jump2 = InvertPass01DbrSchedule(stage0_lifted_dbr);
  ASSERT_FALSE(stage1_lifted_jump2.rpo_blocks.empty()) << "Test case: " << tc.test_name;
  EXPECT_EQ(stage1_lifted_jump2.pass, RtlPassKind::kJump2);
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass01DbrScheduleRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
