#include "core/c_ast_visitor.h"

#include <memory>

#include "core/c_ast.h"

namespace rom_nom_nom {

void AstVisitor::VisitFunction(const FunctionDeclaration& func) {
  if (ShouldStop()) return;
  VisitCompoundStatement(func.Body());
}

void AstVisitor::VisitStatement(const CStatement& stmt) {
  if (ShouldStop()) return;
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      VisitCompoundStatement(static_cast<const CompoundStatement&>(stmt));
      break;
    case CStatementKind::kExpressionStatement:
      VisitExpressionStatement(static_cast<const ExpressionStatement&>(stmt));
      break;
    case CStatementKind::kVariableDeclarationStatement:
      VisitVariableDeclarationStatement(static_cast<const VariableDeclarationStatement&>(stmt));
      break;
    case CStatementKind::kIfStatement:
      VisitIfStatement(static_cast<const IfStatement&>(stmt));
      break;
    case CStatementKind::kWhileStatement:
      VisitWhileStatement(static_cast<const WhileStatement&>(stmt));
      break;
    case CStatementKind::kDoWhileStatement:
      VisitDoWhileStatement(static_cast<const DoWhileStatement&>(stmt));
      break;
    case CStatementKind::kForStatement:
      VisitForStatement(static_cast<const ForStatement&>(stmt));
      break;
    case CStatementKind::kReturnStatement:
      VisitReturnStatement(static_cast<const ReturnStatement&>(stmt));
      break;
    case CStatementKind::kBreakStatement:
      VisitBreakStatement(static_cast<const BreakStatement&>(stmt));
      break;
    case CStatementKind::kContinueStatement:
      VisitContinueStatement(static_cast<const ContinueStatement&>(stmt));
      break;
    case CStatementKind::kGotoStatement:
      VisitGotoStatement(static_cast<const GotoStatement&>(stmt));
      break;
    case CStatementKind::kLabelStatement:
      VisitLabelStatement(static_cast<const LabelStatement&>(stmt));
      break;
    case CStatementKind::kCaseStatement:
      VisitCaseStatement(static_cast<const CaseStatement&>(stmt));
      break;
    case CStatementKind::kSwitchStatement:
      VisitSwitchStatement(static_cast<const SwitchStatement&>(stmt));
      break;
    case CStatementKind::kCommentStatement:
      VisitCommentStatement(static_cast<const CommentStatement&>(stmt));
      break;
  }
}

void AstVisitor::VisitExpression(const CExpression& expr) {
  if (ShouldStop()) return;
  switch (expr.Kind()) {
    case CExpressionKind::kIntegerLiteral:
      VisitIntegerLiteralExpression(static_cast<const IntegerLiteralExpression&>(expr));
      break;
    case CExpressionKind::kFloatLiteral:
      VisitFloatLiteralExpression(static_cast<const FloatLiteralExpression&>(expr));
      break;
    case CExpressionKind::kStringLiteral:
      VisitStringLiteralExpression(static_cast<const StringLiteralExpression&>(expr));
      break;
    case CExpressionKind::kIdentifier:
      VisitIdentifierExpression(static_cast<const IdentifierExpression&>(expr));
      break;
    case CExpressionKind::kUnaryExpression:
      VisitUnaryExpression(static_cast<const UnaryExpression&>(expr));
      break;
    case CExpressionKind::kBinaryExpression:
      VisitBinaryExpression(static_cast<const BinaryExpression&>(expr));
      break;
    case CExpressionKind::kAssignmentExpression:
      VisitAssignmentExpression(static_cast<const AssignmentExpression&>(expr));
      break;
    case CExpressionKind::kCallExpression:
      VisitCallExpression(static_cast<const CallExpression&>(expr));
      break;
    case CExpressionKind::kCastExpression:
      VisitCastExpression(static_cast<const CastExpression&>(expr));
      break;
    case CExpressionKind::kMemberAccessExpression:
      VisitMemberAccessExpression(static_cast<const MemberAccessExpression&>(expr));
      break;
    case CExpressionKind::kArrayIndexExpression:
      VisitArrayIndexExpression(static_cast<const ArrayIndexExpression&>(expr));
      break;
    case CExpressionKind::kTernaryExpression:
      VisitTernaryExpression(static_cast<const TernaryExpression&>(expr));
      break;
  }
}

