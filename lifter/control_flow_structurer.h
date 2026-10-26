#ifndef LIFTER_CONTROL_FLOW_STRUCTURER_H_
#define LIFTER_CONTROL_FLOW_STRUCTURER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

  // Static factory helpers
  static std::unique_ptr<StructuredRegion> CreateBlock(uint32_t block_id);
  static std::unique_ptr<StructuredRegion> CreateBreak(uint32_t target_id);
  static std::unique_ptr<StructuredRegion> CreateContinue(uint32_t target_id);
  static std::unique_ptr<StructuredRegion> CreateGoto(uint32_t target_id);
  static std::unique_ptr<StructuredRegion> CreateSequence();
  static std::unique_ptr<StructuredRegion> CreateIfThen(uint32_t condition_block_id,
                                                        bool invert = false);
  static std::unique_ptr<StructuredRegion> CreateIfThenElse(uint32_t condition_block_id,
                                                            bool invert = false);
  static std::unique_ptr<StructuredRegion> CreateLoop(LoopType type, uint32_t header);
  static std::unique_ptr<StructuredRegion> CreateSwitch(uint32_t block_id, const JumpTable* jt,
                                                        Register reg = Register::kZero);

  // Formats the region as readable indented pseudo-code for testing and debugging.
  std::string ToString(int indent = 0) const;
};

class ControlFlowStructurer;

// Abstract base class for modular control-flow structuring strategies (loop structuring, switch
// structuring, short-circuit chain collapsing, and conditional diamond structuring).
class RegionStructuringStrategy {
 public:
  virtual ~RegionStructuringStrategy() = default;
  virtual std::string_view Name() const = 0;
  virtual std::unique_ptr<StructuredRegion> TryStructure(ControlFlowStructurer* structurer,
                                                         uint32_t entry,
                                                         std::optional<uint32_t> follow,
                                                         const Loop* current_loop) const = 0;
};

// Reconstructs natural loops (`while`, `do-while`, and `while (1)`), inverting GCC 2.7.2's
// `stmt.c:expand_start_loop` and `jump.c:duplicate_loop_exit_test`.
class LoopStructurer : public RegionStructuringStrategy {
 public:
  std::string_view Name() const override { return "LoopStructurer"; }
  std::unique_ptr<StructuredRegion> TryStructure(ControlFlowStructurer* structurer, uint32_t entry,
                                                 std::optional<uint32_t> follow,
                                                 const Loop* current_loop) const override;
};

// Reconstructs jump-table switches (`CASE_VECTOR_MODE`) and binary decision-tree switches
// (`USE_CASE_THRESHOLD = 4`, `stmt.c:expand_end_case`).
class SwitchStructurer : public RegionStructuringStrategy {
 public:
  std::string_view Name() const override { return "SwitchStructurer"; }
  std::unique_ptr<StructuredRegion> TryStructure(ControlFlowStructurer* structurer, uint32_t entry,
                                                 std::optional<uint32_t> follow,
                                                 const Loop* current_loop) const override;
};

// Reconstructs short-circuit `&&` and `||` condition chains (`dojump.c:do_jump`).
class ShortCircuitStructurer : public RegionStructuringStrategy {
 public:
  std::string_view Name() const override { return "ShortCircuitStructurer"; }
  std::unique_ptr<StructuredRegion> TryStructure(ControlFlowStructurer* structurer, uint32_t entry,
                                                 std::optional<uint32_t> follow,
                                                 const Loop* current_loop) const override;
};

// Reconstructs 2-successor conditional branches (`if-then`, `if-then-else`, `0x4F000000`
// float-to-uint conversion diamonds, and loop/switch break branches).
class ConditionalDiamondStructurer : public RegionStructuringStrategy {
 public:
  std::string_view Name() const override { return "ConditionalDiamondStructurer"; }
  std::unique_ptr<StructuredRegion> TryStructure(ControlFlowStructurer* structurer, uint32_t entry,
                                                 std::optional<uint32_t> follow,
                                                 const Loop* current_loop) const override;
};

// Post-structuring pass over a `StructuredRegion` tree.
class RegionPass {
 public:
  virtual ~RegionPass() = default;
  virtual std::string_view Name() const = 0;
  virtual bool Run(StructuredRegion* root, const ControlFlowGraph& cfg) = 0;
};

