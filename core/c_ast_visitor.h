#ifndef CORE_C_AST_VISITOR_H_
#define CORE_C_AST_VISITOR_H_

#include <memory>

#include "core/c_ast.h"

namespace rom_nom_nom {

// Read-only AST visitor with default recursive traversal over all expression and statement nodes.
// Subclasses can override specific `Visit*` methods to inspect nodes and call `Stop()` to
// short-circuit traversal early.
class AstVisitor {
 public:
  virtual ~AstVisitor() = default;

  virtual void VisitFunction(const FunctionDeclaration& func);
  virtual void VisitStatement(const CStatement& stmt);
  virtual void VisitExpression(const CExpression& expr);

  // Statement visitors (default implementations recurse into child expressions and statements).
  virtual void VisitCompoundStatement(const CompoundStatement& stmt);
  virtual void VisitExpressionStatement(const ExpressionStatement& stmt);
  virtual void VisitVariableDeclarationStatement(const VariableDeclarationStatement& stmt);
  virtual void VisitIfStatement(const IfStatement& stmt);
  virtual void VisitWhileStatement(const WhileStatement& stmt);
  virtual void VisitDoWhileStatement(const DoWhileStatement& stmt);
  virtual void VisitForStatement(const ForStatement& stmt);
  virtual void VisitReturnStatement(const ReturnStatement& stmt);
  virtual void VisitBreakStatement(const BreakStatement& stmt);
  virtual void VisitContinueStatement(const ContinueStatement& stmt);
  virtual void VisitGotoStatement(const GotoStatement& stmt);
  virtual void VisitLabelStatement(const LabelStatement& stmt);
  virtual void VisitCaseStatement(const CaseStatement& stmt);
  virtual void VisitSwitchStatement(const SwitchStatement& stmt);
  virtual void VisitCommentStatement(const CommentStatement& stmt);

  // Expression visitors (default implementations recurse into child expressions).
  virtual void VisitIntegerLiteralExpression(const IntegerLiteralExpression& expr);
  virtual void VisitFloatLiteralExpression(const FloatLiteralExpression& expr);
  virtual void VisitStringLiteralExpression(const StringLiteralExpression& expr);
  virtual void VisitIdentifierExpression(const IdentifierExpression& expr);
  virtual void VisitUnaryExpression(const UnaryExpression& expr);
  virtual void VisitBinaryExpression(const BinaryExpression& expr);
  virtual void VisitAssignmentExpression(const AssignmentExpression& expr);
  virtual void VisitCallExpression(const CallExpression& expr);
  virtual void VisitCastExpression(const CastExpression& expr);
  virtual void VisitMemberAccessExpression(const MemberAccessExpression& expr);
  virtual void VisitArrayIndexExpression(const ArrayIndexExpression& expr);
  virtual void VisitTernaryExpression(const TernaryExpression& expr);

  bool ShouldStop() const { return should_stop_; }

 protected:
  void Stop() { should_stop_ = true; }
  void ResetStop() { should_stop_ = false; }

 private:
  bool should_stop_ = false;
};

// In-place AST mutator with default bottom-up recursive traversal over all expression and
// statement nodes. Subclasses can override specific `Mutate*` methods to replace or modify nodes
// in place and call `MarkChanged()` to track whether any transformation occurred.
class AstMutator {
 public:
  virtual ~AstMutator() = default;

  // Entrypoint for mutating an entire function declaration. Returns true if any change was
  // recorded.
  virtual bool MutateFunction(FunctionDeclaration* func);

  // Dispatches to the concrete statement/expression mutator for `*stmt` / `*expr`.
  // Subclasses may replace `*stmt` or `*expr` with a new `std::unique_ptr`.
  virtual void MutateStatement(std::unique_ptr<CStatement>* stmt);
  virtual void MutateExpression(std::unique_ptr<CExpression>* expr);
  virtual void MutateCompoundStatement(CompoundStatement* stmt);

