#ifndef LIFTER_CFG_ANALYSIS_MANAGER_H_
#define LIFTER_CFG_ANALYSIS_MANAGER_H_

#include <cstdint>
#include <optional>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

// Per-basic-block register liveness sets (modeling GCC 2.7.2's `flow.c:life_analysis`
// `basic_block_live_at_start`).
struct BlockRegisterLiveness {
  absl::flat_hash_set<Register> gpr_gen;
  absl::flat_hash_set<Register> gpr_kill;
  absl::flat_hash_set<Register> gpr_live_in;
  absl::flat_hash_set<Register> gpr_live_out;

  absl::flat_hash_set<FpRegister> fpr_gen;
  absl::flat_hash_set<FpRegister> fpr_kill;
  absl::flat_hash_set<FpRegister> fpr_live_in;
  absl::flat_hash_set<FpRegister> fpr_live_out;
};

// Whole-CFG backward dataflow liveness analysis results.
class CfgLivenessInfo {
 public:
  CfgLivenessInfo() = default;

  static CfgLivenessInfo Compute(const ControlFlowGraph& cfg);

  const BlockRegisterLiveness* GetBlockLiveness(uint32_t block_id) const;
  bool IsGprLiveIn(uint32_t block_id, Register reg) const;
  bool IsGprLiveOut(uint32_t block_id, Register reg) const;
  bool IsFprLiveIn(uint32_t block_id, FpRegister reg) const;
  bool IsFprLiveOut(uint32_t block_id, FpRegister reg) const;

 private:
  absl::flat_hash_map<uint32_t, BlockRegisterLiveness> block_liveness_;
};

// Lazily computes, caches, and invalidates CFG-level analyses (`DominatorTree`,
// `PostDominatorTree`, `LoopInfo`, and `CfgLivenessInfo`) across CFG transformation passes.
class CfgAnalysisManager {
 public:
  explicit CfgAnalysisManager(const ControlFlowGraph* cfg = nullptr) : cfg_(cfg) {}

  void SetCfg(const ControlFlowGraph* cfg) {
    if (cfg_ != cfg) {
      cfg_ = cfg;
      InvalidateAll();
    }
  }

  void InvalidateAll() {
    dom_tree_.reset();
    post_dom_tree_.reset();
    loop_info_.reset();
    liveness_info_.reset();
  }

  const DominatorTree& GetDominatorTree();
  const DominatorTree& GetPostDominatorTree();
  const LoopInfo& GetLoopInfo();
  const CfgLivenessInfo& GetLivenessInfo();

  bool HasCachedDominatorTree() const { return dom_tree_.has_value(); }
  bool HasCachedPostDominatorTree() const { return post_dom_tree_.has_value(); }
  bool HasCachedLoopInfo() const { return loop_info_.has_value(); }
  bool HasCachedLivenessInfo() const { return liveness_info_.has_value(); }

 private:
  const ControlFlowGraph* cfg_ = nullptr;
  std::optional<DominatorTree> dom_tree_;
  std::optional<DominatorTree> post_dom_tree_;
  std::optional<LoopInfo> loop_info_;
  std::optional<CfgLivenessInfo> liveness_info_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CFG_ANALYSIS_MANAGER_H_