void AstVisitor::VisitCompoundStatement(const CompoundStatement& stmt) {
  for (const auto& child : stmt.Statements()) {
    if (ShouldStop()) return;
    if (child != nullptr) VisitStatement(*child);
  }
}

void AstVisitor::VisitExpressionStatement(const ExpressionStatement& stmt) {
  VisitExpression(stmt.Expression());
}

void AstVisitor::VisitVariableDeclarationStatement(const VariableDeclarationStatement& stmt) {
  if (stmt.Initializer() != nullptr) {
    VisitExpression(*stmt.Initializer());
  }
}

void AstVisitor::VisitIfStatement(const IfStatement& stmt) {
  VisitExpression(stmt.Condition());
  if (ShouldStop()) return;
  VisitStatement(stmt.ThenBranch());
  if (ShouldStop()) return;
  if (stmt.ElseBranch() != nullptr) {
    VisitStatement(*stmt.ElseBranch());
  }
}

void AstVisitor::VisitWhileStatement(const WhileStatement& stmt) {
  VisitExpression(stmt.Condition());
  if (ShouldStop()) return;
  VisitStatement(stmt.Body());
}

void AstVisitor::VisitDoWhileStatement(const DoWhileStatement& stmt) {
  VisitStatement(stmt.Body());
  if (ShouldStop()) return;
  VisitExpression(stmt.Condition());
}

void AstVisitor::VisitForStatement(const ForStatement& stmt) {
  if (stmt.Init() != nullptr) {
    VisitStatement(*stmt.Init());
    if (ShouldStop()) return;
  }
  if (stmt.Condition() != nullptr) {
    VisitExpression(*stmt.Condition());
    if (ShouldStop()) return;
  }
  if (stmt.Step() != nullptr) {
    VisitExpression(*stmt.Step());
    if (ShouldStop()) return;
  }
  VisitStatement(stmt.Body());
}

void AstVisitor::VisitReturnStatement(const ReturnStatement& stmt) {
  if (stmt.ReturnValue() != nullptr) {
    VisitExpression(*stmt.ReturnValue());
  }
}

void AstVisitor::VisitBreakStatement(const BreakStatement& /*stmt*/) {}
void AstVisitor::VisitContinueStatement(const ContinueStatement& /*stmt*/) {}
void AstVisitor::VisitGotoStatement(const GotoStatement& /*stmt*/) {}
void AstVisitor::VisitLabelStatement(const LabelStatement& /*stmt*/) {}
void AstVisitor::VisitCaseStatement(const CaseStatement& /*stmt*/) {}

void AstVisitor::VisitSwitchStatement(const SwitchStatement& stmt) {
  VisitExpression(stmt.Condition());
  if (ShouldStop()) return;
  for (const auto& switch_case : stmt.Cases()) {
    if (ShouldStop()) return;
    if (switch_case.body != nullptr) {
      VisitCompoundStatement(*switch_case.body);
    }
  }
}

void AstVisitor::VisitCommentStatement(const CommentStatement& /*stmt*/) {}

void AstVisitor::VisitIntegerLiteralExpression(const IntegerLiteralExpression& /*expr*/) {}
void AstVisitor::VisitFloatLiteralExpression(const FloatLiteralExpression& /*expr*/) {}
void AstVisitor::VisitStringLiteralExpression(const StringLiteralExpression& /*expr*/) {}
void AstVisitor::VisitIdentifierExpression(const IdentifierExpression& /*expr*/) {}

void AstVisitor::VisitUnaryExpression(const UnaryExpression& expr) {
  VisitExpression(expr.Operand());
}

void AstVisitor::VisitBinaryExpression(const BinaryExpression& expr) {
  VisitExpression(expr.Lhs());
  if (ShouldStop()) return;
  VisitExpression(expr.Rhs());
}

