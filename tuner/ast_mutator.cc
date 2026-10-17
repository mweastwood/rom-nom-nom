#include "tuner/ast_mutator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "core/c_ast.h"

namespace rom_nom_nom {

namespace {

bool IsPureExpression(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kIntegerLiteral:
    case CExpressionKind::kFloatLiteral:
    case CExpressionKind::kStringLiteral:
    case CExpressionKind::kIdentifier:
      return true;
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.Op() == "++" || u.Op() == "--") return false;
      return IsPureExpression(u.Operand());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return IsPureExpression(b.Lhs()) && IsPureExpression(b.Rhs());
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return IsPureExpression(c.Operand());
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return IsPureExpression(m.Object());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      return IsPureExpression(a.Array()) && IsPureExpression(a.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return IsPureExpression(t.Condition()) && IsPureExpression(t.TrueExpression()) &&
             IsPureExpression(t.FalseExpression());
    }
    case CExpressionKind::kAssignmentExpression:
    case CExpressionKind::kCallExpression:
      return false;
  }
  return false;
}

std::optional<std::string> CommutedBinaryOp(std::string_view op) {
  if (op == "+" || op == "*" || op == "&" || op == "|" || op == "^" || op == "==" || op == "!=") {
    return std::string(op);
  }
  if (op == "<") return ">";
  if (op == ">") return "<";
  if (op == "<=") return ">=";
  if (op == ">=") return "<=";
  return std::nullopt;
}

bool IsCommutableBinary(const BinaryExpression& bin) {
  return CommutedBinaryOp(bin.Op()).has_value() && IsPureExpression(bin.Lhs()) &&
         IsPureExpression(bin.Rhs());
}

std::unique_ptr<CExpression> NegateCondition(const CExpression& cond) {
  if (cond.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& u = static_cast<const UnaryExpression&>(cond);
    if (u.IsPrefix() && u.Op() == "!") {
      return u.Operand().Clone();
    }
  }
  if (cond.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& b = static_cast<const BinaryExpression&>(cond);
    if (b.Op() == "==") return CExpression::Binary("!=", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == "!=") return CExpression::Binary("==", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == "<") return CExpression::Binary(">=", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == ">=") return CExpression::Binary("<", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == ">") return CExpression::Binary("<=", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == "<=") return CExpression::Binary(">", b.Lhs().Clone(), b.Rhs().Clone());
  }
  return CExpression::Unary("!", cond.Clone(), /*is_prefix=*/true);
}

bool IsToggableCompoundAssignment(const AssignmentExpression& assign) {
  if (assign.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  const std::string& lhs_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  if (assign.Op() == "+=" || assign.Op() == "-=" || assign.Op() == "&=" || assign.Op() == "|=" ||
      assign.Op() == "^=") {
    return true;
  }
  if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
    if ((bin.Op() == "+" || bin.Op() == "-" || bin.Op() == "&" || bin.Op() == "|" ||
         bin.Op() == "^") &&
        bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(bin.Lhs()).Name() == lhs_name) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<CExpression> ToggleCompoundAssignment(const AssignmentExpression& assign) {
  if (assign.Op() == "+=" || assign.Op() == "-=" || assign.Op() == "&=" || assign.Op() == "|=" ||
      assign.Op() == "^=") {
    std::string bin_op(assign.Op().substr(0, 1));
    return CExpression::Assignment(
        "=", assign.Lhs().Clone(),
        CExpression::Binary(bin_op, assign.Lhs().Clone(), assign.Rhs().Clone()));
  }
  const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
  std::string comp_op = bin.Op() + "=";
  return CExpression::Assignment(comp_op, assign.Lhs().Clone(), bin.Rhs().Clone());
}

bool IsToggableIntegerCast(const CastExpression& cast) {
  const auto& t = cast.TargetType();
  return t.pointer_depth == 0 && !t.is_const && (t.name == "s32" || t.name == "u32");
}

int CountIdentifierOccurrencesInExpr(const CExpression& expr, std::string_view name) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier:
      return static_cast<const IdentifierExpression&>(expr).Name() == name ? 1 : 0;
    case CExpressionKind::kUnaryExpression:
      return CountIdentifierOccurrencesInExpr(static_cast<const UnaryExpression&>(expr).Operand(),
                                              name);
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(b.Lhs(), name) +
             CountIdentifierOccurrencesInExpr(b.Rhs(), name);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(a.Lhs(), name) +
             CountIdentifierOccurrencesInExpr(a.Rhs(), name);
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      int total = CountIdentifierOccurrencesInExpr(c.Callee(), name);
      for (const auto& arg : c.Arguments()) {
        total += CountIdentifierOccurrencesInExpr(*arg, name);
      }
      return total;
    }
    case CExpressionKind::kCastExpression:
      return CountIdentifierOccurrencesInExpr(static_cast<const CastExpression&>(expr).Operand(),
                                              name);
    case CExpressionKind::kMemberAccessExpression:
      return CountIdentifierOccurrencesInExpr(
          static_cast<const MemberAccessExpression&>(expr).Object(), name);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(arr.Array(), name) +
             CountIdentifierOccurrencesInExpr(arr.Index(), name);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(t.Condition(), name) +
             CountIdentifierOccurrencesInExpr(t.TrueExpression(), name) +
             CountIdentifierOccurrencesInExpr(t.FalseExpression(), name);
    }
    default:
      return 0;
  }
}

