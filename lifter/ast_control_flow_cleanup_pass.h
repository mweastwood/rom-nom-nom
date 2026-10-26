#ifndef LIFTER_AST_CONTROL_FLOW_CLEANUP_PASS_H_
#define LIFTER_AST_CONTROL_FLOW_CLEANUP_PASS_H_

#include <string_view>

#include "core/c_ast.h"
#include "lifter/ast_pass_pipeline.h"

namespace rom_nom_nom {

// Removes any `LabelStatement` in `root_body` that is not targeted by any `GotoStatement`.
bool PruneUnusedGotoLabels(CompoundStatement* root_body);

// Collapses alias switch cases (`case X: block_N: ...` and `case Y: goto block_N;`) into
// shared `case X: case Y:` labels (or duplicated bodies for adjacent case values) and prunes
// dead labels.
bool CollapseAliasSwitchGotoCases(CompoundStatement* root_body);

// Inlines small cross-jumped tail-call/switch/diamond blocks reached via `goto` so each branch
// emits a structured call + return/break.
void InlineCrossJumpedCallTails(CompoundStatement* root_body, bool allow_diamond_tails = false);

// Sinks identical trailing argument assignments (`arg0 = ...;`, `var_sp_N = ...;`) and trailing
// labels out of `if/else` branches into the post-branch block, and rewrites loop-continue/exit
// `goto`s into `continue;` / `break;`.
void SinkCommonBranchTailArgAssignments(CompoundStatement* comp);

// Attempts to fold a guarded `do-while` loop (`init; ... if (guard_cond) { do { ... } while (cond);
// }`) into a structured `for (init; cond; step)` statement at the tail of `target_block`.
bool TrySynthesizeGuardedForLoop(const CExpression& guard_cond, const CompoundStatement& then_body,
                                 const CompoundStatement& else_body,
                                 CompoundStatement* target_block);

// Recursively scans `body` and upgrades guarded `do-while` loops preceded by an initializer into
// `for` loops.
bool SynthesizeForLoopsInBlock(CompoundStatement* body);

// Concrete `AstPass` classes for post-conversion control-flow cleanup.
class SwitchGotoCaseCollapserPass : public AstPass {
 public:
  std::string_view Name() const override { return "CollapseAliasSwitchGotoCases"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

class CrossJumpedCallTailInlinerPass : public AstPass {
 public:
  explicit CrossJumpedCallTailInlinerPass(bool allow_diamond_tails = false)
      : allow_diamond_tails_(allow_diamond_tails) {}

  std::string_view Name() const override { return "InlineCrossJumpedCallTails"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;

 private:
  bool allow_diamond_tails_ = false;
};

class CommonBranchTailSinkerPass : public AstPass {
 public:
  std::string_view Name() const override { return "SinkCommonBranchTailArgAssignments"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

class GotoAndLabelCleanupPass : public AstPass {
 public:
  explicit GotoAndLabelCleanupPass(bool allow_diamond_tails = false)
      : allow_diamond_tails_(allow_diamond_tails) {}

  std::string_view Name() const override { return "GotoAndLabelCleanup"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;

 private:
  bool allow_diamond_tails_ = false;
};

class ForLoopSynthesizerPass : public AstPass {
 public:
  std::string_view Name() const override { return "SynthesizeForLoops"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_CONTROL_FLOW_CLEANUP_PASS_H_