void AstVisitor::VisitAssignmentExpression(const AssignmentExpression& expr) {
  VisitExpression(expr.Lhs());
  if (ShouldStop()) return;
  VisitExpression(expr.Rhs());
}

void AstVisitor::VisitCallExpression(const CallExpression& expr) {
  VisitExpression(expr.Callee());
  if (ShouldStop()) return;
  for (const auto& arg : expr.Arguments()) {
    if (ShouldStop()) return;
    if (arg != nullptr) VisitExpression(*arg);
  }
}

void AstVisitor::VisitCastExpression(const CastExpression& expr) {
  VisitExpression(expr.Operand());
}

void AstVisitor::VisitMemberAccessExpression(const MemberAccessExpression& expr) {
  VisitExpression(expr.Object());
}

void AstVisitor::VisitArrayIndexExpression(const ArrayIndexExpression& expr) {
  VisitExpression(expr.Array());
  if (ShouldStop()) return;
  VisitExpression(expr.Index());
}

void AstVisitor::VisitTernaryExpression(const TernaryExpression& expr) {
  VisitExpression(expr.Condition());
  if (ShouldStop()) return;
  VisitExpression(expr.TrueExpression());
  if (ShouldStop()) return;
  VisitExpression(expr.FalseExpression());
}

bool AstMutator::MutateFunction(FunctionDeclaration* func) {
  if (func == nullptr || func->MutableBody() == nullptr) return false;
  ResetChanged();
  MutateCompoundStatement(func->MutableBody());
  return Changed();
}

void AstMutator::MutateStatement(std::unique_ptr<CStatement>* stmt) {
  if (stmt == nullptr || *stmt == nullptr) return;
  switch ((*stmt)->Kind()) {
    case CStatementKind::kCompoundStatement:
      MutateCompoundStatement(static_cast<CompoundStatement*>(stmt->get()));
      break;
    case CStatementKind::kExpressionStatement:
      MutateExpressionStatement(stmt, static_cast<ExpressionStatement*>(stmt->get()));
      break;
    case CStatementKind::kVariableDeclarationStatement:
      MutateVariableDeclarationStatement(stmt,
                                         static_cast<VariableDeclarationStatement*>(stmt->get()));
      break;
    case CStatementKind::kIfStatement:
      MutateIfStatement(stmt, static_cast<IfStatement*>(stmt->get()));
      break;
    case CStatementKind::kWhileStatement:
      MutateWhileStatement(stmt, static_cast<WhileStatement*>(stmt->get()));
      break;
    case CStatementKind::kDoWhileStatement:
      MutateDoWhileStatement(stmt, static_cast<DoWhileStatement*>(stmt->get()));
      break;
    case CStatementKind::kForStatement:
      MutateForStatement(stmt, static_cast<ForStatement*>(stmt->get()));
      break;
    case CStatementKind::kReturnStatement:
      MutateReturnStatement(stmt, static_cast<ReturnStatement*>(stmt->get()));
      break;
    case CStatementKind::kBreakStatement:
      MutateBreakStatement(stmt, static_cast<BreakStatement*>(stmt->get()));
      break;
    case CStatementKind::kContinueStatement:
      MutateContinueStatement(stmt, static_cast<ContinueStatement*>(stmt->get()));
      break;
    case CStatementKind::kGotoStatement:
      MutateGotoStatement(stmt, static_cast<GotoStatement*>(stmt->get()));
      break;
    case CStatementKind::kLabelStatement:
      MutateLabelStatement(stmt, static_cast<LabelStatement*>(stmt->get()));
      break;
    case CStatementKind::kCaseStatement:
      MutateCaseStatement(stmt, static_cast<CaseStatement*>(stmt->get()));
      break;
    case CStatementKind::kSwitchStatement:
      MutateSwitchStatement(stmt, static_cast<SwitchStatement*>(stmt->get()));
      break;
    case CStatementKind::kCommentStatement:
      MutateCommentStatement(stmt, static_cast<CommentStatement*>(stmt->get()));
      break;
  }
}

