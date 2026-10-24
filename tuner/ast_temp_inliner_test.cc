#include "tuner/ast_temp_inliner.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "gtest/gtest.h"
#include "tuner/ast_mutator.h"

namespace rom_nom_nom {
namespace {

TEST(AstTempInlinerTest, TogglesCompoundAssignmentAndInlinesSingleUseTemp) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("t0"),
      CExpression::Binary("+", CExpression::Identifier("a0"), CExpression::Integer(4)))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("t0")));

  FunctionDeclaration func(CType::S32(), "TempInline", {{CType::S32(), "a0"}}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  step.set_site_index(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  EXPECT_FALSE(absl::StrContains(code, "t0 ="));
  EXPECT_TRUE(absl::StrContains(code, "return (a0 + 4);")) << code;
}

TEST(AstTempInlinerTest, LivenessAwareInlineAcrossOverwrittenTempAndHoist) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  // v0 = a0 + 1;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("a0"), CExpression::Integer(1)))));
  // v0 = v0 * 2; (single use of previous v0, and overwrites v0)
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("*", CExpression::Identifier("v0"), CExpression::Integer(2)))));
  // temp_s0 = v0; (consumes first v0 chain)
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("v0"))));
  // v0 = a1; (overwrites v0 again later in block)
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Identifier("a1"))));
  body->AddStatement(CStatement::Return(
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Identifier("v0"))));

  FunctionDeclaration func(CType::S32(), "ChainInline",
                           {{CType::S32(), "a0"}, {CType::S32(), "a1"}}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  step.set_site_index(0);
  step.set_param(1);  // inline all

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  EXPECT_FALSE(absl::StrContains(code, "v0 =")) << code;
  EXPECT_FALSE(absl::StrContains(code, "temp_s0 =")) << code;
  EXPECT_TRUE(absl::StrContains(code, "return (((a0 + 1) * 2) + a1);")) << code;
}

TEST(AstTempInlinerTest, InlineInsideInfiniteLoopAndFuncPtrCastCallee) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", CExpression::Integer(0)));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  // v0 = 10; (overwritten at top of while(1) loop)
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(10))));

  auto if_body = std::make_unique<CompoundStatement>();
  if_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("g_idle_callback"))));
  if_body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("((void (*)())temp_s0)"), {})));

  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("g_idle_callback"))));
  loop_body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("v0"), CExpression::Integer(0)),
      std::move(if_body)));

  body->AddStatement(CStatement::While(CExpression::Integer(1), std::move(loop_body)));

  FunctionDeclaration func(CType::Void(), "idle", {}, std::move(body));
  auto presets = AstMutator::GenerateCanonicalPresets(func);
  ASSERT_FALSE(presets.empty());
  FunctionDeclaration tuned = AstMutator::ApplyMutations(func, presets[0]);
  std::string code = tuned.ToString();
  EXPECT_FALSE(absl::StrContains(code, "v0")) << code;
  EXPECT_FALSE(absl::StrContains(code, "temp_s0")) << code;
  EXPECT_TRUE(absl::StrContains(code, "if ((g_idle_callback != 0))")) << code;
  EXPECT_TRUE(absl::StrContains(code, "((void (*)())g_idle_callback)();")) << code;
}

TEST(AstTempInlinerTest, MultiStatementInlineTempSinkAndHoistAndHazardRejection) {
  // temp_t6 = arg0 + 1;
  // g_other = 99;        (independent intervening statement)
  // g_dest = temp_t6;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t6"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t6"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("g_other"), CExpression::Integer(99))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_dest"), CExpression::Identifier("temp_t6"))));

  FunctionDeclaration func(CType::Void(), "CrossInline", {{CType::S32(), "arg0"}}, std::move(body));

  // param = 0: sink definition down to use site (`g_other = 99;` precedes `g_dest = (arg0 + 1);`)
  AstMutationStep sink_step;
  sink_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  sink_step.set_site_index(0);
  sink_step.set_param(0);
  std::string sunk = AstMutator::ApplyMutation(func, sink_step).ToString();
  size_t sunk_other_pos = sunk.find("g_other = 99;");
  size_t sunk_dest_pos = sunk.find("g_dest = (arg0 + 1);");
  ASSERT_NE(sunk_other_pos, std::string::npos) << sunk;
  ASSERT_NE(sunk_dest_pos, std::string::npos) << sunk;
  EXPECT_LT(sunk_other_pos, sunk_dest_pos) << sunk;

  // param = 2: hoist use statement up to definition site (`g_dest = (arg0 + 1);` precedes `g_other
  // = 99;`)
  AstMutationStep hoist_step;
  hoist_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  hoist_step.set_site_index(0);
  hoist_step.set_param(2);
  std::string hoisted = AstMutator::ApplyMutation(func, hoist_step).ToString();
  size_t hoisted_other_pos = hoisted.find("g_other = 99;");
  size_t hoisted_dest_pos = hoisted.find("g_dest = (arg0 + 1);");
  ASSERT_NE(hoisted_other_pos, std::string::npos) << hoisted;
  ASSERT_NE(hoisted_dest_pos, std::string::npos) << hoisted;
  EXPECT_LT(hoisted_dest_pos, hoisted_other_pos) << hoisted;

  // If the intervening statement modifies `arg0` AND reads `g_dest`, neither sink nor hoist is
  // valid.
  auto hazard_body = std::make_unique<CompoundStatement>();
  hazard_body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t6"));
  hazard_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t6"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  hazard_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"), CExpression::Identifier("g_dest"))));
  hazard_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_dest"), CExpression::Identifier("temp_t6"))));
  FunctionDeclaration hazard_func(CType::Void(), "HazardInline", {{CType::S32(), "arg0"}},
                                  std::move(hazard_body));
  std::string hazard_out = AstMutator::ApplyMutation(hazard_func, sink_step).ToString();
  EXPECT_TRUE(absl::StrContains(hazard_out, "temp_t6 = (arg0 + 1);")) << hazard_out;
}

