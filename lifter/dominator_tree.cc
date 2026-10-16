#include "lifter/dominator_tree.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"

namespace rom_nom_nom {

namespace {

constexpr uint32_t kVirtualExitId = 0xFFFFFFFE;

}  // namespace

const std::vector<uint32_t>& DominatorTree::EmptyVector() {
  static const auto* empty_vector = new std::vector<uint32_t>();
  return *empty_vector;
}

DominatorTree DominatorTree::ComputeOnGraph(
    uint32_t root_id, bool is_post_dom, const std::vector<uint32_t>& rpo,
    const absl::flat_hash_map<uint32_t, std::vector<uint32_t>>& predecessors) {
  DominatorTree dt;
  dt.root_id_ = root_id;
  dt.is_post_dom_ = is_post_dom;
  if (rpo.empty()) {
    return dt;
  }

  absl::flat_hash_map<uint32_t, size_t> rpo_index;
  rpo_index.reserve(rpo.size());
  for (size_t i = 0; i < rpo.size(); ++i) {
    rpo_index[rpo[i]] = i;
    dt.reachable_.insert(rpo[i]);
  }

  // Intersect function in Cooper-Harvey-Kennedy
  auto intersect = [&](uint32_t b1, uint32_t b2,
                       const absl::flat_hash_map<uint32_t, uint32_t>& idom) -> uint32_t {
    uint32_t finger1 = b1;
    uint32_t finger2 = b2;
    while (finger1 != finger2) {
      while (rpo_index[finger1] > rpo_index[finger2]) {
        auto it = idom.find(finger1);
        if (it == idom.end() || it->second == finger1) {
          return root_id;
        }
        finger1 = it->second;
      }
      while (rpo_index[finger2] > rpo_index[finger1]) {
        auto it = idom.find(finger2);
        if (it == idom.end() || it->second == finger2) {
          return root_id;
        }
        finger2 = it->second;
      }
    }
    return finger1;
  };

  absl::flat_hash_map<uint32_t, uint32_t> idom;
  idom[root_id] = root_id;

  bool changed = true;
  while (changed) {
    changed = false;
    // Iterate over RPO, skipping the root (index 0)
    for (size_t i = 1; i < rpo.size(); ++i) {
      uint32_t block_id = rpo[i];
      auto it_preds = predecessors.find(block_id);
      if (it_preds == predecessors.end()) continue;
      const auto& preds = it_preds->second;

      // Find first processed predecessor
      uint32_t new_idom = UINT32_MAX;
      for (uint32_t pred_id : preds) {
        if (idom.contains(pred_id)) {
          new_idom = pred_id;
          break;
        }
      }
      if (new_idom == UINT32_MAX) continue;

      for (uint32_t pred_id : preds) {
        if (pred_id != new_idom && idom.contains(pred_id)) {
          new_idom = intersect(pred_id, new_idom, idom);
        }
      }

      auto it = idom.find(block_id);
      if (it == idom.end() || it->second != new_idom) {
        idom[block_id] = new_idom;
        changed = true;
      }
    }
  }

  // Populate final idom_ (omitting self-root and virtual exit) and children_
  for (const auto& [block_id, parent_id] : idom) {
    if (block_id != root_id && block_id != kVirtualExitId && parent_id != kVirtualExitId) {
      dt.idom_[block_id] = parent_id;
      dt.children_[parent_id].push_back(block_id);
    }
  }

  // Sort children for deterministic iteration
  for (auto& [_, child_list] : dt.children_) {
    std::sort(child_list.begin(), child_list.end());
  }

  // Compute Dominance Frontiers using CHK algorithm
  absl::flat_hash_map<uint32_t, absl::flat_hash_set<uint32_t>> df_sets;
  for (uint32_t block_id : rpo) {
    if (block_id == kVirtualExitId || !dt.IsReachable(block_id)) continue;
    auto it_preds = predecessors.find(block_id);
    if (it_preds == predecessors.end() || it_preds->second.size() < 2) continue;
    uint32_t idom_block = UINT32_MAX;
    auto it_b = dt.idom_.find(block_id);
    if (it_b != dt.idom_.end()) {
      idom_block = it_b->second;
    }
    for (uint32_t pred_id : it_preds->second) {
      if (pred_id == kVirtualExitId || !dt.IsReachable(pred_id)) continue;
      uint32_t runner = pred_id;
      while (runner != idom_block && runner != kVirtualExitId) {
        df_sets[runner].insert(block_id);
        auto it_r = dt.idom_.find(runner);
        if (it_r == dt.idom_.end()) break;
        runner = it_r->second;
      }
    }
  }

  for (auto& [node, set] : df_sets) {
    auto& list = dt.df_[node];
    list.assign(set.begin(), set.end());
    std::sort(list.begin(), list.end());
  }

  return dt;
}

DominatorTree DominatorTree::Compute(const ControlFlowGraph& cfg) {
  if (cfg.Blocks().empty()) {
    return DominatorTree();
  }

  std::vector<uint32_t> rpo = cfg.ReversePostOrder();
  absl::flat_hash_map<uint32_t, std::vector<uint32_t>> predecessors;
  predecessors.reserve(cfg.Blocks().size());
  for (const auto& block : cfg.Blocks()) {
    predecessors[block.id] = block.predecessors;
  }

  return ComputeOnGraph(cfg.EntryBlockId(), /*is_post_dom=*/false, rpo, predecessors);
}

DominatorTree DominatorTree::ComputePostDominators(const ControlFlowGraph& cfg) {
  if (cfg.Blocks().empty()) {
    return DominatorTree();
  }

  // Build reverse CFG: forward predecessors become successors, forward successors become
  // predecessors.
  absl::flat_hash_map<uint32_t, std::vector<uint32_t>> reverse_cfg_predecessors;
  absl::flat_hash_map<uint32_t, std::vector<uint32_t>> reverse_cfg_successors;
  std::vector<uint32_t> exit_block_ids;

  std::vector<uint32_t> forward_rpo = cfg.ReversePostOrder();
  absl::flat_hash_set<uint32_t> reachable_block_ids(forward_rpo.begin(), forward_rpo.end());

  for (const auto& block : cfg.Blocks()) {
    if (!reachable_block_ids.contains(block.id)) {
      continue;
    }
    std::vector<uint32_t> reachable_forward_successors;
    for (uint32_t successor_id : block.successors) {
      if (reachable_block_ids.contains(successor_id)) {
        reachable_forward_successors.push_back(successor_id);
      }
    }
    std::vector<uint32_t> reachable_forward_predecessors;
    for (uint32_t predecessor_id : block.predecessors) {
      if (reachable_block_ids.contains(predecessor_id)) {
        reachable_forward_predecessors.push_back(predecessor_id);
      }
    }
    reverse_cfg_predecessors[block.id] = std::move(reachable_forward_successors);
    reverse_cfg_successors[block.id] = std::move(reachable_forward_predecessors);
    if (reverse_cfg_predecessors[block.id].empty()) {
      exit_block_ids.push_back(block.id);
    }
  }

  uint32_t root_block_id = 0;
  if (exit_block_ids.empty()) {
    // Infinite loop with no exit blocks: fall back to last reachable block.
    root_block_id = forward_rpo.empty() ? cfg.Blocks().back().id : forward_rpo.back();
  } else if (exit_block_ids.size() == 1) {
    root_block_id = exit_block_ids.front();
  } else {
    // Multiple exits: connect via a virtual exit node.
    root_block_id = kVirtualExitId;
    reverse_cfg_successors[root_block_id] = exit_block_ids;
    reverse_cfg_predecessors[root_block_id] = {};
    for (uint32_t exit_id : exit_block_ids) {
      reverse_cfg_predecessors[exit_id].push_back(root_block_id);
    }
  }

  // Compute Reverse Post-Order on the reversed graph.
  std::vector<uint32_t> reverse_rpo;
  absl::flat_hash_set<uint32_t> visited_blocks;

  auto dfs = [&](auto& self, uint32_t node_id) -> void {
    visited_blocks.insert(node_id);
    auto it = reverse_cfg_successors.find(node_id);
    if (it != reverse_cfg_successors.end()) {
      for (uint32_t successor_node_id : it->second) {
        if (!visited_blocks.contains(successor_node_id)) {
          self(self, successor_node_id);
        }
      }
    }
    reverse_rpo.push_back(node_id);
  };
  dfs(dfs, root_block_id);
  std::reverse(reverse_rpo.begin(), reverse_rpo.end());

  return ComputeOnGraph(root_block_id, /*is_post_dom=*/true, reverse_rpo, reverse_cfg_predecessors);
}

std::optional<uint32_t> DominatorTree::ImmediateDominator(uint32_t block_id) const {
  auto it = idom_.find(block_id);
  if (it != idom_.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool DominatorTree::Dominates(uint32_t a, uint32_t b) const {
  if (a == b) {
    return IsReachable(a);
  }
  if (!IsReachable(a) || !IsReachable(b)) {
    return false;
  }

  uint32_t curr = b;
  while (true) {
    auto it = idom_.find(curr);
    if (it == idom_.end()) {
      return false;
    }
    curr = it->second;
    if (curr == a) {
      return true;
    }
    if (curr == root_id_) {
      break;
    }
  }
  return false;
}

bool DominatorTree::StrictlyDominates(uint32_t a, uint32_t b) const {
  return a != b && Dominates(a, b);
}

bool DominatorTree::IsReachable(uint32_t block_id) const {
  return reachable_.contains(block_id);
}

const std::vector<uint32_t>& DominatorTree::Children(uint32_t block_id) const {
  auto it = children_.find(block_id);
  if (it != children_.end()) {
    return it->second;
  }
  return EmptyVector();
}

const std::vector<uint32_t>& DominatorTree::DominanceFrontier(uint32_t block_id) const {
  auto it = df_.find(block_id);
  if (it != df_.end()) {
    return it->second;
  }
  return EmptyVector();
}

}  // namespace rom_nom_nom
