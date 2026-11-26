#include "lifter/invert_make_return_insns.h"

#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_fill_eager_delay_slots.h"
#include "lifter/invert_fill_simple_delay_slots.h"
#include "lifter/invert_relax_delay_slots.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

// Subclasses `SubfunctionInverseProvider` directly so that
// `RightInverseVerifier` exercises only the production `lifter/`
// implementation of `InvertMakeReturnInsns` (and Stage 0..1 suffix composition).
class ProductionMakeReturnInsnsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) const override {
    return ::rom_nom_nom::lifter::InvertFinalAndGasFunction(input_x);
  }

  RtlControlFlowGraph InvertMakeReturnInsns(
      const RtlControlFlowGraph& dbr_return_cfg_x) const override {
    return ::rom_nom_nom::lifter::InvertMakeReturnInsns(dbr_return_cfg_x);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) const override {
    return ::rom_nom_nom::lifter::InvertPass01DbrSchedule(dbr_cfg_x);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x,
                                        const CompilationProfile& profile) const override {
    return ::rom_nom_nom::lifter::InvertPass01DbrSchedule(dbr_cfg_x, profile);
  }
};

// Adversarial Stage 0..1 inverse provider that bypasses `InvertMakeReturnInsns`
// (running `InvertRelaxDelaySlots` -> `InvertFillEagerDelaySlots` ->
// `InvertFillSimpleDelaySlots` directly on the packed return `SEQUENCE`) so
// `RightInverseVerifier::VerifySuffixRightInverse` rejects it whenever
// `InvertMakeReturnInsns` needed to unpack a return delay-slot compensation.
class BypassedUnpackMakeReturnInsnsProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) const override {
    return ::rom_nom_nom::lifter::InvertFinalAndGasFunction(input_x);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) const override {
    return InvertDbrSchedule(dbr_cfg_x, CompilationProfile{.optimize_level = 2});
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x,
                                        const CompilationProfile& profile) const override {
    RtlControlFlowGraph cfg = ::rom_nom_nom::lifter::InvertRelaxDelaySlots(dbr_cfg_x, profile);
    if (cfg.rpo_blocks.empty()) {
      return {};
    }
    cfg = ::rom_nom_nom::lifter::InvertFillEagerDelaySlots(cfg, profile);
    if (cfg.rpo_blocks.empty()) {
      return {};
    }
    return ::rom_nom_nom::lifter::InvertFillSimpleDelaySlots(cfg, profile);
  }
};

TEST(InvertMakeReturnInsnsTest, SingleBlockSequenceReturnInternalRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  RtlControlFlowGraph ret_cfg;
  ret_cfg.pass = RtlPassKind::kDbr;
  ret_cfg.rpo_blocks.resize(1);
  ret_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
      "(insn (set (reg/i:SI 2 v0) (const_int 42)))])");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(ret_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
            "(insn (set (reg/i:SI 2 v0) (const_int 42)))])");
  EXPECT_TRUE(lifted.rpo_blocks[0].successors.empty());

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(ret_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, MultiBlockMultipleSequenceReturnInternalRightInverse) {
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
      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
      "(insn (set (reg/i:SI 2 v0) (const_int 10)))])");
  cfg.rpo_blocks[2].has_label = true;
  cfg.rpo_blocks[2].AddPatternOnly(
      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
      "(insn (set (reg/i:SI 2 v0) (const_int 20)))])");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_TRUE(lifted.rpo_blocks[1].successors.empty());
  EXPECT_TRUE(lifted.rpo_blocks[2].successors.empty());

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
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
      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 31 ra) (nil))) "
      "(insn (set (reg/i:SI 2 v0) (const_int 7)))])");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] ) "
            "(expr_list:REG_DEAD (reg:SI 31 ra) (nil))) "
            "(insn (set (reg/i:SI 2 v0) (const_int 7)))])");

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, StandaloneReturnInternalIsPreserved) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

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

