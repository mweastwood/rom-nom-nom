#include "lifter/global_alloc_model.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gtest/gtest.h"
#include "lifter/mips_reg_alloc_common.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;

// =============================================================================
// Unit Tests: Priority, Ordering, and Conflict Matrix
// =============================================================================

TEST(GlobalAllocModelUnitTest, PriorityOrderAndAsymmetricConflicts) {
  GlobalAllocModel m;
  m.max_allocno = 3;
  m.allocno_n_refs = {4, 16, 16};
  m.allocno_live_length = {4, 4, 4};
  m.allocno_size = {1, 1, 1};
  m.conflicts.assign(9, 0);

  // Higher priority first; equal priority breaks ties by ascending allocno index.
  const std::vector<int> order = SortAllocnoOrder(m);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 0}));

  m.SetConflict(0, 2);
  EXPECT_TRUE(m.ConflictP(0, 2));
  EXPECT_FALSE(m.ConflictP(2, 0));
}

// =============================================================================
// Differential Parity Tests vs. Live GCC 2.7.2 `global.c:global_alloc`
// =============================================================================

class GlobalAllocModelDifferentialTest : public ::testing::Test {
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

  // Runs `BuildGlobalAllocModel` + `SimulateGlobalAlloc` strictly from
  // `pre_state` (passing an empty `greg_cfg` — zero post-state peeking!) and
  // returns the combined `pseudo -> hard_reg` map (preserved local assignments
  // minus any evictions, plus global-alloc simulated assignments).
  static absl::btree_map<int, int> RunForwardGlobalAllocModel(const RtlControlFlowGraph& pre_state,
                                                              const CompilationProfile& profile,
                                                              bool simulate_reload_spills = false) {
    RtlControlFlowGraph out = pre_state;
    RtlControlFlowGraph liveness_cfg = pre_state;
    ReconstructRtlLiveness(&liveness_cfg, profile,
                           {.reload_completed = false,
                            .update_reg_notes = true,
                            .preserve_phantom_pseudos = true,
                            .apply_post_sched_and_equiv_live_length = true,
                            .include_call_reg_notes = true});
    if (out.sideband.reg_n_refs.empty()) {
      out.sideband.max_regno = liveness_cfg.sideband.max_regno;
      out.sideband.basic_block_live_at_start = liveness_cfg.sideband.basic_block_live_at_start;
      out.sideband.reg_n_refs = liveness_cfg.sideband.reg_n_refs;
      out.sideband.reg_n_sets = liveness_cfg.sideband.reg_n_sets;
      out.sideband.reg_n_deaths = liveness_cfg.sideband.reg_n_deaths;
      out.sideband.reg_live_length = liveness_cfg.sideband.reg_live_length;
      out.sideband.reg_n_calls_crossed = liveness_cfg.sideband.reg_n_calls_crossed;
    }

    const MipsTargetConfig mips_cfg = ExtractMipsTargetConfig(ParseProfileFlags(profile));
    const MipsCfgRegAllocSummary summary = AnalyzeCfgForMipsRegAlloc(out, liveness_cfg, mips_cfg);
    const RtlControlFlowGraph empty_greg;
    const GlobalAllocModel model = BuildGlobalAllocModel(
        out, liveness_cfg, empty_greg, mips_cfg, summary.pseudo_summaries, summary.pseudo_modes,
        summary.fp_op_pseudos, summary.gr_or_mem_op_pseudos, summary.scratch_hard_regs);

    absl::btree_map<int, int> predicted;
    if (model.max_allocno > 0) {
      const std::vector<int> order = SortAllocnoOrder(model);
      const SimResult sim = SimulateGlobalAlloc(model, order, simulate_reload_spills);
      for (int p = kFirstPseudoRegister; p < static_cast<int>(sim.reg_renumber.size()); ++p) {
        if (sim.reg_renumber[p] >= 0) {
          predicted[p] = sim.reg_renumber[p];
        }
      }
    } else {
      for (const auto& [p, hr] : out.sideband.reg_renumber) {
        if (p >= kFirstPseudoRegister && hr >= 0) {
          predicted[p] = hr;
        }
      }
    }
    return predicted;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
};

GccContext* GlobalAllocModelDifferentialTest::ctx = nullptr;
FunctionHooks* GlobalAllocModelDifferentialTest::hooks = nullptr;

TEST_F(GlobalAllocModelDifferentialTest, MatchesGccOnMultiBlockDiamondAndPreservedLocal) {
  ctx->BeginFunction("global_diamond_fn");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);
  regclass_init();

