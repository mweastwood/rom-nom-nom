#include "lifter/rtl_liveness.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gtest/gtest.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

void StripAllLivenessAndSidebandStats(RtlControlFlowGraph* cfg, bool clear_call_fusage = false) {
  cfg->sideband.basic_block_live_at_start.clear();
  cfg->sideband.reg_n_refs.clear();
  cfg->sideband.reg_n_sets.clear();
  cfg->sideband.reg_n_deaths.clear();
  cfg->sideband.reg_live_length.clear();
  cfg->sideband.reg_n_calls_crossed.clear();
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto pn = ParsePatternAndNotes(blk.instructions[i]);
      if (!pn.ok() || pn->pattern.op == RtlOp::kNote) {
        continue;
      }
      pn->reg_notes.erase(std::remove_if(pn->reg_notes.begin(), pn->reg_notes.end(),
                                         [](const RtlRegNote& n) {
                                           return n.kind == RtlMachineMode::kRegDead ||
                                                  n.kind == RtlMachineMode::kRegUnused;
                                         }),
                          pn->reg_notes.end());
      const bool is_call =
          (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage) ||
          pn->pattern.op == RtlOp::kCall ||
          (pn->pattern.IsSet() && pn->pattern.SetSrc().op == RtlOp::kCall) ||
          (pn->pattern.IsParallel() && !pn->pattern.children.empty() &&
           (pn->pattern.children[0].op == RtlOp::kCall ||
            (pn->pattern.children[0].IsSet() &&
             pn->pattern.children[0].SetSrc().op == RtlOp::kCall)));
      if (!is_call) {
        blk.instructions[i] = pn->ToGccPrintRtlString();
      }
    }
    blk.SyncRtlInsnsFromInstructions();
    if (clear_call_fusage) {
      for (RtlInsn& rinsn : blk.rtl_insns) {
        rinsn.has_explicit_call_fusage = false;
        rinsn.call_fusage.clear();
      }
    }
  }
}

TEST(RtlLivenessTest, SyntheticDiamondAndBackEdgeLoopSynthesizesFlowNotesAndLiveSets) {
  // B0 -> {B1, B2}; B1 -> B1 (loop), B2 (exit); B2 returns $2.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kFlow;
  cfg.rpo_blocks.resize(3);

  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {1, 2};
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 80) (reg:SI 4 a0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 81) (reg:SI 5 a1))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 82) (const_int 99)) (expr_list:REG_EQUAL (const_int 99) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (le (reg:SI 81) (const_int 0)) (label_ref 2) (pc)))");

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].has_label = true;
  cfg.rpo_blocks[1].successors = {1, 2};
  cfg.rpo_blocks[1].AddPatternOnly("(note NOTE_INSN_LOOP_BEG)");
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 80) (plus:SI (reg:SI 80) (reg:SI 81)))");
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 81) (plus:SI (reg:SI 81) (const_int -1)))");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (ne (reg:SI 81) (const_int 0)) (label_ref 1) (pc)))");
  cfg.rpo_blocks[1].AddPatternOnly("(note NOTE_INSN_LOOP_END)");

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].has_label = true;
  cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 80))");
  cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  ReconstructRtlLiveness(&cfg, o2_profile, {.reload_completed = false, .update_reg_notes = true});

  EXPECT_EQ(cfg.sideband.max_regno, 83);
  // B0 live-in: incoming hard regs 4, 5 + exit-live sp(29), fp(30).
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[0].contains(4));
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[0].contains(5));
  EXPECT_FALSE(cfg.sideband.basic_block_live_at_start[0].contains(80));
  // B1 (loop header) live-in: pseudos 80 and 81.
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[1].contains(80));
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[1].contains(81));
  // B2 (exit) live-in: pseudo 80 only (81 is dead at exit).
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[2].contains(80));
  EXPECT_FALSE(cfg.sideband.basic_block_live_at_start[2].contains(81));

  // Dead set of 82 in B0 gets `REG_UNUSED` prepended ahead of preserved `REG_EQUAL`!
  EXPECT_EQ(cfg.rpo_blocks[0].instructions[2],
            "(set (reg:SI 82) (const_int 99)) "
            "(expr_list:REG_UNUSED (reg:SI 82) (expr_list:REG_EQUAL (const_int 99) (nil)))");
  // Exit copy in B2 kills pseudo 80 via `REG_DEAD`.
  EXPECT_EQ(cfg.rpo_blocks[2].instructions[0],
            "(set (reg/i:SI 2 v0) (reg:SI 80)) (expr_list:REG_DEAD (reg:SI 80) (nil))");
  // Loop body refs for 80/81 are weighted by `loop_depth == 2`.
  EXPECT_GT(cfg.sideband.reg_n_refs[80], 3);
  EXPECT_EQ(cfg.sideband.reg_n_sets[80], 2);
}

