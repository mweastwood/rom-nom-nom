#include "lifter/invert_block_alloc.h"

#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"
#include "lifter/pass04_greg_reload.h"

extern "C" void regclass_init();  // NOLINT(readability-identifier-naming)

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;

// Inverse provider that ONLY wires `InvertBlockAlloc`, leaving all other 25
// subfunction methods as empty stubs (`SubfunctionInverseProvider` defaults).
class BlockAllocOnlyInverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertBlockAlloc(x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& x,
                                       const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertBlockAlloc(x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

// Corrupted inverse provider for negative right-inverse verification testing:
// corrupts the instruction pattern so replaying `kBlockAlloc` fails to match.
class CorruptedBlockAllocInverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& x,
                                       const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertBlockAlloc(x, profile);
    if (!res.ok() || res->rpo_blocks.empty() || res->rpo_blocks[0].instructions.empty()) {
      return RtlControlFlowGraph{};
    }
    res->rpo_blocks[0].instructions[0] = "(set (reg/i:SI 2 v0) (const_int 999))";
    res->rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return *res;
  }
  using SubfunctionInverseProvider::InvertBlockAlloc;
};

class InvertBlockAllocTest : public ::testing::Test {
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

GccContext* InvertBlockAllocTest::ctx = nullptr;
FunctionHooks* InvertBlockAllocTest::hooks = nullptr;

// =============================================================================
// Positive Synthetic Right-Inverse Tests
// =============================================================================

TEST_F(InvertBlockAllocTest, SingleBlockGprAndFprLocalPseudosRoundTrip) {
  BlockAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kLreg;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 11)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 73) (neg:SF (reg:SF 44 $f12)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 2)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 32 $f0) (reg:SF 73))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg:SF 32 $f0))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, pre_cfg);
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(72));
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(73));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertBlockAlloc(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_TRUE(lifted->sideband.reg_renumber.empty());

  const RightInverseResult res = verifier.VerifyBlockAllocRightInverse(snap.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertBlockAllocTest, MultiBlockLocalPseudosAllocatedWhileCrossBlockPseudoPreserved) {
  BlockAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kLreg;
  pre_cfg.rpo_blocks.resize(3);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 4 a0) (const_int 3)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (ashift:SI (reg:SI 73) (const_int 1)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 72) (const_int 0)) (label_ref L2) (pc)))");
  pre_cfg.rpo_blocks[0].successors = {2, 1};

  pre_cfg.rpo_blocks[1].id = 1;
  pre_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 74) (plus:SI (reg:SI 5 a1) (const_int 5)))");
  pre_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 72) (reg:SI 74)))");
  pre_cfg.rpo_blocks[1].successors = {2};

  pre_cfg.rpo_blocks[2].id = 2;
  pre_cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 75) (plus:SI (reg:SI 72) (const_int 9)))");
  pre_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 75))");
  pre_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, pre_cfg);
  EXPECT_FALSE(snap.post_state.sideband.reg_renumber.contains(72));
  EXPECT_TRUE(snap.post_state.sideband.reg_renumber.contains(73));
  EXPECT_TRUE(snap.post_state.sideband.reg_renumber.contains(74));
  EXPECT_TRUE(snap.post_state.sideband.reg_renumber.contains(75));

  const RightInverseResult res = verifier.VerifyBlockAllocRightInverse(snap.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertBlockAllocTest, ScratchClobbersAndSharedRegUnusedNotesRestoredAndRoundTrip) {
  BlockAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  RtlControlFlowGraph raw_cfg;
  raw_cfg.pass = RtlPassKind::kFlow;
  raw_cfg.rpo_blocks.resize(1);
  raw_cfg.rpo_blocks[0].id = 0;
  raw_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel [(set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "(clobber (scratch:SI)) (clobber (scratch:SI))])");
  raw_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))");
  raw_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 65 lo) (reg:SI 72)))");
  raw_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot flow_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, raw_cfg);
  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, flow_snap.post_state);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);
  EXPECT_TRUE(snap.post_state.sideband.reg_renumber.contains(72));
  EXPECT_EQ(snap.post_state.rpo_blocks[0].instructions[0],
            "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
            "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) "
            "(expr_list:REG_UNUSED (reg:SI 64 hi) "
            "(expr_list:REG_UNUSED (reg:SI 66 accum) (nil))))");

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertBlockAlloc(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_TRUE(lifted->sideband.reg_renumber.empty());
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[0],
            "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
            "(clobber (scratch:SI)) (clobber (scratch:SI)) ] ) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) "
            "(expr_list:REG_UNUSED (scratch:SI) "
            "(expr_list:REG_UNUSED (scratch:SI) (nil))))");

  const RightInverseResult res = verifier.VerifyBlockAllocRightInverse(snap.post_state);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertBlockAllocTest, PrunesLocalOnlyCalleeSavedRegsAndSupportsO0Profile) {
  BlockAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_callee_saved");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);

  // Construct `pre_cfg` with single-block call-crossing pseudos `72` (`SImode`) and
  // `73` (`DFmode`) where `4 * calls >= refs` (`4 * 1 >= 2`), so `block_alloc`
  // genuinely assigns `$16` (`$s0`) and `$52` (`$f20..$f21`), while `$17` (`$s1`)
  // is directly referenced in the RTL stream.
  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kLreg;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:DF 73) (reg:DF 44 $f12))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"ext_fn\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (mem:DF (reg:SI 17 s1)) (reg:DF 73))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 17 s1)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot flow_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, pre_cfg, {}, 2, {"-O2"});
  RtlControlFlowGraph flow_cfg = flow_snap.post_state;
  flow_cfg.pass = RtlPassKind::kLreg;
  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, flow_cfg, {}, 2, {"-O2"});
  ASSERT_EQ(snap.post_state.sideband.reg_renumber.at(72), 16);
  ASSERT_EQ(snap.post_state.sideband.reg_renumber.at(73), 52);

  RtlControlFlowGraph lreg_with_saved = snap.post_state;
  lreg_with_saved.sideband.saved_gpr_regnos = {16, 17};
  lreg_with_saved.sideband.saved_fpr_regnos = {52, 53};

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertBlockAlloc(lreg_with_saved, o2_profile);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.saved_gpr_regnos, std::vector<int>({17}));
  EXPECT_TRUE(lifted->sideband.saved_fpr_regnos.empty());

  hooks->ApplyCompilationProfile(o2_profile);
  const RightInverseResult ba_res = verifier.VerifyBlockAllocRightInverse(snap.post_state);
  EXPECT_TRUE(ba_res.matches) << ba_res.diff_detail;
  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;

  const FunctionCompilationTrace o0_trace = hooks->TraceForwardCompilation(
      "int o0_reg_fn(int a, int b) { register int x = a + b; return x * 2; }\n", "o0_reg_fn",
      {"-O0"}, "block_alloc_o0");
  ASSERT_TRUE(o0_trace.pass_snapshots.contains(RtlPassKind::kLreg));
  ctx->BeginFunction("harness_o0");
  hooks->ApplyCompilationProfile(o0_trace.stage0_input.profile);
  const RightInverseResult o0_res =
      verifier.VerifyBlockAllocRightInverse(o0_trace.pass_snapshots.at(RtlPassKind::kLreg));
  EXPECT_TRUE(o0_res.matches) << o0_res.diff_detail;
}