TEST(InvertMakeReturnInsnsTest, UnreferencedTrailingUseLabelProtectedByAppendedEndLabel) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // Block 1 falls through into a labeled `(use ...)` block (`LABEL_NUSES == 0`
  // on `B2`); `InvertMakeReturnInsns` appends a trailing unused
  // `end_of_function_label` block (`B3`) so `RunMakeReturnInsns` deletes `B3`
  // rather than `B2`'s label.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband.frame_size_bytes = 24;
  cfg.sideband.saved_gpr_regnos = {31};
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].has_label = true;
  cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 1)) "
      "(label_ref L0) (pc)))) (insn (set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1))))])");
  cfg.rpo_blocks[0].successors = {0, 1};
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  cfg.rpo_blocks[1].successors = {2};
  cfg.rpo_blocks[2].has_label = true;
  cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RtlControlFlowGraph lifted = InvertMakeReturnInsns(cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted.rpo_blocks[2].successors, std::vector<size_t>{3});
  EXPECT_TRUE(lifted.rpo_blocks[3].instructions.empty());
  EXPECT_TRUE(lifted.rpo_blocks[3].has_label);

  const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(InvertMakeReturnInsnsTest, UnpacksReturnSequenceCompensatingEagerBranchDelaySlotRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const std::vector<std::string> mips1_reorder_flags = {"-O2", "-mips1", "-funsigned-char",
                                                        "-Wa,-O2"};
  const CompilationProfile mips1_reorder_profile{.optimize_level = 2,
                                                 .c_flags = mips1_reorder_flags};
  hooks.ApplyCompilationProfile(mips1_reorder_profile);
  ProductionMakeReturnInsnsProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  BypassedUnpackMakeReturnInsnsProvider bypassed_provider;
  RightInverseVerifier bypassed_verifier(&ctx, &hooks, &bypassed_provider);

  // 1. Constant-immediate opposite arithmetic (`+3` in non-annulled backward
  //    loop branch delay slot compensated by `-3` packed into the fallthrough
  //    return `SEQUENCE` under `-O2 -mips1 -Wa,-O2`): `InvertMakeReturnInsns`
  //    unpacks `-3` ahead of `(use (reg/i:SI 2 v0))` and moves `(return)` into
  //    a labeled `end_blk` without appending a duplicate empty end block.
  {
    RtlControlFlowGraph const_comp_cfg;
    const_comp_cfg.pass = RtlPassKind::kDbr;
    const_comp_cfg.rpo_blocks.resize(3);
    const_comp_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
    const_comp_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))");
    const_comp_cfg.rpo_blocks[0].successors = {1};
    const_comp_cfg.rpo_blocks[1].has_label = true;
    const_comp_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 4 a0) (minus:SI (reg:SI 4 a0) (reg:SI 2 v0)))");
    const_comp_cfg.rpo_blocks[1].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) "
        "(label_ref L0) (pc)))) "
        "(insn/s (set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3))))])");
    const_comp_cfg.rpo_blocks[1].successors = {1, 2};
    const_comp_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
    const_comp_cfg.rpo_blocks[2].AddPatternOnly(
        "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
        "(insn (set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -3))))])");

    const RtlControlFlowGraph lifted = InvertMakeReturnInsns(const_comp_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
    EXPECT_FALSE(lifted.rpo_blocks[2].has_label);
    EXPECT_EQ(lifted.rpo_blocks[2].instructions,
              (std::vector<std::string>{
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -3)))",
                  "(use (reg/i:SI 2 v0))",
              }));
    EXPECT_EQ(lifted.rpo_blocks[2].successors, std::vector<size_t>{3});
    EXPECT_TRUE(lifted.rpo_blocks[3].has_label);
    EXPECT_EQ(lifted.rpo_blocks[3].instructions,
              std::vector<std::string>{"(parallel[ (return) (use (reg:SI 31 ra)) ] )"});
    EXPECT_TRUE(lifted.rpo_blocks[3].successors.empty());

    ctx.MaterializeFromCfg(const_comp_cfg);
    Stage0FunctionInput const_stage0;
    const_stage0.machine_words = hooks.RunGasMdAssemble(hooks.RunFinal());
    const_stage0.profile = mips1_reorder_profile;
    ASSERT_FALSE(const_stage0.machine_words.empty());

    const RightInverseResult res =
        verifier.VerifySuffixRightInverse(RtlPassKind::kJump2, const_stage0);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_FALSE(
        bypassed_verifier.VerifySuffixRightInverse(RtlPassKind::kJump2, const_stage0).matches);
  }

  // 2. Register-operand opposite arithmetic (`plus:SI` in the backward loop
  //    branch delay slot vs `minus:SI` in the fallthrough return `SEQUENCE`,
  //    verified on both synthetic RTL with differing RHS register flags and a
  //    real `cc1` + GAS `-O2 -mips1 -Wa,-O2` compilation of `DoWhileSum`).
  {
    RtlControlFlowGraph reg_comp_cfg;
    reg_comp_cfg.pass = RtlPassKind::kDbr;
    reg_comp_cfg.rpo_blocks.resize(3);
    reg_comp_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
    reg_comp_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg/v:SI 5 a1)))");
    reg_comp_cfg.rpo_blocks[0].successors = {1};
    reg_comp_cfg.rpo_blocks[1].has_label = true;
    reg_comp_cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 4 a0) (minus:SI (reg:SI 4 a0) (reg:SI 2 v0)))");
    reg_comp_cfg.rpo_blocks[1].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) "
        "(label_ref L0) (pc)))) "
        "(insn/s (set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg/v:SI 5 a1))))])");
    reg_comp_cfg.rpo_blocks[1].successors = {1, 2};
    reg_comp_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
    reg_comp_cfg.rpo_blocks[2].AddPatternOnly(
        "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
        "(insn (set (reg:SI 2 v0) (minus:SI (reg:SI 2 v0) (reg:SI 5 a1))))])");

    const RtlControlFlowGraph lifted = InvertMakeReturnInsns(reg_comp_cfg);
    ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
    EXPECT_EQ(lifted.rpo_blocks[2].instructions,
              (std::vector<std::string>{
                  "(set (reg:SI 2 v0) (minus:SI (reg:SI 2 v0) (reg:SI 5 a1)))",
                  "(use (reg/i:SI 2 v0))",
              }));
    EXPECT_TRUE(lifted.rpo_blocks[3].has_label);

    const auto sum_trace = hooks.TraceForwardCompilation(
        "int DoWhileSum(int n) {\n"
        "  int sum = 0;\n"
        "  do {\n"
        "    sum += n;\n"
        "    n--;\n"
        "  } while (n > 0);\n"
        "  return sum;\n"
        "}\n",
        "DoWhileSum", mips1_reorder_flags, "do_while_sum_return_eager_comp");
    const RtlControlFlowGraph stage0_dbr = InvertFinalAndGasFunction(sum_trace.stage0_input);
    ASSERT_FALSE(stage0_dbr.rpo_blocks.empty());
    EXPECT_EQ(InvertMakeReturnInsns(stage0_dbr).rpo_blocks.size(),
              stage0_dbr.rpo_blocks.size() + 1);

    const RightInverseResult res =
        verifier.VerifySuffixRightInverse(RtlPassKind::kJump2, sum_trace.stage0_input);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_FALSE(
        bypassed_verifier.VerifySuffixRightInverse(RtlPassKind::kJump2, sum_trace.stage0_input)
            .matches);
  }

  // 3. Guard check: when the preceding branch is annulled (`jump_insn/u`) or
  //    the return delay slot is non-opposite arithmetic (`+1` and `+1`), the
  //    return `SEQUENCE` is NOT unpacked into an extra block.
  {
    hooks.ApplyCompilationProfile(CompilationProfile{.optimize_level = 2});
    RtlControlFlowGraph same_sign_cfg;
    same_sign_cfg.pass = RtlPassKind::kDbr;
    same_sign_cfg.rpo_blocks.resize(2);
    same_sign_cfg.rpo_blocks[0].has_label = true;
    same_sign_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))");
    same_sign_cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) "
        "(label_ref L0) (pc)))) "
        "(insn (set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (const_int 1))))])");
    same_sign_cfg.rpo_blocks[0].successors = {0, 1};
    same_sign_cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");
    same_sign_cfg.rpo_blocks[1].AddPatternOnly(
        "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
        "(insn (set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (const_int 1))))])");

    const RtlControlFlowGraph lifted = InvertMakeReturnInsns(same_sign_cfg);
    EXPECT_EQ(lifted.rpo_blocks.size(), 2u);
    const RightInverseResult res = verifier.VerifyMakeReturnInsnsRightInverse(same_sign_cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;

    RtlControlFlowGraph annulled_cfg = same_sign_cfg;
    annulled_cfg.rpo_blocks[0].instructions.back() =
        "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) "
        "(label_ref L0) (pc)))) "
        "(insn/s (set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (const_int 1))))])";
    annulled_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    annulled_cfg.rpo_blocks[1].instructions.back() =
        "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
        "(insn (set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (const_int -1))))])";
    annulled_cfg.rpo_blocks[1].SyncRtlInsnsFromInstructions();
    EXPECT_EQ(InvertMakeReturnInsns(annulled_cfg).rpo_blocks.size(), 2u);
  }
}