TEST(RtlLivenessTest, MultiWordHardRegPairsAndPartialSubwordDeathsFollowTargetWidthFlags) {
  RtlControlFlowGraph base_cfg;
  base_cfg.pass = RtlPassKind::kGreg;
  base_cfg.rpo_blocks.resize(1);
  base_cfg.rpo_blocks[0].id = 0;
  // Define `DImode` in `$4` (`$4`+`$5` on `-mgp32`, single `$4` on `-mips3`), then read only `$4`!
  base_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:DI 4 a0) (mem:DI (reg:SI 6 a2)))");
  base_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 4 a0))");
  base_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  // 1) 32-bit GPR mode (`-mips2`): `$5` is unused on insn 0 -> partial `REG_UNUSED (reg:SI 5 a1)`!
  RtlControlFlowGraph cfg32 = base_cfg;
  CompilationProfile p32{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  ReconstructRtlLiveness(&cfg32, p32, {.reload_completed = true, .update_reg_notes = true});
  EXPECT_TRUE(cfg32.sideband.basic_block_live_at_start[0].contains(6));
  EXPECT_EQ(cfg32.rpo_blocks[0].instructions[0],
            "(set (reg:DI 4 a0) (mem:DI (reg:SI 6 a2))) "
            "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_UNUSED (reg:SI 5 a1) (nil)))");

  // 2) 64-bit GPR mode (`-mips3`): `(reg:DI 4)` occupies 1 register (`nregs == 1`), so no partial
  // `REG_UNUSED` on `$5`!
  RtlControlFlowGraph cfg64 = base_cfg;
  CompilationProfile p64{.optimize_level = 2, .c_flags = {"-O2", "-mips3"}};
  ReconstructRtlLiveness(&cfg64, p64, {.reload_completed = true, .update_reg_notes = true});
  EXPECT_EQ(cfg64.rpo_blocks[0].instructions[0],
            "(set (reg:DI 4 a0) (mem:DI (reg:SI 6 a2))) "
            "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
}

TEST(RtlLivenessTest,
     ReconstructsMissingInterBlockCallFusageAndTracksCallsCrossedWithoutCallNotes) {
  // B0 sets `$4`, branches to B1/B2; B1 sets `$5`; B2 sets `$5`; B3 calls `callee` without explicit
  // `call_fusage`, while pseudo `90` lives across the call!
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kFlow;
  cfg.rpo_blocks.resize(4);

  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {1, 2};
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 90) (reg:SI 6 a2))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 4 a0) (reg:SI 7 a3))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq (reg:SI 90) (const_int 0)) (label_ref 2) (pc)))");

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].successors = {3};
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 5 a1) (const_int 10))");
  cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref 3))");

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].has_label = true;
  cfg.rpo_blocks[2].successors = {3};
  cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 5 a1) (const_int 20))");

  cfg.rpo_blocks[3].id = 3;
  cfg.rpo_blocks[3].has_label = true;
  cfg.rpo_blocks[3].AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");
  cfg.rpo_blocks[3].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 90)))");
  cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");

  ASSERT_FALSE(cfg.rpo_blocks[3].rtl_insns[0].has_explicit_call_fusage);

  CompilationProfile p{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  ReconstructRtlLiveness(&cfg, p, {.reload_completed = false, .update_reg_notes = true});

  // `call_fusage` was reconstructed on B3[0] for `$4` and `$5`, and `CALL_INSN` pattern string
  // never had `REG_NOTES` appended!
  EXPECT_TRUE(cfg.rpo_blocks[3].rtl_insns[0].has_explicit_call_fusage);
  EXPECT_EQ(cfg.rpo_blocks[3].rtl_insns[0].call_fusage.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[3].instructions[0],
            "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
            "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
  // Both `$4` and `$5` are live at entry to B3, along with call-crossing pseudo `90`.
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[3].contains(4));
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[3].contains(5));
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[3].contains(90));
  EXPECT_EQ(cfg.sideband.reg_n_calls_crossed[90], 1);
}