// Pipeline of post-structuring `RegionPass` transformations.
class RegionPassPipeline {
 public:
  RegionPassPipeline() = default;

  static RegionPassPipeline BuildDefaultPipeline();

  void AddPass(std::unique_ptr<RegionPass> pass);
  bool Run(StructuredRegion* root, const ControlFlowGraph& cfg) const;

  size_t PassCount() const { return passes_.size(); }
  const std::vector<std::unique_ptr<RegionPass>>& Passes() const { return passes_; }

 private:
  std::vector<std::unique_ptr<RegionPass>> passes_;
};

// Analyzes CFG, DominatorTree, and LoopInfo to produce a structured control flow tree.
class ControlFlowStructurer {
 public:
  ControlFlowStructurer();

  // Structures the CFG into a hierarchical StructuredRegion tree.
  static std::unique_ptr<StructuredRegion> Structure(const ControlFlowGraph& cfg,
                                                     const DominatorTree& dom_tree,
                                                     const DominatorTree& post_dom_tree,
                                                     const LoopInfo& loop_info);

  const ControlFlowGraph& Cfg() const { return *cfg_; }
  const DominatorTree& DomTree() const { return *dom_tree_; }
  const DominatorTree& PostDomTree() const { return *post_dom_tree_; }
  const LoopInfo& Loops() const { return *loop_info_; }

  absl::flat_hash_set<uint32_t>& Visited() { return visited_; }
  const absl::flat_hash_set<uint32_t>& Visited() const { return visited_; }

  std::optional<uint32_t> CurrentSwitchBreak() const { return current_switch_break_; }
  void SetCurrentSwitchBreak(std::optional<uint32_t> target) { current_switch_break_ = target; }

  absl::flat_hash_map<const Loop*, uint32_t>& LoopFollowMap() { return loop_follow_map_; }
  const absl::flat_hash_map<const Loop*, uint32_t>& LoopFollowMap() const {
    return loop_follow_map_;
  }

  std::unique_ptr<StructuredRegion> StructureRegion(uint32_t entry, std::optional<uint32_t> follow,
                                                    const Loop* current_loop);

  std::unique_ptr<StructuredRegion> StructureLoop(const Loop* loop, std::optional<uint32_t> follow,
                                                  const Loop* parent_loop);

  std::unique_ptr<StructuredRegion> StructureJumpTableSwitch(uint32_t entry,
                                                             std::optional<uint32_t> follow,
                                                             const Loop* current_loop);

  std::unique_ptr<StructuredRegion> StructureDecisionTreeSwitch(uint32_t entry,
                                                                std::optional<uint32_t> follow,
                                                                const Loop* current_loop);

  std::unique_ptr<StructuredRegion> StructureShortCircuitConditional(uint32_t entry,
                                                                     std::optional<uint32_t> follow,
                                                                     const Loop* current_loop);

  std::unique_ptr<StructuredRegion> StructureConditionalBranch(uint32_t entry,
                                                               std::optional<uint32_t> follow,
                                                               const Loop* current_loop);

  static void FlattenSequence(StructuredRegion* seq, std::unique_ptr<StructuredRegion> region);

  const std::vector<std::unique_ptr<RegionStructuringStrategy>>& Strategies() const {
    return strategies_;
  }

  // Computes the conditional join block for a 2-successor branch at `entry`.
  std::optional<uint32_t> ComputeConditionalJoinBlock(uint32_t entry,
                                                      std::optional<uint32_t> follow,
                                                      const Loop* current_loop) const;

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

 private:
  const ControlFlowGraph* cfg_ = nullptr;
  const DominatorTree* dom_tree_ = nullptr;
  const DominatorTree* post_dom_tree_ = nullptr;
  const LoopInfo* loop_info_ = nullptr;

  absl::flat_hash_set<uint32_t> visited_;
  std::optional<uint32_t> current_switch_break_;
  absl::flat_hash_map<const Loop*, uint32_t> loop_follow_map_;
  std::vector<std::unique_ptr<RegionStructuringStrategy>> strategies_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CONTROL_FLOW_STRUCTURER_H_
