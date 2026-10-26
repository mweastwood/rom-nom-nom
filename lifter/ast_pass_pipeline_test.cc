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

TEST(AstPassPipelineTest, ScratchRegisterWebSplitterPassSplitsDisjointBasicBlockWebs) {
  CompoundStatement body;
  body.AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t0"));

  auto then_block = std::make_unique<CompoundStatement>();
  then_block->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  then_block->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Unary("*", CExpression::Identifier("arg1")),
                              CExpression::Identifier("temp_t0"))));

  auto else_block = std::make_unique<CompoundStatement>();
  else_block->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0"),
      CExpression::Binary("-", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  else_block->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Unary("*", CExpression::Identifier("arg1")),
                              CExpression::Identifier("temp_t0"))));

  body.AddStatement(CStatement::If(
      CExpression::Binary(">", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(then_block), std::move(else_block)));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"},
                                    {.type = CType::Named("s32*"), .name = "arg1"}};
  AstPassContext ctx{.parameters = params, .return_type = CType::Void()};

  absl::flat_hash_map<std::string, std::vector<std::string>> split_vars;
  ScratchRegisterWebSplitterPass pass({"temp_t0"}, &split_vars);
  EXPECT_EQ(pass.Name(), "ScratchRegisterWebSplitter");
  EXPECT_TRUE(pass.Run(&body, ctx));
  ASSERT_TRUE(split_vars.contains("temp_t0"));
  ASSERT_EQ(split_vars["temp_t0"].size(), 1u);
  EXPECT_EQ(split_vars["temp_t0"][0], "temp_t0_2");

  const std::string rendered = body.ToString();
  EXPECT_THAT(rendered, HasSubstr("s32 temp_t0_2;"));
  EXPECT_THAT(rendered, HasSubstr("temp_t0 = (arg0 + 1);"));
  EXPECT_THAT(rendered, HasSubstr("temp_t0_2 = (arg0 - 1);"));
  EXPECT_THAT(rendered, HasSubstr("*arg1 = temp_t0_2;"));
}

TEST(AstPassPipelineTest, LoopInvariantSinkerSubstitutesConditionConstAndSymbolAddrAndFoldsLatch) {
  CompoundStatement body;
  // Pre-loop invariant constant `arg1 = -1` used only in the inner loop condition, and pre-loop
  // symbol address `temp_t0 = &D_8016F500` used inside an outer do-while loop with a trailing
  // latch comparison `v0 = ((u32)(arg1 & 255) < 24);` before a hoisted delay-slot `arg0 = 0;`.
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_t0"),
                              CExpression::Unary("&", CExpression::Identifier("D_8016F500")))));

  auto outer_body = std::make_unique<CompoundStatement>();
  outer_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg3"),
      CExpression::Binary(
          "+", CExpression::Binary("*", CExpression::Identifier("arg1"), CExpression::Integer(20)),
          CExpression::Identifier("temp_t0")))));
  outer_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("arg3"))))));
  outer_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg1"),
      CExpression::Binary("+", CExpression::Identifier("arg1"), CExpression::Integer(1)))));
  outer_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary(
          "<",
          CExpression::Cast(CType::U32(), CExpression::Binary("&", CExpression::Identifier("arg1"),
                                                              CExpression::Integer(255))),
          CExpression::Integer(24)))));
  outer_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg0"), CExpression::Integer(0))));

  body.AddStatement(CStatement::DoWhile(
      std::move(outer_body),
      CExpression::Binary("!=", CExpression::Identifier("v0"), CExpression::Integer(0))));

  // Second loop testing `arg2 = -1` read only in loop condition.
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg2"), CExpression::Integer(-1))));
  auto second_loop_body = std::make_unique<CompoundStatement>();
  second_loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("-", CExpression::Identifier("v1"), CExpression::Integer(1)))));
  body.AddStatement(CStatement::DoWhile(
      std::move(second_loop_body),
      CExpression::Binary("!=", CExpression::Identifier("v1"), CExpression::Identifier("arg2"))));

  std::vector<CParameter> params;
  AstPassContext ctx{.parameters = params, .return_type = CType::Void()};
  LoopInvariantSinkerPass pass;
  EXPECT_EQ(pass.Name(), "LoopInvariantSinker");
  EXPECT_TRUE(pass.Run(&body, ctx));

  const std::string rendered = body.ToString();
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t0 = &D_8016F500;")));
  EXPECT_THAT(rendered, HasSubstr("(s32)&D_8016F500"));
  EXPECT_THAT(rendered, HasSubstr("while (((u32)(arg1 & 255) < 24));"));
  EXPECT_THAT(rendered, Not(HasSubstr("arg2 = 0xFFFFFFFFFFFFFFFF;")));
  EXPECT_THAT(rendered, HasSubstr("while ((v1 != 0xFFFFFFFFFFFFFFFF));"));
}

