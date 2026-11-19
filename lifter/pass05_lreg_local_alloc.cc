#include "lifter/pass05_lreg_local_alloc.h"

#include <utility>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_block_alloc.h"
#include "lifter/invert_update_equiv_regs.h"

namespace rom_nom_nom::lifter {

RtlControlFlowGraph InvertPass05LregLocalAlloc(const RtlControlFlowGraph& lreg_cfg_x,
                                               const CompilationProfile& profile) {
  if (lreg_cfg_x.rpo_blocks.empty()) {
    return {};
  }
  absl::StatusOr<RtlControlFlowGraph> pre_block = InvertBlockAlloc(lreg_cfg_x, profile);
  if (!pre_block.ok() || pre_block->rpo_blocks.empty()) {
    return {};
  }
  absl::StatusOr<RtlControlFlowGraph> pre_equiv = InvertUpdateEquivRegs(*pre_block, profile);
  if (!pre_equiv.ok() || pre_equiv->rpo_blocks.empty()) {
    return {};
  }
  pre_equiv->pass = RtlPassKind::kSched;
  return *std::move(pre_equiv);
}

}  // namespace rom_nom_nom::lifter