int CountIdentifierOccurrencesInStmt(const CStatement& stmt, std::string_view name) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      int total = 0;
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        total += CountIdentifierOccurrencesInStmt(*s, name);
      }
      return total;
    }
    case CStatementKind::kExpressionStatement:
      return CountIdentifierOccurrencesInExpr(
          static_cast<const ExpressionStatement&>(stmt).Expression(), name);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      int total = (vd.Name() == name) ? 1 : 0;
      if (vd.Initializer() != nullptr) {
        total += CountIdentifierOccurrencesInExpr(*vd.Initializer(), name);
      }
      return total;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      int total = CountIdentifierOccurrencesInExpr(ifs.Condition(), name) +
                  CountIdentifierOccurrencesInStmt(ifs.ThenBranch(), name);
      if (ifs.ElseBranch() != nullptr) {
        total += CountIdentifierOccurrencesInStmt(*ifs.ElseBranch(), name);
      }
      return total;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CountIdentifierOccurrencesInExpr(ws.Condition(), name) +
             CountIdentifierOccurrencesInStmt(ws.Body(), name);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CountIdentifierOccurrencesInStmt(dws.Body(), name) +
             CountIdentifierOccurrencesInExpr(dws.Condition(), name);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      int total = CountIdentifierOccurrencesInStmt(fs.Body(), name);
      if (fs.Init() != nullptr) {
        total += CountIdentifierOccurrencesInStmt(*fs.Init(), name);
      }
      if (fs.Condition() != nullptr) {
        total += CountIdentifierOccurrencesInExpr(*fs.Condition(), name);
      }
      if (fs.Step() != nullptr) {
        total += CountIdentifierOccurrencesInExpr(*fs.Step(), name);
      }
      return total;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr ? CountIdentifierOccurrencesInExpr(*rs.ReturnValue(), name)
                                         : 0;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      int total = CountIdentifierOccurrencesInExpr(ss.Condition(), name);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) total += CountIdentifierOccurrencesInStmt(*c.body, name);
      }
      return total;
    }
    default:
      return 0;
  }
}

