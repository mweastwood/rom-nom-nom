#include "lifter/local_alloc_model.h"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
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
// Unit Tests: Local Quantity Priority and Mode Tieability
// =============================================================================

TEST(LocalAllocModelUnitTest, QtyPriorityAndModesTieable) {
  EXPECT_GT(ComputeLocalQtyPriority(6, 4, 1), ComputeLocalQtyPriority(2, 4, 1));
  EXPECT_TRUE(ModesTieableForLocalAlloc({RtlMachineMode::kSi, ""}, {RtlMachineMode::kHi, ""}));
  EXPECT_TRUE(ModesTieableForLocalAlloc({RtlMachineMode::kSf, ""}, {RtlMachineMode::kDf, ""}));
  EXPECT_FALSE(ModesTieableForLocalAlloc({RtlMachineMode::kSi, ""}, {RtlMachineMode::kSf, ""}));
}

// =============================================================================
// Differential Parity Tests vs. Live GCC 2.7.2 `local-alloc.c:block_alloc`
// =============================================================================

class LocalAllocModelDifferentialTest : public ::testing::Test {
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

  // Runs `BuildSingleBlockLocalAllocModel` + `SimulateLocalBlockAlloc` strictly
  // from `pre_state` (zero post-state peeking!) across all blocks and returns
  // the combined `pseudo -> hard_reg` map for allocated local pseudos.
  static absl::btree_map<int, int> RunForwardLocalAllocModel(const RtlControlFlowGraph& pre_state,
                                                             const CompilationProfile& profile) {
    RtlControlFlowGraph out = pre_state;
    out.sideband.reg_renumber.clear();
    RtlControlFlowGraph liveness_cfg = pre_state;
    liveness_cfg.sideband.reg_renumber.clear();
    ReconstructRtlLiveness(&liveness_cfg, profile,
                           {.reload_completed = false,
                            .update_reg_notes = true,
                            .preserve_phantom_pseudos = true,
                            .include_call_reg_notes = true});
    // Ensure `out` has flow liveness sidebands when synthetic inputs omit them.
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

    absl::btree_map<int, int> predicted;
    for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
      std::optional<LocalBlockAllocModel> model = BuildSingleBlockLocalAllocModel(
          b, &out, liveness_cfg, mips_cfg, summary.live_at_any_block_start,
          summary.pseudo_summaries, summary.pseudo_modes, summary.fp_op_pseudos,
          summary.gr_or_mem_op_pseudos, /*bump_caller_saves_refs=*/false);
      if (!model.has_value()) {
        continue;
      }
      absl::btree_map<int, int> sim_hr;
      SimulateLocalBlockAlloc(*model, /*custom_pass2_order=*/nullptr, &sim_hr);
      for (const auto& [p, hr] : sim_hr) {
        if (hr >= 0) {
          predicted[p] = hr;
        }
      }
    }
    return predicted;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
};

GccContext* LocalAllocModelDifferentialTest::ctx = nullptr;
FunctionHooks* LocalAllocModelDifferentialTest::hooks = nullptr;

TEST_F(LocalAllocModelDifferentialTest, MatchesGccOnSingleBlockGprFprCopyPrefsAndCombine) {
  ctx->BeginFunction("local_model_gpr_fpr");
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  hooks->ApplyCompilationProfile(o2_profile);
  regclass_init();

  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kFlow;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 11)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (reg:SI 72))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:DF 74) (neg:DF (reg:DF 44 $f12)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (reg:SI 5 a1)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:DF 32 $f0) (reg:DF 74))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg:DF 32 $f0))");

  const SubfunctionBoundarySnapshot flow_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kPropagateBlock, pre_cfg);
  const SubfunctionBoundarySnapshot gcc_snap =
      hooks->ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, flow_snap.post_state);

  const absl::btree_map<int, int> predicted =
      RunForwardLocalAllocModel(flow_snap.post_state, o2_profile);
  ASSERT_FALSE(gcc_snap.post_state.sideband.reg_renumber.empty());
  for (const auto& [p, gcc_hr] : gcc_snap.post_state.sideband.reg_renumber) {
    ASSERT_TRUE(predicted.contains(p)) << "Missing predicted pseudo " << p;
    EXPECT_EQ(predicted.at(p), gcc_hr) << "Mismatch on pseudo " << p;
  }
  EXPECT_EQ(predicted.size(), gcc_snap.post_state.sideband.reg_renumber.size());

  // Non-vacuous negative check: corrupting a target hard reg in the model must
  // be reported as a mismatch by `SimulateLocalBlockAlloc`.
  RtlControlFlowGraph check_out = gcc_snap.post_state;
  RtlControlFlowGraph liveness_cfg = flow_snap.post_state;
  ReconstructRtlLiveness(
      &liveness_cfg, o2_profile,
      {.reload_completed = false, .update_reg_notes = true, .include_call_reg_notes = true});
  const MipsTargetConfig mips_cfg = ExtractMipsTargetConfig(ParseProfileFlags(o2_profile));
  const MipsCfgRegAllocSummary summary =
      AnalyzeCfgForMipsRegAlloc(check_out, liveness_cfg, mips_cfg);
  std::optional<LocalBlockAllocModel> model = BuildSingleBlockLocalAllocModel(
      0, &check_out, liveness_cfg, mips_cfg, summary.live_at_any_block_start,
      summary.pseudo_summaries, summary.pseudo_modes, summary.fp_op_pseudos,
      summary.gr_or_mem_op_pseudos, /*bump_caller_saves_refs=*/false);
  ASSERT_TRUE(model.has_value());
  EXPECT_EQ(SimulateLocalBlockAlloc(*model, nullptr), 0);
  model->target_hr_by_pseudo[72] = 17;  // Wrong hard reg!
  EXPECT_GT(SimulateLocalBlockAlloc(*model, nullptr), 0);
}