void AstMutator::MutateExpression(std::unique_ptr<CExpression>* expr) {
  if (expr == nullptr || *expr == nullptr) return;
  switch ((*expr)->Kind()) {
    case CExpressionKind::kIntegerLiteral:
      MutateIntegerLiteralExpression(expr, static_cast<IntegerLiteralExpression*>(expr->get()));
      break;
    case CExpressionKind::kFloatLiteral:
      MutateFloatLiteralExpression(expr, static_cast<FloatLiteralExpression*>(expr->get()));
      break;
    case CExpressionKind::kStringLiteral:
      MutateStringLiteralExpression(expr, static_cast<StringLiteralExpression*>(expr->get()));
      break;
    case CExpressionKind::kIdentifier:
      MutateIdentifierExpression(expr, static_cast<IdentifierExpression*>(expr->get()));
      break;
    case CExpressionKind::kUnaryExpression:
      MutateUnaryExpression(expr, static_cast<UnaryExpression*>(expr->get()));
      break;
    case CExpressionKind::kBinaryExpression:
      MutateBinaryExpression(expr, static_cast<BinaryExpression*>(expr->get()));
      break;
    case CExpressionKind::kAssignmentExpression:
      MutateAssignmentExpression(expr, static_cast<AssignmentExpression*>(expr->get()));
      break;
    case CExpressionKind::kCallExpression:
      MutateCallExpression(expr, static_cast<CallExpression*>(expr->get()));
      break;
    case CExpressionKind::kCastExpression:
      MutateCastExpression(expr, static_cast<CastExpression*>(expr->get()));
      break;
    case CExpressionKind::kMemberAccessExpression:
      MutateMemberAccessExpression(expr, static_cast<MemberAccessExpression*>(expr->get()));
      break;
    case CExpressionKind::kArrayIndexExpression:
      MutateArrayIndexExpression(expr, static_cast<ArrayIndexExpression*>(expr->get()));
      break;
    case CExpressionKind::kTernaryExpression:
      MutateTernaryExpression(expr, static_cast<TernaryExpression*>(expr->get()));
      break;
  }
}

void AstMutator::MutateCompoundStatement(CompoundStatement* stmt) {
  if (stmt == nullptr) return;
  for (auto& child : stmt->MutableStatements()) {
    MutateStatement(&child);
  }
}

void AstMutator::MutateExpressionStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                           ExpressionStatement* expr_stmt) {
  MutateExpression(&expr_stmt->MutableExpression());
}

void AstMutator::MutateVariableDeclarationStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                                    VariableDeclarationStatement* decl_stmt) {
  if (decl_stmt->Initializer() != nullptr) {
    MutateExpression(&decl_stmt->MutableInitializer());
  }
}

void AstMutator::MutateIfStatement(std::unique_ptr<CStatement>* /*stmt*/, IfStatement* if_stmt) {
  MutateExpression(&if_stmt->MutableCondition());
  MutateStatement(&if_stmt->MutableThenBranchPtr());
  if (if_stmt->MutableElseBranchPtr() != nullptr) {
    MutateStatement(&if_stmt->MutableElseBranchPtr());
  }
}

void AstMutator::MutateWhileStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                      WhileStatement* while_stmt) {
  MutateExpression(&while_stmt->MutableCondition());
  MutateStatement(&while_stmt->MutableBodyPtr());
}

void AstMutator::MutateDoWhileStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                        DoWhileStatement* do_while_stmt) {
  MutateStatement(&do_while_stmt->MutableBodyPtr());
  MutateExpression(&do_while_stmt->MutableCondition());
}

void AstMutator::MutateForStatement(std::unique_ptr<CStatement>* /*stmt*/, ForStatement* for_stmt) {
  if (for_stmt->MutableInitPtr() != nullptr) {
    MutateStatement(&for_stmt->MutableInitPtr());
  }
  if (for_stmt->MutableConditionPtr() != nullptr) {
    MutateExpression(&for_stmt->MutableConditionPtr());
  }
  if (for_stmt->MutableStepPtr() != nullptr) {
    MutateExpression(&for_stmt->MutableStepPtr());
  }
  MutateStatement(&for_stmt->MutableBodyPtr());
}