std::unique_ptr<CExpression> ReplaceIdentifierInExpr(const CExpression& expr,
                                                     std::string_view target_name,
                                                     const CExpression& replacement) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      if (id.Name() == target_name) {
        return replacement.Clone();
      }
      return expr.Clone();
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(
          u.Op(), ReplaceIdentifierInExpr(u.Operand(), target_name, replacement), u.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(b.Op(), ReplaceIdentifierInExpr(b.Lhs(), target_name, replacement),
                                 ReplaceIdentifierInExpr(b.Rhs(), target_name, replacement));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      // Do not replace the LHS target of a direct assignment if it's the identifier itself.
      std::unique_ptr<CExpression> new_lhs =
          (a.Lhs().Kind() == CExpressionKind::kIdentifier &&
           static_cast<const IdentifierExpression&>(a.Lhs()).Name() == target_name && a.Op() == "=")
              ? a.Lhs().Clone()
              : ReplaceIdentifierInExpr(a.Lhs(), target_name, replacement);
      return CExpression::Assignment(a.Op(), std::move(new_lhs),
                                     ReplaceIdentifierInExpr(a.Rhs(), target_name, replacement));
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        args.push_back(ReplaceIdentifierInExpr(*arg, target_name, replacement));
      }
      return CExpression::Call(ReplaceIdentifierInExpr(c.Callee(), target_name, replacement),
                               std::move(args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(c.TargetType(),
                               ReplaceIdentifierInExpr(c.Operand(), target_name, replacement));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(
          ReplaceIdentifierInExpr(m.Object(), target_name, replacement), m.MemberName(),
          m.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(
          ReplaceIdentifierInExpr(arr.Array(), target_name, replacement),
          ReplaceIdentifierInExpr(arr.Index(), target_name, replacement));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          ReplaceIdentifierInExpr(t.Condition(), target_name, replacement),
          ReplaceIdentifierInExpr(t.TrueExpression(), target_name, replacement),
          ReplaceIdentifierInExpr(t.FalseExpression(), target_name, replacement));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> ReplaceIdentifierInSingleStmt(const CStatement& stmt,
                                                          std::string_view target_name,
                                                          const CExpression& replacement) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      return CStatement::Expression(
          ReplaceIdentifierInExpr(es.Expression(), target_name, replacement));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return rs.Clone();
      return CStatement::Return(
          ReplaceIdentifierInExpr(*rs.ReturnValue(), target_name, replacement));
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(ReplaceIdentifierInExpr(ifs.Condition(), target_name, replacement),
                            ifs.ThenBranch().Clone(),
                            ifs.ElseBranch() ? ifs.ElseBranch()->Clone() : nullptr);
    }
    default:
      return stmt.Clone();
  }
}

bool CanInlineSingleUseTempAt(const std::vector<std::unique_ptr<CStatement>>& stmts, size_t idx,
                              std::string* out_var_name = nullptr,
                              const CExpression** out_rhs = nullptr) {
  if (idx + 1 >= stmts.size()) return false;
  if (stmts[idx]->Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& es = static_cast<const ExpressionStatement&>(*stmts[idx]);
  if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assign = static_cast<const AssignmentExpression&>(es.Expression());
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  if (!IsPureExpression(assign.Rhs())) return false;

  const std::string& var_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  // Ensure `rhs` does not reference `var_name` itself.
  if (CountIdentifierOccurrencesInExpr(assign.Rhs(), var_name) != 0) return false;

  // Next statement must be an ExpressionStatement, ReturnStatement, or IfStatement condition using
  // `var_name` exactly once, and `var_name` must not be referenced in any subsequent statements
  // in this block.
  const CStatement& next = *stmts[idx + 1];
  int next_uses = 0;
  if (next.Kind() == CStatementKind::kExpressionStatement) {
    const auto& next_es = static_cast<const ExpressionStatement&>(next);
    if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& next_assign = static_cast<const AssignmentExpression&>(next_es.Expression());
      if (next_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(next_assign.Lhs()).Name() == var_name &&
          next_assign.Op() == "=") {
        next_uses = CountIdentifierOccurrencesInExpr(next_assign.Rhs(), var_name);
      } else {
        next_uses = CountIdentifierOccurrencesInExpr(next_es.Expression(), var_name);
      }
    } else {
      next_uses = CountIdentifierOccurrencesInExpr(next_es.Expression(), var_name);
    }
  } else if (next.Kind() == CStatementKind::kReturnStatement) {
    const auto& rs = static_cast<const ReturnStatement&>(next);
    if (rs.ReturnValue() != nullptr) {
      next_uses = CountIdentifierOccurrencesInExpr(*rs.ReturnValue(), var_name);
    }
  } else if (next.Kind() == CStatementKind::kIfStatement) {
    const auto& ifs = static_cast<const IfStatement&>(next);
    if (CountIdentifierOccurrencesInStmt(ifs.ThenBranch(), var_name) == 0 &&
        (ifs.ElseBranch() == nullptr ||
         CountIdentifierOccurrencesInStmt(*ifs.ElseBranch(), var_name) == 0)) {
      next_uses = CountIdentifierOccurrencesInExpr(ifs.Condition(), var_name);
    }
  }

  if (next_uses != 1) return false;

  for (size_t k = idx + 2; k < stmts.size(); ++k) {
    if (CountIdentifierOccurrencesInStmt(*stmts[k], var_name) > 0) {
      return false;
    }
  }

  if (out_var_name != nullptr) *out_var_name = var_name;
  if (out_rhs != nullptr) *out_rhs = &assign.Rhs();
  return true;
}