  // Statement mutators (default implementations recurse into children).
  virtual void MutateExpressionStatement(std::unique_ptr<CStatement>* stmt,
                                         ExpressionStatement* expr_stmt);
  virtual void MutateVariableDeclarationStatement(std::unique_ptr<CStatement>* stmt,
                                                  VariableDeclarationStatement* decl_stmt);
  virtual void MutateIfStatement(std::unique_ptr<CStatement>* stmt, IfStatement* if_stmt);
  virtual void MutateWhileStatement(std::unique_ptr<CStatement>* stmt, WhileStatement* while_stmt);
  virtual void MutateDoWhileStatement(std::unique_ptr<CStatement>* stmt,
                                      DoWhileStatement* do_while_stmt);
  virtual void MutateForStatement(std::unique_ptr<CStatement>* stmt, ForStatement* for_stmt);
  virtual void MutateReturnStatement(std::unique_ptr<CStatement>* stmt,
                                     ReturnStatement* return_stmt);
  virtual void MutateBreakStatement(std::unique_ptr<CStatement>* stmt, BreakStatement* break_stmt);
  virtual void MutateContinueStatement(std::unique_ptr<CStatement>* stmt,
                                       ContinueStatement* continue_stmt);
  virtual void MutateGotoStatement(std::unique_ptr<CStatement>* stmt, GotoStatement* goto_stmt);
  virtual void MutateLabelStatement(std::unique_ptr<CStatement>* stmt, LabelStatement* label_stmt);
  virtual void MutateCaseStatement(std::unique_ptr<CStatement>* stmt, CaseStatement* case_stmt);
  virtual void MutateSwitchStatement(std::unique_ptr<CStatement>* stmt,
                                     SwitchStatement* switch_stmt);
  virtual void MutateCommentStatement(std::unique_ptr<CStatement>* stmt,
                                      CommentStatement* comment_stmt);

  // Expression mutators (default implementations recurse into children).
  virtual void MutateIntegerLiteralExpression(std::unique_ptr<CExpression>* expr,
                                              IntegerLiteralExpression* int_expr);
  virtual void MutateFloatLiteralExpression(std::unique_ptr<CExpression>* expr,
                                            FloatLiteralExpression* float_expr);
  virtual void MutateStringLiteralExpression(std::unique_ptr<CExpression>* expr,
                                             StringLiteralExpression* str_expr);
  virtual void MutateIdentifierExpression(std::unique_ptr<CExpression>* expr,
                                          IdentifierExpression* id_expr);
  virtual void MutateUnaryExpression(std::unique_ptr<CExpression>* expr,
                                     UnaryExpression* unary_expr);
  virtual void MutateBinaryExpression(std::unique_ptr<CExpression>* expr,
                                      BinaryExpression* binary_expr);
  virtual void MutateAssignmentExpression(std::unique_ptr<CExpression>* expr,
                                          AssignmentExpression* assign_expr);
  virtual void MutateCallExpression(std::unique_ptr<CExpression>* expr, CallExpression* call_expr);
  virtual void MutateCastExpression(std::unique_ptr<CExpression>* expr, CastExpression* cast_expr);
  virtual void MutateMemberAccessExpression(std::unique_ptr<CExpression>* expr,
                                            MemberAccessExpression* member_expr);
  virtual void MutateArrayIndexExpression(std::unique_ptr<CExpression>* expr,
                                          ArrayIndexExpression* array_expr);
  virtual void MutateTernaryExpression(std::unique_ptr<CExpression>* expr,
                                       TernaryExpression* ternary_expr);

  bool Changed() const { return changed_; }

 protected:
  void MarkChanged() { changed_ = true; }
  void ResetChanged() { changed_ = false; }

 private:
  bool changed_ = false;
};

}  // namespace rom_nom_nom

#endif  // CORE_C_AST_VISITOR_H_
