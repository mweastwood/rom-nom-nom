#ifndef LIFTER_BRANCH_CONDITION_BUILDER_H_
#define LIFTER_BRANCH_CONDITION_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"

namespace rom_nom_nom {

// Information about a comparison instruction folded into a subsequent conditional branch.
struct FoldedComparison {
  size_t instruction_index = 0;
  bool can_suppress_statement = false;
};

// Analyzes and lifts MIPS branch conditions, folded comparisons, and delay-slot hazards.
class BranchConditionBuilder {
 public:
  using FoldedComparison = ::rom_nom_nom::FoldedComparison;

  // Finds a comparison instruction in `instructions` that is folded into a subsequent conditional
  // branch. Returns std::nullopt if no instruction is folded.
  static std::optional<FoldedComparison> FindFoldedComparison(
      absl::Span<const Instruction> instructions);

  // Extracts the boolean condition CExpression from a basic block ending in a conditional branch.
  static std::unique_ptr<CExpression> ExtractBranchCondition(const BasicBlock& block,
                                                             bool invert_condition = false);

  // Returns true if the non-likely branch delay slot clobbers a register used in the branch
  // condition or its folded comparison, requiring delay-slot decoupling into both branches.
  static bool ShouldDecoupleBranchConditionDelaySlot(const BasicBlock& cond_block);
  static bool ShouldDecoupleBranchConditionDelaySlot(const ControlFlowGraph& cfg,
                                                     uint32_t cond_block_id);

  // Returns true if the loop latch delay slot defines only registers that are killed at the loop
  // header before being used, allowing the delay slot to be emitted after the loop.
  static bool ShouldDecoupleLoopConditionDelaySlot(const BasicBlock& cond_block,
                                                   const BasicBlock* header_block);
  static bool ShouldDecoupleLoopConditionDelaySlot(const ControlFlowGraph& cfg,
                                                   uint32_t cond_block_id,
                                                   uint32_t header_block_id);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_BRANCH_CONDITION_BUILDER_H_