  RtlControlFlowGraph lreg_pre;
  lreg_pre.pass = RtlPassKind::kLreg;
  lreg_pre.sideband.max_regno = 75;
  lreg_pre.sideband.reg_renumber[72] = 3;  // Pre-assigned block-local pseudo.

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 73) (ashift:SI (reg:SI 72) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (reg:SI 74) (plus:SI (reg:SI 5 a1) (const_int 7))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
      "(set (pc) (if_then_else (gt:SI (reg:SI 6 a2) (const_int 0)) (label_ref L2) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
  };
  bb0.successors = {1, 2};
  bb0.SyncRtlInsnsFromInstructions();

  RtlBasicBlock bb1;
  bb1.id = 1;
  bb1.instructions = {
      "(set (reg:SI 73) (plus:SI (reg:SI 73) (reg:SI 74)))",
  };
  bb1.successors = {2};
  bb1.SyncRtlInsnsFromInstructions();

  RtlBasicBlock bb2;
  bb2.id = 2;
  bb2.has_label = true;
  bb2.instructions = {
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (reg:SI 74))) "
      "(expr_list:REG_DEAD (reg:SI 73) (expr_list:REG_DEAD (reg:SI 74) (nil)))",
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
  };
  bb2.successors = {};
  bb2.SyncRtlInsnsFromInstructions();
  lreg_pre.rpo_blocks = {bb0, bb1, bb2};

  const SubfunctionBoundarySnapshot snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, lreg_pre);
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(73));
  ASSERT_TRUE(snap.post_state.sideband.reg_renumber.contains(74));

  const absl::btree_map<int, int> predicted =
      RunForwardGlobalAllocModel(lreg_pre, o2_profile, /*simulate_reload_spills=*/false);
  for (const auto& [p, gcc_hr] : snap.post_state.sideband.reg_renumber) {
    ASSERT_TRUE(predicted.contains(p)) << "Missing predicted pseudo " << p;
    EXPECT_EQ(predicted.at(p), gcc_hr) << "Mismatch on pseudo " << p;
  }
  EXPECT_EQ(predicted.size(), snap.post_state.sideband.reg_renumber.size());

  // Non-vacuous negative check: corrupting a target hard reg in the model must
  // trigger `primary_mismatches > 0`.
  RtlControlFlowGraph liveness_cfg = lreg_pre;
  ReconstructRtlLiveness(
      &liveness_cfg, o2_profile,
      {.reload_completed = false, .update_reg_notes = true, .include_call_reg_notes = true});
  RtlControlFlowGraph out = lreg_pre;
  out.sideband = liveness_cfg.sideband;
  out.sideband.reg_renumber = lreg_pre.sideband.reg_renumber;
  const MipsTargetConfig mips_cfg = ExtractMipsTargetConfig(ParseProfileFlags(o2_profile));
  const MipsCfgRegAllocSummary summary = AnalyzeCfgForMipsRegAlloc(out, liveness_cfg, mips_cfg);
  GlobalAllocModel model = BuildGlobalAllocModel(
      out, liveness_cfg, snap.post_state, mips_cfg, summary.pseudo_summaries, summary.pseudo_modes,
      summary.fp_op_pseudos, summary.gr_or_mem_op_pseudos, summary.scratch_hard_regs);
  const std::vector<int> order = SortAllocnoOrder(model);
  EXPECT_EQ(SimulateGlobalAlloc(model, order, /*simulate_reload_spills=*/false).primary_mismatches,
            0);
  model.target_hr[0] = 23;  // Wrong hard reg!
  EXPECT_GT(SimulateGlobalAlloc(model, order, /*simulate_reload_spills=*/false).primary_mismatches,
            0);
}