TEST(InvertMakeReturnInsnsTest, RejectsEmptyAndMalformedControlFlowGraphsIncludingBareReturn) {
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

  // 3. Unbalanced / malformed RTL pattern strings, including synthetic bare `(return)`:
  for (const std::string& bad_pat : std::vector<std::string>{
           "",
           "   ",
           "not_an_sexpr",
           "(unknown_op (const_int 1))",
           "(set (reg:SI 2 v0) (const_int 1)",
           "(set (reg:SI 2 v0) (const_int 1)))",
           "(return)",
           "(sequence [(jump_insn (return)) (insn (const_int 0))])",
           "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] ))])",
           "(sequence (jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) (insn (const_int "
           "0)))",
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
    bad_succ_cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    bad_succ_cfg.rpo_blocks[0].successors = {5};
    EXPECT_TRUE(InvertMakeReturnInsns(bad_succ_cfg).rpo_blocks.empty());
    EXPECT_FALSE(verifier.VerifyMakeReturnInsnsRightInverse(bad_succ_cfg).matches);
  }

  // 5. Out-of-range `addr_vec_targets` index:
  {
    RtlControlFlowGraph bad_addr_vec_cfg;
    bad_addr_vec_cfg.pass = RtlPassKind::kDbr;
    bad_addr_vec_cfg.rpo_blocks.resize(1);
    bad_addr_vec_cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
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
    bad_snap.post_state.rpo_blocks[0].instructions.push_back("(unbalanced (const_int 0)");
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
  const absl::StatusOr<const gcc_harness::FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const gcc_harness::FunctionCompilationTrace& trace = **cached_trace;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

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
