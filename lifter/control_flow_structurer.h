#ifndef LIFTER_CONTROL_FLOW_STRUCTURER_H_
#define LIFTER_CONTROL_FLOW_STRUCTURER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
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
  kSwitch,      // multi-way switch statement
};

struct StructuredRegion;

// Represents a case branch in a structured switch statement.
struct StructuredCase {
  std::vector<int64_t> case_values;
  bool is_default = false;
  std::unique_ptr<StructuredRegion> body;
  std::optional<Register> delay_slot_const_reg;
  std::optional<int64_t> delay_slot_const_val;
  std::vector<Instruction> delay_instructions;
};

// Information about a detected 0x4F000000 float-to-uint conversion diamond.
struct FloatToUintInfo {
  uint32_t entry_block_id = 0;
  uint32_t join_block_id = 0;
  FpRegister source_float_register = FpRegister::kF0;
  Register destination_integer_register = Register::kZero;
  bool is_double = false;
  std::optional<Instruction> trailing_instruction = std::nullopt;
};

// Represents a node in the structured control flow AST tree.
struct StructuredRegion {
  RegionType type = RegionType::kBlock;

  // Block ID for kBlock, kGoto, kBreak, kContinue
  uint32_t block_id = 0;

  // Condition block ID for kIfThen, kIfThenElse
  uint32_t condition_block_id = 0;
  bool invert_condition = false;

  // Short-circuit condition details (&& / ||)
  std::string condition_op;
  std::vector<uint32_t> additional_condition_blocks;
  std::vector<bool> additional_invert_conditions;

  // Loop details when type == kLoop
  LoopType loop_type = LoopType::kWhile;
  uint32_t loop_header = 0;

  // Switch details when type == RegionType::kSwitch
  uint32_t switch_block_id = 0;
  Register switch_register = Register::kZero;
  std::unique_ptr<JumpTable> owned_jump_table;
  const JumpTable* jump_table = nullptr;
  std::vector<StructuredCase> cases;
  std::optional<uint32_t> switch_join_block;

  // Float-to-uint conversion details when this region represents a 0x4F000000 diamond
  std::optional<FloatToUintInfo> float_to_uint_info;

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
  std::optional<uint32_t> current_switch_break_;
  absl::flat_hash_map<const Loop*, uint32_t> loop_follow_map_;

  std::unique_ptr<StructuredRegion> StructureRegion(uint32_t entry, std::optional<uint32_t> follow,
                                                    const Loop* current_loop);

  std::unique_ptr<StructuredRegion> StructureLoop(const Loop* loop, std::optional<uint32_t> follow,
                                                  const Loop* parent_loop);

  static void FlattenSequence(StructuredRegion* seq, std::unique_ptr<StructuredRegion> region);

  // Finds the common convergence block reached by non-returning switch cases.
  std::optional<uint32_t> FindSwitchJoinBlock(
      const absl::flat_hash_set<uint32_t>& decision_blocks,
      const absl::flat_hash_map<uint32_t, std::vector<int64_t>>& target_to_cases,
      std::optional<uint32_t> default_block_id, std::optional<uint32_t> follow,
      const Loop* current_loop = nullptr);

  // Detects a 0x4F000000 float-to-uint conversion diamond starting at entry.
  std::optional<FloatToUintInfo> DetectFloatToUintDiamond(uint32_t entry, uint32_t succ_taken,
                                                          uint32_t succ_fallthrough,
                                                          std::optional<uint32_t> join);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CONTROL_FLOW_STRUCTURER_H_
