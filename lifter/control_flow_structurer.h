#ifndef LIFTER_CONTROL_FLOW_STRUCTURER_H_
#define LIFTER_CONTROL_FLOW_STRUCTURER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "lifter/control_flow_graph.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

// Classification of a structured control flow region.
enum class RegionType {
  kBlock,       // Leaf basic block
  kSequence,    // Linear sequence of sub-regions
  kIfThen,      // if (cond) { then_body }
  kIfThenElse,  // if (cond) { then_body } else { else_body }
  kLoop,        // while / do-while / infinite loop
  kBreak,       // break out of loop
  kContinue,    // continue to next loop iteration
  kGoto,        // fallback unstructured jump
};

// Represents a node in the structured control flow AST tree.
struct StructuredRegion {
  RegionType type = RegionType::kBlock;

  // Block ID for kBlock, kGoto, kBreak, kContinue
  uint32_t block_id = 0;

  // Condition block ID for kIfThen, kIfThenElse
  uint32_t condition_block_id = 0;
  bool invert_condition = false;

  // Loop details when type == kLoop
  LoopType loop_type = LoopType::kWhile;
  uint32_t loop_header = 0;

  // Sub-regions:
  // - kSequence: list of sequential regions
  // - kIfThen: [0] = then_body
  // - kIfThenElse: [0] = then_body, [1] = else_body
  // - kLoop: [0] = loop_body
  std::vector<std::unique_ptr<StructuredRegion>> children;

  // Formats the region as readable indented pseudo-code for testing and debugging.
  std::string ToString(int indent = 0) const;
};

// Analyzes CFG, DominatorTree, and LoopInfo to produce a structured control flow tree.
class ControlFlowStructurer {
 public:
  ControlFlowStructurer() = default;

  // Structures the CFG into a hierarchical StructuredRegion tree.
  static std::unique_ptr<StructuredRegion> Structure(const ControlFlowGraph& cfg,
                                                     const DominatorTree& dom_tree,
                                                     const DominatorTree& post_dom_tree,
                                                     const LoopInfo& loop_info);

 private:
  const ControlFlowGraph* cfg_ = nullptr;
  const DominatorTree* dom_tree_ = nullptr;
  const DominatorTree* post_dom_tree_ = nullptr;
  const LoopInfo* loop_info_ = nullptr;

  absl::flat_hash_set<uint32_t> visited_;

  std::unique_ptr<StructuredRegion> StructureRegion(uint32_t entry, std::optional<uint32_t> follow,
                                                    const Loop* current_loop);

  std::unique_ptr<StructuredRegion> StructureLoop(const Loop* loop, std::optional<uint32_t> follow,
                                                  const Loop* parent_loop);

  static void FlattenSequence(StructuredRegion* seq, std::unique_ptr<StructuredRegion> region);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CONTROL_FLOW_STRUCTURER_H_
