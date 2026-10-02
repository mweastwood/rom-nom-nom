#ifndef LIFTER_LOOP_ANALYZER_H_
#define LIFTER_LOOP_ANALYZER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "lifter/control_flow_graph.h"
#include "lifter/dominator_tree.h"

namespace rom_nom_nom {

// Classification of a natural loop's control structure.
enum class LoopType {
  kWhile,     // Condition evaluated at header before loop body
  kDoWhile,   // Condition evaluated at latch after loop body
  kInfinite,  // Unconditional / infinite loop (e.g. while (1))
};

// Represents a natural loop identified within a ControlFlowGraph.
struct Loop {
  uint32_t id = 0;
  uint32_t header = 0;
  std::vector<uint32_t> latches;
  absl::flat_hash_set<uint32_t> blocks;

  // Edges leading from a block inside the loop to a block outside
  std::vector<CfgEdge> exit_edges;
  // Unique target blocks reached by exit edges
  std::vector<uint32_t> exit_blocks;

  LoopType type = LoopType::kWhile;
  uint32_t depth = 1;

  // Nesting hierarchy
  Loop* parent = nullptr;
  std::vector<Loop*> sub_loops;

  // Convenience queries
  bool Contains(uint32_t block_id) const { return blocks.contains(block_id); }
  bool IsLatch(uint32_t block_id) const;
  bool IsExitBlock(uint32_t block_id) const;
};

// Container and analyzer for natural loops and loop nesting trees.
class LoopInfo {
 public:
  LoopInfo() = default;

  // Analyzes the CFG using dominator tree to identify all natural loops.
  static LoopInfo Analyze(const ControlFlowGraph& cfg, const DominatorTree& dom_tree);

  // Returns all loops found in the CFG.
  const std::vector<std::unique_ptr<Loop>>& AllLoops() const { return all_loops_; }

  // Returns the top-level (outermost) loops.
  const std::vector<Loop*>& TopLevelLoops() const { return top_level_loops_; }

  // Returns the innermost loop containing the given block, or nullptr if none.
  const Loop* GetLoopFor(uint32_t block_id) const;

  // Returns true if the given block is part of any loop.
  bool IsInLoop(uint32_t block_id) const { return GetLoopFor(block_id) != nullptr; }

  // Returns the nesting depth of the block (0 if not in a loop, 1+ if in loop).
  uint32_t LoopDepth(uint32_t block_id) const;

 private:
  std::vector<std::unique_ptr<Loop>> all_loops_;
  std::vector<Loop*> top_level_loops_;
  absl::flat_hash_map<uint32_t, Loop*> block_to_innermost_loop_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_LOOP_ANALYZER_H_