TEST_F(GlobalAllocModelDifferentialTest, MatchesGccOnCallCrossingCalleeSavedFpAndR3000VsR4000) {
  const std::string source =
      "double ext_fn(double x);\n"
      "double loop_fp(double a, int n) {\n"
      "  int i;\n"
      "  double acc = a;\n"
      "  for (i = 0; i < n; ++i) {\n"
      "    acc += ext_fn(acc);\n"
      "  }\n"
      "  return acc;\n"
      "}\n";

  for (const std::vector<std::string>& flags : std::vector<std::vector<std::string>>{
           {"-O2"}, {"-O2", "-mcpu=r3000"}, {"-O2", "-mcpu=r4000"}}) {
    FunctionCompilationTrace trace = hooks->TraceForwardCompilation(source, "loop_fp", flags);
    bool found = false;
    for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id != SubfunctionId::kGlobalAlloc) {
        continue;
      }
      found = true;
      ctx->BeginFunction("harness_func");
      hooks->ApplyCompilationProfile(trace.stage0_input.profile);
      regclass_init();
      const SubfunctionBoundarySnapshot live_snap =
          hooks->ExecuteSubfunctionBoundary(snap, snap.pre_state);

      const absl::btree_map<int, int> predicted = RunForwardGlobalAllocModel(
          snap.pre_state, trace.stage0_input.profile, /*simulate_reload_spills=*/false);
      ASSERT_FALSE(live_snap.post_state.sideband.reg_renumber.empty());
      for (const auto& [p, gcc_hr] : live_snap.post_state.sideband.reg_renumber) {
        ASSERT_TRUE(predicted.contains(p)) << "Missing predicted pseudo " << p;
        EXPECT_EQ(predicted.at(p), gcc_hr) << "Mismatch on pseudo " << p;
      }
      for (const auto& [p, pred_hr] : predicted) {
        EXPECT_TRUE(live_snap.post_state.sideband.reg_renumber.contains(p))
            << "Extra predicted pseudo " << p << " -> hr=" << pred_hr;
      }
      EXPECT_EQ(predicted.size(), live_snap.post_state.sideband.reg_renumber.size());
    }
    EXPECT_TRUE(found);
  }
}

TEST_F(GlobalAllocModelDifferentialTest, StratifiedCorpusParityAgainstLiveGccGlobalAlloc) {
  const std::vector<RoundTripTestCase>& all_cases = GetSharedRoundTripTestCases();
  ASSERT_FALSE(all_cases.empty());

  constexpr size_t kTargetSamples = 64;
  const size_t stride = std::max<size_t>(1, all_cases.size() / kTargetSamples);

  size_t cases_checked = 0;
  size_t allocnos_checked = 0;
  size_t allocnos_matched = 0;
  size_t cases_exact = 0;

  for (size_t idx = 0; idx < all_cases.size() && cases_checked < kTargetSamples; idx += stride) {
    const absl::StatusOr<const FunctionCompilationTrace*> trace_or =
        gcc_harness::GetCachedSharedCorpusTraceByIndex(idx);
    ASSERT_TRUE(trace_or.ok()) << all_cases[idx].test_name << ": " << trace_or.status();
    const FunctionCompilationTrace& trace = **trace_or;
    if (trace.stage0_input.profile.optimize_level <= 0) {
      continue;
    }

    for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id != SubfunctionId::kGlobalAlloc) {
        continue;
      }
      ctx->BeginFunction("harness_func");
      hooks->ApplyCompilationProfile(trace.stage0_input.profile);
      regclass_init();
      const SubfunctionBoundarySnapshot live_snap =
          hooks->ExecuteSubfunctionBoundary(snap, snap.pre_state);

      const absl::btree_map<int, int> predicted = RunForwardGlobalAllocModel(
          snap.pre_state, trace.stage0_input.profile, /*simulate_reload_spills=*/false);

      ++cases_checked;
      bool case_all_match = true;
      for (const auto& [p, gcc_hr] : live_snap.post_state.sideband.reg_renumber) {
        if (snap.pre_state.sideband.reg_renumber.contains(p)) {
          continue;  // Pre-assigned by local-alloc; count only global-alloc allocnos.
        }
        ++allocnos_checked;
        auto it = predicted.find(p);
        if (it != predicted.end() && it->second == gcc_hr) {
          ++allocnos_matched;
        } else {
          case_all_match = false;
        }
      }
      if (case_all_match && predicted.size() == live_snap.post_state.sideband.reg_renumber.size()) {
        ++cases_exact;
      }
    }
  }

  std::cout << "[GlobalAllocModelParity] cases_checked=" << cases_checked
            << " cases_exact=" << cases_exact << " allocnos_matched=" << allocnos_matched << "/"
            << allocnos_checked << "\n";
  ASSERT_GE(cases_checked, 40u);
  ASSERT_GT(allocnos_checked, 50u);
  const double match_rate =
      static_cast<double>(allocnos_matched) / static_cast<double>(allocnos_checked);
  EXPECT_GE(match_rate, 0.95) << "allocnos_matched=" << allocnos_matched << "/" << allocnos_checked;
}

}  // namespace
}  // namespace rom_nom_nom::lifter