void AstMutator::MutateReturnStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                       ReturnStatement* return_stmt) {
  if (return_stmt->MutableReturnValue() != nullptr) {
    MutateExpression(&return_stmt->MutableReturnValue());
  }
}

void AstMutator::MutateBreakStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                      BreakStatement* /*break_stmt*/) {}
void AstMutator::MutateContinueStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                         ContinueStatement* /*continue_stmt*/) {}
void AstMutator::MutateGotoStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                     GotoStatement* /*goto_stmt*/) {}
void AstMutator::MutateLabelStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                      LabelStatement* /*label_stmt*/) {}
void AstMutator::MutateCaseStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                     CaseStatement* /*case_stmt*/) {}

void AstMutator::MutateSwitchStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                       SwitchStatement* switch_stmt) {
  MutateExpression(&switch_stmt->MutableCondition());
  for (auto& switch_case : switch_stmt->MutableCases()) {
    if (switch_case.body != nullptr) {
      MutateCompoundStatement(switch_case.body.get());
    }
  }
}

void AstMutator::MutateCommentStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                        CommentStatement* /*comment_stmt*/) {}

void AstMutator::MutateIntegerLiteralExpression(std::unique_ptr<CExpression>* /*expr*/,
                                                IntegerLiteralExpression* /*int_expr*/) {}
void AstMutator::MutateFloatLiteralExpression(std::unique_ptr<CExpression>* /*expr*/,
                                              FloatLiteralExpression* /*float_expr*/) {}
void AstMutator::MutateStringLiteralExpression(std::unique_ptr<CExpression>* /*expr*/,
                                               StringLiteralExpression* /*str_expr*/) {}
void AstMutator::MutateIdentifierExpression(std::unique_ptr<CExpression>* /*expr*/,
                                            IdentifierExpression* /*id_expr*/) {}

void AstMutator::MutateUnaryExpression(std::unique_ptr<CExpression>* /*expr*/,
                                       UnaryExpression* unary_expr) {
  MutateExpression(&unary_expr->MutableOperand());
}

void AstMutator::MutateBinaryExpression(std::unique_ptr<CExpression>* /*expr*/,
                                        BinaryExpression* binary_expr) {
  MutateExpression(&binary_expr->MutableLhs());
  MutateExpression(&binary_expr->MutableRhs());
}

void AstMutator::MutateAssignmentExpression(std::unique_ptr<CExpression>* /*expr*/,
                                            AssignmentExpression* assign_expr) {
  MutateExpression(&assign_expr->MutableLhs());
  MutateExpression(&assign_expr->MutableRhs());
}

void AstMutator::MutateCallExpression(std::unique_ptr<CExpression>* /*expr*/,
                                      CallExpression* call_expr) {
  MutateExpression(&call_expr->MutableCallee());
  for (auto& arg : call_expr->MutableArguments()) {
    MutateExpression(&arg);
  }
}

void AstMutator::MutateCastExpression(std::unique_ptr<CExpression>* /*expr*/,
                                      CastExpression* cast_expr) {
  MutateExpression(&cast_expr->MutableOperand());
}

void AstMutator::MutateMemberAccessExpression(std::unique_ptr<CExpression>* /*expr*/,
                                              MemberAccessExpression* member_expr) {
  MutateExpression(&member_expr->MutableObject());
}

void AstMutator::MutateArrayIndexExpression(std::unique_ptr<CExpression>* /*expr*/,
                                            ArrayIndexExpression* array_expr) {
  MutateExpression(&array_expr->MutableArray());
  MutateExpression(&array_expr->MutableIndex());
}

void AstMutator::MutateTernaryExpression(std::unique_ptr<CExpression>* /*expr*/,
                                         TernaryExpression* ternary_expr) {
  MutateExpression(&ternary_expr->MutableCondition());
  MutateExpression(&ternary_expr->MutableTrueExpression());
  MutateExpression(&ternary_expr->MutableFalseExpression());
}

}  // namespace rom_nom_nom