TEST(AstPassPipelineTest, StoreFlagConditionalRecoveryPassRewritesStoreFlagIncDecToIf) {
  CompoundStatement body;
  // `count = (count + ((u32)(x ^ y) < 1));` -> `if ((x == y)) { count = (count + 1); }`
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("count"),
      CExpression::Binary(
          "+", CExpression::Identifier("count"),
          CExpression::Binary(
              "<",
              CExpression::Cast(CType::U32(), CExpression::Binary("^", CExpression::Identifier("x"),
                                                                  CExpression::Identifier("y"))),
              CExpression::Integer(1))))));

  // `*out = (*out - ((u32)0 < (u32)flag));` -> `if ((flag != 0)) { *out = (*out - 1); }`
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Unary("*", CExpression::Identifier("out")),
      CExpression::Binary(
          "-", CExpression::Unary("*", CExpression::Identifier("out")),
          CExpression::Binary("<", CExpression::Cast(CType::U32(), CExpression::Integer(0)),
                              CExpression::Cast(CType::U32(), CExpression::Identifier("flag")))))));

  EXPECT_TRUE(RecoverStoreFlagConditionals(&body));

  const std::string rendered = body.ToString();
  EXPECT_THAT(rendered, HasSubstr("if ((x == y)) {\n        count = (count + 1);\n    }"));
  EXPECT_THAT(rendered, HasSubstr("if ((flag != 0)) {\n        *out = (*out - 1);\n    }"));
}

TEST(AstPassPipelineTest, SparseSwitchDecisionTreePassReconstructsBinarySearchSwitch) {
  CompoundStatement body;
  // Build an AST corresponding to `stmt.c:emit_case_nodes` for sparse switch cases
  // `{10, 50, 100, 200, 500}` with default `*out = -1;`:
  //   if (arg0 == 100) {
  //     *out = 3;
  //   } else if (arg0 < 100) {
  //     if (arg0 == 10) {
  //       *out = 1;
  //     } else if (arg0 == 50) {
  //       *out = 2;
  //     } else {
  //       *out = -1;
  //     }
  //   } else {
  //     if (arg0 == 200) {
  //       *out = 4;
  //     } else if (arg0 == 500) {
  //       *out = 5;
  //     } else {
  //       *out = -1;
  //     }
  //   }
  auto make_out_assign = [](int64_t val) {
    auto comp = std::make_unique<CompoundStatement>();
    comp->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Unary("*", CExpression::Identifier("out")), CExpression::Integer(val))));
    return comp;
  };

  auto left_50 = CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("arg0"), CExpression::Integer(50)),
      make_out_assign(2), make_out_assign(-1));
  auto left_10 = CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("arg0"), CExpression::Integer(10)),
      make_out_assign(1), std::move(left_50));

  auto right_500 = CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("arg0"), CExpression::Integer(500)),
      make_out_assign(5), make_out_assign(-1));
  auto right_200 = CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("arg0"), CExpression::Integer(200)),
      make_out_assign(4), std::move(right_500));

  auto range_split = CStatement::If(
      CExpression::Binary("<", CExpression::Identifier("arg0"), CExpression::Integer(100)),
      std::move(left_10), std::move(right_200));

  body.AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("arg0"), CExpression::Integer(100)),
      make_out_assign(3), std::move(range_split)));

  std::vector<CParameter> params;
  AstPassContext ctx{.parameters = params, .return_type = CType::Void()};
  SparseSwitchDecisionTreePass pass;
  EXPECT_EQ(pass.Name(), "SparseSwitchDecisionTree");
  EXPECT_TRUE(pass.Run(&body, ctx));

  const std::string rendered = body.ToString();
  EXPECT_THAT(rendered, HasSubstr("switch (arg0)"));
  EXPECT_THAT(rendered, HasSubstr("case 10:"));
  EXPECT_THAT(rendered, HasSubstr("case 50:"));
  EXPECT_THAT(rendered, HasSubstr("case 100:"));
  EXPECT_THAT(rendered, HasSubstr("case 200:"));
  EXPECT_THAT(rendered, HasSubstr("case 500:"));
  EXPECT_THAT(rendered, HasSubstr("default:"));
}

