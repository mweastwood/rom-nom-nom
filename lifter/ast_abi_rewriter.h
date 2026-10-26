#ifndef LIFTER_AST_ABI_REWRITER_H_
#define LIFTER_AST_ABI_REWRITER_H_

#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "core/c_ast.h"
#include "lifter/ast_pass_pipeline.h"

namespace rom_nom_nom {

// Folds `v0 = expr; return v0;` and conditional return diamonds into direct
// `return expr;` statements.
void SimplifyReturnStatements(CompoundStatement* comp, bool returns_v0, bool returns_f0);

// Simplifies early returns whose delay slot was decoupled across an `if/else`
// diamond back into `if (cond) return ...; return default_val;`.
void SimplifyDecoupledDelaySlotEarlyReturns(CompoundStatement* comp);

// Sinks identical trailing argument assignments (`arg0 = ...;`, `var_sp_N = ...;`)
// out of `if/else` branches into the post-branch block.
void SinkCommonBranchTailArgAssignments(CompoundStatement* comp);

// Inlines small cross-jumped tail-call blocks reached via `goto` so each branch
// emits a structured call + return/break.
void InlineCrossJumpedCallTails(CompoundStatement* root_body, bool allow_diamond_tails = false);

// Rewrites calls that pass `f32`/`f64` values in GPRs or stack slots via
// `*(s32*)&fX` / `*(s64*)&fX_d` into typed function-pointer casts with direct
// float/double arguments.
void RewriteFloatInGprAndStackCallArguments(CompoundStatement* comp,
                                            const std::vector<CParameter>& parameters = {});

// Trims trailing `arg0..arg3` arguments from calls to functions with unknown
// parameter counts when those argument registers were never assigned in `body`.
void TrimUnassignedTrailingCallArgRegisters(
    CompoundStatement* body, const std::vector<CParameter>& parameters,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts);

// Concrete `AstPass` wrappers for ABI and control-flow tail rewrites.
class ReturnStatementSimplifierPass : public AstPass {
 public:
  std::string_view Name() const override { return "SimplifyReturnStatements"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

class DecoupledDelaySlotEarlyReturnPass : public AstPass {
 public:
  std::string_view Name() const override { return "SimplifyDecoupledDelaySlotEarlyReturns"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

class CommonBranchTailSinkerPass : public AstPass {
 public:
  std::string_view Name() const override { return "SinkCommonBranchTailArgAssignments"; }
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

class FloatCallArgumentRewriterPass : public AstPass {
 public:
  std::string_view Name() const override { return "RewriteFloatInGprAndStackCallArguments"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

class UnassignedTrailingCallArgTrimmerPass : public AstPass {
 public:
  std::string_view Name() const override { return "TrimUnassignedTrailingCallArgRegisters"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_ABI_REWRITER_H_
