#include "lifter/invert_reload.h"

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ReloadOnlyInverseProvider : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertReload(reload_cfg);
    return res.ok() ? *res : reload_cfg;
  }
};

class CorruptedReloadInverseProvider : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg) const override {
    RtlControlFlowGraph corrupted = reload_cfg;
    if (!corrupted.rpo_blocks.empty()) {
      corrupted.rpo_blocks[0].instructions.clear();
      corrupted.rpo_blocks[0].instructions.push_back("(set (reg:SI 2 $2) (const_int 999999))");
      corrupted.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }
    return corrupted;
  }
};

class InvertReloadTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
  }

  static void TearDownTestSuite() {
    delete hooks;
    hooks = nullptr;
    delete ctx;
    ctx = nullptr;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
};

GccContext* InvertReloadTest::ctx = nullptr;
FunctionHooks* InvertReloadTest::hooks = nullptr;

// Positive synthetic test 1: Frame-pointer elimination reversal (`$sp` -> `$fp`).
TEST_F(InvertReloadTest, ReversesFramePointerEliminationInStackAccesses) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  reload_cfg.sideband.frame_pointer_needed = false;
  reload_cfg.sideband.frame_size_bytes = 24;
  reload_cfg.sideband.max_regno = 74;
  reload_cfg.sideband.reg_renumber[72] = 4;
  reload_cfg.sideband.reg_renumber[73] = 3;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
      "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_cfg);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  ASSERT_EQ(lifted->rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[1],
            "(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int 16))) (reg:SI 72)) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[2],
            "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 30 fp) (const_int 16))))");

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 2: Preserves `$fp` when `frame_pointer_needed` is true.
TEST_F(InvertReloadTest, PreservesFramePointerWhenFramePointerNeeded) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  reload_cfg.sideband.frame_pointer_needed = true;
  reload_cfg.sideband.frame_size_bytes = 16;
  reload_cfg.sideband.max_regno = 74;
  reload_cfg.sideband.reg_renumber[72] = 4;
  reload_cfg.sideband.reg_renumber[73] = 3;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (reg:SI 72)))",
      "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 8))) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 8))))",
      "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_cfg);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[2],
            "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 8))) (reg:SI 72)) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 3: Reverses earlyclobber clobber output reload after PARALLEL.
TEST_F(InvertReloadTest, ReversesEarlyclobberClobberOutputReloadsOnParallel) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  reload_cfg.sideband.frame_pointer_needed = false;
  reload_cfg.sideband.max_regno = 67;
  reload_cfg.sideband.saved_gpr_regnos = {2, 3, 4, 5, 6, 7};
  reload_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29};
  reload_cfg.sideband.reg_n_deaths[4] = 1;
  reload_cfg.sideband.reg_n_deaths[5] = 1;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(parallel[ (set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
      "(clobber (reg:SI 2 v0)) (clobber (reg:SI 3 v1)) "
      "(clobber (reg:SI 6 a2)) (clobber (reg:SI 7 a3)) "
      "(use (const_int 8)) (use (const_int 1)) (use (const_int 0)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_UNUSED (scratch:SI) "
      "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_UNUSED (scratch:SI) "
      "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))))))",
      "(set (reg:SI 5 a1) (reg:SI 7 a3))",
      "(set (reg:SI 4 a0) (reg:SI 6 a2))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_cfg);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  ASSERT_EQ(lifted->rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[0],
            "(parallel [(set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
            "(clobber (reg:SI 2 v0)) (clobber (reg:SI 3 v1)) "
            "(clobber (reg:SI 4 a0)) (clobber (reg:SI 5 a1)) "
            "(use (const_int 8)) (use (const_int 1)) (use (const_int 0))]) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_UNUSED (scratch:SI) "
            "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_UNUSED (scratch:SI) "
            "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))))))");

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 4: Reverses frame-offset address calculation and REG_NOTES.
TEST_F(InvertReloadTest, ReversesFramePointerInAddressArithmeticAndNotes) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int take_ptr(int* p);\nint foo(int x) { int a = x + 1; return take_ptr(&a); }\n", "foo",
      {"-O2"});
  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_reload = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      found_reload = true;
      absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(snap.post_state);
      ASSERT_TRUE(lifted.ok()) << lifted.status();
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_reload);
}

// Positive synthetic test 5: Multi-register pressure / stack spill & reload sequence.
TEST_F(InvertReloadTest, ReversesSpillAndReloadUnderHighRegisterPressure) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int bar(int a, int b, int c, int d) {\n"
      "  volatile int s0 = a + b;\n"
      "  volatile int s1 = c + d;\n"
      "  return s0 * s1 + (a ^ c) + (b ^ d);\n"
      "}\n",
      "bar", {"-O2"});
  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_reload = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      found_reload = true;
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_reload);
}

// Negative test 1: Rejects empty CFG.
TEST_F(InvertReloadTest, RejectsEmptyCfg) {
  RtlControlFlowGraph empty_cfg;
  absl::StatusOr<RtlControlFlowGraph> res = InvertReload(empty_cfg);
  EXPECT_FALSE(res.ok());
}

// Negative test 2: Rejects malformed S-expression in instruction.
TEST_F(InvertReloadTest, RejectsMalformedSexprInstruction) {
  RtlControlFlowGraph bad_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 2 $2) (plus:SI (reg:SI 4 $4)"};
  bad_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> res = InvertReload(bad_cfg);
  EXPECT_FALSE(res.ok());
}

// Negative test 3: Rejects out-of-bounds CFG successor edge.
TEST_F(InvertReloadTest, RejectsOutOfBoundsSuccessorEdge) {
  RtlControlFlowGraph bad_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 2 $2) (const_int 0))"};
  bb0.successors = {5};
  bad_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> res = InvertReload(bad_cfg);
  EXPECT_FALSE(res.ok());
}

// Negative test 4: Rejects invalid negative register number in instruction or sideband.
TEST_F(InvertReloadTest, RejectsInvalidNegativeRegisterNumber) {
  RtlControlFlowGraph bad_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI -5) (const_int 0))"};
  bad_cfg.rpo_blocks.push_back(bb0);

  EXPECT_FALSE(InvertReload(bad_cfg).ok());

  RtlControlFlowGraph bad_sideband_cfg;
  RtlBasicBlock valid_bb;
  valid_bb.id = 0;
  valid_bb.instructions = {"(set (reg:SI 2 $2) (const_int 0))"};
  bad_sideband_cfg.rpo_blocks.push_back(valid_bb);
  bad_sideband_cfg.sideband.reg_renumber[10] = 2;  // Not a pseudo register (< 67).
  EXPECT_FALSE(InvertReload(bad_sideband_cfg).ok());
}

// Negative test 5: Verifies that `VerifyReloadRightInverse` rejects a corrupted inverse.
TEST_F(InvertReloadTest, VerifierRejectsCorruptedInverseProvider) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 2 $2) (const_int 42))",
      "(use (reg/i:SI 2 $2))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  CorruptedReloadInverseProvider bad_provider;
  RightInverseVerifier verifier(ctx, hooks, &bad_provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_FALSE(vres.matches);
}

class InvertReloadCorpusTest : public InvertReloadTest,
                               public ::testing::WithParamInterface<RoundTripTestCase> {};

TEST_P(InvertReloadCorpusTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  bool found_reload = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      found_reload = true;
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << "Case " << tc.test_name
                                << " failed VerifyReloadRightInverse: " << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_reload) << "Missing kReload snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(AllCases, InvertReloadCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