TEST_F(InvertBlockAllocTest, PreservesCallClobberRaPseudoClobbersAndFlowSideband) {
  BlockAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  RtlControlFlowGraph raw_cfg;
  raw_cfg.pass = RtlPassKind::kFlow;
  raw_cfg.rpo_blocks.resize(1);
  raw_cfg.rpo_blocks[0].id = 0;
  raw_cfg.rpo_blocks[0].AddPatternOnly(
      "(call_insn (parallel [(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI \"callee\")) "
      "(const_int 16))) (clobber (reg:SI 31 $31))]))");
  raw_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel [(set (reg:DI 72) (plus:DI (reg:DI 4 a0) (reg:DI 6 a2))) "
      "(clobber (reg:SI 74))])");
  raw_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (subreg:SI (reg:DI 72) 1))");
  raw_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot flow_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, raw_cfg);
  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, flow_snap.post_state);
  ASSERT_EQ(snap.post_state.BlockCount(), 1u);
  EXPECT_TRUE(snap.post_state.sideband.reg_renumber.contains(72));
  EXPECT_TRUE(snap.post_state.sideband.reg_renumber.contains(74));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertBlockAlloc(snap.post_state);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_TRUE(lifted->sideband.reg_renumber.empty());
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[0], snap.post_state.rpo_blocks[0].instructions[0]);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[1], snap.post_state.rpo_blocks[0].instructions[1]);
  EXPECT_EQ(lifted->sideband.reg_n_refs, snap.post_state.sideband.reg_n_refs);
  EXPECT_EQ(lifted->sideband.reg_n_sets, snap.post_state.sideband.reg_n_sets);
  EXPECT_EQ(lifted->sideband.reg_n_deaths, snap.post_state.sideband.reg_n_deaths);
  EXPECT_EQ(lifted->sideband.reg_live_length, snap.post_state.sideband.reg_live_length);
  EXPECT_EQ(lifted->sideband.reg_n_calls_crossed, snap.post_state.sideband.reg_n_calls_crossed);
  EXPECT_EQ(lifted->sideband.basic_block_live_at_start,
            snap.post_state.sideband.basic_block_live_at_start);

  const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(snap_res.matches) << snap_res.diff_detail;

  const RightInverseResult direct_res = verifier.VerifyBlockAllocRightInverse(snap.post_state);
  EXPECT_TRUE(direct_res.matches) << direct_res.diff_detail;
}

