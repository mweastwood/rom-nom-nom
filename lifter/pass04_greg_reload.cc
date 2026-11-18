#include "lifter/pass04_greg_reload.h"

#include <utility>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_alter_reg.h"
#include "lifter/invert_global_alloc.h"
#include "lifter/invert_reload.h"

namespace rom_nom_nom::lifter {

RtlControlFlowGraph InvertPass04GregReload(const RtlControlFlowGraph& greg_cfg_x,
                                           const CompilationProfile& profile) {
  if (greg_cfg_x.rpo_blocks.empty()) {
    return {};
  }
  RtlControlFlowGraph pre_alter = InvertAlterReg(greg_cfg_x, profile, /*first_pseudo_regno=*/72);
  if (pre_alter.rpo_blocks.empty()) {
    return {};
  }
  absl::StatusOr<RtlControlFlowGraph> pre_reload = InvertReload(pre_alter, profile);
  if (!pre_reload.ok() || pre_reload->rpo_blocks.empty()) {
    return {};
  }
  absl::StatusOr<RtlControlFlowGraph> pre_global = InvertGlobalAlloc(*pre_reload, profile);
  if (!pre_global.ok() || pre_global->rpo_blocks.empty()) {
    return {};
  }
  pre_global->pass = RtlPassKind::kLreg;
  return *std::move(pre_global);
}

}  // namespace rom_nom_nom::lifter