TEST(RtlLivenessTest, O0ProfileUsesStupidLifeAnalysisExplicitlyWithoutSynthesizingNotes) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kFlow;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 75) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"foo\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 75))) "
      "(expr_list:REG_DEAD (reg:SI 75) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  CompilationProfile o0_profile{.optimize_level = 0, .c_flags = {"-O0"}};
  ReconstructRtlLiveness(&cfg, o0_profile, {.reload_completed = false, .update_reg_notes = true});

  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start.empty());
  EXPECT_TRUE(cfg.sideband.reg_n_sets.empty());
  EXPECT_TRUE(cfg.sideband.reg_live_length.empty());
  for (int r = 0; r < cfg.sideband.max_regno; ++r) {
    EXPECT_EQ(cfg.sideband.reg_n_deaths[r], 1);
  }
  EXPECT_EQ(cfg.sideband.reg_n_refs[75], 2);
  EXPECT_EQ(cfg.sideband.reg_n_calls_crossed[75], 1);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions[0], "(set (reg:SI 75) (reg:SI 4 a0))");
  EXPECT_EQ(cfg.rpo_blocks[0].instructions[2],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 75)))");
}

TEST(RtlLivenessTest, PostReloadStandaloneRegLibcallClobberPreservesCoalescedInputSubword) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  // Standalone `(clobber (reg:DI 2 v0))` tagged with `REG_LIBCALL` (`optabs.c:2488,2521`
  // `emit_no_conflict_block`) where `global_alloc` coalesced an incoming `SImode` input in `$3`
  // (`v1`) into the low subword of `(reg:DI 2 v0)`: `$3` remains live at block entry while `$2`
  // (written before any read) is not live at block entry.
  cfg.rpo_blocks[0].AddPatternOnly(
      "(clobber (reg:DI 2 v0)) (insn_list:REG_LIBCALL (insn 2) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1)))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  ReconstructRtlLiveness(&cfg, o2_profile, {.reload_completed = true, .update_reg_notes = false});

  EXPECT_FALSE(cfg.sideband.basic_block_live_at_start[0].contains(2));
  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[0].contains(3));
}

TEST(RtlLivenessTest, PostReloadSubregMultiwordHardRegOverwriteDoesNotReadOldSubword) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  // Post-reload `zero_extendsidi2` / `extendsidi2` write `(subreg:SI (reg:DI 8 t0) 0)`
  // and `(subreg:SI (reg:DI 8 t0) 1)`, completely overwriting hard registers `$8` and
  // `$9` without reading their prior values.
  cfg.rpo_blocks[0].AddPatternOnly("(set (subreg:SI (reg:DI 8 t0) 0) (const_int 0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (subreg:SI (reg:DI 8 t0) 1) (reg:SI 4 a0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (subreg:SI (reg:DI 8 t0) 1))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  ReconstructRtlLiveness(&cfg, o2_profile, {.reload_completed = true, .update_reg_notes = false});

  EXPECT_TRUE(cfg.sideband.basic_block_live_at_start[0].contains(4));
  EXPECT_FALSE(cfg.sideband.basic_block_live_at_start[0].contains(8));
  EXPECT_FALSE(cfg.sideband.basic_block_live_at_start[0].contains(9));
}

class RtlLivenessRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new gcc_harness::GccContext();
    hooks = new gcc_harness::FunctionHooks(ctx);
  }

  static void TearDownTestSuite() {
    delete hooks;
    delete ctx;
  }

  static gcc_harness::GccContext* ctx;
  static gcc_harness::FunctionHooks* hooks;
};

gcc_harness::GccContext* RtlLivenessRoundTripTest::ctx = nullptr;
gcc_harness::FunctionHooks* RtlLivenessRoundTripTest::hooks = nullptr;

TEST_P(RtlLivenessRoundTripTest, RoundTrip4172Cases) {
  const RoundTripTestCase& tc = GetParam();
  const gcc_harness::FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);
  const CompilationProfile& profile = trace.stage0_input.profile;

  // 1. Pre-reload `.05.flow` exact comparison (notes + live sets + flow stats).
  ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kFlow)) << tc.test_name;
  const RtlControlFlowGraph& expected_flow = trace.pass_snapshots.at(RtlPassKind::kFlow);
  RtlControlFlowGraph rebuilt_flow = expected_flow;
  StripAllLivenessAndSidebandStats(&rebuilt_flow);

  ReconstructRtlLiveness(&rebuilt_flow, profile,
                         {.reload_completed = false, .update_reg_notes = true});

  const RtlCfgPassComparison cmp_flow = rebuilt_flow.CompareAgainst(expected_flow);
  EXPECT_TRUE(cmp_flow.MatchesExactly())
      << "Flow RTL mismatch on " << tc.test_name << ": " << cmp_flow.mismatch_detail;
  if (profile.optimize_level == 0) {
    EXPECT_TRUE(rebuilt_flow.sideband.basic_block_live_at_start.empty()) << tc.test_name;
  } else {
    // Materialize `rebuilt_flow` (already in canonical RPO block order) and run
    // GCC's real `flow_analysis` so `basic_block_live_at_start` is keyed by the
    // exact same RPO block indices (including switch `addr_vec` cases).
    gcc_harness::Rtx flow_head = ctx->MaterializeFromCfg(rebuilt_flow);
    hooks->RunFlowAnalysis(flow_head, 0);
    const RtlControlFlowGraph gcc_rpo_flow = ctx->DematerializeToCfg(flow_head, RtlPassKind::kFlow);
    EXPECT_EQ(rebuilt_flow.sideband.basic_block_live_at_start,
              gcc_rpo_flow.sideband.basic_block_live_at_start)
        << "Flow live_at_start mismatch on " << tc.test_name;
  }
  EXPECT_EQ(rebuilt_flow.sideband.reg_n_refs, expected_flow.sideband.reg_n_refs)
      << "Flow reg_n_refs mismatch on " << tc.test_name;
  EXPECT_EQ(rebuilt_flow.sideband.reg_n_sets, expected_flow.sideband.reg_n_sets)
      << "Flow reg_n_sets mismatch on " << tc.test_name;
  EXPECT_EQ(rebuilt_flow.sideband.reg_n_deaths, expected_flow.sideband.reg_n_deaths)
      << "Flow reg_n_deaths mismatch on " << tc.test_name;
  EXPECT_EQ(rebuilt_flow.sideband.reg_live_length, expected_flow.sideband.reg_live_length)
      << "Flow reg_live_length mismatch on " << tc.test_name;
  EXPECT_EQ(rebuilt_flow.sideband.reg_n_calls_crossed, expected_flow.sideband.reg_n_calls_crossed)
      << "Flow reg_n_calls_crossed mismatch on " << tc.test_name;

  // 2. Post-reload `.10.sched2` liveness preservation & reconstruction with
  // `update_reg_notes = false`.
  ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kSched2)) << tc.test_name;
  const RtlControlFlowGraph& expected_sched2 = trace.pass_snapshots.at(RtlPassKind::kSched2);
  RtlControlFlowGraph rebuilt_sched2 = expected_sched2;
  rebuilt_sched2.sideband.basic_block_live_at_start.clear();
  rebuilt_sched2.sideband.reg_n_refs.clear();
  rebuilt_sched2.sideband.reg_n_sets.clear();
  rebuilt_sched2.sideband.reg_n_deaths.clear();
  rebuilt_sched2.sideband.reg_live_length.clear();
  rebuilt_sched2.sideband.reg_n_calls_crossed.clear();
  ReconstructRtlLiveness(&rebuilt_sched2, profile,
                         {.reload_completed = true, .update_reg_notes = false});
  const RtlCfgPassComparison cmp_sched2 = rebuilt_sched2.CompareAgainst(expected_sched2);
  EXPECT_TRUE(cmp_sched2.MatchesExactly())
      << "Sched2 insn mismatch on " << tc.test_name << ": " << cmp_sched2.mismatch_detail;
  if (profile.optimize_level == 0) {
    EXPECT_TRUE(rebuilt_sched2.sideband.basic_block_live_at_start.empty()) << tc.test_name;
  } else {
    RtlControlFlowGraph ref_sched2 = expected_sched2;
    ref_sched2.sideband.basic_block_live_at_start = {{0, {}}};
    RecomputePostReloadLiveAtStart(&ref_sched2, profile);
    EXPECT_EQ(rebuilt_sched2.sideband.basic_block_live_at_start,
              ref_sched2.sideband.basic_block_live_at_start)
        << "Sched2 live_at_start mismatch on " << tc.test_name;
  }

  // Also verify that stripping `call_fusage` and running `ReconstructRtlLiveness`
  // populates explicit `call_fusage` on every `CALL_INSN` without mutating any
  // body instruction or `REG_NOTES`.
  RtlControlFlowGraph stripped_fusage_sched2 = expected_sched2;
  for (RtlBasicBlock& blk : stripped_fusage_sched2.rpo_blocks) {
    for (RtlInsn& rinsn : blk.rtl_insns) {
      rinsn.has_explicit_call_fusage = false;
      rinsn.call_fusage.clear();
    }
  }
  ReconstructRtlLiveness(&stripped_fusage_sched2, profile,
                         {.reload_completed = true, .update_reg_notes = false});
  for (size_t b = 0; b < stripped_fusage_sched2.rpo_blocks.size(); ++b) {
    EXPECT_EQ(stripped_fusage_sched2.rpo_blocks[b].instructions,
              expected_sched2.rpo_blocks[b].instructions)
        << tc.test_name << " block " << b;
    for (size_t i = 0; i < stripped_fusage_sched2.rpo_blocks[b].rtl_insns.size(); ++i) {
      if (expected_sched2.rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage) {
        EXPECT_TRUE(stripped_fusage_sched2.rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage)
            << tc.test_name << " block " << b << " insn " << i;
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, RtlLivenessRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

TEST(RtlLivenessTest, PreReloadVsPostReloadHardSubregAndAsmOperandsVectorOrder) {
  CompilationProfile profile;
  profile.optimize_level = 2;
  profile.c_flags = {"-O2", "-mips2"};

  // 1. `(subreg:SI (reg:DI 4 a0) 1)` read:
  // - Pre-reload (`reload_completed == false`), `flow.c:2395-2468` unwraps `SUBREG` to `(reg:DI 4)`
  //   and marks BOTH `$4` and `$5` live at block entry (plus exit-fixed `$29` and `$30`).
  // - Post-reload (`reload_completed == true`), only `$5` is read and live at block entry.
  RtlControlFlowGraph subreg_use_cfg;
  subreg_use_cfg.pass = RtlPassKind::kFlow;
  RtlBasicBlock b0;
  b0.id = 0;
  b0.instructions = {
      "(set (reg:SI 2 v0) (subreg:SI (reg:DI 4 a0) 1))",
      "(use (reg/i:SI 2 v0))",
  };
  b0.SyncRtlInsnsFromInstructions();
  subreg_use_cfg.rpo_blocks = {b0};

  RtlControlFlowGraph pre_subreg = subreg_use_cfg;
  ReconstructRtlLiveness(&pre_subreg, profile,
                         {.reload_completed = false, .update_reg_notes = true});
  ASSERT_TRUE(pre_subreg.sideband.basic_block_live_at_start.contains(0));
  EXPECT_TRUE(pre_subreg.sideband.basic_block_live_at_start[0].contains(4));
  EXPECT_TRUE(pre_subreg.sideband.basic_block_live_at_start[0].contains(5));
  EXPECT_TRUE(pre_subreg.sideband.basic_block_live_at_start[0].contains(30));

  RtlControlFlowGraph post_subreg = subreg_use_cfg;
  post_subreg.pass = RtlPassKind::kSched2;
  ReconstructRtlLiveness(&post_subreg, profile,
                         {.reload_completed = true, .update_reg_notes = true});
  ASSERT_TRUE(post_subreg.sideband.basic_block_live_at_start.contains(0));
  EXPECT_FALSE(post_subreg.sideband.basic_block_live_at_start[0].contains(4));
  EXPECT_TRUE(post_subreg.sideband.basic_block_live_at_start[0].contains(5));
  EXPECT_FALSE(post_subreg.sideband.basic_block_live_at_start[0].contains(30));

  // 2. Post-reload `(strict_low_part (subreg:SI (reg:DI 4 a0) 1))` destination:
  // `MarkSet1` must capture `SImode` from the `subreg:SI` (not `VOIDmode` from `strict_low_part`),
  // killing `$5` (`4 + 1` with 1-reg `SImode` span) while `$4` remains live from `strict_low_part`.
  RtlControlFlowGraph strict_low_cfg;
  strict_low_cfg.pass = RtlPassKind::kSched2;
  RtlBasicBlock sb0;
  sb0.id = 0;
  sb0.instructions = {
      "(set (strict_low_part (subreg:SI (reg:DI 4 a0) 1)) (reg:SI 6 a2))",
      "(set (reg/i:SI 2 v0) (reg:SI 5 a1))",
      "(use (reg/i:SI 2 v0))",
  };
  sb0.SyncRtlInsnsFromInstructions();
  strict_low_cfg.rpo_blocks = {sb0};
  ReconstructRtlLiveness(&strict_low_cfg, profile,
                         {.reload_completed = true, .update_reg_notes = true});
  ASSERT_TRUE(strict_low_cfg.sideband.basic_block_live_at_start.contains(0));
  EXPECT_TRUE(strict_low_cfg.sideband.basic_block_live_at_start[0].contains(6));
  auto parsed_slp = ParsePatternAndNotes(strict_low_cfg.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(parsed_slp.ok());
  for (const RtlRegNote& note : parsed_slp->reg_notes) {
    EXPECT_NE(note.kind, RtlMachineMode::kRegUnused);
  }

  // 3. `asm_operands` vector traversal order (`flow.c:2668-2673` scans `'E'` vectors forward
  // from `j = 0` (`73`) to `j = len - 1` (`74`), prepending each `REG_DEAD` note onto `REG_NOTES`
  // so `74` ends up at `reg_notes[0]` and `73` at `reg_notes[1]`, matching GCC's `flow_analysis`):
  RtlControlFlowGraph asm_cfg;
  asm_cfg.pass = RtlPassKind::kFlow;
  RtlBasicBlock ab0;
  ab0.id = 0;
  ab0.instructions = {
      "(set (reg:SI 72) (asm_operands:SI (\"addu %0,%1,%2\") (\"=r\") 0[ (reg:SI 73) "
      "(reg:SI 74) ] [ (asm_input:SI (\"r\")) (asm_input:SI (\"r\")) ] (\"t.c\") 1))",
      "(set (reg/i:SI 2 v0) (reg:SI 72))",
      "(use (reg/i:SI 2 v0))",
  };
  ab0.SyncRtlInsnsFromInstructions();
  asm_cfg.rpo_blocks = {ab0};

  gcc_harness::GccContext ctx;
  gcc_harness::FunctionHooks hooks(&ctx);
  hooks.ApplyCompilationProfile(profile);
  ctx.BeginFunction("asm_vec_test");
  gcc_harness::Rtx asm_head = ctx.MaterializeFromCfg(asm_cfg);
  hooks.RunFlowAnalysis(asm_head, 0);
  const RtlControlFlowGraph gcc_asm_cfg = ctx.DematerializeToCfg(asm_head, RtlPassKind::kFlow);
  auto parsed_gcc_asm = ParsePatternAndNotes(gcc_asm_cfg.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(parsed_gcc_asm.ok());

  ReconstructRtlLiveness(&asm_cfg, profile, {.reload_completed = false, .update_reg_notes = true});
  auto parsed_asm = ParsePatternAndNotes(asm_cfg.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(parsed_asm.ok());
  ASSERT_EQ(parsed_asm->reg_notes.size(), 2u);
  EXPECT_EQ(parsed_asm->reg_notes[0].kind, RtlMachineMode::kRegDead);
  EXPECT_EQ(parsed_asm->reg_notes[0].datum.RegNo(), 74);
  EXPECT_EQ(parsed_asm->reg_notes[1].kind, RtlMachineMode::kRegDead);
  EXPECT_EQ(parsed_asm->reg_notes[1].datum.RegNo(), 73);
  EXPECT_EQ(parsed_asm->reg_notes, parsed_gcc_asm->reg_notes);
  EXPECT_EQ(asm_cfg.sideband.basic_block_live_at_start,
            gcc_asm_cfg.sideband.basic_block_live_at_start);
}

TEST(RtlLivenessTest, PreservePhantomPseudosOptionControlsZeroOccurrenceCombineTemporaries) {
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {}};
  RtlControlFlowGraph base;
  base.pass = RtlPassKind::kLreg;
  RtlBasicBlock b0;
  b0.id = 0;
  b0.instructions = {
      "(set (reg/i:SI 2 v0) (reg:SI 4 a0))",
      "(use (reg/i:SI 2 v0))",
  };
  b0.SyncRtlInsnsFromInstructions();
  base.rpo_blocks = {b0};
  base.sideband.max_regno = 76;
  base.sideband.reg_n_refs[75] = 2;
  base.sideband.reg_n_deaths[75] = 1;
  base.sideband.reg_live_length[75] = 2;

  RtlControlFlowGraph flow_cfg = base;
  ReconstructRtlLiveness(
      &flow_cfg, o2_profile,
      {.reload_completed = false, .update_reg_notes = true, .preserve_phantom_pseudos = false});
  EXPECT_FALSE(flow_cfg.sideband.reg_n_refs.contains(75));
  EXPECT_FALSE(flow_cfg.sideband.reg_n_deaths.contains(75));
  EXPECT_FALSE(flow_cfg.sideband.reg_live_length.contains(75));

  RtlControlFlowGraph post_combine_cfg = base;
  ReconstructRtlLiveness(
      &post_combine_cfg, o2_profile,
      {.reload_completed = false, .update_reg_notes = true, .preserve_phantom_pseudos = true});
  EXPECT_EQ(post_combine_cfg.sideband.reg_n_refs[75], 2);
  EXPECT_EQ(post_combine_cfg.sideband.reg_n_deaths[75], 1);
  EXPECT_EQ(post_combine_cfg.sideband.reg_live_length[75], 2);
  EXPECT_FALSE(post_combine_cfg.sideband.reg_n_sets.contains(75));
  EXPECT_FALSE(post_combine_cfg.sideband.reg_renumber.contains(75));
  EXPECT_GE(post_combine_cfg.sideband.max_regno, 76);
}

}  // namespace
}  // namespace rom_nom_nom::lifter