// =============================================================================
// Negative Input & Verification Tests
// =============================================================================

TEST_F(InvertBlockAllocTest, RejectsEmptyCfg) {
  RtlControlFlowGraph empty_cfg;
  absl::StatusOr<RtlControlFlowGraph> res = InvertBlockAlloc(empty_cfg);
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(InvertBlockAllocTest, RejectsMalformedSexprAndUnknownRtlOp) {
  RtlControlFlowGraph bad_sexpr;
  bad_sexpr.rpo_blocks.resize(1);
  bad_sexpr.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0)");
  EXPECT_FALSE(InvertBlockAlloc(bad_sexpr).ok());

  RtlControlFlowGraph unknown_op;
  unknown_op.rpo_blocks.resize(1);
  unknown_op.rpo_blocks[0].AddPatternOnly("(bogus_rtl_op (reg:SI 72))");
  EXPECT_FALSE(InvertBlockAlloc(unknown_op).ok());
}

TEST_F(InvertBlockAllocTest, RejectsOutOfBoundsSuccessorsAndAddrVecTargets) {
  RtlControlFlowGraph bad_succ;
  bad_succ.rpo_blocks.resize(1);
  bad_succ.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_succ.rpo_blocks[0].successors = {3};
  EXPECT_FALSE(InvertBlockAlloc(bad_succ).ok());

  RtlControlFlowGraph bad_addr_vec;
  bad_addr_vec.rpo_blocks.resize(1);
  bad_addr_vec.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  bad_addr_vec.rpo_blocks[0].addr_vec_targets = {2};
  EXPECT_FALSE(InvertBlockAlloc(bad_addr_vec).ok());
}

TEST_F(InvertBlockAllocTest, RejectsInvalidSidebandRegisterAndBlockNumbers) {
  RtlControlFlowGraph base;
  base.rpo_blocks.resize(1);
  base.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");

  RtlControlFlowGraph bad_renum_key = base;
  bad_renum_key.sideband.reg_renumber[2] = 4;
  EXPECT_FALSE(InvertBlockAlloc(bad_renum_key).ok());

  RtlControlFlowGraph bad_fcr31_key = base;
  bad_fcr31_key.sideband.reg_renumber[kMipsStatusRegnum] = 2;
  EXPECT_FALSE(InvertBlockAlloc(bad_fcr31_key).ok());

  RtlControlFlowGraph bad_renum_val = base;
  bad_renum_val.sideband.reg_renumber[72] = 75;
  EXPECT_FALSE(InvertBlockAlloc(bad_renum_val).ok());

  RtlControlFlowGraph bad_deaths = base;
  bad_deaths.sideband.reg_n_deaths[-1] = 1;
  EXPECT_FALSE(InvertBlockAlloc(bad_deaths).ok());

  RtlControlFlowGraph bad_live_bb = base;
  bad_live_bb.sideband.basic_block_live_at_start[-1] = {4};
  EXPECT_FALSE(InvertBlockAlloc(bad_live_bb).ok());
}

TEST_F(InvertBlockAllocTest, CorruptedInverseIsRejectedByRightInverseVerifier) {
  CorruptedBlockAllocInverseProvider bad_provider;
  RightInverseVerifier bad_verifier(ctx, hooks, &bad_provider);
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kLreg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 42))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RightInverseResult res = bad_verifier.VerifyBlockAllocRightInverse(cfg);
  EXPECT_FALSE(res.matches);
}