TEST(AstTempInlinerTest, CoalesceParameterCopyPreservesPreCopyUsesAndPrunesDeadInitializer) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", CExpression::Integer(0)));
  // Pre-copy use of temp_s0 as a temporary before `temp_s0 = arg1;`
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType::U8().MakePointer(), CExpression::Identifier("arg1"))))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_val"), CExpression::Identifier("temp_s0"))));
  // Now copy parameter `arg1` into `temp_s0` and increment it
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  FunctionDeclaration function(CType::S32(), "PreCopyTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  AstMutationStep coalesce_step;
  coalesce_step.set_kind(AST_MUTATION_COALESCE_PARAM_COPY);
  coalesce_step.set_param(1);
  FunctionDeclaration coalesced = AstMutator::ApplyMutation(function, coalesce_step);
  std::string code = coalesced.ToString();
  // Pre-copy assignment to temp_s0 must NOT be overwritten with `arg1 = *(u8*)arg1;`
  EXPECT_TRUE(absl::StrContains(code, "temp_s0 = *(u8*)arg1;")) << code;
  EXPECT_TRUE(absl::StrContains(code, "arg1 = (arg1 + 1);")) << code;
  EXPECT_TRUE(absl::StrContains(code, "return arg1;")) << code;

  // Bulk inline (param = 1) should inline the pre-copy temp_s0 and strip `s32 temp_s0 = 0;`
  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string inlined = AstMutator::ApplyMutation(coalesced, inline_step).ToString();
  EXPECT_TRUE(absl::StrContains(inlined, "g_val = *(u8*)arg1;")) << inlined;
  EXPECT_FALSE(absl::StrContains(inlined, "temp_s0")) << inlined;
}

TEST(AstTempInlinerTest, ShiftAddMultiplyAndLoopPreheaderBackedgeNormalization) {
  // temp_s0 = 0;
  // arg0 = (0 & 255);
  // do {
  //   v0 = (arg0 << 1);
  //   v0 = (v0 + arg0);
  //   v0 = (v0 << 2);
  //   v0 = (v0 - arg0);
  //   v1 = (v0 << 2);
  //   temp_s0 = (temp_s0 + 1);
  //   if ((v1 != 0)) {
  //     Foo(arg0);
  //   }
  //   v0 = (temp_s0 & 255);
  //   v0 = ((u32)v0 < 10);
  //   arg0 = (temp_s0 & 255);
  // } while ((v0 != 0));
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1", CExpression::Integer(0)));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s0"), CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("&", CExpression::Integer(0), CExpression::Integer(255)))));

  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("<<", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Identifier("arg0")))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("<<", CExpression::Identifier("v0"), CExpression::Integer(2)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("-", CExpression::Identifier("v0"), CExpression::Identifier("arg0")))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("<<", CExpression::Identifier("v0"), CExpression::Integer(2)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));

  auto then_body = std::make_unique<CompoundStatement>();
  std::vector<std::unique_ptr<CExpression>> call_arguments;
  call_arguments.push_back(CExpression::Identifier("arg0"));
  then_body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("Foo"), std::move(call_arguments))));
  loop_body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("v1"), CExpression::Integer(0)),
      std::move(then_body)));

  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("&", CExpression::Identifier("temp_s0"), CExpression::Integer(255)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("<", CExpression::Cast(CType::U32(), CExpression::Identifier("v0")),
                          CExpression::Integer(10)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("&", CExpression::Identifier("temp_s0"), CExpression::Integer(255)))));

  body->AddStatement(CStatement::DoWhile(
      std::move(loop_body),
      CExpression::Binary("!=", CExpression::Identifier("v0"), CExpression::Integer(0))));

  FunctionDeclaration function(CType::Void(), "LoopMultiplyTest", {}, std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  EXPECT_FALSE(absl::StrContains(output, "arg0 = (0 & 255);")) << output;
  EXPECT_TRUE(absl::StrContains(output, "arg0 * 44")) << output;
  EXPECT_LT(output.find("Foo(arg0);"), output.find("temp_s0 = (temp_s0 + 1);")) << output;
}