TEST(AstPassPipelineTest,
     LoopInvariantSinkerHandlesNestedLoopsLongPreheadersAndGeneralizedGivRecovery) {
  CompoundStatement body;

  // 1. Nested loop with a 32-bit constant `temp_t3 = 0x00FF00FF` and condition constant
  // `temp_t4 = 16` separated by 5 intermediate assignments before the inner loop:
  auto outer_loop_body = std::make_unique<CompoundStatement>();
  outer_loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t3"), CExpression::Integer(0x00FF00FF))));
  outer_loop_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_t4"), CExpression::Integer(16))));
  for (int idx = 0; idx < 5; ++idx) {
    outer_loop_body->AddStatement(CStatement::Expression(
        CExpression::Assignment("=", CExpression::Identifier("temp_s" + std::to_string(idx)),
                                CExpression::Integer(idx + 1))));
  }
  auto inner_loop_body = std::make_unique<CompoundStatement>();
  inner_loop_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Unary("*(s32*)", CExpression::Identifier("arg0")),
                              CExpression::Binary("&", CExpression::Identifier("v1"),
                                                  CExpression::Identifier("temp_t3")))));
  inner_loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v1"), CExpression::Integer(1)))));
  outer_loop_body->AddStatement(CStatement::DoWhile(
      std::move(inner_loop_body),
      CExpression::Binary("<", CExpression::Identifier("v1"), CExpression::Identifier("temp_t4"))));
  body.AddStatement(CStatement::DoWhile(
      std::move(outer_loop_body),
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0))));

  // 2. Guarded for-loop with GIV in `temp_t2` initialized to `arg0` and stepped by `+ 12`:
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s7"), CExpression::Integer(0))));
  auto guard_then = std::make_unique<CompoundStatement>();
  guard_then->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t2"), CExpression::Identifier("arg0"))));
  auto giv_loop_body = std::make_unique<CompoundStatement>();
  giv_loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Unary("*(s32*)", CExpression::Identifier("temp_t2")),
      CExpression::Identifier("temp_s7"))));
  giv_loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s7"),
      CExpression::Binary("+", CExpression::Identifier("temp_s7"), CExpression::Integer(1)))));
  giv_loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t2"),
      CExpression::Binary("+", CExpression::Identifier("temp_t2"), CExpression::Integer(12)))));
  guard_then->AddStatement(CStatement::DoWhile(
      std::move(giv_loop_body),
      CExpression::Binary("<", CExpression::Identifier("temp_s7"), CExpression::Identifier("n"))));
  body.AddStatement(CStatement::If(
      CExpression::Binary(">", CExpression::Identifier("n"), CExpression::Integer(0)),
      std::move(guard_then)));

  std::vector<CParameter> params;
  AstPassContext ctx{.parameters = params, .return_type = CType::Void()};
  LoopInvariantSinkerPass pass;
  EXPECT_TRUE(pass.Run(&body, ctx));

  const std::string rendered = body.ToString();
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t3 =")));
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t4 =")));
  EXPECT_THAT(rendered, HasSubstr("(v1 & 16711935)"));
  EXPECT_THAT(rendered, HasSubstr("while ((v1 < 16));"));
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t2")));
  EXPECT_THAT(rendered, HasSubstr("for (temp_s7 = 0; (temp_s7 < n); temp_s7++)"));
  EXPECT_THAT(rendered, HasSubstr("*(s32*)(arg0 + (temp_s7 * 12)) = temp_s7;"));
}

TEST(AstPassPipelineTest,
     StoreFlagConditionalRecoveryHandlesPinnedRegistersArgsAndTwoStatementChains) {
  CompoundStatement body;

  // 1. Callee-saved register `temp_s0 = temp_s0 + (arg1 < 0)`:
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary(
          "+", CExpression::Identifier("temp_s0"),
          CExpression::Binary("<", CExpression::Identifier("arg1"), CExpression::Integer(0))))));

  // 2. Argument register `arg0 = arg0 - (arg2 >= 10)`:
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary(
          "-", CExpression::Identifier("arg0"),
          CExpression::Binary(">=", CExpression::Identifier("arg2"), CExpression::Integer(10))))));

  // 3. Two-statement store-flag chain `temp_t0 = (arg3 == 0); temp_s1 = temp_s1 + temp_t0;`:
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0"),
      CExpression::Binary("==", CExpression::Identifier("arg3"), CExpression::Integer(0)))));
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"),
                              CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                  CExpression::Identifier("temp_t0")))));

  std::vector<CParameter> params;
  AstPassContext ctx{.parameters = params, .return_type = CType::Void()};
  StoreFlagConditionalRecoveryPass pass;
  EXPECT_TRUE(pass.Run(&body, ctx));

  const std::string rendered = body.ToString();
  EXPECT_THAT(rendered, HasSubstr("if ((arg1 < 0)) {\n        temp_s0 = (temp_s0 + 1);\n    }"));
  EXPECT_THAT(rendered, HasSubstr("if ((arg2 >= 10)) {\n        arg0 = (arg0 - 1);\n    }"));
  EXPECT_THAT(rendered, HasSubstr("if ((arg3 == 0)) {\n        temp_s1 = (temp_s1 + 1);\n    }"));
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t0")));
}

