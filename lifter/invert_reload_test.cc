#include "lifter/invert_reload.h"

#include <string>
#include <vector>

#include "absl/status/statusor.h"
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
    return res.ok() ? *res : RtlControlFlowGraph{};
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
  reload_cfg.sideband.max_regno = 68;
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
  bad_sideband_cfg.sideband.reg_renumber[10] = 2;  // Not a pseudo register (< 68).
  EXPECT_FALSE(InvertReload(bad_sideband_cfg).ok());

  RtlControlFlowGraph fcr31_sideband_cfg = bad_sideband_cfg;
  fcr31_sideband_cfg.sideband.reg_renumber.clear();
  fcr31_sideband_cfg.sideband.reg_renumber[67] = 2;  // $fcr31 (67) is a hard register (< 68).
  EXPECT_FALSE(InvertReload(fcr31_sideband_cfg).ok());
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

TEST_F(InvertReloadTest, ReversesCallerSaveSaveRestorePairsAndRestoresFrameOffset) {
  RtlControlFlowGraph pre_reload_cfg;
  pre_reload_cfg.pass = RtlPassKind::kLreg;
  pre_reload_cfg.sideband.frame_pointer_needed = false;
  pre_reload_cfg.sideband.outgoing_args_size = 16;
  pre_reload_cfg.sideband.frame_size_bytes = 16;
  pre_reload_cfg.sideband.reg_renumber[72] = 8;  // $t0 (call-clobbered)
  pre_reload_cfg.sideband.reg_n_calls_crossed[72] = 1;
  pre_reload_cfg.sideband.reg_n_refs[72] = 4;
  pre_reload_cfg.sideband.reg_live_length[72] = 4;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.AddPatternOnly("(set (reg:SI 72) (const_int 11))");
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  call_insn.call_fusage = {};
  bb0.AddInsn(std::move(call_insn));
  bb0.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 72))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  bb0.AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_reload_cfg.rpo_blocks.push_back(std::move(bb0));

  const SubfunctionBoundarySnapshot reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, pre_reload_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2"});
  ASSERT_GT(reload_snap.post_state.rpo_blocks[0].instructions.size(), 4u)
      << reload_snap.post_state.FormatSummary();
  EXPECT_GT(reload_snap.post_state.sideband.frame_size_bytes, 16);

  absl::StatusOr<RtlControlFlowGraph> pre_or = InvertReload(reload_snap.post_state);
  ASSERT_TRUE(pre_or.ok()) << pre_or.status();
  EXPECT_EQ(pre_or->rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(pre_or->sideband.frame_size_bytes, 16);

  ReloadOnlyInverseProvider good_provider;
  RightInverseVerifier good_verifier(ctx, hooks, &good_provider);
  const RightInverseResult pos_sub_res =
      good_verifier.VerifySubfunctionBoundaryRightInverse(reload_snap);
  EXPECT_TRUE(pos_sub_res.matches) << pos_sub_res.diff_detail;
  const RightInverseResult pos_stage_res =
      good_verifier.VerifyReloadRightInverse(reload_snap.post_state);
  EXPECT_TRUE(pos_stage_res.matches) << pos_stage_res.diff_detail;

  // Adversarial negative: failing to strip the caller-save save/restore pairs or corrupting the
  // pre-image must fail `RightInverseVerifier`.
  class NoOpCallerSaveReloadProvider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& x) const override { return x; }
  };
  NoOpCallerSaveReloadProvider noop_provider;
  RightInverseVerifier noop_verifier(ctx, hooks, &noop_provider);
  EXPECT_FALSE(noop_verifier.VerifySubfunctionBoundaryRightInverse(reload_snap).matches);
}

TEST_F(InvertReloadTest, ReconstructsSpilledPseudosAlongsideCallerSaveAcrossOutgoingArgCall) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int Dispatch8Args(int a, int b, int c, int d, int e, int f, int g, int h);\n"
      "int SpillAndCallerSave(int* p) {\n"
      "  int v0 = p[0], v1 = p[1], v2 = p[2], v3 = p[3], v4 = p[4], v5 = p[5];\n"
      "  int v6 = p[6], v7 = p[7], v8 = p[8], v9 = p[9], v10 = p[10], v11 = p[11];\n"
      "  int c0 = v0 ^ v1, c1 = v2 ^ v3, c2 = v4 ^ v5, c3 = v6 ^ v7;\n"
      "  int r = Dispatch8Args(c0, c1, c2, c3, v8 ^ v9, v10 ^ v11, v0 + v6, v1 + v7);\n"
      "  return r + v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11;\n"
      "}\n",
      "SpillAndCallerSave", {"-O2", "-mips2", "-mcpu=r4000"});

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