TEST(AstTempInlinerTest, InlinesTemporaryInsideIfBranchWhenOverwrittenAfterOuterIfStatement) {
  // if (arg0 != 0) {
  //   v0 = arg0 + 5;
  //   g_result = v0;
  // }
  // if (arg1 != 0) {
  //   v0 = 10;
  // } else {
  //   v0 = 20;
  // }
  // return v0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0", CExpression::Integer(0)));

  auto first_then = std::make_unique<CompoundStatement>();
  first_then->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(5)))));
  first_then->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_result"), CExpression::Identifier("v0"))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(first_then)));

  auto second_then = std::make_unique<CompoundStatement>();
  second_then->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(10))));
  auto second_else = std::make_unique<CompoundStatement>();
  second_else->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(20))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(second_then), std::move(second_else)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration function(CType::S32(), "NestedIfOverwriteTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  EXPECT_TRUE(absl::StrContains(output, "g_result = (arg0 + 5);")) << output;
  // The `= 0` initializer on `s32 v0` is also stripped since `v0` is overwritten in both branches
  // of the second `if` before `return v0;`
  EXPECT_TRUE(absl::StrContains(output, "s32 v0;")) << output;
  EXPECT_FALSE(absl::StrContains(output, "s32 v0 = 0;")) << output;
}

TEST(AstTempInlinerTest, InfiniteWhileLoopPreheaderAndBackedgeRollIntoDoWhileLoop) {
  // v1 = *(u8 *)arg0;
  // while (1) {
  //   g_last = v1;
  //   arg0 = (arg0 + 1);
  //   if ((v0 == 0)) {
  //     break;
  //   }
  //   v1 = *(u8 *)arg0;
  // }
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
  auto load_byte = [&]() {
    return CExpression::Unary(
        "*", CExpression::Cast(CType::U8().MakePointer(), CExpression::Identifier("arg0")));
  };
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v1"), load_byte())));

  auto while_body = std::make_unique<CompoundStatement>();
  while_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_last"), CExpression::Identifier("v1"))));
  while_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  auto break_then = std::make_unique<CompoundStatement>();
  break_then->AddStatement(CStatement::Break());
  while_body->AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("v0"), CExpression::Integer(0)),
      std::move(break_then)));
  while_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v1"), load_byte())));

  body->AddStatement(CStatement::While(CExpression::Integer(1), std::move(while_body)));

  FunctionDeclaration function(CType::Void(), "StringScanTest", {{CType::S32(), "arg0"}},
                               std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  EXPECT_TRUE(absl::StrContains(output, "do {")) << output;
  EXPECT_TRUE(absl::StrContains(output, "} while ((v0 != 0));")) << output;
  EXPECT_FALSE(absl::StrContains(output, "while (1)")) << output;
}

TEST(AstTempInlinerTest, BranchDelaySlotSinksIntoAndAcrossIfStatement) {
  // Case A (sink into if):
  //   v0 = (arg0 + 4);
  //   if ((arg1 != 0)) {
  //     g_sink = v0;
  //   }
  // Case B (sink across if when !prefer_hoist):
  //   temp_s0 = (temp_s0 + 1);
  //   if ((arg1 == 0)) {
  //     g_other = 1;
  //   }
  //   return temp_s0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));

  // Case A
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(4)))));
  auto then_a = std::make_unique<CompoundStatement>();
  then_a->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_sink"), CExpression::Identifier("v0"))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(then_a)));

  // Case B
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));
  auto then_b = std::make_unique<CompoundStatement>();
  then_b->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("g_other"), CExpression::Integer(1))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(then_b)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  FunctionDeclaration function(CType::S32(), "DelaySlotSinkTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  // Case A: v0 is sunk into `then_a` and inlined into `g_sink = (arg0 + 4);`
  EXPECT_TRUE(absl::StrContains(output, "g_sink = (arg0 + 4);")) << output;
  EXPECT_FALSE(absl::StrContains(output, "v0")) << output;
  // Case B: `temp_s0 = (temp_s0 + 1);` is sunk after `if ((arg1 == 0))` and inlined into `return`
  EXPECT_LT(output.find("g_other = 1;"), output.find("return")) << output;
}