TEST(AstPassPipelineTest,
     ForLoopSynthesizerRecoversMultiplePreheaderGivsAndFoldsScratchLatchCondition) {
  CompoundStatement body;

  // Guarded loop with TWO simultaneous preheader GIVs (`temp_t1` stepping by 12, `temp_t2` stepping
  // by 4) and a trailing `temp_t0 = (temp_s0 < arg2)` latch comparison before both GIV steps.
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s0"), CExpression::Integer(0))));
  auto guard_then = std::make_unique<CompoundStatement>();
  guard_then->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_t1"), CExpression::Integer(0))));
  guard_then->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_t2"), CExpression::Integer(0))));

  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*(s32*)", CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                        CExpression::Identifier("temp_t1"))),
      CExpression::Unary("*(s32*)", CExpression::Binary("+", CExpression::Identifier("arg1"),
                                                        CExpression::Identifier("temp_t2"))))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));
  loop_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_t0"),
                              CExpression::Binary("<", CExpression::Identifier("temp_s0"),
                                                  CExpression::Identifier("arg2")))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t1"),
      CExpression::Binary("+", CExpression::Identifier("temp_t1"), CExpression::Integer(12)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t2"),
      CExpression::Binary("+", CExpression::Identifier("temp_t2"), CExpression::Integer(4)))));

  guard_then->AddStatement(CStatement::DoWhile(
      std::move(loop_body),
      CExpression::Binary("!=", CExpression::Identifier("temp_t0"), CExpression::Integer(0))));
  body.AddStatement(CStatement::If(
      CExpression::Binary(">", CExpression::Identifier("arg2"), CExpression::Integer(0)),
      std::move(guard_then)));

  std::vector<CParameter> params;
  AstPassContext ctx{.parameters = params, .return_type = CType::Void()};
  LoopInvariantSinkerPass pass;
  EXPECT_TRUE(pass.Run(&body, ctx));

  const std::string rendered = body.ToString();
  EXPECT_THAT(rendered, HasSubstr("for (temp_s0 = 0; (temp_s0 < arg2); temp_s0++)"));
  EXPECT_THAT(rendered, HasSubstr("(temp_s0 * 12)"));
  EXPECT_THAT(rendered, HasSubstr("(temp_s0 * 4)"));
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t0")));
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t1")));
  EXPECT_THAT(rendered, Not(HasSubstr("temp_t2")));
}

TEST(AstPassPipelineTest, InferAndPromoteNarrowCondSteppedVarsHandlesU8MaskAndNonUnitElseStep) {
  auto body = std::make_unique<CompoundStatement>();

  // `temp_s0 = arg0;`
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
  // `if (arg1 != 0) {} else { temp_s0 = (temp_s0 + 2); }`
  auto empty_then = std::make_unique<CompoundStatement>();
  auto step_else = std::make_unique<CompoundStatement>();
  step_else->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(2)))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(empty_then), std::move(step_else)));
  // `return ((temp_s0 + arg2) & 255);`
  body->AddStatement(CStatement::Return(CExpression::Binary(
      "&",
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg2")),
      CExpression::Integer(255))));

  std::vector<std::string> candidates = {"temp_s0"};
  auto narrow_types = InferAndPromoteNarrowCondSteppedVars(candidates, &body);
  ASSERT_TRUE(narrow_types.contains("temp_s0"));
  EXPECT_EQ(narrow_types.at("temp_s0").ToString(), "u8");
  EXPECT_THAT(body->ToString(), HasSubstr("((u8)(temp_s0 + (u8)arg2) & 255)"));
}

}  // namespace
}  // namespace rom_nom_nom