struct MutationContext {
  const AstMutationStep* step_to_apply = nullptr;
  std::vector<AstMutationStep>* discovered = nullptr;
  size_t max_mutations = 64;

  uint32_t commute_index = 0;
  uint32_t invert_if_index = 0;
  uint32_t reorder_decl_index = 0;
  uint32_t compound_assign_index = 0;
  uint32_t while_for_index = 0;
  uint32_t cast_index = 0;
  uint32_t inline_temp_index = 0;

  void Record(AstMutationKind kind, uint32_t site_index, uint32_t param = 0) {
    if (discovered == nullptr || discovered->size() >= max_mutations) return;
    AstMutationStep s;
    s.set_kind(kind);
    s.set_site_index(site_index);
    s.set_param(param);
    discovered->push_back(std::move(s));
  }

  bool ShouldApply(AstMutationKind kind, uint32_t site_index, uint32_t* out_param = nullptr) const {
    if (step_to_apply == nullptr) return false;
    if (step_to_apply->kind() == kind && step_to_apply->site_index() == site_index) {
      if (out_param != nullptr) *out_param = step_to_apply->param();
      return true;
    }
    return false;
  }
};

std::unique_ptr<CExpression> TraverseExpression(const CExpression& expr, MutationContext* ctx) {
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(u.Op(), TraverseExpression(u.Operand(), ctx), u.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      auto new_lhs = TraverseExpression(b.Lhs(), ctx);
      auto new_rhs = TraverseExpression(b.Rhs(), ctx);
      if (IsCommutableBinary(b)) {
        uint32_t site = ctx->commute_index++;
        ctx->Record(AST_MUTATION_COMMUTE_BINARY_OP, site);
        if (ctx->ShouldApply(AST_MUTATION_COMMUTE_BINARY_OP, site)) {
          return CExpression::Binary(*CommutedBinaryOp(b.Op()), std::move(new_rhs),
                                     std::move(new_lhs));
        }
      }
      return CExpression::Binary(b.Op(), std::move(new_lhs), std::move(new_rhs));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      auto new_lhs = TraverseExpression(a.Lhs(), ctx);
      auto new_rhs = TraverseExpression(a.Rhs(), ctx);
      AssignmentExpression current(a.Op(), std::move(new_lhs), std::move(new_rhs));
      if (IsToggableCompoundAssignment(current)) {
        uint32_t site = ctx->compound_assign_index++;
        ctx->Record(AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT, site);
        if (ctx->ShouldApply(AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT, site)) {
          return ToggleCompoundAssignment(current);
        }
      }
      return current.Clone();
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      auto new_callee = TraverseExpression(c.Callee(), ctx);
      std::vector<std::unique_ptr<CExpression>> new_args;
      new_args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        new_args.push_back(TraverseExpression(*arg, ctx));
      }
      return CExpression::Call(std::move(new_callee), std::move(new_args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      auto new_op = TraverseExpression(c.Operand(), ctx);
      if (IsToggableIntegerCast(c)) {
        uint32_t site = ctx->cast_index++;
        ctx->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 0);
        ctx->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 1);
        uint32_t param = 0;
        if (ctx->ShouldApply(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, &param)) {
          if (param == 0) {
            return new_op;
          }
          CType flipped = (c.TargetType().name == "u32") ? CType::S32() : CType::U32();
          return CExpression::Cast(flipped, std::move(new_op));
        }
      }
      return CExpression::Cast(c.TargetType(), std::move(new_op));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(TraverseExpression(m.Object(), ctx), m.MemberName(),
                                       m.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(TraverseExpression(a.Array(), ctx),
                                     TraverseExpression(a.Index(), ctx));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(TraverseExpression(t.Condition(), ctx),
                                  TraverseExpression(t.TrueExpression(), ctx),
                                  TraverseExpression(t.FalseExpression(), ctx));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> TraverseStatement(const CStatement& stmt, MutationContext* ctx);

std::unique_ptr<CompoundStatement> TraverseCompound(const CompoundStatement& comp,
                                                    MutationContext* ctx) {
  const auto& stmts = comp.Statements();
  std::vector<std::unique_ptr<CStatement>> new_stmts;
  new_stmts.reserve(stmts.size());

  for (size_t i = 0; i < stmts.size(); ++i) {
    // 1. Check adjacent VariableDeclarationStatements for reordering
    if (i + 1 < stmts.size() && stmts[i]->Kind() == CStatementKind::kVariableDeclarationStatement &&
        stmts[i + 1]->Kind() == CStatementKind::kVariableDeclarationStatement) {
      const auto& d1 = static_cast<const VariableDeclarationStatement&>(*stmts[i]);
      const auto& d2 = static_cast<const VariableDeclarationStatement&>(*stmts[i + 1]);
      bool independent = (d2.Initializer() == nullptr ||
                          CountIdentifierOccurrencesInExpr(*d2.Initializer(), d1.Name()) == 0) &&
                         (d1.Initializer() == nullptr ||
                          CountIdentifierOccurrencesInExpr(*d1.Initializer(), d2.Name()) == 0);
      if (independent) {
        uint32_t site = ctx->reorder_decl_index++;
        ctx->Record(AST_MUTATION_REORDER_LOCAL_DECLS, site);
        if (ctx->ShouldApply(AST_MUTATION_REORDER_LOCAL_DECLS, site)) {
          new_stmts.push_back(TraverseStatement(*stmts[i + 1], ctx));
          new_stmts.push_back(TraverseStatement(*stmts[i], ctx));
          ++i;
          continue;
        }
      }
    }

    // 2. Check single-use temporary inlining across stmts[i] and stmts[i + 1]
    std::string inline_var;
    const CExpression* inline_rhs = nullptr;
    if (CanInlineSingleUseTempAt(stmts, i, &inline_var, &inline_rhs)) {
      uint32_t site = ctx->inline_temp_index++;
      ctx->Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site);
      if (ctx->ShouldApply(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site)) {
        auto replaced = ReplaceIdentifierInSingleStmt(*stmts[i + 1], inline_var, *inline_rhs);
        new_stmts.push_back(TraverseStatement(*replaced, ctx));
        ++i;
        continue;
      }
    }

    new_stmts.push_back(TraverseStatement(*stmts[i], ctx));
  }

  return std::make_unique<CompoundStatement>(std::move(new_stmts));
}

std::unique_ptr<CStatement> TraverseStatement(const CStatement& stmt, MutationContext* ctx) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      return TraverseCompound(static_cast<const CompoundStatement&>(stmt), ctx);
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      return CStatement::Expression(TraverseExpression(es.Expression(), ctx));
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      std::unique_ptr<CExpression> init =
          vd.Initializer() != nullptr ? TraverseExpression(*vd.Initializer(), ctx) : nullptr;
      return CStatement::VariableDeclaration(vd.Type(), vd.Name(), std::move(init), vd.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      auto new_cond = TraverseExpression(ifs.Condition(), ctx);
      auto new_then = TraverseStatement(ifs.ThenBranch(), ctx);
      std::unique_ptr<CStatement> new_else =
          ifs.ElseBranch() != nullptr ? TraverseStatement(*ifs.ElseBranch(), ctx) : nullptr;
      if (new_else != nullptr) {
        uint32_t site = ctx->invert_if_index++;
        ctx->Record(AST_MUTATION_INVERT_IF_ELSE, site);
        if (ctx->ShouldApply(AST_MUTATION_INVERT_IF_ELSE, site)) {
          return CStatement::If(NegateCondition(*new_cond), std::move(new_else),
                                std::move(new_then));
        }
      }
      return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      auto new_cond = TraverseExpression(ws.Condition(), ctx);
      auto new_body = TraverseStatement(ws.Body(), ctx);
      uint32_t site = ctx->while_for_index++;
      ctx->Record(AST_MUTATION_WHILE_TO_FOR_LOOP, site);
      if (ctx->ShouldApply(AST_MUTATION_WHILE_TO_FOR_LOOP, site)) {
        return CStatement::For(nullptr, std::move(new_cond), nullptr, std::move(new_body));
      }
      return CStatement::While(std::move(new_cond), std::move(new_body));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      auto new_body = TraverseStatement(dws.Body(), ctx);
      auto new_cond = TraverseExpression(dws.Condition(), ctx);
      return CStatement::DoWhile(std::move(new_body), std::move(new_cond));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      std::unique_ptr<CStatement> new_init =
          fs.Init() != nullptr ? TraverseStatement(*fs.Init(), ctx) : nullptr;
      std::unique_ptr<CExpression> new_cond =
          fs.Condition() != nullptr ? TraverseExpression(*fs.Condition(), ctx) : nullptr;
      std::unique_ptr<CExpression> new_step =
          fs.Step() != nullptr ? TraverseExpression(*fs.Step(), ctx) : nullptr;
      auto new_body = TraverseStatement(fs.Body(), ctx);
      if (new_init == nullptr && new_step == nullptr && new_cond != nullptr) {
        uint32_t site = ctx->while_for_index++;
        ctx->Record(AST_MUTATION_WHILE_TO_FOR_LOOP, site);
        if (ctx->ShouldApply(AST_MUTATION_WHILE_TO_FOR_LOOP, site)) {
          return CStatement::While(std::move(new_cond), std::move(new_body));
        }
      }
      return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                             std::move(new_body));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(TraverseExpression(*rs.ReturnValue(), ctx));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      auto new_cond = TraverseExpression(ss.Condition(), ctx);
      std::vector<SwitchCase> new_cases;
      new_cases.reserve(ss.Cases().size());
      for (const auto& c : ss.Cases()) {
        SwitchCase sc;
        sc.case_values = c.case_values;
        sc.is_default = c.is_default;
        if (c.body != nullptr) {
          sc.body = TraverseCompound(*c.body, ctx);
        }
        new_cases.push_back(std::move(sc));
      }
      return CStatement::Switch(std::move(new_cond), std::move(new_cases));
    }
    default:
      return stmt.Clone();
  }
}

}  // namespace

