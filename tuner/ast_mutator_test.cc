#include "tuner/ast_mutator.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(AstMutatorTest, CommutesBinaryAndRelationalExpressions) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Return(
      CExpression::Binary("+", CExpression::Identifier("a"), CExpression::Identifier("b"))));

  FunctionDeclaration func(CType::S32(), "Add", {{CType::S32(), "a"}, {CType::S32(), "b"}},
                           std::move(body));

  auto steps = AstMutator::DiscoverMutations(func);
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].kind(), AST_MUTATION_COMMUTE_BINARY_OP);
  EXPECT_EQ(steps[0].site_index(), 0u);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, steps[0]);
  EXPECT_TRUE(absl::StrContains(mutated.ToString(), "return (b + a);"));
}

TEST(AstMutatorTest, InvertsIfElseAndNegatesCondition) {
  auto then_block = std::make_unique<CompoundStatement>();
  then_block->AddStatement(CStatement::Return(CExpression::Integer(1)));
  auto else_block = std::make_unique<CompoundStatement>();
  else_block->AddStatement(CStatement::Return(CExpression::Integer(2)));

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("x"), CExpression::Integer(0)),
      std::move(then_block), std::move(else_block)));

  FunctionDeclaration func(CType::S32(), "Check", {{CType::S32(), "x"}}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_INVERT_IF_ELSE);
  step.set_site_index(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(code, "if ((x != 0))")) << code;
  size_t pos_ret2 = code.find("return 2;");
  size_t pos_ret1 = code.find("return 1;");
  ASSERT_NE(pos_ret2, std::string::npos);
  ASSERT_NE(pos_ret1, std::string::npos);
  EXPECT_LT(pos_ret2, pos_ret1);
}

TEST(AstMutatorTest, ReordersIndependentLocalDeclarations) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  FunctionDeclaration func(CType::S32(), "Locals", {}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_REORDER_LOCAL_DECLS);
  step.set_site_index(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  size_t pos_v1 = code.find("s32 v1;");
  size_t pos_v0 = code.find("s32 v0;");
  ASSERT_NE(pos_v1, std::string::npos);
  ASSERT_NE(pos_v0, std::string::npos);
  EXPECT_LT(pos_v1, pos_v0);
}

TEST(AstMutatorTest, TogglesCompoundAssignmentAndInlinesSingleUseTemp) {
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

}  // namespace
}  // namespace rom_nom_nom