TEST(AstTempInlinerTest, SwapAdjacentStatementsRejectsIndirectMemoryReadsInAssignment) {
  // Statement 0: *(s32*)arg0 = 42;
  // Statement 1: v0 = *(s32*)arg0;
  // Should NOT be swappable!
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("arg0"))),
      CExpression::Integer(42))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("arg0"))))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration function(CType::S32(), "StoreLoadSwapTest",
                               {{CType::S32().MakePointer(), "arg0"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_SWAP_ADJACENT_STATEMENTS);
  }
}

TEST(AstTempInlinerTest, DeadAssignmentPreservesLiveVariableAssignedInInnerBlock) {
  // if (arg0 != 0) {
  //   v0 = (arg1 + 7);
  // }
  // return v0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));

  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("arg1"), CExpression::Integer(7)))));

  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(then_branch)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration function(CType::S32(), "InnerBlockAssignTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN);
  }
}

TEST(AstTempInlinerTest, CoalesceParamCopyRejectsReassignedParameter) {
  // temp_s0 = arg0;
  // arg0 = 0;
  // return temp_s0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg0"), CExpression::Integer(0))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  FunctionDeclaration function(CType::S32(), "ReassignedParamTest", {{CType::S32(), "arg0"}},
                               std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_COALESCE_PARAM_COPY);
  }
}

TEST(AstTempInlinerTest, CoalesceParamCopyReplacesInsideNestedBlocks) {
  // temp_s0 = arg0;
  // if (arg1 != 0) {
  //   g_dest = temp_s0;
  // }
  // return 0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));

  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_dest"), CExpression::Identifier("temp_s0"))));

  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(then_branch)));
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  FunctionDeclaration function(CType::S32(), "NestedParamCoalesceTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  bool found_coalesce = false;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_COALESCE_PARAM_COPY) {
      found_coalesce = true;
      FunctionDeclaration mutated = AstMutator::ApplyMutation(function, step);
      std::string code = mutated.ToString();
      EXPECT_FALSE(absl::StrContains(code, "temp_s0"));
      EXPECT_TRUE(absl::StrContains(code, "g_dest = arg0;"));
    }
  }
  EXPECT_TRUE(found_coalesce);
}

TEST(AstTempInlinerTest, AddressOfInliningRejectsBinaryPointerArithmetic) {
  // temp = &D_80145450;
  // return (arg0 + temp);
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp"));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp"),
                              CExpression::Unary("&", CExpression::Identifier("D_80145450")))));
  body->AddStatement(CStatement::Return(
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Identifier("temp"))));

  FunctionDeclaration function(CType::S32(), "AddressOfArithmeticTest", {{CType::S32(), "arg0"}},
                               std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  }
}

TEST(AstTempInlinerTest, AddressTakenVariableRejectsInlining) {
  // temp_at = 0x43380000;
  // *(f32 *)(temp_s1 + 68) = *(f32 *)&temp_at;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_at"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_at"), CExpression::Integer(0x43380000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::F32().MakePointer(),
                                 CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                     CExpression::Integer(68)))),
      CExpression::Unary(
          "*", CExpression::Cast(CType::F32().MakePointer(),
                                 CExpression::Unary("&", CExpression::Identifier("temp_at")))))));

  FunctionDeclaration function(CType::Void(), "AddressTakenInlineTest", {{CType::S32(), "temp_s1"}},
                               std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  }
}

TEST(AstTempInlinerTest, CompoundUnaryOpAddressTakenVariableRejectsInlining) {
  // temp_at = 0x43380000;
  // f0 = *(f32*)&temp_at;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_at"));
  body->AddStatement(CStatement::VariableDeclaration(CType::F32(), "f0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_at"), CExpression::Integer(0x43380000))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("f0"),
                              CExpression::Unary("*(f32*)&", CExpression::Identifier("temp_at")))));

  FunctionDeclaration function(CType::Void(), "CompoundUnaryOpInlineTest", {}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  }
}

TEST(AstTempInlinerTest, PruneUnusedLocalDeclarationsPreservesTrailingReturn) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "unused_var"));
  body->AddStatement(CStatement::Return());

  // Default preserve_trailing_return = true retains trailing `return;`
  auto pruned_preserve = PruneUnusedLocalDeclarations(*body, /*preserve_trailing_return=*/true);
  EXPECT_TRUE(absl::StrContains(pruned_preserve->ToString(), "return;"));
  EXPECT_FALSE(absl::StrContains(pruned_preserve->ToString(), "unused_var"));

  // preserve_trailing_return = false removes trailing `return;`
  auto pruned_strip = PruneUnusedLocalDeclarations(*body, /*preserve_trailing_return=*/false);
  EXPECT_FALSE(absl::StrContains(pruned_strip->ToString(), "return;"));
}

}  // namespace
}  // namespace rom_nom_nom
