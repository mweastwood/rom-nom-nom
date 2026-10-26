#include "lifter/ast_pass_pipeline.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "core/c_ast.h"
#include "lifter/ast_abi_rewriter.h"
#include "lifter/ast_arithmetic_simplifier.h"
#include "lifter/ast_expression_folder.h"
#include "lifter/ast_memory_idiom_rewriter.h"
#include "lifter/ast_parameter_coalescer.h"

namespace rom_nom_nom {
namespace {

// Splits scratch-reused parameter registers and re-runs expression folding and call-arg trimming
// when a parameter copy was coalesced.
class ScratchReusedParameterSplitAndFoldPass : public AstPass {
 public:
  std::string_view Name() const override { return "ScratchReusedParameterSplitAndFold"; }

  bool Run(CompoundStatement* body, const AstPassContext& ctx) override {
    if (body == nullptr) return false;
    std::string before = body->ToString();
    if (SplitScratchReusedParameterRegisters(body, ctx.parameters, ctx.frame_size,
                                             ctx.mutable_parameters)) {
      FoldSingleUseExpressions(body);
      TrimUnassignedTrailingCallArgRegisters(body, ctx.parameters, ctx.function_parameter_counts);
    }
    return body->ToString() != before;
  }
};

// Rewrites `-O0` frame-pointer struct offset accesses and strips redundant trailing `return;` in
// void functions without goto targets.
class FramePointerCleanupPass : public AstPass {
 public:
  std::string_view Name() const override { return "FramePointerCleanup"; }

  bool Run(CompoundStatement* body, const AstPassContext& ctx) override {
    if (body == nullptr || !ctx.has_frame_pointer) return false;
    std::string before = body->ToString();
    RewriteFramePointerStructOffsetAccesses(body);
    if (ctx.return_type.ToString() == "void" && !ctx.has_goto_targets &&
        !body->Statements().empty()) {
      const auto& last_stmt = *body->Statements().back();
      if (last_stmt.Kind() == CStatementKind::kReturnStatement &&
          static_cast<const ReturnStatement&>(last_stmt).ReturnValue() == nullptr) {
        body->MutableStatements().pop_back();
      }
    }
    return body->ToString() != before;
  }
};

}  // namespace

bool FunctionAstPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr || !callback_) return false;
  std::string before = body->ToString();
  callback_(body, ctx);
  return body->ToString() != before;
}

void FixedPointAstPassGroup::AddPass(std::unique_ptr<AstPass> pass) {
  if (pass != nullptr) {
    passes_.push_back(std::move(pass));
  }
}

bool FixedPointAstPassGroup::Run(CompoundStatement* body, const AstPassContext& ctx) {
  last_iterations_executed_ = 0;
  if (body == nullptr || passes_.empty() || max_iterations_ <= 0) {
    return false;
  }
  std::vector<AstPass*> raw_passes;
  raw_passes.reserve(passes_.size());
  for (const auto& pass : passes_) {
    raw_passes.push_back(pass.get());
  }
  std::string initial_snapshot = body->ToString();
  last_iterations_executed_ =
      AstPassPipeline::RunToFixedPoint(body, ctx, absl::MakeConstSpan(raw_passes), max_iterations_);
  return body->ToString() != initial_snapshot;
}

void AstPassPipeline::AddPass(std::unique_ptr<AstPass> pass) {
  if (pass != nullptr) {
    passes_.push_back(std::move(pass));
  }
}

bool AstPassPipeline::RunOnBody(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return false;
  bool any_changed = false;
  for (const auto& pass : passes_) {
    if (pass->Run(body, ctx)) {
      any_changed = true;
    }
  }
  return any_changed;
}

