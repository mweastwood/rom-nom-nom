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
#include "lifter/ast_arithmetic_simplifier.h"
#include "lifter/ast_control_flow_cleanup_pass.h"
#include "lifter/ast_expression_folder.h"
#include "lifter/ast_parameter_coalescer.h"
#include "lifter/ast_type_inference_pass.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

TEST(AstPassPipelineTest, RunToFixedPointStopsEarlyWhenPassesConverge) {
  CompoundStatement body;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
  body.AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  AstPassContext ctx{.parameters = params, .frame_size = 24};

  int call_count = 0;
  FunctionAstPass coalesce_pass(
      "CoalescePrologueParameterCopies",
      [&call_count](CompoundStatement* comp, const AstPassContext& pass_ctx) {
        ++call_count;
        CoalescePrologueParameterCopies(comp, pass_ctx.parameters, pass_ctx.frame_size);
      });
  AstPass* pass_ptr = &coalesce_pass;

  // First iteration coalesces temp_s0 -> arg0; second iteration observes no change and stops.
  int iterations = AstPassPipeline::RunToFixedPoint(&body, ctx, {&pass_ptr, 1},
                                                    /*max_iterations=*/5);
  EXPECT_EQ(iterations, 2);
  EXPECT_EQ(call_count, 2);
  EXPECT_THAT(body.ToString(), HasSubstr("return arg0;"));
  EXPECT_THAT(body.ToString(), Not(HasSubstr("temp_s0")));
}

TEST(AstPassPipelineTest, RunToFixedPointRespectsMaxIterationsAndHandlesNullBody) {
  std::vector<CParameter> params;
  AstPassContext ctx{.parameters = params};

  int toggle_count = 0;
  FunctionAstPass mutating_pass(
      "AlwaysMutatingPass", [&toggle_count](CompoundStatement* comp, const AstPassContext&) {
        ++toggle_count;
        comp->AddStatement(CStatement::Expression(CExpression::Integer(toggle_count)));
      });
  AstPass* pass_ptr = &mutating_pass;

  EXPECT_EQ(AstPassPipeline::RunToFixedPoint(nullptr, ctx, {&pass_ptr, 1}, 4), 0);

  CompoundStatement body;
  int iterations = AstPassPipeline::RunToFixedPoint(&body, ctx, {&pass_ptr, 1}, 3);
  EXPECT_EQ(iterations, 3);
  EXPECT_EQ(toggle_count, 3);
}

TEST(AstPassPipelineTest, FixedPointAstPassGroupComposesPolymorphicPasses) {
  CompoundStatement body;
  // v0 = (arg0 << 2) + arg0; return v0;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary(
          "+", CExpression::Binary("<<", CExpression::Identifier("arg0"), CExpression::Integer(2)),
          CExpression::Identifier("arg0")))));
  body.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  AstPassContext ctx{.parameters = params, .frame_size = 24, .return_type = CType::S32()};

  FixedPointAstPassGroup group("ArithAndFoldGroup", /*max_iterations=*/4);
  group.AddPass(std::make_unique<ShiftAddMultiplicationSimplifierPass>());
  group.AddPass(std::make_unique<SingleUseExpressionFolderPass>());

  EXPECT_EQ(group.Name(), "ArithAndFoldGroup");
  EXPECT_EQ(group.Passes().size(), 2u);
  EXPECT_TRUE(group.Run(&body, ctx));
  EXPECT_EQ(group.LastIterationsExecuted(), 2);
  EXPECT_THAT(body.ToString(), HasSubstr("return (arg0 * 5);"));
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

TEST(AstPassPipelineTest, ForLoopSynthesizerPassUpgradesGuardedDoWhileLoop) {
  CompoundStatement body;
  // temp_v1 = 0;
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_v1"), CExpression::Integer(0))));

  // if (temp_v1 < arg0) { do { *(s32*)arg1 = temp_v1; temp_v1 = temp_v1 + 1; } while (temp_v1 <
  // arg0); }
  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("arg1"))),
      CExpression::Identifier("temp_v1"))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_v1"),
      CExpression::Binary("+", CExpression::Identifier("temp_v1"), CExpression::Integer(1)))));

  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::DoWhile(
      std::move(loop_body), CExpression::Binary("<", CExpression::Identifier("temp_v1"),
                                                CExpression::Identifier("arg0"))));

  body.AddStatement(CStatement::If(
      CExpression::Binary(">", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(then_branch)));

  std::vector<CParameter> params = {
      {.type = CType::S32(), .name = "arg0"},
      {.type = CType::S32(), .name = "arg1"},
  };
  AstPassContext ctx{.parameters = params};

  ForLoopSynthesizerPass pass;
  EXPECT_EQ(pass.Name(), "SynthesizeForLoops");
  EXPECT_TRUE(pass.Run(&body, ctx));
  EXPECT_THAT(body.ToString(), HasSubstr("for (temp_v1 = 0; (temp_v1 < arg0); temp_v1++)"));
}

TEST(AstPassPipelineTest, ReturnAndGlobalTypePromotionPassPromotesNarrowCondSteppedVars) {
  CompoundStatement body;
  // if (arg0 != 0) { temp_v1 = temp_v1 + 1; }
  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_v1"),
      CExpression::Binary("+", CExpression::Identifier("temp_v1"), CExpression::Integer(1)))));
  body.AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(then_branch)));

  // return (u32)((temp_v1 & 255) ^ 3) < 1;
  body.AddStatement(CStatement::Return(CExpression::Binary(
      "<",
      CExpression::Cast(
          CType::U32(),
          CExpression::Binary("^",
                              CExpression::Binary("&", CExpression::Identifier("temp_v1"),
                                                  CExpression::Integer(255)),
                              CExpression::Integer(3))),
      CExpression::Integer(1))));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  AstPassContext ctx{.parameters = params, .return_type = CType::S32()};

  absl::flat_hash_map<std::string, CType> promoted_types;
  ReturnAndGlobalTypePromotionPass pass({"temp_v1"}, &promoted_types);
  EXPECT_EQ(pass.Name(), "ReturnAndGlobalTypePromotion");
  EXPECT_TRUE(pass.Run(&body, ctx));
  ASSERT_TRUE(promoted_types.contains("temp_v1"));
  EXPECT_EQ(promoted_types["temp_v1"].name, "u8");
  EXPECT_THAT(body.ToString(), HasSubstr("return ((temp_v1 & 255) == 3);"));
}

}  // namespace
}  // namespace rom_nom_nom
