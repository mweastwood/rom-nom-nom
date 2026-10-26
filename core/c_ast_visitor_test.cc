#include "core/c_ast_visitor.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/c_ast.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(AstVisitorTest, CountsNodesAcrossComplexFunction) {
  class NodeCounterVisitor : public AstVisitor {
   public:
    int identifier_count = 0;
    int binary_count = 0;
    int statement_count = 0;

    void VisitStatement(const CStatement& stmt) override {
      ++statement_count;
      AstVisitor::VisitStatement(stmt);
    }

    void VisitIdentifierExpression(const IdentifierExpression& /*expr*/) override {
      ++identifier_count;
    }

    void VisitBinaryExpression(const BinaryExpression& expr) override {
      ++binary_count;
      AstVisitor::VisitBinaryExpression(expr);
    }
  };

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType{"s32"}, "x", CExpression::Integer(0)));
  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("x"),
      CExpression::Binary("+", CExpression::Identifier("a"), CExpression::Identifier("b")))));
  body->AddStatement(CStatement::If(
      CExpression::Binary(">", CExpression::Identifier("a"), CExpression::Integer(0)),
      std::move(then_branch)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("x")));

  FunctionDeclaration func(CType{"s32"}, "test_fn", {{CType{"s32"}, "a"}, {CType{"s32"}, "b"}},
                           std::move(body));

  NodeCounterVisitor visitor;
  visitor.VisitFunction(func);
  EXPECT_EQ(visitor.identifier_count, 5);
  EXPECT_EQ(visitor.binary_count, 2);
  EXPECT_EQ(visitor.statement_count, 5);
}

TEST(AstVisitorTest, ShortCircuitsWhenStopped) {
  class StopAtFirstIdentifierVisitor : public AstVisitor {
   public:
    std::vector<std::string> visited;

    void VisitIdentifierExpression(const IdentifierExpression& expr) override {
      visited.push_back(expr.Name());
      Stop();
    }
  };

  auto expr =
      CExpression::Binary("+", CExpression::Identifier("first"), CExpression::Identifier("second"));
  StopAtFirstIdentifierVisitor visitor;
  visitor.VisitExpression(*expr);
  ASSERT_EQ(visitor.visited.size(), 1u);
  EXPECT_EQ(visitor.visited[0], "first");
  EXPECT_TRUE(visitor.ShouldStop());
}

TEST(AstMutatorTest, RewritesIdentifiersAndReplacesStatementsInPlace) {
  class RenameAndUpgradeMutator : public AstMutator {
   public:
    void MutateIdentifierExpression(std::unique_ptr<CExpression>* expr,
                                    IdentifierExpression* id_expr) override {
      if (id_expr->Name() == "old_var") {
        *expr = CExpression::Identifier("new_var");
        MarkChanged();
      }
    }

    void MutateReturnStatement(std::unique_ptr<CStatement>* stmt,
                               ReturnStatement* return_stmt) override {
      AstMutator::MutateReturnStatement(stmt, return_stmt);
      if (return_stmt->ReturnValue() != nullptr) {
        *stmt = CStatement::Return(
            CExpression::Binary("+", return_stmt->ReturnValue()->Clone(), CExpression::Integer(1)));
        MarkChanged();
      }
    }
  };

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("old_var"), CExpression::Integer(10))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("old_var")));
  FunctionDeclaration func(CType{"s32"}, "mutate_fn", {}, std::move(body));

  RenameAndUpgradeMutator mutator;
  EXPECT_TRUE(mutator.MutateFunction(&func));
  EXPECT_EQ(func.ToString(),
            "s32 mutate_fn(void) {\n"
            "    new_var = 10;\n"
            "    return (new_var + 1);\n"
            "}\n");
}

}  // namespace
}  // namespace rom_nom_nom