TEST_F(InvertReloadTest, ReconstructsActiveSpilledPseudoAlongsidePhantomAlterRegPseudos) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int ProbeStep(int iter, int acc);\n"
      "int LoopSpillAndCallerSave(const int* base, int unused_arg, int steps) {\n"
      "  int k0 = base[0], k1 = base[1], k2 = base[2], k3 = base[3], k4 = base[4], k5 = base[5];\n"
      "  int a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6;\n"
      "  int b0 = 7, b1 = 8, b2 = 9, b3 = 10, b4 = 11, b5 = 12;\n"
      "  int i;\n"
      "  for (i = 0; i < steps; i++) {\n"
      "    int p = ProbeStep(i, a0 + b5);\n"
      "    a0 = (a0 + k0) ^ p; a1 = (a1 + k1) + a0; a2 = (a2 ^ k2) + a1;\n"
      "    a3 = (a3 + k3) ^ a2; a4 = (a4 - k4) + a3; a5 = (a5 + k5) ^ a4;\n"
      "    b0 = (b0 ^ a5) + k5; b1 = (b1 + b0) ^ k4; b2 = (b2 - b1) + k3;\n"
      "    b3 = (b3 ^ b2) + k2; b4 = (b4 + b3) ^ k1; b5 = (b5 - b4) + k0;\n"
      "  }\n"
      "  return a0 + a1 + a2 + a3 + a4 + a5 + b0 + b1 + b2 + b3 + b4 + b5;\n"
      "}\n",
      "LoopSpillAndCallerSave", {"-O2", "-mips2", "-mcpu=r4000"});

  const SubfunctionBoundarySnapshot* base_reload = nullptr;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      base_reload = &snap;
      break;
    }
  }
  ASSERT_NE(base_reload, nullptr);

  // Inject both a lower-numbered phantom pseudo (`73 > LAST_VIRTUAL_REGISTER`
  // and `< 74`, exercising `phantoms_before_min_active`) and an additional
  // higher-numbered phantom pseudo (`max_regno + 2`, exercising trailing
  // phantom `alter_reg` slots).
  RtlControlFlowGraph pre_cfg = base_reload->pre_state;
  const int base_frame = pre_cfg.sideband.frame_size_bytes;
  constexpr int kLowPhantomPseudo = 73;
  ASSERT_FALSE(pre_cfg.sideband.reg_renumber.contains(kLowPhantomPseudo));
  const int high_phantom_pseudo = pre_cfg.sideband.max_regno + 2;
  pre_cfg.sideband.max_regno = high_phantom_pseudo + 1;
  pre_cfg.sideband.reg_n_refs[kLowPhantomPseudo] = 2;
  pre_cfg.sideband.reg_live_length[kLowPhantomPseudo] = 2;
  pre_cfg.sideband.reg_n_sets.erase(kLowPhantomPseudo);
  pre_cfg.sideband.reg_n_deaths.erase(kLowPhantomPseudo);

  pre_cfg.sideband.reg_n_refs[high_phantom_pseudo] = 2;
  pre_cfg.sideband.reg_live_length[high_phantom_pseudo] = 2;

  const SubfunctionBoundarySnapshot reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, pre_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2", "-mcpu=r4000"});
  // Two extra 8-byte `alter_reg` stack slots (`kLowPhantomPseudo` + `high_phantom_pseudo`).
  EXPECT_EQ(reload_snap.post_state.sideband.frame_size_bytes,
            base_reload->post_state.sideband.frame_size_bytes + 16);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.frame_size_bytes, base_frame);
  ASSERT_TRUE(lifted->sideband.reg_n_refs.contains(kLowPhantomPseudo));
  ASSERT_TRUE(lifted->sideband.reg_n_refs.contains(high_phantom_pseudo));
  EXPECT_EQ(lifted->sideband.reg_n_refs[kLowPhantomPseudo], 2);
  EXPECT_EQ(lifted->sideband.reg_n_refs[high_phantom_pseudo], 2);

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  const RightInverseResult sub_res = verifier.VerifySubfunctionBoundaryRightInverse(reload_snap);
  EXPECT_TRUE(sub_res.matches) << sub_res.diff_detail;
  const RightInverseResult stage_res = verifier.VerifyReloadRightInverse(reload_snap.post_state);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
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
