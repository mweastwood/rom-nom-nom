#ifndef LIFTER_DOMINATOR_TREE_H_
#define LIFTER_DOMINATOR_TREE_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "lifter/control_flow_graph.h"

namespace rom_nom_nom {

// Represents the Dominator Tree of a ControlFlowGraph.
// Computes immediate dominators (idom), dominance queries, and dominance frontiers.
// Also supports computing Post-Dominator trees for conditional join-point discovery.
class DominatorTree {
 public:
  DominatorTree() = default;

  // Computes the forward dominator tree for the given CFG.
  static DominatorTree Compute(const ControlFlowGraph& cfg);

  // Computes the post-dominator tree for the given CFG.
  // In the post-dominator tree, node P post-dominates node N if every path
  // from N to function exit must pass through P.
  static DominatorTree ComputePostDominators(const ControlFlowGraph& cfg);

  // Returns the immediate dominator (idom) of block_id, or nullopt if block_id
  // is the entry block or unreachable.
  std::optional<uint32_t> ImmediateDominator(uint32_t block_id) const;

  // Returns true if block 'a' dominates block 'b'.
  // Every block dominates itself (reflexive).
  bool Dominates(uint32_t a, uint32_t b) const;

  // Returns true if block 'a' strictly dominates block 'b' (a dominates b and a != b).
  bool StrictlyDominates(uint32_t a, uint32_t b) const;

  // Returns true if block_id is reachable from the entry block.
  bool IsReachable(uint32_t block_id) const;

  // Returns the immediate children of block_id in the dominator tree.
  const std::vector<uint32_t>& Children(uint32_t block_id) const;

  // Computes the dominance frontier DF(x) for block_id:
  // The set of all nodes y such that block_id dominates a predecessor of y,
  // but block_id does not strictly dominate y.
  const std::vector<uint32_t>& DominanceFrontier(uint32_t block_id) const;

 private:
  uint32_t root_id_ = 0;
  bool is_post_dom_ = false;

  // idom_[block_id] = immediate dominator of block_id
  absl::flat_hash_map<uint32_t, uint32_t> idom_;

  // children_[block_id] = list of blocks immediately dominated by block_id
  absl::flat_hash_map<uint32_t, std::vector<uint32_t>> children_;

  // df_[block_id] = list of nodes in dominance frontier of block_id
  absl::flat_hash_map<uint32_t, std::vector<uint32_t>> df_;

  // Set of blocks reachable in this analysis
  absl::flat_hash_set<uint32_t> reachable_;

  static const std::vector<uint32_t>& EmptyVector();
};

}  // namespace rom_nom_nom

#endif  // LIFTER_DOMINATOR_TREE_H_
