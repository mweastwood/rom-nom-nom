#include "lifter/cumulative_lift_chain.h"

#include <string>

#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"
#include "lifter/pass04_greg_reload.h"
#include "lifter/pass05_lreg_local_alloc.h"
#include "lifter/pass06_sched1_pre_alloc.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

int PipelineRank(RtlPassKind pass) {
  switch (pass) {
    case RtlPassKind::kRtl:
      return 0;
    case RtlPassKind::kJump:
      return 1;
    case RtlPassKind::kCse:
      return 2;
    case RtlPassKind::kLoop:
      return 3;
    case RtlPassKind::kCse2:
      return 4;
    case RtlPassKind::kFlow:
      return 5;
    case RtlPassKind::kCombine:
      return 6;
    case RtlPassKind::kSched:
      return 7;
    case RtlPassKind::kLreg:
      return 8;
    case RtlPassKind::kGreg:
      return 9;
    case RtlPassKind::kSched2:
      return 10;
    case RtlPassKind::kJump2:
      return 11;
    case RtlPassKind::kDbr:
      return 12;
  }
  return 12;
}

}  // namespace

CompilationProfile BuildEffectiveStage0Profile(const Stage0FunctionInput& input_x) {
  CompilationProfile effective_profile = input_x.profile;
  if (input_x.base_vram != 0 && input_x.relocations.empty()) {
    for (const auto& [vram, sym] : input_x.vram_to_symbol) {
      if (sym.empty()) {
        continue;
      }
      const auto sz_it = input_x.vram_to_symbol_size.find(vram);
      if (sz_it != input_x.vram_to_symbol_size.end() && sz_it->second > 0) {
        effective_profile.gp_symbol_sizes.try_emplace(sym, sz_it->second);
      }
    }
  }
  return effective_profile;
}

CumulativeLiftChain LiftCumulativeChain(const Stage0FunctionInput& input_x,
                                        RtlPassKind target_pass) {
  CumulativeLiftChain chain;
  chain.effective_profile = BuildEffectiveStage0Profile(input_x);

  if (input_x.machine_words.empty()) {
    chain.failure_stage = "Pass00 (kDbr)";
    return chain;
  }

  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(chain.effective_profile);
  const bool opt_enabled = flags.RunsOptimizerPasses();
  const bool dbr_enabled = flags.RunsDbrSchedule();
  const bool sched1_enabled = flags.RunsSched1();
  const int target_rank = PipelineRank(target_pass);

  // Stage 0: `.o` / linked ROM -> `.12.dbr`.
  RtlControlFlowGraph y = InvertFinalAndGasFunction(input_x);
  if (y.rpo_blocks.empty()) {
    chain.failure_stage = "Pass00 (kDbr)";
    return chain;
  }
  y.pass = RtlPassKind::kDbr;
  chain.stages[RtlPassKind::kDbr] = y;
  chain.deepest_pass = RtlPassKind::kDbr;

  // Stage 1: `.12.dbr` -> `.11.jump2`.
  if (target_rank <= PipelineRank(RtlPassKind::kJump2)) {
    if (dbr_enabled) {
      y = InvertPass01DbrSchedule(y, chain.effective_profile);
      if (y.rpo_blocks.empty()) {
        chain.failure_stage = "Pass01 (kJump2)";
        return chain;
      }
    }
    y.pass = RtlPassKind::kJump2;
    chain.stages[RtlPassKind::kJump2] = y;
    chain.deepest_pass = RtlPassKind::kJump2;
  }

  // Stage 2: `.11.jump2` -> `.10.sched2`.
  if (target_rank <= PipelineRank(RtlPassKind::kSched2)) {
    if (opt_enabled) {
      y = InvertPass02Jump2CrossJump(y, chain.effective_profile);
      if (y.rpo_blocks.empty()) {
        chain.failure_stage = "Pass02 (kSched2)";
        return chain;
      }
    }
    y.pass = RtlPassKind::kSched2;
    chain.stages[RtlPassKind::kSched2] = y;
    chain.deepest_pass = RtlPassKind::kSched2;
  }

  // Stage 3: `.10.sched2` -> `.09.greg`.
  if (target_rank <= PipelineRank(RtlPassKind::kGreg)) {
    y = InvertPass03Sched2PostReload(y, chain.effective_profile);
    if (y.rpo_blocks.empty()) {
      chain.failure_stage = "Pass03 (kGreg)";
      return chain;
    }
    y.pass = RtlPassKind::kGreg;
    chain.stages[RtlPassKind::kGreg] = y;
    chain.deepest_pass = RtlPassKind::kGreg;
  }

  // Stage 4: `.09.greg` -> `.08.lreg`.
  if (target_rank <= PipelineRank(RtlPassKind::kLreg)) {
    y = InvertPass04GregReload(y, chain.effective_profile);
    if (y.rpo_blocks.empty()) {
      chain.failure_stage = "Pass04 (kLreg)";
      return chain;
    }
    y.pass = RtlPassKind::kLreg;
    chain.stages[RtlPassKind::kLreg] = y;
    chain.deepest_pass = RtlPassKind::kLreg;
  }

  // Stage 5: `.08.lreg` -> `.07.sched`.
  if (target_rank <= PipelineRank(RtlPassKind::kSched)) {
    if (opt_enabled) {
      y = InvertPass05LregLocalAlloc(y, chain.effective_profile);
      if (y.rpo_blocks.empty()) {
        chain.failure_stage = "Pass05 (kSched)";
        return chain;
      }
    }
    y.pass = RtlPassKind::kSched;
    chain.stages[RtlPassKind::kSched] = y;
    chain.deepest_pass = RtlPassKind::kSched;
  }

  // Stage 6: `.07.sched` -> `.06.combine`.
  if (target_rank <= PipelineRank(RtlPassKind::kCombine)) {
    if (sched1_enabled) {
      y = InvertPass06Sched1PreAlloc(y, chain.effective_profile);
      if (y.rpo_blocks.empty()) {
        chain.failure_stage = "Pass06 (kCombine)";
        return chain;
      }
    }
    y.pass = RtlPassKind::kCombine;
    chain.stages[RtlPassKind::kCombine] = y;
    chain.deepest_pass = RtlPassKind::kCombine;
  }

  // Future stages 7..13 (`kFlow`, `kCse2`, `kLoop`, `kCse`, `kJump`, `kRtl`)
  // extend directly below as each pass stitcher comes online.
  if (target_rank < PipelineRank(RtlPassKind::kCombine)) {
    chain.failure_stage = "Unsupported target_pass deeper than kCombine";
  }
  return chain;
}

}  // namespace rom_nom_nom::lifter
