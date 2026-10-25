#include "lifter/ast_pass_pipeline.h"

#include <string>

#include "absl/types/span.h"
#include "core/c_ast.h"
#include "lifter/ast_abi_rewriter.h"
#include "lifter/ast_arithmetic_simplifier.h"
#include "lifter/ast_expression_folder.h"
#include "lifter/ast_memory_idiom_rewriter.h"
#include "lifter/ast_parameter_coalescer.h"

namespace rom_nom_nom {
namespace {

void RunCollapseUnalignedBlockCopies(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  CollapseUnalignedBlockCopies(body);
}

void RunCoalescePrologueParameterCopies(CompoundStatement* body, const AstPassContext& ctx) {
  CoalescePrologueParameterCopies(body, ctx.parameters, ctx.frame_size, ctx.mutable_parameters);
}

void RunInlineCrossJumpedCallTails(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  InlineCrossJumpedCallTails(body);
}

void RunSinkCommonBranchTailArgAssignments(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  SinkCommonBranchTailArgAssignments(body);
}

void RunSimplifyShiftAddMultiplication(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  SimplifyShiftAddMultiplication(body);
}

void RunSimplifyReciprocalAndUnsignedFloat(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  SimplifyReciprocalDivisionAndUnsignedFloatConversions(body);
}

void RunFoldSingleUseExpressions(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  FoldSingleUseExpressions(body);
}

void RunTrimUnassignedTrailingCallArgRegisters(CompoundStatement* body, const AstPassContext& ctx) {
  TrimUnassignedTrailingCallArgRegisters(body, ctx.parameters, ctx.function_parameter_counts);
}

void RunRewriteFloatInGprAndStackCallArguments(CompoundStatement* body, const AstPassContext& ctx) {
  RewriteFloatInGprAndStackCallArguments(body, ctx.parameters);
}

void RunSplitReusedFloatGprParameters(CompoundStatement* body, const AstPassContext& ctx) {
  SplitReusedFloatGprParameters(body, ctx.parameters);
}

void RunSimplifyDecoupledDelaySlotEarlyReturns(CompoundStatement* body,
                                               const AstPassContext& /*ctx*/) {
  SimplifyDecoupledDelaySlotEarlyReturns(body);
}

constexpr AstPass kPreCanonicalizationPasses[] = {
    {.name = "CollapseUnalignedBlockCopies", .run = &RunCollapseUnalignedBlockCopies},
    {.name = "CoalescePrologueParameterCopies", .run = &RunCoalescePrologueParameterCopies},
    {.name = "InlineCrossJumpedCallTails", .run = &RunInlineCrossJumpedCallTails},
    {.name = "SinkCommonBranchTailArgAssignments", .run = &RunSinkCommonBranchTailArgAssignments},
};

constexpr AstPass kArithmeticAndExpressionFoldingPasses[] = {
    {.name = "SimplifyShiftAddMultiplication", .run = &RunSimplifyShiftAddMultiplication},
    {.name = "SimplifyReciprocalDivisionAndUnsignedFloatConversions",
     .run = &RunSimplifyReciprocalAndUnsignedFloat},
    {.name = "FoldSingleUseExpressions", .run = &RunFoldSingleUseExpressions},
};

constexpr AstPass kParameterAndFloatCallCoalescingPasses[] = {
    {.name = "CoalescePrologueParameterCopies", .run = &RunCoalescePrologueParameterCopies},
    {.name = "RewriteFloatInGprAndStackCallArguments",
     .run = &RunRewriteFloatInGprAndStackCallArguments},
    {.name = "FoldSingleUseExpressions", .run = &RunFoldSingleUseExpressions},
};

constexpr AstPass kPostEarlyReturnPasses[] = {
    {.name = "SimplifyDecoupledDelaySlotEarlyReturns",
     .run = &RunSimplifyDecoupledDelaySlotEarlyReturns},
    {.name = "SinkCommonBranchTailArgAssignments", .run = &RunSinkCommonBranchTailArgAssignments},
    {.name = "SimplifyShiftAddMultiplication", .run = &RunSimplifyShiftAddMultiplication},
    {.name = "SimplifyReciprocalDivisionAndUnsignedFloatConversions",
     .run = &RunSimplifyReciprocalAndUnsignedFloat},
    {.name = "FoldSingleUseExpressions", .run = &RunFoldSingleUseExpressions},
    {.name = "TrimUnassignedTrailingCallArgRegisters",
     .run = &RunTrimUnassignedTrailingCallArgRegisters},
};

}  // namespace

int AstPassPipeline::RunToFixedPoint(CompoundStatement* body, const AstPassContext& ctx,
                                     absl::Span<const AstPass> passes, int max_iterations) {
  if (body == nullptr || passes.empty() || max_iterations <= 0) {
    return 0;
  }
  int iterations = 0;
  std::string prev_snapshot = body->ToString();
  while (iterations < max_iterations) {
    ++iterations;
    for (const AstPass& pass : passes) {
      if (pass.run != nullptr) {
        pass.run(body, ctx);
      }
    }
    std::string next_snapshot = body->ToString();
    if (next_snapshot == prev_snapshot) {
      break;
    }
    prev_snapshot = std::move(next_snapshot);
  }
  return iterations;
}

void AstPassPipeline::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return;

  for (const AstPass& pass : kPreCanonicalizationPasses) {
    pass.run(body, ctx);
  }

  RunToFixedPoint(body, ctx, kArithmeticAndExpressionFoldingPasses, /*max_iterations=*/3);

  RunTrimUnassignedTrailingCallArgRegisters(body, ctx);
  RunRewriteFloatInGprAndStackCallArguments(body, ctx);
  RunSplitReusedFloatGprParameters(body, ctx);

  RunToFixedPoint(body, ctx, kParameterAndFloatCallCoalescingPasses);

  for (const AstPass& pass : kPostEarlyReturnPasses) {
    pass.run(body, ctx);
  }

  if (SplitScratchReusedParameterRegisters(body, ctx.parameters, ctx.frame_size,
                                           ctx.mutable_parameters)) {
    FoldSingleUseExpressions(body);
    TrimUnassignedTrailingCallArgRegisters(body, ctx.parameters, ctx.function_parameter_counts);
  }

  if (ctx.has_frame_pointer) {
    RewriteFramePointerStructOffsetAccesses(body);
    if (ctx.return_type.ToString() == "void" && !ctx.has_goto_targets &&
        !body->Statements().empty()) {
      const auto& last_stmt = *body->Statements().back();
      if (last_stmt.Kind() == CStatementKind::kReturnStatement &&
          static_cast<const ReturnStatement&>(last_stmt).ReturnValue() == nullptr) {
        body->MutableStatements().pop_back();
      }
    }
  }
}

}  // namespace rom_nom_nom