TEST_F(InvertBlockAllocTest, RestoredMatchScratchClobberSubtractsHardRegSetAndDeathCounts) {
  // `flow.c:2046-2047` (`mark_set_1`) and `local-alloc.c:1396-1401`
  // (`alloc_qty_for_scratch`): a pre-`local_alloc` `(clobber (scratch:SI))`
  // operand in a MIPS `match_scratch` parallel (such as `fix_truncsfsi2` /
  // `fix_truncdfsi2`) is not counted in `reg_n_sets[hr]` / `reg_n_deaths[hr]`.
  // When `InvertBlockAlloc` restores `(clobber (reg:SI hr))` back to
  // `(clobber (scratch:SI))`, it must subtract the scratch's set/death counts
  // while leaving unrelated non-scratch counts untouched.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  auto make_cfg = [](bool is_match_scratch_parallel) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kLreg;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 72) (reg:SF 44 $f12))");
    if (is_match_scratch_parallel) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(parallel[ (set (reg:SI 73) (fix:SI (reg:SF 72))) "
          "(clobber (reg:SF 32 $f0)) (clobber (reg:SI 8 t0)) ] ) "
          "(expr_list:REG_DEAD (reg:SF 72) "
          "(expr_list:REG_UNUSED (reg:SF 32 $f0) "
          "(expr_list:REG_UNUSED (reg:SI 8 t0) (nil))))");
    } else {
      // Non-scratch single `SET` writing `$t0` directly: not restored to
      // `(scratch:SI)`, so `reg_n_sets[8]` and `reg_n_deaths[8]` remain intact.
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (const_int 1))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 73) (plus:SI (reg:SI 8 t0) (const_int 2))) "
          "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
    }
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    cfg.sideband.reg_renumber[72] = 44;
    cfg.sideband.reg_renumber[73] = 2;
    cfg.sideband.reg_n_sets[8] = 1;
    cfg.sideband.reg_n_deaths[8] = 1;
    cfg.sideband.reg_n_sets[32] = 1;
    cfg.sideband.reg_n_deaths[32] = 1;
    cfg.sideband.reg_n_sets[2] = 1;
    return cfg;
  };

  // Positive: restoring `(clobber (reg:SF 32 $f0))` and `(clobber (reg:SI 8 t0))`
  // removes their scratch counts while preserving `reg_n_sets[2]`.
  {
    const auto lifted = InvertBlockAlloc(make_cfg(/*is_match_scratch_parallel=*/true), o2_prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    EXPECT_NE(lifted->rpo_blocks[0].instructions[1].find("(clobber (scratch:SF))"),
              std::string::npos);
    EXPECT_NE(lifted->rpo_blocks[0].instructions[1].find("(clobber (scratch:SI))"),
              std::string::npos);
    EXPECT_FALSE(lifted->sideband.reg_n_sets.contains(8));
    EXPECT_FALSE(lifted->sideband.reg_n_deaths.contains(8));
    EXPECT_FALSE(lifted->sideband.reg_n_sets.contains(32));
    EXPECT_FALSE(lifted->sideband.reg_n_deaths.contains(32));
    EXPECT_EQ(lifted->sideband.reg_n_sets.at(2), 1);
  }

  // Negative: genuine non-scratch `$t0` sets/deaths are preserved verbatim.
  {
    const auto lifted = InvertBlockAlloc(make_cfg(/*is_match_scratch_parallel=*/false), o2_prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    ASSERT_TRUE(lifted->sideband.reg_n_sets.contains(8));
    EXPECT_EQ(lifted->sideband.reg_n_sets.at(8), 1);
    ASSERT_TRUE(lifted->sideband.reg_n_deaths.contains(8));
    EXPECT_EQ(lifted->sideband.reg_n_deaths.at(8), 1);
  }
}

// =============================================================================
// 4,172-Case Parameterized Round-Trip Verification Suite (Standalone + Fresh)
// =============================================================================

class InvertBlockAllocRoundTripTest : public InvertBlockAllocTest,
                                      public ::testing::WithParamInterface<RoundTripTestCase> {};

TEST_P(InvertBlockAllocRoundTripTest, StandaloneAndFreshStage0To4AcrossAll4172Cases) {
  const RoundTripTestCase& tc = GetParam();
  ctx->BeginFunction("harness_init");
  hooks->ApplyCompilationProfile({2, tc.options.c_flags, {}});
  regclass_init();
  const FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << tc.test_name;

  BlockAllocOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  int verified_count = 0;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kBlockAlloc) {
      continue;
    }
    const RightInverseResult snap_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(snap_res.matches) << "Snapshot verification failed for " << tc.test_name << ": "
                                  << snap_res.diff_detail;
    ++verified_count;
  }
  EXPECT_GT(verified_count, 0) << "No kBlockAlloc snapshot captured for " << tc.test_name;

  // Fresh cumulative Stage 0..4 `.08.lreg` boundary check:
  const RtlControlFlowGraph p0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(p0.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p1 = InvertPass01DbrSchedule(p0, trace.stage0_input.profile);
  ASSERT_FALSE(p1.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p2 = InvertPass02Jump2CrossJump(p1, trace.stage0_input.profile);
  ASSERT_FALSE(p2.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p3 = InvertPass03Sched2PostReload(p2, trace.stage0_input.profile);
  ASSERT_FALSE(p3.rpo_blocks.empty()) << tc.test_name;
  const RtlControlFlowGraph p4 = InvertPass04GregReload(p3, trace.stage0_input.profile);
  ASSERT_FALSE(p4.rpo_blocks.empty()) << tc.test_name;

  ctx->BeginFunction("harness_fresh_block_alloc");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  const RightInverseResult fresh_res = verifier.VerifyBlockAllocRightInverse(p4);
  EXPECT_TRUE(fresh_res.matches) << "Fresh kBlockAlloc right-inverse failed for " << tc.test_name
                                 << ":\n"
                                 << fresh_res.diff_detail;
}

INSTANTIATE_TEST_SUITE_P(AllRoundTripCases, InvertBlockAllocRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
