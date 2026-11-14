#include "lifter/pass03_sched2_post_reload.h"

#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_schedule_block.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {

RtlControlFlowGraph InvertPass03Sched2PostReload(const RtlControlFlowGraph& sched2_cfg_x,
                                                 const CompilationProfile& profile) {
  if (sched2_cfg_x.rpo_blocks.empty()) {
    return {};
  }
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);

  RtlControlFlowGraph cfg = sched2_cfg_x;
  ReconstructRtlLiveness(&cfg, profile, {.reload_completed = true, .update_reg_notes = false});
  if (flags.RunsSched2()) {
    cfg = InvertScheduleBlock(cfg, profile);
    if (cfg.rpo_blocks.empty()) {
      return {};
    }
  }
  cfg = InvertThreadPrologueAndEpilogueInsns(cfg, profile);
  if (cfg.rpo_blocks.empty()) {
    return {};
  }
  cfg.pass = RtlPassKind::kGreg;
  ReconstructRtlLiveness(&cfg, profile, {.reload_completed = true, .update_reg_notes = false});
  return cfg;
}

}  // namespace rom_nom_nom::lifter