TEST_F(LocalAllocModelDifferentialTest, MatchesGccOnCallCrossingCalleeSavedVsCallerSave) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int ext_f(int x);\n"
      "int call_crossing_local(int a, int b) {\n"
      "  int x = a + 7;\n"
      "  int y = b * 3;\n"
      "  int z = ext_f(a);\n"
      "  return x + y + z;\n"
      "}\n",
      "call_crossing_local", {"-O2"});

  bool found = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kBlockAlloc) {
      continue;
    }
    found = true;
    const absl::btree_map<int, int> predicted =
        RunForwardLocalAllocModel(snap.pre_state, trace.stage0_input.profile);
    ASSERT_FALSE(snap.post_state.sideband.reg_renumber.empty());
    for (const auto& [p, gcc_hr] : snap.post_state.sideband.reg_renumber) {
      ASSERT_TRUE(predicted.contains(p)) << "Missing predicted pseudo " << p;
      EXPECT_EQ(predicted.at(p), gcc_hr) << "Mismatch on pseudo " << p;
    }
    EXPECT_EQ(predicted.size(), snap.post_state.sideband.reg_renumber.size());
  }
  EXPECT_TRUE(found);
}

TEST_F(LocalAllocModelDifferentialTest, StratifiedCorpusParityAgainstLiveGccBlockAlloc) {
  const std::vector<RoundTripTestCase>& all_cases = GetSharedRoundTripTestCases();
  ASSERT_FALSE(all_cases.empty());

  constexpr size_t kTargetSamples = 64;
  const size_t stride = std::max<size_t>(1, all_cases.size() / kTargetSamples);

  size_t cases_checked = 0;
  size_t blocks_checked = 0;
  size_t pseudos_checked = 0;
  size_t pseudos_matched = 0;
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
      if (snap.subfunction_id != SubfunctionId::kBlockAlloc) {
        continue;
      }
      ctx->BeginFunction("harness_func");
      hooks->ApplyCompilationProfile(trace.stage0_input.profile);
      regclass_init();
      const SubfunctionBoundarySnapshot live_snap =
          hooks->ExecuteSubfunctionBoundary(snap, snap.pre_state);

      const absl::btree_map<int, int> predicted =
          RunForwardLocalAllocModel(snap.pre_state, trace.stage0_input.profile);

      ++cases_checked;
      blocks_checked += snap.pre_state.rpo_blocks.size();

      bool case_all_match = true;
      for (const auto& [p, gcc_hr] : live_snap.post_state.sideband.reg_renumber) {
        ++pseudos_checked;
        auto it = predicted.find(p);
        if (it != predicted.end() && it->second == gcc_hr) {
          ++pseudos_matched;
        } else {
          case_all_match = false;
        }
      }
      if (case_all_match && predicted.size() == live_snap.post_state.sideband.reg_renumber.size()) {
        ++cases_exact;
      }
    }
  }

  std::cout << "[LocalAllocModelParity] cases_checked=" << cases_checked
            << " cases_exact=" << cases_exact << " blocks_checked=" << blocks_checked
            << " pseudos_matched=" << pseudos_matched << "/" << pseudos_checked << "\n";
  ASSERT_GE(cases_checked, 40u);
  ASSERT_GT(pseudos_checked, 100u);
  // Expect high exact forward parity against live GCC `block_alloc`.
  const double match_rate =
      static_cast<double>(pseudos_matched) / static_cast<double>(pseudos_checked);
  EXPECT_GE(match_rate, 0.95) << "pseudos_matched=" << pseudos_matched << "/" << pseudos_checked;
}

}  // namespace
}  // namespace rom_nom_nom::lifter
