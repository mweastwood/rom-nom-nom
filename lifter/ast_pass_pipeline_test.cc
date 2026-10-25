#include "lifter/ast_pass_pipeline.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/c_ast.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/ast_parameter_coalescer.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::UnorderedElementsAre;

TEST(AstPassPipelineTest, RunToFixedPointStopsEarlyWhenPassesConverge) {
  CompoundStatement body;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
  body.AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  AstPassContext ctx{.parameters = params, .frame_size = 24};

  static int call_count = 0;
  call_count = 0;
  AstPass coalesce_pass{
      .name = "CoalescePrologueParameterCopies",
      .run =
          [](CompoundStatement* comp, const AstPassContext& pass_ctx) {
            ++call_count;
            CoalescePrologueParameterCopies(comp, pass_ctx.parameters, pass_ctx.frame_size);
          },
  };

  // First iteration coalesces temp_s0 -> arg0; second iteration observes no change and stops.
  int iterations = AstPassPipeline::RunToFixedPoint(&body, ctx, {&coalesce_pass, 1},
                                                    /*max_iterations=*/5);
  EXPECT_EQ(iterations, 2);
  EXPECT_EQ(call_count, 2);
  EXPECT_THAT(body.ToString(), HasSubstr("return arg0;"));
  EXPECT_THAT(body.ToString(), Not(HasSubstr("temp_s0")));
}

TEST(AstPassPipelineTest, RunToFixedPointRespectsMaxIterationsAndHandlesNullBody) {
  std::vector<CParameter> params;
  AstPassContext ctx{.parameters = params};

  static int toggle_count = 0;
  toggle_count = 0;
  AstPass mutating_pass{
      .name = "AlwaysMutatingPass",
      .run =
          [](CompoundStatement* comp, const AstPassContext&) {
            ++toggle_count;
            comp->AddStatement(CStatement::Expression(CExpression::Integer(toggle_count)));
          },
  };

  EXPECT_EQ(AstPassPipeline::RunToFixedPoint(nullptr, ctx, {&mutating_pass, 1}, 4), 0);

  CompoundStatement body;
  int iterations = AstPassPipeline::RunToFixedPoint(&body, ctx, {&mutating_pass, 1}, 3);
  EXPECT_EQ(iterations, 3);
  EXPECT_EQ(toggle_count, 3);
}

TEST(AstPassPipelineTest, RunConsolidatesBranchTailSinkingAfterParameterCoalescingAndReturnFold) {
  // Construct a function body where `temp_s0 = arg0` is coalesced in the prologue, exposing a
  // common branch tail assignment (`v0 = (arg0 + 4);`) after
  // `SimplifyDecoupledDelaySlotEarlyReturns` and `CoalescePrologueParameterCopies`, which
  // `SinkCommonBranchTailArgAssignments` in `kPostEarlyReturnPasses` sinks and folds into `return
  // (arg0 + 4);`.
  CompoundStatement body;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));

  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Call(
      CExpression::Identifier("func_80001000"), std::vector<std::unique_ptr<CExpression>>{})));
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(4)))));

  auto else_branch = std::make_unique<CompoundStatement>();
  else_branch->AddStatement(CStatement::Expression(CExpression::Call(
      CExpression::Identifier("func_80001004"), std::vector<std::unique_ptr<CExpression>>{})));
  else_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(4)))));

  body.AddStatement(CStatement::If(CExpression::Identifier("cond"), std::move(then_branch),
                                   std::move(else_branch)));
  body.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  AstPassContext ctx{
      .parameters = params,
      .frame_size = 24,
      .return_type = CType::S32(),
      .mutable_parameters = &params,
  };

  AstPassPipeline::Run(&body, ctx);

  std::string emitted = body.ToString();
  EXPECT_THAT(emitted, Not(HasSubstr("temp_s0")));
  EXPECT_THAT(emitted, HasSubstr("return (arg0 + 4);"));
}

}  // namespace
}  // namespace rom_nom_nom
