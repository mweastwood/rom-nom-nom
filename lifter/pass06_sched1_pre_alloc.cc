#include "lifter/pass06_sched1_pre_alloc.h"

#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_schedule_block.h"
#include "lifter/invert_update_flow_info.h"
#include "lifter/sched_model.h"

namespace rom_nom_nom::lifter {

RtlControlFlowGraph InvertPass06Sched1PreAlloc(const RtlControlFlowGraph& sched1_cfg_x,
                                               const CompilationProfile& profile) {
  if (sched1_cfg_x.rpo_blocks.empty()) {
    return {};
  }
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);

  RtlControlFlowGraph cfg = sched1_cfg_x;
  cfg.pass = RtlPassKind::kSched;
  // Validates structure; unschedules only when `flags.RunsSched1()` (`toplev.c:3028`).
  cfg = InvertScheduleBlock(cfg, profile);
  if (cfg.rpo_blocks.empty()) {
    return {};
  }
  if (flags.RunsSched1()) {
    const RtlControlFlowGraph unsched_cfg = cfg;
    // Exact reverse of `sched.c:4825-4854`: unschedule first, then invert `try_split` +
    // `update_flow_info`, then restore pre-`sched1` `flow.c` liveness (`REG_DEAD`/`REG_UNUSED`
    // and pre-sched register life statistics).
    cfg = InvertUpdateFlowInfo(cfg);
    if (cfg.rpo_blocks.empty()) {
      return {};
    }
    RestorePreSched1FlowLiveness(&cfg, profile);

    // Soundness guard (`sched.c:4825-4848`): if `InvertUpdateFlowInfo` folded a
    // non-adjacent `large_int` split (`lui` + `ori`) across intervening insns
    // and shifted `INSN_LUID` tie-breaks so forward `kSched1` replay no longer
    // reproduces `sched1_cfg_x` on block `b`, revert block `b` to the verified
    // pre-merge `unsched_cfg` block when that restores exact `kSched1` replay.
    if (cfg.rpo_blocks.size() == sched1_cfg_x.rpo_blocks.size() &&
        unsched_cfg.rpo_blocks.size() == sched1_cfg_x.rpo_blocks.size()) {
      const RtlControlFlowGraph replayed =
          sched_model::RunScheduleInsns(cfg, profile, sched_model::Phase::kSched1);
      bool any_reverted = false;
      RtlControlFlowGraph candidate = cfg;
      for (size_t b = 0; b < cfg.rpo_blocks.size(); ++b) {
        if (b < replayed.rpo_blocks.size() &&
            replayed.rpo_blocks[b].instructions != sched1_cfg_x.rpo_blocks[b].instructions &&
            cfg.rpo_blocks[b].instructions != unsched_cfg.rpo_blocks[b].instructions) {
          candidate.rpo_blocks[b] = unsched_cfg.rpo_blocks[b];
          any_reverted = true;
        }
      }
      if (any_reverted) {
        RestorePreSched1FlowLiveness(&candidate, profile);
        const RtlControlFlowGraph replayed_cand =
            sched_model::RunScheduleInsns(candidate, profile, sched_model::Phase::kSched1);
        if (replayed_cand.CompareAgainst(sched1_cfg_x).MatchesExactly()) {
          cfg = std::move(candidate);
        }
      }
    }
  }
  cfg.pass = RtlPassKind::kCombine;
  return cfg;
}

}  // namespace rom_nom_nom::lifter