std::vector<AstMutationStep> AstMutator::DiscoverMutations(const FunctionDeclaration& func,
                                                           size_t max_mutations) {
  std::vector<AstMutationStep> mutations;
  MutationContext ctx;
  ctx.discovered = &mutations;
  ctx.max_mutations = max_mutations;
  (void)TraverseCompound(func.Body(), &ctx);
  return mutations;
}

FunctionDeclaration AstMutator::ApplyMutation(const FunctionDeclaration& func,
                                              const AstMutationStep& step) {
  MutationContext ctx;
  ctx.step_to_apply = &step;
  auto mutated_body = TraverseCompound(func.Body(), &ctx);
  return FunctionDeclaration(func.ReturnType(), func.Name(), func.Parameters(),
                             std::move(mutated_body), func.IsStatic());
}

FunctionDeclaration AstMutator::ApplyMutations(const FunctionDeclaration& func,
                                               absl::Span<const AstMutationStep> steps) {
  FunctionDeclaration current = func.Clone();
  for (const auto& step : steps) {
    current = ApplyMutation(current, step);
  }
  return current;
}

std::string AstMutator::DescribeStep(const AstMutationStep& step) {
  switch (step.kind()) {
    case AST_MUTATION_COMMUTE_BINARY_OP:
      return absl::StrFormat("CommuteBinaryOp(site=%u)", step.site_index());
    case AST_MUTATION_INVERT_IF_ELSE:
      return absl::StrFormat("InvertIfElse(site=%u)", step.site_index());
    case AST_MUTATION_REORDER_LOCAL_DECLS:
      return absl::StrFormat("ReorderLocalDecls(site=%u)", step.site_index());
    case AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT:
      return absl::StrFormat("ToggleCompoundAssign(site=%u)", step.site_index());
    case AST_MUTATION_WHILE_TO_FOR_LOOP:
      return absl::StrFormat("WhileToForLoop(site=%u)", step.site_index());
    case AST_MUTATION_TOGGLE_UNSIGNED_CAST:
      return absl::StrFormat("ToggleUnsignedCast(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_INLINE_SINGLE_USE_TEMP:
      return absl::StrFormat("InlineSingleUseTemp(site=%u)", step.site_index());
    default:
      return "UnspecifiedMutation";
  }
}

}  // namespace rom_nom_nom