int AstPassPipeline::RunToFixedPoint(CompoundStatement* body, const AstPassContext& ctx,
                                     absl::Span<AstPass* const> passes, int max_iterations) {
  if (body == nullptr || passes.empty() || max_iterations <= 0) {
    return 0;
  }
  int iterations = 0;
  std::string prev_snapshot = body->ToString();
  while (iterations < max_iterations) {
    ++iterations;
    for (AstPass* pass : passes) {
      if (pass != nullptr) {
        pass->Run(body, ctx);
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

AstPassPipeline AstPassPipeline::BuildDefaultPipeline() {
  AstPassPipeline pipeline;

  // 1. Pre-canonicalization passes.
  pipeline.AddPass(std::make_unique<ReturnStatementSimplifierPass>());
  pipeline.AddPass(std::make_unique<UnalignedBlockCopyCollapserPass>());
  pipeline.AddPass(std::make_unique<PrologueParameterCoalescerPass>());
  pipeline.AddPass(std::make_unique<CrossJumpedCallTailInlinerPass>(/*allow_diamond_tails=*/false));
  pipeline.AddPass(std::make_unique<CommonBranchTailSinkerPass>());

  // 2. Bounded fixed-point arithmetic strength-reduction & expression folding group.
  auto arith_and_fold_group =
      std::make_unique<FixedPointAstPassGroup>("ArithmeticAndExpressionFolding",
                                               /*max_iterations=*/3);
  arith_and_fold_group->AddPass(std::make_unique<ShiftAddMultiplicationSimplifierPass>());
  arith_and_fold_group->AddPass(std::make_unique<ReciprocalAndUnsignedFloatSimplifierPass>());
  arith_and_fold_group->AddPass(std::make_unique<SingleUseExpressionFolderPass>());
  pipeline.AddPass(std::move(arith_and_fold_group));

  // 3. Call argument trimming, float-in-GPR rewriting, and reused float GPR parameter splitting.
  pipeline.AddPass(std::make_unique<UnassignedTrailingCallArgTrimmerPass>());
  pipeline.AddPass(std::make_unique<FloatCallArgumentRewriterPass>());
  pipeline.AddPass(std::make_unique<ReusedFloatGprParameterSplitterPass>());

  // 4. Bounded fixed-point parameter & float call coalescing group.
  auto param_and_float_group =
      std::make_unique<FixedPointAstPassGroup>("ParameterAndFloatCallCoalescing",
                                               /*max_iterations=*/kDefaultMaxIterations);
  param_and_float_group->AddPass(std::make_unique<PrologueParameterCoalescerPass>());
  param_and_float_group->AddPass(std::make_unique<FloatCallArgumentRewriterPass>());
  param_and_float_group->AddPass(std::make_unique<SingleUseExpressionFolderPass>());
  pipeline.AddPass(std::move(param_and_float_group));

  // 5. Post-early-return simplification and canonicalization passes.
  pipeline.AddPass(std::make_unique<DecoupledDelaySlotEarlyReturnPass>());
  pipeline.AddPass(std::make_unique<CrossJumpedCallTailInlinerPass>(/*allow_diamond_tails=*/false));
  pipeline.AddPass(std::make_unique<CommonBranchTailSinkerPass>());
  pipeline.AddPass(std::make_unique<ShiftAddMultiplicationSimplifierPass>());
  pipeline.AddPass(std::make_unique<ReciprocalAndUnsignedFloatSimplifierPass>());
  pipeline.AddPass(std::make_unique<SingleUseExpressionFolderPass>());
  pipeline.AddPass(std::make_unique<FloatCallArgumentRewriterPass>());
  pipeline.AddPass(std::make_unique<SingleUseExpressionFolderPass>());
  pipeline.AddPass(std::make_unique<UnassignedTrailingCallArgTrimmerPass>());

  // 6. Scratch-reused parameter splitting, diamond tail inlining, loop-invariant sinking,
  // store-flag conditional recovery, and `-O0` frame pointer cleanup.
  pipeline.AddPass(std::make_unique<ScratchReusedParameterSplitAndFoldPass>());
  pipeline.AddPass(std::make_unique<CrossJumpedCallTailInlinerPass>(/*allow_diamond_tails=*/true));
  pipeline.AddPass(std::make_unique<CommonBranchTailSinkerPass>());
  pipeline.AddPass(std::make_unique<LoopInvariantSinkerPass>());
  pipeline.AddPass(std::make_unique<StoreFlagConditionalRecoveryPass>());
  pipeline.AddPass(std::make_unique<FramePointerCleanupPass>());

  return pipeline;
}

void AstPassPipeline::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return;
  AstPassPipeline pipeline = BuildDefaultPipeline();
  pipeline.RunOnBody(body, ctx);
}

}  // namespace rom_nom_nom
