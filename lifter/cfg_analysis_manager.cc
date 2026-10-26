#include "lifter/cfg_analysis_manager.h"

#include <cstdint>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

CfgLivenessInfo CfgLivenessInfo::Compute(const ControlFlowGraph& cfg) {
  CfgLivenessInfo info;
  for (const auto& block : cfg.Blocks()) {
    BlockRegisterLiveness& bl = info.block_liveness_[block.id];
    for (const auto& inst : block.instructions) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        if (u != Register::kZero && !bl.gpr_kill.contains(u)) {
          bl.gpr_gen.insert(u);
        }
      }
      for (Register d : ud.gpr_defs) {
        if (d != Register::kZero) {
          bl.gpr_kill.insert(d);
        }
      }
      for (FpRegister fu : ud.fpr_uses) {
        if (!bl.fpr_kill.contains(fu)) {
          bl.fpr_gen.insert(fu);
        }
      }
      for (FpRegister fd : ud.fpr_defs) {
        bl.fpr_kill.insert(fd);
      }
    }
    bl.gpr_live_in = bl.gpr_gen;
    bl.fpr_live_in = bl.fpr_gen;
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : cfg.Blocks()) {
      BlockRegisterLiveness& bl = info.block_liveness_[block.id];
      for (uint32_t succ_id : block.successors) {
        auto succ_it = info.block_liveness_.find(succ_id);
        if (succ_it == info.block_liveness_.end()) continue;
        for (Register r : succ_it->second.gpr_live_in) {
          if (bl.gpr_live_out.insert(r).second) {
            changed = true;
          }
          if (!bl.gpr_kill.contains(r) && bl.gpr_live_in.insert(r).second) {
            changed = true;
          }
        }
        for (FpRegister fr : succ_it->second.fpr_live_in) {
          if (bl.fpr_live_out.insert(fr).second) {
            changed = true;
          }
          if (!bl.fpr_kill.contains(fr) && bl.fpr_live_in.insert(fr).second) {
            changed = true;
          }
        }
      }
    }
  }

  return info;
}

const BlockRegisterLiveness* CfgLivenessInfo::GetBlockLiveness(uint32_t block_id) const {
  auto it = block_liveness_.find(block_id);
  return (it != block_liveness_.end()) ? &it->second : nullptr;
}

bool CfgLivenessInfo::IsGprLiveIn(uint32_t block_id, Register reg) const {
  const auto* bl = GetBlockLiveness(block_id);
  return bl != nullptr && bl->gpr_live_in.contains(reg);
}

bool CfgLivenessInfo::IsGprLiveOut(uint32_t block_id, Register reg) const {
  const auto* bl = GetBlockLiveness(block_id);
  return bl != nullptr && bl->gpr_live_out.contains(reg);
}

bool CfgLivenessInfo::IsFprLiveIn(uint32_t block_id, FpRegister reg) const {
  const auto* bl = GetBlockLiveness(block_id);
  return bl != nullptr && bl->fpr_live_in.contains(reg);
}

bool CfgLivenessInfo::IsFprLiveOut(uint32_t block_id, FpRegister reg) const {
  const auto* bl = GetBlockLiveness(block_id);
  return bl != nullptr && bl->fpr_live_out.contains(reg);
}

const DominatorTree& CfgAnalysisManager::GetDominatorTree() {
  if (!dom_tree_.has_value()) {
    dom_tree_ = (cfg_ != nullptr) ? DominatorTree::Compute(*cfg_) : DominatorTree{};
  }
  return *dom_tree_;
}

const DominatorTree& CfgAnalysisManager::GetPostDominatorTree() {
  if (!post_dom_tree_.has_value()) {
    post_dom_tree_ =
        (cfg_ != nullptr) ? DominatorTree::ComputePostDominators(*cfg_) : DominatorTree{};
  }
  return *post_dom_tree_;
}

const LoopInfo& CfgAnalysisManager::GetLoopInfo() {
  if (!loop_info_.has_value()) {
    loop_info_ = (cfg_ != nullptr) ? LoopInfo::Analyze(*cfg_, GetDominatorTree()) : LoopInfo{};
  }
  return *loop_info_;
}

const CfgLivenessInfo& CfgAnalysisManager::GetLivenessInfo() {
  if (!liveness_info_.has_value()) {
    liveness_info_ = (cfg_ != nullptr) ? CfgLivenessInfo::Compute(*cfg_) : CfgLivenessInfo{};
  }
  return *liveness_info_;
}

}  // namespace rom_nom_nom
