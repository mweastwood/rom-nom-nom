#include "lifter/ast_control_flow_cleanup_pass.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "core/c_ast_visitor.h"

namespace rom_nom_nom {
namespace {

bool IsScratchGprOrArgVar(std::string_view name) {
  return name == "v0" || name == "v1" || name == "at" || absl::StartsWith(name, "arg") ||
         absl::StartsWith(name, "temp_t");
}

bool ExtractSimpleVarAssign(const CStatement& stmt, std::string* out_lhs,
                            const CExpression** out_rhs) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) {
    return false;
  }
  const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return false;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
    return false;
  }
  if (out_lhs != nullptr) {
    *out_lhs = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  }
  if (out_rhs != nullptr) {
    *out_rhs = &assign.Rhs();
  }
  return true;
}

bool ExtractSymbolAddressOperand(const CExpression& expr, const CExpression** out_addr_of) {
  const CExpression* cur = &expr;
  while (cur->Kind() == CExpressionKind::kCastExpression) {
    cur = &static_cast<const CastExpression&>(*cur).Operand();
  }
  if (cur->Kind() == CExpressionKind::kUnaryExpression) {
    const auto& un = static_cast<const UnaryExpression&>(*cur);
    if (un.IsPrefix() && un.Op() == "&" && un.Operand().Kind() == CExpressionKind::kIdentifier) {
      std::string_view sym_name = static_cast<const IdentifierExpression&>(un.Operand()).Name();
      if (absl::StartsWith(sym_name, "D_") || absl::StartsWith(sym_name, "g_") ||
          absl::StartsWith(sym_name, "s_")) {
        if (out_addr_of != nullptr) {
          *out_addr_of = &un;
        }
        return true;
      }
    }
  }
  return false;
}

bool IsRelationalOrEqualityBinary(const CExpression& expr) {
  if (expr.Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  std::string_view op = static_cast<const BinaryExpression&>(expr).Op();
  return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
}

bool StatementDefinitelyKillsVarBeforeRead(const CStatement& stmt, std::string_view var,
                                           bool* out_reads_var) {
  if (out_reads_var != nullptr && *out_reads_var) {
    return false;
  }
  if (stmt.Kind() == CStatementKind::kExpressionStatement) {
    const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
    if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == var &&
          !ExpressionUsesVariable(assign.Rhs(), var)) {
        return true;
      }
    }
    if (ExpressionUsesVariable(expr, var) && out_reads_var != nullptr) {
      *out_reads_var = true;
    }
    return false;
  }
  if (stmt.Kind() == CStatementKind::kCompoundStatement) {
    for (const auto& sub : static_cast<const CompoundStatement&>(stmt).Statements()) {
      if (sub == nullptr) continue;
      if (StatementDefinitelyKillsVarBeforeRead(*sub, var, out_reads_var)) {
        return true;
      }
      if (out_reads_var != nullptr && *out_reads_var) {
        return false;
      }
    }
    return false;
  }
  if (stmt.Kind() == CStatementKind::kDoWhileStatement) {
    const auto& dw = static_cast<const DoWhileStatement&>(stmt);
    if (StatementDefinitelyKillsVarBeforeRead(dw.Body(), var, out_reads_var)) {
      return true;
    }
    if (ExpressionUsesVariable(dw.Condition(), var) && out_reads_var != nullptr) {
      *out_reads_var = true;
    }
    return false;
  }
  if (StatementReferencesVariable(stmt, var) && out_reads_var != nullptr) {
    *out_reads_var = true;
  }
  return false;
}

class SubstituteExpressionInStatementMutator : public AstMutator {
 public:
  SubstituteExpressionInStatementMutator(std::string_view target, const CExpression& replacement)
      : target_(target), replacement_(replacement) {}

  void MutateVariableDeclarationStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                          VariableDeclarationStatement* /*decl_stmt*/) override {}

  void MutateExpression(std::unique_ptr<CExpression>* expr) override {
    if (expr == nullptr || *expr == nullptr) return;
    if (ExpressionUsesVariable(**expr, target_)) {
      *expr = SubstituteIdentifier(**expr, target_, replacement_);
      MarkChanged();
    }
  }

 private:
  std::string_view target_;
  const CExpression& replacement_;
};

bool FoldDoWhileTrailingLatchComparison(
    DoWhileStatement* do_while, const std::vector<std::unique_ptr<CStatement>>& enclosing_stmts,
    size_t loop_stmt_idx, const CompoundStatement* root_body = nullptr) {
  if (do_while == nullptr) {
    return false;
  }
  const CExpression& cond = do_while->Condition();
  if (cond.Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  const auto& bin_cond = static_cast<const BinaryExpression&>(cond);
  const bool is_unfolded_var_cond =
      (bin_cond.Op() == "!=" && bin_cond.Lhs().Kind() == CExpressionKind::kIdentifier &&
       bin_cond.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
       static_cast<const IntegerLiteralExpression&>(bin_cond.Rhs()).Value() == 0);
  if (!is_unfolded_var_cond && !IsRelationalOrEqualityBinary(cond)) {
    return false;
  }
  std::string cond_var;
  if (is_unfolded_var_cond) {
    cond_var = std::string(static_cast<const IdentifierExpression&>(bin_cond.Lhs()).Name());
    if (!(IsScratchGprOrArgVar(cond_var) || cond_var == "at" || cond_var == "temp_at")) {
      return false;
    }
  }
  if (do_while->MutableBody() == nullptr ||
      do_while->MutableBody()->Kind() != CStatementKind::kCompoundStatement) {
    return false;
  }
  auto* body_comp = static_cast<CompoundStatement*>(do_while->MutableBody());
  auto& body_stmts = body_comp->MutableStatements();
  if (body_stmts.size() < 2) {
    return false;
  }

  int assign_idx = -1;
  const CExpression* latch_rhs = nullptr;
  for (int offset = 0; offset <= 4; ++offset) {
    int cand_idx = static_cast<int>(body_stmts.size()) - 1 - offset;
    if (cand_idx < 0 || body_stmts[cand_idx] == nullptr) continue;
    std::string lhs;
    const CExpression* rhs = nullptr;
    if (!ExtractSimpleVarAssign(*body_stmts[cand_idx], &lhs, &rhs) || rhs == nullptr ||
        !IsRelationalOrEqualityBinary(*rhs) || !IsPureExpression(*rhs)) {
      continue;
    }
    if (is_unfolded_var_cond) {
      if (lhs != cond_var || ExpressionUsesVariable(*rhs, cond_var)) {
        continue;
      }
    } else {
      if (!(IsScratchGprOrArgVar(lhs) || lhs == "at" || lhs == "temp_at") ||
          ExpressionUsesVariable(*rhs, lhs) || cond.ToString() != rhs->ToString()) {
        continue;
      }
      cond_var = lhs;
    }
    bool trailing_ok = true;
    for (int t = cand_idx + 1; t < static_cast<int>(body_stmts.size()); ++t) {
      if (body_stmts[t] == nullptr) {
        trailing_ok = false;
        break;
      }
      std::string tail_lhs;
      const CExpression* tail_rhs = nullptr;
      if (!ExtractSimpleVarAssign(*body_stmts[t], &tail_lhs, &tail_rhs) || tail_lhs == cond_var ||
          !IsPureExpression(*tail_rhs) || StatementReferencesVariable(*body_stmts[t], cond_var) ||
          ExpressionUsesVariable(*rhs, tail_lhs)) {
        trailing_ok = false;
        break;
      }
    }
    if (!trailing_ok) continue;
    assign_idx = cand_idx;
    latch_rhs = rhs;
    break;
  }
  if (assign_idx < 0 || latch_rhs == nullptr || cond_var.empty()) {
    return false;
  }

  for (size_t after = loop_stmt_idx + 1; after < enclosing_stmts.size(); ++after) {
    if (enclosing_stmts[after] != nullptr &&
        StatementReferencesVariable(*enclosing_stmts[after], cond_var)) {
      return false;
    }
  }

  // Ensure `cond_var` is killed before any read on the next iteration of the loop body and that
  // the loop does not exit early via `break`/`return`/`goto` after `assign_idx`.
  bool reads_before_kill = false;
  bool killed_on_entry = false;
  for (int i = 0; i < assign_idx; ++i) {
    if (body_stmts[i] == nullptr) continue;
    if (StatementDefinitelyKillsVarBeforeRead(*body_stmts[i], cond_var, &reads_before_kill)) {
      killed_on_entry = true;
      break;
    }
    if (reads_before_kill) {
      return false;
    }
  }
  const bool only_used_in_latch =
      (root_body != nullptr && CountVariableReadUsesInStatement(*root_body, cond_var) == 1 &&
       !StatementTakesAddressOf(*root_body, cond_var));
  if ((is_unfolded_var_cond && !killed_on_entry && !only_used_in_latch) || reads_before_kill) {
    return false;
  }

  auto new_cond = latch_rhs->Clone();
  body_stmts.erase(body_stmts.begin() + assign_idx);
  do_while->MutableCondition() = std::move(new_cond);
  return true;
}

bool FoldWhileTrueLeadingBreakCondition(
    WhileStatement* ws, const std::vector<std::unique_ptr<CStatement>>& enclosing_stmts,
    size_t loop_stmt_idx) {
  if (ws == nullptr || ws->Condition().Kind() != CExpressionKind::kIntegerLiteral ||
      static_cast<const IntegerLiteralExpression&>(ws->Condition()).Value() != 1 ||
      ws->MutableBody() == nullptr ||
      ws->MutableBody()->Kind() != CStatementKind::kCompoundStatement) {
    return false;
  }
  auto* body_comp = static_cast<CompoundStatement*>(ws->MutableBody());
  auto& body_stmts = body_comp->MutableStatements();
  if (body_stmts.size() < 2) {
    return false;
  }

  for (size_t if_idx = 1; if_idx <= 2 && if_idx < body_stmts.size(); ++if_idx) {
    if (body_stmts[if_idx] == nullptr ||
        body_stmts[if_idx]->Kind() != CStatementKind::kIfStatement) {
      continue;
    }
    const auto& ifs = static_cast<const IfStatement&>(*body_stmts[if_idx]);
    if (ifs.ElseBranch() != nullptr ||
        ifs.ThenBranch().Kind() != CStatementKind::kCompoundStatement) {
      continue;
    }
    const auto& then_comp = static_cast<const CompoundStatement&>(ifs.ThenBranch());
    if (then_comp.Statements().size() != 1 || then_comp.Statements()[0] == nullptr ||
        then_comp.Statements()[0]->Kind() != CStatementKind::kBreakStatement) {
      continue;
    }

    std::vector<std::pair<std::string, const CExpression*>> assigns;
    bool prefix_ok = true;
    for (size_t k = 0; k < if_idx; ++k) {
      std::string lhs;
      const CExpression* rhs = nullptr;
      if (body_stmts[k] == nullptr || !ExtractSimpleVarAssign(*body_stmts[k], &lhs, &rhs) ||
          rhs == nullptr || !IsPureExpression(*rhs) ||
          !(IsScratchGprOrArgVar(lhs) || lhs == "at" || lhs == "temp_at") ||
          ExpressionUsesVariable(*rhs, lhs)) {
        prefix_ok = false;
        break;
      }
      assigns.emplace_back(std::move(lhs), rhs);
    }
    if (!prefix_ok) continue;

    bool all_dead_after = true;
    for (const auto& [lhs, _] : assigns) {
      for (size_t after = loop_stmt_idx + 1; after < enclosing_stmts.size(); ++after) {
        if (enclosing_stmts[after] != nullptr &&
            StatementReferencesVariable(*enclosing_stmts[after], lhs)) {
          all_dead_after = false;
          break;
        }
      }
      if (!all_dead_after) break;
      bool reads_before_kill = false;
      bool killed = false;
      for (size_t rem = if_idx + 1; rem < body_stmts.size(); ++rem) {
        if (body_stmts[rem] == nullptr) continue;
        if (StatementDefinitelyKillsVarBeforeRead(*body_stmts[rem], lhs, &reads_before_kill)) {
          killed = true;
          break;
        }
        if (reads_before_kill) {
          all_dead_after = false;
          break;
        }
      }
      if (!all_dead_after || (reads_before_kill && !killed)) {
        all_dead_after = false;
        break;
      }
    }
    if (!all_dead_after) continue;

    std::unique_ptr<CExpression> folded_break_cond = ifs.Condition().Clone();
    for (int k = static_cast<int>(assigns.size()) - 1; k >= 0; --k) {
      folded_break_cond =
          SubstituteIdentifier(*folded_break_cond, assigns[k].first, *assigns[k].second);
    }
    if (!IsPureExpression(*folded_break_cond)) {
      continue;
    }
    bool unused_refs = false;
    for (const auto& [lhs, _] : assigns) {
      if (ExpressionUsesVariable(*folded_break_cond, lhs)) {
        unused_refs = true;
        break;
      }
    }
    if (unused_refs) continue;

    ws->MutableCondition() = NegateCondition(*folded_break_cond);
    body_stmts.erase(body_stmts.begin(), body_stmts.begin() + static_cast<ptrdiff_t>(if_idx + 1));
    return true;
  }
  return false;
}

class GotoTargetCollectorVisitor : public AstVisitor {
 public:
  const absl::flat_hash_set<std::string>& Targets() const { return targets_; }

  void VisitGotoStatement(const GotoStatement& stmt) override {
    targets_.insert(std::string(stmt.Label()));
  }

 private:
  absl::flat_hash_set<std::string> targets_;
};

const CExpression& UnwrapScalarCast(const CExpression& expr) {
  if (expr.Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(expr);
    if (cast.TargetType().pointer_depth == 0 &&
        (cast.TargetType().name == "s32" || cast.TargetType().name == "u32" ||
         cast.TargetType().name == "s64" || cast.TargetType().name == "u64")) {
      return cast.Operand();
    }
  }
  return expr;
}

bool ExpressionsStructurallyEqual(const CExpression& a, const CExpression& b) {
  if (a.Kind() != b.Kind()) {
    return false;
  }
  switch (a.Kind()) {
    case CExpressionKind::kIntegerLiteral: {
      const auto& ia = static_cast<const IntegerLiteralExpression&>(a);
      const auto& ib = static_cast<const IntegerLiteralExpression&>(b);
      return ia.Value() == ib.Value();
    }
    case CExpressionKind::kFloatLiteral: {
      const auto& fa = static_cast<const FloatLiteralExpression&>(a);
      const auto& fb = static_cast<const FloatLiteralExpression&>(b);
      return fa.Value() == fb.Value() && fa.IsFloat() == fb.IsFloat();
    }
    case CExpressionKind::kStringLiteral: {
      const auto& sa = static_cast<const StringLiteralExpression&>(a);
      const auto& sb = static_cast<const StringLiteralExpression&>(b);
      return sa.Value() == sb.Value();
    }
    case CExpressionKind::kIdentifier: {
      const auto& ida = static_cast<const IdentifierExpression&>(a);
      const auto& idb = static_cast<const IdentifierExpression&>(b);
      return ida.Name() == idb.Name();
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& ua = static_cast<const UnaryExpression&>(a);
      const auto& ub = static_cast<const UnaryExpression&>(b);
      return ua.Op() == ub.Op() && ua.IsPrefix() == ub.IsPrefix() &&
             ExpressionsStructurallyEqual(ua.Operand(), ub.Operand());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& ba = static_cast<const BinaryExpression&>(a);
      const auto& bb = static_cast<const BinaryExpression&>(b);
      return ba.Op() == bb.Op() && ExpressionsStructurallyEqual(ba.Lhs(), bb.Lhs()) &&
             ExpressionsStructurallyEqual(ba.Rhs(), bb.Rhs());
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& aa = static_cast<const AssignmentExpression&>(a);
      const auto& ab = static_cast<const AssignmentExpression&>(b);
      return aa.Op() == ab.Op() && ExpressionsStructurallyEqual(aa.Lhs(), ab.Lhs()) &&
             ExpressionsStructurallyEqual(aa.Rhs(), ab.Rhs());
    }
    case CExpressionKind::kCallExpression: {
      const auto& ca = static_cast<const CallExpression&>(a);
      const auto& cb = static_cast<const CallExpression&>(b);
      if (!ExpressionsStructurallyEqual(ca.Callee(), cb.Callee()) ||
          ca.Arguments().size() != cb.Arguments().size()) {
        return false;
      }
      for (size_t i = 0; i < ca.Arguments().size(); ++i) {
        if (!ExpressionsStructurallyEqual(*ca.Arguments()[i], *cb.Arguments()[i])) {
          return false;
        }
      }
      return true;
    }
    case CExpressionKind::kCastExpression: {
      const auto& ca = static_cast<const CastExpression&>(a);
      const auto& cb = static_cast<const CastExpression&>(b);
      return ca.TargetType().name == cb.TargetType().name &&
             ca.TargetType().is_const == cb.TargetType().is_const &&
             ca.TargetType().pointer_depth == cb.TargetType().pointer_depth &&
             ExpressionsStructurallyEqual(ca.Operand(), cb.Operand());
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& ma = static_cast<const MemberAccessExpression&>(a);
      const auto& mb = static_cast<const MemberAccessExpression&>(b);
      return ma.MemberName() == mb.MemberName() && ma.IsArrow() == mb.IsArrow() &&
             ExpressionsStructurallyEqual(ma.Object(), mb.Object());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& sa = static_cast<const ArrayIndexExpression&>(a);
      const auto& sb = static_cast<const ArrayIndexExpression&>(b);
      return ExpressionsStructurallyEqual(sa.Array(), sb.Array()) &&
             ExpressionsStructurallyEqual(sa.Index(), sb.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ca = static_cast<const TernaryExpression&>(a);
      const auto& cb = static_cast<const TernaryExpression&>(b);
      return ExpressionsStructurallyEqual(ca.Condition(), cb.Condition()) &&
             ExpressionsStructurallyEqual(ca.TrueExpression(), cb.TrueExpression()) &&
             ExpressionsStructurallyEqual(ca.FalseExpression(), cb.FalseExpression());
    }
  }
  return false;
}

bool IsSupportedForLoopComparisonOp(std::string_view op) {
  return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "!=";
}

std::string_view ExtractForLoopConditionVar(const BinaryExpression& bin_cond) {
  if (!IsSupportedForLoopComparisonOp(bin_cond.Op())) {
    return {};
  }
  const CExpression& lhs = UnwrapScalarCast(bin_cond.Lhs());
  if (lhs.Kind() != CExpressionKind::kIdentifier) {
    return {};
  }
  std::string_view loop_var = static_cast<const IdentifierExpression&>(lhs).Name();
  if (ExpressionUsesVariable(bin_cond.Rhs(), loop_var)) {
    return {};
  }
  return loop_var;
}

struct ForLoopStepMatch {
  std::unique_ptr<CExpression> step_expr;
  int direction = 0;  // +1 for positive, -1 for negative, 0 for symbolic stride
};

std::optional<ForLoopStepMatch> TryBuildForLoopStep(const CStatement& stmt,
                                                    std::string_view loop_var) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) {
    return std::nullopt;
  }
  const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& unary = static_cast<const UnaryExpression&>(expr);
    if ((unary.Op() == "++" || unary.Op() == "--") &&
        unary.Operand().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(unary.Operand()).Name() == loop_var) {
      return ForLoopStepMatch{expr.Clone(), unary.Op() == "++" ? 1 : -1};
    }
    return std::nullopt;
  }
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return std::nullopt;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
      static_cast<const IdentifierExpression&>(assign.Lhs()).Name() != loop_var) {
    return std::nullopt;
  }

  auto make_const_step = [&](int64_t signed_delta) -> std::optional<ForLoopStepMatch> {
    if (signed_delta == 0) {
      return std::nullopt;
    }
    if (signed_delta == 1) {
      return ForLoopStepMatch{
          CExpression::Unary("++", CExpression::Identifier(std::string(loop_var)),
                             /*is_prefix=*/false),
          1};
    }
    if (signed_delta == -1) {
      return ForLoopStepMatch{
          CExpression::Unary("--", CExpression::Identifier(std::string(loop_var)),
                             /*is_prefix=*/false),
          -1};
    }
    if (signed_delta > 1) {
      return ForLoopStepMatch{
          CExpression::Assignment("+=", CExpression::Identifier(std::string(loop_var)),
                                  CExpression::Integer(signed_delta)),
          1};
    }
    return ForLoopStepMatch{
        CExpression::Assignment("-=", CExpression::Identifier(std::string(loop_var)),
                                CExpression::Integer(-signed_delta)),
        -1};
  };

  if (assign.Op() == "+=" || assign.Op() == "-=") {
    if (!IsPureExpression(assign.Rhs()) || ExpressionUsesVariable(assign.Rhs(), loop_var)) {
      return std::nullopt;
    }
    if (assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
      int64_t val = static_cast<const IntegerLiteralExpression&>(assign.Rhs()).Value();
      return make_const_step(assign.Op() == "+=" ? val : -val);
    }
    return ForLoopStepMatch{expr.Clone(), 0};
  }

  if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
    if (bin.Op() != "+" && bin.Op() != "-") {
      return std::nullopt;
    }
    const CExpression* stride = nullptr;
    if (bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(bin.Lhs()).Name() == loop_var) {
      stride = &bin.Rhs();
    } else if (bin.Op() == "+" && bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
               static_cast<const IdentifierExpression&>(bin.Rhs()).Name() == loop_var) {
      stride = &bin.Lhs();
    }
    if (stride == nullptr || !IsPureExpression(*stride) ||
        ExpressionUsesVariable(*stride, loop_var)) {
      return std::nullopt;
    }
    if (stride->Kind() == CExpressionKind::kIntegerLiteral) {
      int64_t val = static_cast<const IntegerLiteralExpression&>(*stride).Value();
      return make_const_step(bin.Op() == "+" ? val : -val);
    }
    return ForLoopStepMatch{
        CExpression::Assignment(bin.Op() == "+" ? "+=" : "-=",
                                CExpression::Identifier(std::string(loop_var)), stride->Clone()),
        0};
  }
  return std::nullopt;
}

bool IsStepDirectionCompatibleWithCondition(std::string_view cond_op, int step_direction) {
  if (cond_op == "<" || cond_op == "<=") {
    return step_direction >= 0;
  }
  if (cond_op == ">" || cond_op == ">=") {
    return step_direction <= 0;
  }
  return cond_op == "!=";
}

const CExpression* MatchSimpleInitializerAssignment(const CStatement& stmt,
                                                    std::string_view loop_var) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) {
    return nullptr;
  }
  const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return nullptr;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
      static_cast<const IdentifierExpression&>(assign.Lhs()).Name() != loop_var) {
    return nullptr;
  }
  if (!IsPureExpression(assign.Rhs()) || ExpressionUsesVariable(assign.Rhs(), loop_var)) {
    return nullptr;
  }
  return &assign.Rhs();
}

bool IsRedundantLatchComparisonAssignment(const CStatement& stmt, const CExpression& loop_cond) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) {
    return false;
  }
  const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return false;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
    return false;
  }
  std::string_view lhs_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  if (!(IsScratchGprOrArgVar(lhs_name) || lhs_name == "at" || lhs_name == "temp_at")) {
    return false;
  }
  return ExpressionsStructurallyEqual(UnwrapScalarCast(assign.Rhs()), UnwrapScalarCast(loop_cond));
}

bool IsCompatibleForLoopEntryGuard(const CExpression& guard_cond, const BinaryExpression& loop_cond,
                                   std::string_view loop_var, const CExpression& init_rhs) {
  if (ExpressionsStructurallyEqual(guard_cond, loop_cond)) {
    return true;
  }
  auto substituted = SubstituteIdentifier(loop_cond, loop_var, init_rhs);
  if (ExpressionsStructurallyEqual(guard_cond, *substituted)) {
    return true;
  }
  if (guard_cond.Kind() != CExpressionKind::kBinaryExpression ||
      substituted->Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  const auto& g_bin = static_cast<const BinaryExpression&>(guard_cond);
  const auto& s_bin = static_cast<const BinaryExpression&>(*substituted);
  const CExpression& g_lhs = UnwrapScalarCast(g_bin.Lhs());
  const CExpression& g_rhs = UnwrapScalarCast(g_bin.Rhs());
  const CExpression& s_lhs = UnwrapScalarCast(s_bin.Lhs());
  const CExpression& s_rhs = UnwrapScalarCast(s_bin.Rhs());

  if (g_bin.Op() == s_bin.Op() && ExpressionsStructurallyEqual(g_lhs, s_lhs) &&
      ExpressionsStructurallyEqual(g_rhs, s_rhs)) {
    return true;
  }
  auto comm_op = CommutedBinaryOperator(s_bin.Op());
  if (comm_op.has_value() && g_bin.Op() == *comm_op && ExpressionsStructurallyEqual(g_lhs, s_rhs) &&
      ExpressionsStructurallyEqual(g_rhs, s_lhs)) {
    return true;
  }

  if (ExpressionsStructurallyEqual(g_lhs, s_lhs) &&
      g_rhs.Kind() == CExpressionKind::kIntegerLiteral &&
      s_rhs.Kind() == CExpressionKind::kIntegerLiteral) {
    int64_t g_val = static_cast<const IntegerLiteralExpression&>(g_rhs).Value();
    int64_t s_val = static_cast<const IntegerLiteralExpression&>(s_rhs).Value();
    if ((s_bin.Op() == "<=" && g_bin.Op() == "<" && g_val == s_val + 1) ||
        (s_bin.Op() == "<" && g_bin.Op() == "<=" && g_val == s_val - 1) ||
        (s_bin.Op() == ">=" && g_bin.Op() == ">" && g_val == s_val - 1) ||
        (s_bin.Op() == ">" && g_bin.Op() == ">=" && g_val == s_val + 1)) {
      return true;
    }
  }

  if (ExpressionsStructurallyEqual(g_lhs, s_rhs) &&
      g_rhs.Kind() == CExpressionKind::kIntegerLiteral &&
      s_lhs.Kind() == CExpressionKind::kIntegerLiteral) {
    int64_t g_val = static_cast<const IntegerLiteralExpression&>(g_rhs).Value();
    int64_t s_val = static_cast<const IntegerLiteralExpression&>(s_lhs).Value();
    if ((s_bin.Op() == "<=" && g_bin.Op() == ">" && g_val == s_val - 1) ||
        (s_bin.Op() == "<" && g_bin.Op() == ">=" && g_val == s_val + 1) ||
        (s_bin.Op() == ">=" && g_bin.Op() == "<" && g_val == s_val + 1) ||
        (s_bin.Op() == ">" && g_bin.Op() == "<=" && g_val == s_val - 1)) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool PruneUnusedGotoLabels(CompoundStatement* root_body) {
  if (root_body == nullptr) return false;

  GotoTargetCollectorVisitor collector;
  collector.VisitCompoundStatement(*root_body);
  const auto& used_labels = collector.Targets();

  bool changed = false;
  std::function<void(CompoundStatement*)> prune_labels = [&](CompoundStatement* cur_comp) {
    if (cur_comp == nullptr) return;
    auto& stmts = cur_comp->MutableStatements();
    for (size_t i = 0; i < stmts.size();) {
      if (stmts[i] == nullptr) {
        ++i;
        continue;
      }
      if (stmts[i]->Kind() == CStatementKind::kLabelStatement) {
        std::string lbl(static_cast<const LabelStatement&>(*stmts[i]).Label());
        if (!used_labels.contains(lbl)) {
          stmts.erase(stmts.begin() + i);
          changed = true;
          continue;
        }
      } else {
        ForEachSubCompoundStatement(stmts[i].get(), prune_labels);
      }
      ++i;
    }
  };
  prune_labels(root_body);
  return changed;
}

bool CollapseAliasSwitchGotoCases(CompoundStatement* root_body) {
  if (root_body == nullptr) return false;
  std::string before = root_body->ToString();

  // Collapse alias switch cases (`case X: block_N: ...` and `case Y: goto block_N;`) into
  // shared `case X: case Y:` labels and ensure case arms do not fall through into a backward goto.
  std::function<void(CompoundStatement*)> merge_alias_switch_cases = [&](CompoundStatement* comp) {
    if (comp == nullptr) return;
    for (auto& stmt_ptr : comp->MutableStatements()) {
      if (stmt_ptr == nullptr) continue;
      ForEachSubCompoundStatement(stmt_ptr.get(), merge_alias_switch_cases);
      if (stmt_ptr->Kind() != CStatementKind::kSwitchStatement) continue;
      auto& sw = static_cast<SwitchStatement&>(*stmt_ptr);
      auto& cases = sw.MutableCases();
      for (size_t a = 0; a < cases.size(); ++a) {
        if (cases[a].is_default || cases[a].body == nullptr) continue;
        const auto& a_stmts = cases[a].body->Statements();
        if (a_stmts.empty() || a_stmts[0]->Kind() != CStatementKind::kLabelStatement) continue;
        std::string target_lbl(static_cast<const LabelStatement&>(*a_stmts[0]).Label());
        for (size_t b = 0; b < cases.size();) {
          if (b == a || cases[b].is_default || cases[b].body == nullptr) {
            ++b;
            continue;
          }
          const auto& b_stmts = cases[b].body->Statements();
          bool is_pure_goto =
              (b_stmts.size() == 1 && b_stmts[0]->Kind() == CStatementKind::kGotoStatement &&
               static_cast<const GotoStatement&>(*b_stmts[0]).Label() == target_lbl) ||
              (b_stmts.size() == 2 && b_stmts[0]->Kind() == CStatementKind::kGotoStatement &&
               static_cast<const GotoStatement&>(*b_stmts[0]).Label() == target_lbl &&
               b_stmts[1]->Kind() == CStatementKind::kBreakStatement);
          if (!is_pure_goto) {
            ++b;
            continue;
          }
          if (b > 0 && b - 1 != a && cases[b - 1].body != nullptr &&
              !cases[b - 1].body->Statements().empty()) {
            CStatementKind prev_last = cases[b - 1].body->Statements().back()->Kind();
            if (prev_last != CStatementKind::kBreakStatement &&
                prev_last != CStatementKind::kReturnStatement &&
                prev_last != CStatementKind::kGotoStatement &&
                prev_last != CStatementKind::kContinueStatement) {
              ++b;
              continue;
            }
          }
          if (b == a + 1 && !cases[a].body->Statements().empty()) {
            CStatementKind a_last = cases[a].body->Statements().back()->Kind();
            if (a_last != CStatementKind::kBreakStatement &&
                a_last != CStatementKind::kReturnStatement &&
                a_last != CStatementKind::kGotoStatement &&
                a_last != CStatementKind::kContinueStatement) {
              cases[a].body->AddStatement(CStatement::Break());
            }
          }
          bool is_adjacent_case_value = false;
          for (int64_t val_b : cases[b].case_values) {
            for (int64_t val_a : cases[a].case_values) {
              if (val_a - val_b == 1 || val_b - val_a == 1) {
                is_adjacent_case_value = true;
                break;
              }
            }
            if (is_adjacent_case_value) break;
          }
          bool a_has_inner_labels = false;
          for (size_t k = 1; k < cases[a].body->Statements().size(); ++k) {
            if (StatementContainsLabel(*cases[a].body->Statements()[k])) {
              a_has_inner_labels = true;
              break;
            }
          }
          if (is_adjacent_case_value && !a_has_inner_labels) {
            auto cloned_body = std::make_unique<CompoundStatement>();
            for (size_t k = 1; k < cases[a].body->Statements().size(); ++k) {
              cloned_body->AddStatement(cases[a].body->Statements()[k]->Clone());
            }
            cases[b].body = std::move(cloned_body);
            ++b;
          } else if (!is_adjacent_case_value) {
            for (int64_t val : cases[b].case_values) {
              cases[a].case_values.push_back(val);
            }
            cases.erase(cases.begin() + b);
            if (b < a) {
              --a;
            }
          } else {
            ++b;
          }
        }
      }
    }
  };
  merge_alias_switch_cases(root_body);
  PruneUnusedGotoLabels(root_body);
  return root_body->ToString() != before;
}

void InlineCrossJumpedCallTails(CompoundStatement* root_body, bool allow_diamond_tails) {
  if (root_body == nullptr) return;

  struct LabelSite {
    CompoundStatement* comp = nullptr;
    const CStatement* goto_scope = nullptr;
    std::string label_name;
    std::vector<std::unique_ptr<CStatement>> tail_stmts;
    bool ends_with_break = false;
    bool requires_tail_goto = false;
  };

  auto is_register_or_local_var_name = [](std::string_view name) -> bool {
    return name == "v0" || name == "v1" || name == "at" || name == "sp" || name == "fp" ||
           name == "ra" || name == "gp" || name == "zero" || name == "hi" || name == "lo" ||
           name == "fcond" || absl::StartsWith(name, "arg") || absl::StartsWith(name, "temp_") ||
           absl::StartsWith(name, "var_") ||
           (name.size() >= 2 && name[0] == 'f' && name[1] >= '0' && name[1] <= '9');
  };

  auto lhs_writes_memory_or_global = [&](const CExpression& lhs) -> bool {
    if (lhs.Kind() == CExpressionKind::kUnaryExpression ||
        lhs.Kind() == CExpressionKind::kArrayIndexExpression ||
        lhs.Kind() == CExpressionKind::kMemberAccessExpression) {
      return true;
    }
    if (lhs.Kind() == CExpressionKind::kIdentifier) {
      return !is_register_or_local_var_name(static_cast<const IdentifierExpression&>(lhs).Name());
    }
    return false;
  };

  auto is_inlinable_tail_stmt = [&](const CStatement& s, bool* out_has_call,
                                    bool* out_writes_memory) -> bool {
    if (s.Kind() == CStatementKind::kReturnStatement) {
      return true;
    }
    if (s.Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(s);
      if (ExpressionContainsCall(es.Expression())) {
        *out_has_call = true;
        *out_writes_memory = true;
      }
      if (es.Expression().Kind() == CExpressionKind::kCallExpression) {
        return true;
      }
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() != "=") return false;
        if (lhs_writes_memory_or_global(a.Lhs())) {
          *out_writes_memory = true;
        }
        return true;
      }
      return false;
    }
    if (s.Kind() == CStatementKind::kIfStatement) {
      const auto& ifs = static_cast<const IfStatement&>(s);
      if (ifs.ElseBranch() != nullptr ||
          ifs.ThenBranch().Kind() != CStatementKind::kCompoundStatement) {
        return false;
      }
      const auto& then_stmts = static_cast<const CompoundStatement&>(ifs.ThenBranch()).Statements();
      if (then_stmts.size() != 1 || then_stmts[0]->Kind() != CStatementKind::kExpressionStatement) {
        return false;
      }
      const auto& es = static_cast<const ExpressionStatement&>(*then_stmts[0]);
      if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
          ExpressionContainsCall(es.Expression())) {
        return false;
      }
      const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
      if (lhs_writes_memory_or_global(a.Lhs())) {
        *out_writes_memory = true;
      }
      return true;
    }
    return false;
  };

  for (int pass = 0; pass < 3; ++pass) {
    std::vector<LabelSite> candidates;
    std::function<void(CompoundStatement*, const CStatement*, bool, bool, bool, bool, bool)>
        collect_labels = [&](CompoundStatement* comp, const CStatement* scope_root, bool is_root,
                             bool parent_ends_with_break, bool is_switch_case_body, bool in_switch,
                             bool in_loop) {
          if (comp == nullptr) return;
          auto& stmts = comp->MutableStatements();
          for (size_t i = 0; i < stmts.size(); ++i) {
            CStatement* s = stmts[i].get();
            if (s == nullptr) continue;
            if (!is_root && s->Kind() == CStatementKind::kLabelStatement) {
              std::string lbl(static_cast<const LabelStatement&>(*s).Label());
              size_t end_idx = stmts.size();
              bool ends_with_break = parent_ends_with_break;
              if (end_idx > i + 1 &&
                  stmts[end_idx - 1]->Kind() == CStatementKind::kBreakStatement) {
                --end_idx;
                ends_with_break = true;
              }
              std::vector<std::unique_ptr<CStatement>> tail;
              bool has_call = false;
              bool writes_memory = false;
              bool has_inner_label = false;
              bool all_tail_stmts_valid = true;
              for (size_t k = i + 1; k < end_idx; ++k) {
                if (stmts[k] == nullptr) {
                  all_tail_stmts_valid = false;
                  break;
                }
                if (stmts[k]->Kind() == CStatementKind::kLabelStatement) {
                  has_inner_label = true;
                  continue;
                }
                if (!is_inlinable_tail_stmt(*stmts[k], &has_call, &writes_memory)) {
                  all_tail_stmts_valid = false;
                  break;
                }
                tail.push_back(stmts[k]->Clone());
              }
              if (in_loop && has_call) continue;
              bool ends_with_return =
                  !tail.empty() && tail.back()->Kind() == CStatementKind::kReturnStatement;
              bool valid_call_tail = all_tail_stmts_valid && !has_inner_label && has_call;
              bool valid_return_tail = allow_diamond_tails && all_tail_stmts_valid &&
                                       !has_inner_label && ends_with_return && writes_memory &&
                                       tail.size() <= 5;
              bool valid_switch_tail = all_tail_stmts_valid && writes_memory &&
                                       is_switch_case_body && i > 0 && ends_with_break;
              bool non_trivial_diamond_tail =
                  writes_memory ||
                  (!tail.empty() && tail[0]->Kind() == CStatementKind::kIfStatement);
              bool valid_diamond_tail = allow_diamond_tails && !in_switch && !in_loop &&
                                        all_tail_stmts_valid && !has_inner_label &&
                                        !ends_with_return && non_trivial_diamond_tail &&
                                        tail.size() <= 5;
              if (!tail.empty() && (valid_call_tail || valid_return_tail || valid_switch_tail ||
                                    valid_diamond_tail)) {
                candidates.push_back(LabelSite{
                    .comp = comp,
                    .goto_scope = scope_root,
                    .label_name = std::move(lbl),
                    .tail_stmts = std::move(tail),
                    .ends_with_break = ends_with_break,
                    .requires_tail_goto = !valid_call_tail && !valid_return_tail &&
                                          !valid_switch_tail && !(in_switch && ends_with_break)});
              }
            } else if (s->Kind() == CStatementKind::kIfStatement) {
              bool stmt_at_tail = (i + 1 == stmts.size()) ||
                                  (i + 2 == stmts.size() &&
                                   (stmts[i + 1]->Kind() == CStatementKind::kBreakStatement ||
                                    stmts[i + 1]->Kind() == CStatementKind::kReturnStatement));
              bool branch_ends_with_break = stmt_at_tail && parent_ends_with_break;
              const CStatement* child_scope = stmt_at_tail ? scope_root : s;
              auto& ifs = static_cast<IfStatement&>(*s);
              if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
                collect_labels(static_cast<CompoundStatement*>(ifs.MutableThenBranch()),
                               child_scope, false, branch_ends_with_break, false, in_switch,
                               in_loop);
              }
              if (ifs.MutableElseBranch() != nullptr &&
                  ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
                collect_labels(static_cast<CompoundStatement*>(ifs.MutableElseBranch()),
                               child_scope, false, branch_ends_with_break, false, in_switch,
                               in_loop);
              }
            } else if (s->Kind() == CStatementKind::kSwitchStatement) {
              for (auto& sc : static_cast<SwitchStatement&>(*s).MutableCases()) {
                if (sc.body != nullptr) {
                  bool sc_ends_with_break =
                      !sc.body->Statements().empty() &&
                      (sc.body->Statements().back()->Kind() == CStatementKind::kBreakStatement ||
                       sc.body->Statements().back()->Kind() == CStatementKind::kReturnStatement);
                  collect_labels(sc.body.get(), sc.body.get(), false, sc_ends_with_break, true,
                                 true, in_loop);
                }
              }
            } else if (allow_diamond_tails && s->Kind() == CStatementKind::kWhileStatement) {
              auto* body = static_cast<WhileStatement&>(*s).MutableBody();
              if (body != nullptr && body->Kind() == CStatementKind::kCompoundStatement) {
                collect_labels(static_cast<CompoundStatement*>(body), body, true, false, false,
                               false, true);
              }
            } else if (allow_diamond_tails && s->Kind() == CStatementKind::kDoWhileStatement) {
              auto* body = static_cast<DoWhileStatement&>(*s).MutableBody();
              if (body != nullptr && body->Kind() == CStatementKind::kCompoundStatement) {
                collect_labels(static_cast<CompoundStatement*>(body), body, true, false, false,
                               false, true);
              }
            } else if (allow_diamond_tails && s->Kind() == CStatementKind::kForStatement) {
              auto* body = static_cast<ForStatement&>(*s).MutableBody();
              if (body != nullptr && body->Kind() == CStatementKind::kCompoundStatement) {
                collect_labels(static_cast<CompoundStatement*>(body), body, true, false, false,
                               false, true);
              }
            } else if (s->Kind() == CStatementKind::kCompoundStatement) {
              bool stmt_at_tail = (i + 1 == stmts.size()) ||
                                  (i + 2 == stmts.size() &&
                                   (stmts[i + 1]->Kind() == CStatementKind::kBreakStatement ||
                                    stmts[i + 1]->Kind() == CStatementKind::kReturnStatement));
              bool sub_ends_with_break = stmt_at_tail && parent_ends_with_break;
              const CStatement* child_scope = stmt_at_tail ? scope_root : s;
              collect_labels(static_cast<CompoundStatement*>(s), child_scope, false,
                             sub_ends_with_break, false, in_switch, in_loop);
            }
          }
        };
    collect_labels(root_body, root_body, true, true, false, false, false);

    bool any_inlined_in_pass = false;
    for (const auto& site : candidates) {
      int total_gotos = 0;
      int valid_tail_gotos = 0;
      std::function<void(const CStatement&)> check_gotos = [&](const CStatement& s) {
        if (s.Kind() == CStatementKind::kGotoStatement) {
          if (static_cast<const GotoStatement&>(s).Label() == site.label_name) {
            ++total_gotos;
          }
        } else if (s.Kind() == CStatementKind::kCompoundStatement) {
          const auto& cs = static_cast<const CompoundStatement&>(s).Statements();
          for (size_t i = 0; i < cs.size(); ++i) {
            if (cs[i]->Kind() == CStatementKind::kGotoStatement &&
                static_cast<const GotoStatement&>(*cs[i]).Label() == site.label_name) {
              ++total_gotos;
              if (i + 1 == cs.size() ||
                  (i + 2 == cs.size() && cs[i + 1]->Kind() == CStatementKind::kBreakStatement)) {
                ++valid_tail_gotos;
              }
            } else {
              check_gotos(*cs[i]);
            }
          }
        } else if (s.Kind() == CStatementKind::kIfStatement) {
          const auto& ifs = static_cast<const IfStatement&>(s);
          check_gotos(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) check_gotos(*ifs.ElseBranch());
        } else if (s.Kind() == CStatementKind::kSwitchStatement) {
          for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
            if (sc.body != nullptr) check_gotos(*sc.body);
          }
        } else if (s.Kind() == CStatementKind::kWhileStatement) {
          check_gotos(static_cast<const WhileStatement&>(s).Body());
        } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
          check_gotos(static_cast<const DoWhileStatement&>(s).Body());
        } else if (s.Kind() == CStatementKind::kForStatement) {
          check_gotos(static_cast<const ForStatement&>(s).Body());
        }
      };
      check_gotos(*root_body);

      if (site.requires_tail_goto && site.goto_scope != nullptr) {
        int scope_tail_gotos = 0;
        std::function<void(const CStatement&)> count_scope_tail_gotos = [&](const CStatement& s) {
          if (s.Kind() == CStatementKind::kCompoundStatement) {
            const auto& cs = static_cast<const CompoundStatement&>(s).Statements();
            if (cs.empty() || cs.back() == nullptr) return;
            const CStatement* last = cs.back().get();
            if (cs.size() >= 2 && cs[cs.size() - 2] != nullptr &&
                ((site.ends_with_break && last->Kind() == CStatementKind::kBreakStatement) ||
                 last->Kind() == CStatementKind::kReturnStatement)) {
              last = cs[cs.size() - 2].get();
            }
            if (last->Kind() == CStatementKind::kGotoStatement &&
                static_cast<const GotoStatement&>(*last).Label() == site.label_name) {
              ++scope_tail_gotos;
            } else if (last->Kind() == CStatementKind::kIfStatement ||
                       last->Kind() == CStatementKind::kCompoundStatement) {
              count_scope_tail_gotos(*last);
            }
          } else if (s.Kind() == CStatementKind::kIfStatement) {
            const auto& ifs = static_cast<const IfStatement&>(s);
            count_scope_tail_gotos(ifs.ThenBranch());
            if (ifs.ElseBranch() != nullptr) count_scope_tail_gotos(*ifs.ElseBranch());
          }
        };
        count_scope_tail_gotos(*site.goto_scope);
        if (scope_tail_gotos != total_gotos) continue;
      }

      if (total_gotos == 0 || total_gotos != valid_tail_gotos || site.comp == nullptr) continue;

      int replaced_gotos = 0;
      std::function<void(CompoundStatement*, bool)> replace_gotos = [&](CompoundStatement* comp,
                                                                        bool in_switch_or_loop) {
        if (comp == nullptr) return;
        auto& stmts = comp->MutableStatements();
        for (size_t i = 0; i < stmts.size(); ++i) {
          CStatement* s = stmts[i].get();
          if (s == nullptr) continue;
          if (s->Kind() == CStatementKind::kGotoStatement &&
              static_cast<const GotoStatement&>(*s).Label() == site.label_name) {
            stmts.erase(stmts.begin() + i);
            for (size_t k = 0; k < site.tail_stmts.size(); ++k) {
              stmts.insert(stmts.begin() + i + k, site.tail_stmts[k]->Clone());
            }
            if (site.ends_with_break && in_switch_or_loop) {
              size_t after_idx = i + site.tail_stmts.size();
              if (after_idx >= stmts.size() ||
                  stmts[after_idx]->Kind() != CStatementKind::kBreakStatement) {
                stmts.insert(stmts.begin() + after_idx, CStatement::Break());
              }
            }
            ++replaced_gotos;
            return;
          }
          bool child_in_switch_or_loop = in_switch_or_loop ||
                                         s->Kind() == CStatementKind::kSwitchStatement ||
                                         s->Kind() == CStatementKind::kWhileStatement ||
                                         s->Kind() == CStatementKind::kDoWhileStatement ||
                                         s->Kind() == CStatementKind::kForStatement;
          ForEachSubCompoundStatement(
              s, [&](CompoundStatement* sub) { replace_gotos(sub, child_in_switch_or_loop); });
        }
      };
      replace_gotos(root_body, false);

      total_gotos = 0;
      check_gotos(*root_body);
      if (total_gotos == 0 && replaced_gotos > 0) {
        std::function<bool(CompoundStatement*)> remove_label =
            [&](CompoundStatement* comp) -> bool {
          if (comp == nullptr) return false;
          auto& stmts = comp->MutableStatements();
          for (size_t i = 0; i < stmts.size(); ++i) {
            if (stmts[i]->Kind() == CStatementKind::kLabelStatement &&
                static_cast<const LabelStatement&>(*stmts[i]).Label() == site.label_name) {
              stmts.erase(stmts.begin() + i);
              return true;
            }
            bool removed = false;
            ForEachSubCompoundStatement(stmts[i].get(), [&](CompoundStatement* sub) {
              if (!removed && remove_label(sub)) removed = true;
            });
            if (removed) return true;
          }
          return false;
        };
        remove_label(root_body);
        any_inlined_in_pass = true;
      }
    }
    if (!any_inlined_in_pass) break;
  }

  CollapseAliasSwitchGotoCases(root_body);
}

void SinkCommonBranchTailArgAssignments(CompoundStatement* comp) {
  if (comp == nullptr) return;
  auto extract_sinkable_assign = [](const CStatement& s, std::string* lhs_out,
                                    std::string* rhs_str_out,
                                    const CExpression** rhs_expr_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
    std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
    if (lhs == "sp" || lhs == "fcond" || !IsPureExpression(a.Rhs()) ||
        ExpressionUsesVariable(a.Rhs(), lhs)) {
      return false;
    }
    bool rhs_has_mem_deref = false;
    std::function<void(const CExpression&)> check_mem = [&](const CExpression& e) {
      if (rhs_has_mem_deref) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos && un.Op() != "*(s32*)&" &&
            un.Op() != "*(u32*)&" && un.Op() != "*(f32*)&" && un.Op() != "*(s32 *)&" &&
            un.Op() != "*(u32 *)&" && un.Op() != "*(f32 *)&") {
          rhs_has_mem_deref = true;
          return;
        }
        check_mem(un.Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        rhs_has_mem_deref = true;
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        if (static_cast<const MemberAccessExpression&>(e).IsArrow()) {
          rhs_has_mem_deref = true;
          return;
        }
        check_mem(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check_mem(static_cast<const BinaryExpression&>(e).Lhs());
        check_mem(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check_mem(static_cast<const CastExpression&>(e).Operand());
      }
    };
    check_mem(a.Rhs());
    if (rhs_has_mem_deref) return false;
    *lhs_out = std::move(lhs);
    *rhs_str_out = a.Rhs().ToString();
    if (rhs_expr_out != nullptr) *rhs_expr_out = &a.Rhs();
    return true;
  };

  auto is_simple_reg_assign = [](const CStatement& s, std::string* lhs_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
    *lhs_out = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
    return true;
  };

  std::function<bool(const CStatement&, std::string_view, bool*)> prefix_reads_before_write =
      [&](const CStatement& s, std::string_view var, bool* written) -> bool {
    if (*written) return false;
    switch (s.Kind()) {
      case CStatementKind::kReturnStatement:
      case CStatementKind::kBreakStatement:
      case CStatementKind::kContinueStatement:
        return true;
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(s);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(a.Lhs()).Name() == var) {
            if (ExpressionUsesVariable(a.Rhs(), var)) return true;
            *written = true;
            return false;
          }
        }
        return StatementReferencesVariable(s, var);
      }
      case CStatementKind::kCompoundStatement: {
        const auto& cs = static_cast<const CompoundStatement&>(s);
        for (const auto& ch : cs.Statements()) {
          if (ch == nullptr) continue;
          if (ch->Kind() == CStatementKind::kLabelStatement) {
            *written = false;
            continue;
          }
          if (prefix_reads_before_write(*ch, var, written)) return true;
        }
        return false;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        if (!*written && ExpressionUsesVariable(ifs.Condition(), var)) return true;
        bool tw = *written;
        if (prefix_reads_before_write(ifs.ThenBranch(), var, &tw)) return true;
        bool ew = *written;
        if (ifs.ElseBranch() != nullptr) {
          if (prefix_reads_before_write(*ifs.ElseBranch(), var, &ew)) return true;
          if (tw && ew) *written = true;
        }
        return false;
      }
      default:
        return StatementReferencesVariable(s, var);
    }
  };

  auto is_sinkable_tail_stmt = [](const CStatement& s) -> bool {
    return s.Kind() == CStatementKind::kExpressionStatement && !StatementContainsControlFlowJump(s);
  };

  std::function<void(CompoundStatement*)> sink_in_comp = [&](CompoundStatement* cur_comp) {
    if (cur_comp == nullptr) return;
    auto& stmts = cur_comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
        auto& ifs = static_cast<IfStatement&>(*stmts[i]);
        if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
          sink_in_comp(static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
        }
        if (ifs.MutableElseBranch() != nullptr &&
            ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          sink_in_comp(static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
        }

        // Rule B: If one branch ends with `goto block_N;` and the other branch ends with
        // `block_N: <sinkable ExpressionStatements>`, sink `block_N: <tail>` after the
        // IfStatement and remove the redundant `goto block_N;`.
        if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
            ifs.MutableElseBranch() != nullptr &&
            ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          auto* then_comp = static_cast<CompoundStatement*>(ifs.MutableThenBranch());
          auto* else_comp = static_cast<CompoundStatement*>(ifs.MutableElseBranch());
          auto& then_stmts = then_comp->MutableStatements();
          auto& else_stmts = else_comp->MutableStatements();
          if (!else_stmts.empty() && else_stmts.back()->Kind() == CStatementKind::kGotoStatement &&
              then_stmts.size() >= 2) {
            std::string target_lbl(static_cast<const GotoStatement&>(*else_stmts.back()).Label());
            size_t lbl_idx = then_stmts.size();
            for (size_t k = then_stmts.size(); k > 0; --k) {
              if (then_stmts[k - 1]->Kind() == CStatementKind::kLabelStatement &&
                  static_cast<const LabelStatement&>(*then_stmts[k - 1]).Label() == target_lbl) {
                lbl_idx = k - 1;
                break;
              }
              if (!is_sinkable_tail_stmt(*then_stmts[k - 1])) {
                break;
              }
            }
            if (lbl_idx >= 1 && lbl_idx < then_stmts.size()) {
              else_stmts.pop_back();
              std::vector<std::unique_ptr<CStatement>> sunk_tail;
              for (size_t k = lbl_idx; k < then_stmts.size(); ++k) {
                sunk_tail.push_back(std::move(then_stmts[k]));
              }
              then_stmts.erase(then_stmts.begin() + lbl_idx, then_stmts.end());
              for (size_t k = 0; k < sunk_tail.size(); ++k) {
                stmts.insert(stmts.begin() + i + 1 + k, std::move(sunk_tail[k]));
              }
              if (else_stmts.empty() && !then_stmts.empty()) {
                stmts[i] = CStatement::If(ifs.Condition().Clone(), ifs.MutableThenBranch()->Clone(),
                                          nullptr);
              }
            }
          } else if (!then_stmts.empty() &&
                     then_stmts.back()->Kind() == CStatementKind::kGotoStatement &&
                     else_stmts.size() >= 2) {
            std::string target_lbl(static_cast<const GotoStatement&>(*then_stmts.back()).Label());
            size_t lbl_idx = else_stmts.size();
            for (size_t k = else_stmts.size(); k > 0; --k) {
              if (else_stmts[k - 1]->Kind() == CStatementKind::kLabelStatement &&
                  static_cast<const LabelStatement&>(*else_stmts[k - 1]).Label() == target_lbl) {
                lbl_idx = k - 1;
                break;
              }
              if (!is_sinkable_tail_stmt(*else_stmts[k - 1])) {
                break;
              }
            }
            if (lbl_idx >= 1 && lbl_idx < else_stmts.size()) {
              then_stmts.pop_back();
              std::vector<std::unique_ptr<CStatement>> sunk_tail;
              for (size_t k = lbl_idx; k < else_stmts.size(); ++k) {
                sunk_tail.push_back(std::move(else_stmts[k]));
              }
              else_stmts.erase(else_stmts.begin() + lbl_idx, else_stmts.end());
              for (size_t k = 0; k < sunk_tail.size(); ++k) {
                stmts.insert(stmts.begin() + i + 1 + k, std::move(sunk_tail[k]));
              }
              if (then_stmts.empty() && !else_stmts.empty()) {
                stmts[i] = CStatement::If(NegateCondition(ifs.Condition()),
                                          ifs.MutableElseBranch()->Clone(), nullptr);
              }
            }
          }
        }

        auto& ifs_after_b = static_cast<IfStatement&>(*stmts[i]);
        if (ifs_after_b.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
            ifs_after_b.MutableElseBranch() != nullptr &&
            ifs_after_b.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          auto* then_comp = static_cast<CompoundStatement*>(ifs_after_b.MutableThenBranch());
          auto* else_comp = static_cast<CompoundStatement*>(ifs_after_b.MutableElseBranch());
          auto& then_stmts = then_comp->MutableStatements();
          auto& else_stmts = else_comp->MutableStatements();
          bool sunk_any = true;
          while (sunk_any && !then_stmts.empty() && !else_stmts.empty()) {
            sunk_any = false;
            for (size_t ti = then_stmts.size(); ti > 0; --ti) {
              std::string t_lhs;
              std::string t_rhs;
              const CExpression* t_rhs_expr = nullptr;
              if (!extract_sinkable_assign(*then_stmts[ti - 1], &t_lhs, &t_rhs, &t_rhs_expr)) {
                continue;
              }
              bool t_blocked = false;
              for (size_t tk = ti; tk < then_stmts.size(); ++tk) {
                std::string after_lhs;
                if (!is_simple_reg_assign(*then_stmts[tk], &after_lhs) || after_lhs == t_lhs ||
                    StatementReferencesVariable(*then_stmts[tk], t_lhs) ||
                    ExpressionUsesVariable(*t_rhs_expr, after_lhs)) {
                  t_blocked = true;
                  break;
                }
              }
              if (t_blocked) continue;

              for (size_t ei = else_stmts.size(); ei > 0; --ei) {
                std::string e_lhs;
                std::string e_rhs;
                const CExpression* e_rhs_expr = nullptr;
                if (!extract_sinkable_assign(*else_stmts[ei - 1], &e_lhs, &e_rhs, &e_rhs_expr)) {
                  continue;
                }
                if (e_lhs != t_lhs || e_rhs != t_rhs) continue;
                bool e_blocked = false;
                for (size_t ek = ei; ek < else_stmts.size(); ++ek) {
                  std::string after_lhs;
                  if (!is_simple_reg_assign(*else_stmts[ek], &after_lhs) || after_lhs == e_lhs ||
                      StatementReferencesVariable(*else_stmts[ek], e_lhs) ||
                      ExpressionUsesVariable(*e_rhs_expr, after_lhs)) {
                    e_blocked = true;
                    break;
                  }
                }
                if (e_blocked) continue;

                auto sunk_stmt = then_stmts[ti - 1]->Clone();
                then_stmts.erase(then_stmts.begin() + (ti - 1));
                else_stmts.erase(else_stmts.begin() + (ei - 1));
                stmts.insert(stmts.begin() + i + 1, std::move(sunk_stmt));
                sunk_any = true;
                break;
              }
              if (sunk_any) break;
            }
          }
          if (else_stmts.empty() && !then_stmts.empty()) {
            stmts[i] = CStatement::If(ifs_after_b.Condition().Clone(),
                                      ifs_after_b.MutableThenBranch()->Clone(), nullptr);
          }
        }

        // Rule A: Sink trailing LabelStatements at the very end of `then` or `else` to
        // immediately after the IfStatement.
        auto& ifs_after_sink = static_cast<IfStatement&>(*stmts[i]);
        if (ifs_after_sink.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
          auto* then_comp = static_cast<CompoundStatement*>(ifs_after_sink.MutableThenBranch());
          auto& then_stmts = then_comp->MutableStatements();
          while (then_stmts.size() >= 2 &&
                 then_stmts.back()->Kind() == CStatementKind::kLabelStatement) {
            auto sunk_lbl = std::move(then_stmts.back());
            then_stmts.pop_back();
            stmts.insert(stmts.begin() + i + 1, std::move(sunk_lbl));
          }
        }
        if (ifs_after_sink.MutableElseBranch() != nullptr &&
            ifs_after_sink.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          auto* then_comp =
              ifs_after_sink.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement
                  ? static_cast<CompoundStatement*>(ifs_after_sink.MutableThenBranch())
                  : nullptr;
          auto* else_comp = static_cast<CompoundStatement*>(ifs_after_sink.MutableElseBranch());
          auto& else_stmts = else_comp->MutableStatements();
          while (!else_stmts.empty() &&
                 else_stmts.back()->Kind() == CStatementKind::kLabelStatement) {
            auto sunk_lbl = std::move(else_stmts.back());
            else_stmts.pop_back();
            stmts.insert(stmts.begin() + i + 1, std::move(sunk_lbl));
          }
          if (else_stmts.empty() && then_comp != nullptr && !then_comp->Statements().empty()) {
            stmts[i] = CStatement::If(ifs_after_sink.Condition().Clone(),
                                      ifs_after_sink.MutableThenBranch()->Clone(), nullptr);
          }
        }

        auto& cur_ifs = static_cast<IfStatement&>(*stmts[i]);
        if (i > 0 && cur_ifs.MutableElseBranch() == nullptr &&
            cur_ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
            IsPureExpression(cur_ifs.Condition())) {
          std::string pre_lhs;
          std::string pre_rhs;
          const CExpression* pre_rhs_expr = nullptr;
          if (extract_sinkable_assign(*stmts[i - 1], &pre_lhs, &pre_rhs, &pre_rhs_expr) &&
              !ExpressionUsesVariable(cur_ifs.Condition(), pre_lhs) &&
              !StatementTakesAddressOf(*cur_ifs.MutableThenBranch(), pre_lhs)) {
            auto* then_comp = static_cast<CompoundStatement*>(cur_ifs.MutableThenBranch());
            auto& then_stmts = then_comp->MutableStatements();
            if (then_stmts.size() >= 2) {
              std::string tail_lhs;
              std::string tail_rhs;
              const CExpression* tail_rhs_expr = nullptr;
              if (extract_sinkable_assign(*then_stmts.back(), &tail_lhs, &tail_rhs,
                                          &tail_rhs_expr) &&
                  tail_lhs == pre_lhs && tail_rhs == pre_rhs) {
                size_t prefix_end = then_stmts.size() - 1;
                bool has_join_label = false;
                if (prefix_end >= 1 &&
                    then_stmts[prefix_end - 1]->Kind() == CStatementKind::kLabelStatement) {
                  has_join_label = true;
                  --prefix_end;
                }
                bool prefix_ok = (prefix_end >= 1);
                bool written = false;
                for (size_t pk = 0; pk < prefix_end && prefix_ok; ++pk) {
                  const auto& ps = *then_stmts[pk];
                  if (!has_join_label && StatementContainsControlFlowJump(ps)) {
                    prefix_ok = false;
                    break;
                  }
                  if (prefix_reads_before_write(ps, pre_lhs, &written)) {
                    prefix_ok = false;
                    break;
                  }
                }
                if (prefix_ok) {
                  std::unique_ptr<CStatement> sunk_assign = std::move(then_stmts.back());
                  then_stmts.pop_back();
                  std::unique_ptr<CStatement> sunk_label;
                  if (has_join_label) {
                    sunk_label = std::move(then_stmts.back());
                    then_stmts.pop_back();
                  }
                  stmts.erase(stmts.begin() + (i - 1));
                  --i;
                  if (sunk_label != nullptr) {
                    stmts.insert(stmts.begin() + i + 1, std::move(sunk_label));
                    stmts.insert(stmts.begin() + i + 2, std::move(sunk_assign));
                  } else {
                    stmts.insert(stmts.begin() + i + 1, std::move(sunk_assign));
                  }
                }
              }
            }
          }
        }
      } else {
        ForEachSubCompoundStatement(stmts[i].get(), sink_in_comp);
      }
    }
  };
  sink_in_comp(comp);

  // Rule D: Rewrite loop-exit and loop-continue `goto block_N;` into structured `break;` /
  // `continue;` when `block_N:` is at the end of a loop body (`continue;`) or immediately after
  // the loop (with 0 or 1 shared post-loop assignment `S`, `break;`).
  auto extract_loop_body = [](CStatement* s) -> CompoundStatement* {
    if (s == nullptr) return nullptr;
    if (s->Kind() == CStatementKind::kDoWhileStatement) {
      auto* b = static_cast<DoWhileStatement*>(s)->MutableBody();
      return (b != nullptr && b->Kind() == CStatementKind::kCompoundStatement)
                 ? static_cast<CompoundStatement*>(b)
                 : nullptr;
    }
    if (s->Kind() == CStatementKind::kWhileStatement) {
      auto* b = static_cast<WhileStatement*>(s)->MutableBody();
      return (b != nullptr && b->Kind() == CStatementKind::kCompoundStatement)
                 ? static_cast<CompoundStatement*>(b)
                 : nullptr;
    }
    if (s->Kind() == CStatementKind::kForStatement) {
      auto* b = static_cast<ForStatement*>(s)->MutableBody();
      return (b != nullptr && b->Kind() == CStatementKind::kCompoundStatement)
                 ? static_cast<CompoundStatement*>(b)
                 : nullptr;
    }
    return nullptr;
  };
  auto extract_guarded_or_direct_loop_body = [&](CStatement* s) -> CompoundStatement* {
    if (auto* direct = extract_loop_body(s); direct != nullptr) {
      return direct;
    }
    if (s != nullptr && s->Kind() == CStatementKind::kIfStatement) {
      auto& ifs = *static_cast<IfStatement*>(s);
      if (ifs.ElseBranch() == nullptr && ifs.MutableThenBranch() != nullptr &&
          ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
        auto& then_stmts =
            static_cast<CompoundStatement*>(ifs.MutableThenBranch())->MutableStatements();
        if (then_stmts.size() == 1) {
          return extract_loop_body(then_stmts[0].get());
        }
      }
    }
    return nullptr;
  };

  std::function<void(CompoundStatement*, std::string_view)> rewrite_loop_continue_gotos =
      [&](CompoundStatement* cur, std::string_view cont_label) {
        if (cur == nullptr) return;
        auto& stmts = cur->MutableStatements();
        for (size_t i = 0; i < stmts.size(); ++i) {
          if (stmts[i] == nullptr) continue;
          if (stmts[i]->Kind() == CStatementKind::kGotoStatement &&
              static_cast<const GotoStatement&>(*stmts[i]).Label() == cont_label) {
            stmts[i] = CStatement::Continue();
          } else if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
            auto& ifs = *static_cast<IfStatement*>(stmts[i].get());
            if (ifs.MutableThenBranch() != nullptr &&
                ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
              rewrite_loop_continue_gotos(static_cast<CompoundStatement*>(ifs.MutableThenBranch()),
                                          cont_label);
            }
            if (ifs.MutableElseBranch() != nullptr &&
                ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
              rewrite_loop_continue_gotos(static_cast<CompoundStatement*>(ifs.MutableElseBranch()),
                                          cont_label);
            }
          } else if (stmts[i]->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_loop_continue_gotos(static_cast<CompoundStatement*>(stmts[i].get()),
                                        cont_label);
          } else if (stmts[i]->Kind() == CStatementKind::kSwitchStatement) {
            for (auto& sc : static_cast<SwitchStatement*>(stmts[i].get())->MutableCases()) {
              rewrite_loop_continue_gotos(sc.body.get(), cont_label);
            }
          }
        }
      };

  std::function<void(CompoundStatement*, std::string_view, std::string_view, std::string_view)>
      rewrite_loop_exit_gotos = [&](CompoundStatement* cur, std::string_view exit_label,
                                    std::string_view post_lhs, std::string_view post_rhs) {
        if (cur == nullptr) return;
        auto& stmts = cur->MutableStatements();
        if (post_lhs.empty()) {
          if (!stmts.empty() && stmts.back() != nullptr &&
              stmts.back()->Kind() == CStatementKind::kGotoStatement &&
              static_cast<const GotoStatement&>(*stmts.back()).Label() == exit_label) {
            stmts.back() = CStatement::Break();
          }
        } else if (stmts.size() >= 2 && stmts.back() != nullptr &&
                   stmts.back()->Kind() == CStatementKind::kGotoStatement &&
                   static_cast<const GotoStatement&>(*stmts.back()).Label() == exit_label) {
          std::string cand_lhs;
          std::string cand_rhs;
          const CExpression* cand_rhs_expr = nullptr;
          if (stmts[stmts.size() - 2] != nullptr &&
              extract_sinkable_assign(*stmts[stmts.size() - 2], &cand_lhs, &cand_rhs,
                                      &cand_rhs_expr) &&
              cand_lhs == post_lhs && cand_rhs == post_rhs) {
            stmts.pop_back();
            stmts.back() = CStatement::Break();
          }
        }
        for (size_t i = 0; i < stmts.size(); ++i) {
          if (stmts[i] == nullptr) continue;
          if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
            auto& ifs = *static_cast<IfStatement*>(stmts[i].get());
            if (ifs.MutableThenBranch() != nullptr &&
                ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
              rewrite_loop_exit_gotos(static_cast<CompoundStatement*>(ifs.MutableThenBranch()),
                                      exit_label, post_lhs, post_rhs);
            }
            if (ifs.MutableElseBranch() != nullptr &&
                ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
              rewrite_loop_exit_gotos(static_cast<CompoundStatement*>(ifs.MutableElseBranch()),
                                      exit_label, post_lhs, post_rhs);
            }
          } else if (stmts[i]->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_loop_exit_gotos(static_cast<CompoundStatement*>(stmts[i].get()), exit_label,
                                    post_lhs, post_rhs);
          }
        }
      };

  std::function<void(CompoundStatement*)> rewrite_loops_in_comp = [&](CompoundStatement* cur_comp) {
    if (cur_comp == nullptr) return;
    auto& stmts = cur_comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) continue;
      ForEachSubCompoundStatement(stmts[i].get(), rewrite_loops_in_comp);
      if (auto* direct_loop = extract_loop_body(stmts[i].get()); direct_loop != nullptr) {
        const auto& l_stmts = direct_loop->Statements();
        if (!l_stmts.empty() && l_stmts.back() != nullptr &&
            l_stmts.back()->Kind() == CStatementKind::kLabelStatement) {
          std::string cont_lbl(static_cast<const LabelStatement&>(*l_stmts.back()).Label());
          rewrite_loop_continue_gotos(direct_loop, cont_lbl);
        }
      }
      if (auto* loop_comp = extract_guarded_or_direct_loop_body(stmts[i].get());
          loop_comp != nullptr) {
        if (i + 1 < stmts.size() && stmts[i + 1] != nullptr &&
            stmts[i + 1]->Kind() == CStatementKind::kLabelStatement) {
          std::string exit_lbl(static_cast<const LabelStatement&>(*stmts[i + 1]).Label());
          rewrite_loop_exit_gotos(loop_comp, exit_lbl, "", "");
        }
      }
    }
  };
  rewrite_loops_in_comp(comp);

  PruneUnusedGotoLabels(comp);
}

bool StatementContainsContinue(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kContinueStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr && StatementContainsContinue(*s)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (StatementContainsContinue(ifs.ThenBranch())) return true;
      if (ifs.ElseBranch() != nullptr && StatementContainsContinue(*ifs.ElseBranch())) return true;
      return false;
    }
    default:
      return false;
  }
}

struct PreheaderGivInfo {
  std::string var;
  const CExpression* init_expr = nullptr;
  int inc_idx = -1;
  int64_t stride = 0;
};

bool TrySynthesizeGuardedForLoop(const CExpression& guard_cond, const CompoundStatement& then_body,
                                 const CompoundStatement& else_body,
                                 const std::vector<std::unique_ptr<CStatement>>& enclosing_stmts,
                                 size_t current_stmt_idx, CompoundStatement* target_block) {
  if (target_block == nullptr || !else_body.Statements().empty() ||
      then_body.Statements().empty() || then_body.Statements().back() == nullptr ||
      then_body.Statements().back()->Kind() != CStatementKind::kDoWhileStatement) {
    return false;
  }
  const DoWhileStatement* do_while =
      static_cast<const DoWhileStatement*>(then_body.Statements().back().get());
  std::vector<PreheaderGivInfo> preheader_givs;
  absl::flat_hash_set<std::string> preheader_giv_vars;
  for (size_t gi = 0; gi + 1 < then_body.Statements().size(); ++gi) {
    if (then_body.Statements()[gi] == nullptr) return false;
    std::string giv_lhs;
    const CExpression* giv_rhs = nullptr;
    if (!ExtractSimpleVarAssign(*then_body.Statements()[gi], &giv_lhs, &giv_rhs) ||
        giv_rhs == nullptr || !IsPureExpression(*giv_rhs) ||
        ExpressionUsesVariable(*giv_rhs, giv_lhs) || preheader_giv_vars.contains(giv_lhs) ||
        !(IsScratchGprOrArgVar(giv_lhs) || giv_lhs == "at" || giv_lhs == "temp_at")) {
      return false;
    }
    for (const auto& prev_giv : preheader_givs) {
      if (ExpressionUsesVariable(*giv_rhs, prev_giv.var)) {
        return false;
      }
    }
    for (size_t after = current_stmt_idx + 1; after < enclosing_stmts.size(); ++after) {
      if (enclosing_stmts[after] != nullptr &&
          StatementReferencesVariable(*enclosing_stmts[after], giv_lhs)) {
        return false;
      }
    }
    preheader_giv_vars.insert(giv_lhs);
    preheader_givs.push_back(PreheaderGivInfo{.var = std::move(giv_lhs), .init_expr = giv_rhs});
  }
  const auto& loop_cond = do_while->Condition();
  if (loop_cond.Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  const auto& orig_bin_cond = static_cast<const BinaryExpression&>(loop_cond);
  const auto& loop_body_stmt = do_while->Body();
  if (loop_body_stmt.Kind() != CStatementKind::kCompoundStatement) {
    return false;
  }
  const auto& loop_compound = static_cast<const CompoundStatement&>(loop_body_stmt);
  std::vector<std::unique_ptr<BinaryExpression>> cond_candidates;
  cond_candidates.push_back(std::make_unique<BinaryExpression>(
      orig_bin_cond.Op(), orig_bin_cond.Lhs().Clone(), orig_bin_cond.Rhs().Clone()));
  if (auto comm_op = CommutedBinaryOperator(orig_bin_cond.Op()); comm_op.has_value()) {
    cond_candidates.push_back(std::make_unique<BinaryExpression>(
        *comm_op, orig_bin_cond.Rhs().Clone(), orig_bin_cond.Lhs().Clone()));
  }

  for (const auto& candidate_cond : cond_candidates) {
    const BinaryExpression& bin_cond = *candidate_cond;
    std::string_view loop_var_view = ExtractForLoopConditionVar(bin_cond);
    if (loop_var_view.empty() || preheader_giv_vars.contains(std::string(loop_var_view))) continue;
    std::string loop_var(loop_var_view);

    int inc_idx = -1;
    std::optional<ForLoopStepMatch> step_match;
    for (int i = static_cast<int>(loop_compound.Statements().size()) - 1; i >= 0; --i) {
      auto candidate = TryBuildForLoopStep(*loop_compound.Statements()[i], loop_var);
      if (candidate.has_value() &&
          IsStepDirectionCompatibleWithCondition(bin_cond.Op(), candidate->direction)) {
        inc_idx = i;
        step_match = std::move(candidate);
        break;
      }
    }

    std::vector<PreheaderGivInfo> givs = preheader_givs;
    bool step_placement_valid = (inc_idx >= 0);
    for (size_t i = 0; step_placement_valid && i < loop_compound.Statements().size(); ++i) {
      if (static_cast<int>(i) == inc_idx) continue;
      const CStatement& s = *loop_compound.Statements()[i];
      if (IsRedundantLatchComparisonAssignment(s, loop_cond) ||
          IsRedundantLatchComparisonAssignment(s, bin_cond)) {
        continue;
      }
      if (!givs.empty() && static_cast<int>(i) > inc_idx) {
        std::string s_lhs;
        const CExpression* s_rhs = nullptr;
        bool matched_giv_step = false;
        if (ExtractSimpleVarAssign(s, &s_lhs, &s_rhs) && s_rhs != nullptr &&
            s_rhs->Kind() == CExpressionKind::kBinaryExpression) {
          const auto& b = static_cast<const BinaryExpression&>(*s_rhs);
          if (b.Op() == "+" && b.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(b.Lhs()).Name() == s_lhs &&
              b.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
              static_cast<const IntegerLiteralExpression&>(b.Rhs()).Value() > 1) {
            for (auto& g : givs) {
              if (g.var == s_lhs && g.inc_idx < 0) {
                g.inc_idx = static_cast<int>(i);
                g.stride = static_cast<const IntegerLiteralExpression&>(b.Rhs()).Value();
                matched_giv_step = true;
                break;
              }
            }
          }
        }
        if (matched_giv_step) continue;
      }
      if (!givs.empty()) {
        if (StatementContainsContinue(s)) {
          step_placement_valid = false;
          break;
        }
        for (const auto& g : givs) {
          if (StatementModifiesVariable(s, g.var)) {
            step_placement_valid = false;
            break;
          }
        }
        if (!step_placement_valid) break;
      }
      if (StatementModifiesVariable(s, loop_var)) {
        step_placement_valid = false;
        break;
      }
      if (static_cast<int>(i) > inc_idx &&
          (StatementReferencesVariable(s, loop_var) || StatementContainsCall(s))) {
        step_placement_valid = false;
        break;
      }
    }
    if (!givs.empty()) {
      if (!step_match.has_value() || step_match->direction != 1 ||
          step_match->step_expr->ToString() != (loop_var + "++")) {
        step_placement_valid = false;
      }
      for (const auto& g : givs) {
        if (g.inc_idx < 0 || g.stride <= 1) {
          step_placement_valid = false;
          break;
        }
      }
    }

    int init_idx = -1;
    const CExpression* init_rhs = nullptr;
    for (int si = static_cast<int>(target_block->Statements().size()) - 1; si >= 0; --si) {
      const CExpression* rhs =
          MatchSimpleInitializerAssignment(*target_block->Statements()[si], loop_var);
      if (rhs != nullptr) {
        init_idx = si;
        init_rhs = rhs;
        break;
      }
    }
    if (!givs.empty()) {
      if (init_rhs == nullptr || init_rhs->Kind() != CExpressionKind::kIntegerLiteral) {
        continue;
      }
      int64_t init_val = static_cast<const IntegerLiteralExpression&>(*init_rhs).Value();
      bool giv_inits_ok = true;
      for (const auto& g : givs) {
        if (g.init_expr == nullptr || ExpressionUsesVariable(*g.init_expr, loop_var)) {
          giv_inits_ok = false;
          break;
        }
        if (init_val != 0 && g.init_expr->Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(*g.init_expr).Value() !=
                init_val * g.stride) {
          giv_inits_ok = false;
          break;
        }
        std::vector<std::string> giv_init_vars;
        absl::flat_hash_set<std::string> giv_init_seen;
        CollectVariablesInExpression(*g.init_expr, &giv_init_vars, &giv_init_seen);
        for (const auto& gv : giv_init_vars) {
          if (StatementModifiesVariable(loop_compound, gv)) {
            giv_inits_ok = false;
            break;
          }
        }
        if (!giv_inits_ok) break;
      }
      if (!giv_inits_ok) continue;
    }

    bool guard_compatible =
        (init_idx >= 0 && init_rhs != nullptr &&
         IsCompatibleForLoopEntryGuard(guard_cond, bin_cond, loop_var, *init_rhs));
    if (!guard_compatible && init_idx >= 0 && init_rhs != nullptr) {
      std::unique_ptr<CExpression> normalized_guard = guard_cond.Clone();
      for (int si = static_cast<int>(target_block->Statements().size()) - 1; si >= 0; --si) {
        if (si == init_idx || target_block->Statements()[si] == nullptr) continue;
        std::string other_var;
        const CExpression* other_rhs = nullptr;
        if (!ExtractSimpleVarAssign(*target_block->Statements()[si], &other_var, &other_rhs) ||
            other_var == loop_var || other_rhs == nullptr ||
            !ExpressionsStructurallyEqual(*other_rhs, *init_rhs)) {
          continue;
        }
        bool modified_after = false;
        for (size_t k = static_cast<size_t>(si + 1); k < target_block->Statements().size(); ++k) {
          if (target_block->Statements()[k] != nullptr &&
              StatementModifiesVariable(*target_block->Statements()[k], other_var)) {
            modified_after = true;
            break;
          }
        }
        if (!modified_after) {
          normalized_guard = SubstituteIdentifier(*normalized_guard, other_var, *init_rhs);
        }
      }
      guard_compatible =
          IsCompatibleForLoopEntryGuard(*normalized_guard, bin_cond, loop_var, *init_rhs);
    }

    bool init_placement_valid = guard_compatible;
    if (init_placement_valid) {
      const bool is_callee_saved_loop_var =
          absl::StartsWith(loop_var, "temp_s") || loop_var == "temp_fp";
      std::vector<std::string> init_vars;
      absl::flat_hash_set<std::string> init_seen;
      CollectVariablesInExpression(*init_rhs, &init_vars, &init_seen);
      for (size_t k = static_cast<size_t>(init_idx + 1); k < target_block->Statements().size();
           ++k) {
        const CStatement& after_s = *target_block->Statements()[k];
        if (IsRedundantLatchComparisonAssignment(after_s, guard_cond) ||
            IsRedundantLatchComparisonAssignment(after_s, loop_cond) ||
            IsRedundantLatchComparisonAssignment(after_s, bin_cond)) {
          continue;
        }
        if (is_callee_saved_loop_var || StatementReferencesVariable(after_s, loop_var)) {
          init_placement_valid = false;
          break;
        }
        for (const auto& iv : init_vars) {
          if (StatementModifiesVariable(after_s, iv)) {
            init_placement_valid = false;
            break;
          }
        }
        if (!init_placement_valid) break;
      }
    }

    if (step_placement_valid && init_placement_valid) {
      auto init_rhs_clone = init_rhs->Clone();
      auto for_body = std::make_unique<CompoundStatement>();
      std::vector<std::pair<std::string, std::unique_ptr<CExpression>>> giv_replacements;
      absl::flat_hash_set<int> giv_inc_indices;
      for (const auto& g : givs) {
        giv_inc_indices.insert(g.inc_idx);
        const int64_t init_val = static_cast<const IntegerLiteralExpression&>(*init_rhs).Value();
        std::unique_ptr<CExpression> iter_count =
            (init_val != 0 && g.init_expr->Kind() != CExpressionKind::kIntegerLiteral)
                ? CExpression::Binary("-", CExpression::Identifier(loop_var),
                                      CExpression::Integer(init_val))
                : CExpression::Identifier(loop_var);
        auto scaled_loop_var =
            CExpression::Binary("*", std::move(iter_count), CExpression::Integer(g.stride));
        std::unique_ptr<CExpression> rep;
        if (g.init_expr != nullptr &&
            (g.init_expr->Kind() != CExpressionKind::kIntegerLiteral ||
             (init_val == 0 &&
              static_cast<const IntegerLiteralExpression&>(*g.init_expr).Value() != 0))) {
          rep = CExpression::Binary("+", g.init_expr->Clone(), std::move(scaled_loop_var));
        } else {
          rep = std::move(scaled_loop_var);
        }
        giv_replacements.emplace_back(g.var, std::move(rep));
      }
      for (size_t i = 0; i < loop_compound.Statements().size(); ++i) {
        if (static_cast<int>(i) == inc_idx || giv_inc_indices.contains(static_cast<int>(i))) {
          continue;
        }
        if (IsRedundantLatchComparisonAssignment(*loop_compound.Statements()[i], loop_cond) ||
            IsRedundantLatchComparisonAssignment(*loop_compound.Statements()[i], bin_cond)) {
          continue;
        }
        auto cloned_stmt = loop_compound.Statements()[i]->Clone();
        for (const auto& [gvar, grep] : giv_replacements) {
          SubstituteExpressionInStatementMutator mutator(gvar, *grep);
          mutator.MutateStatement(&cloned_stmt);
        }
        for_body->AddStatement(std::move(cloned_stmt));
      }

      auto& target_stmts = target_block->MutableStatements();
      for (int k = static_cast<int>(target_stmts.size()) - 1; k > init_idx; --k) {
        if (target_stmts[k] == nullptr) continue;
        if (IsRedundantLatchComparisonAssignment(*target_stmts[k], guard_cond) ||
            IsRedundantLatchComparisonAssignment(*target_stmts[k], loop_cond) ||
            IsRedundantLatchComparisonAssignment(*target_stmts[k], bin_cond)) {
          std::string latch_lhs;
          const CExpression* latch_rhs = nullptr;
          if (ExtractSimpleVarAssign(*target_stmts[k], &latch_lhs, &latch_rhs) &&
              !StatementReferencesVariable(*for_body, latch_lhs)) {
            bool used_after_if = false;
            for (size_t after = current_stmt_idx + 1; after < enclosing_stmts.size(); ++after) {
              if (enclosing_stmts[after] != nullptr &&
                  StatementReferencesVariable(*enclosing_stmts[after], latch_lhs)) {
                used_after_if = true;
                break;
              }
            }
            if (!used_after_if) {
              target_stmts.erase(target_stmts.begin() + k);
            }
          }
        }
      }
      target_stmts.erase(target_stmts.begin() + init_idx);

      auto for_init = CStatement::Expression(CExpression::Assignment(
          "=", CExpression::Identifier(loop_var), std::move(init_rhs_clone)));
      target_block->AddStatement(CStatement::For(std::move(for_init), bin_cond.Clone(),
                                                 std::move(step_match->step_expr),
                                                 std::move(for_body)));
      return true;
    }
  }
  return false;
}

bool TrySynthesizeGuardedForLoop(const CExpression& guard_cond, const CompoundStatement& then_body,
                                 const CompoundStatement& else_body,
                                 CompoundStatement* target_block) {
  std::vector<std::unique_ptr<CStatement>> empty_enclosing;
  return TrySynthesizeGuardedForLoop(guard_cond, then_body, else_body, empty_enclosing, 0,
                                     target_block);
}

bool TrySynthesizeBranchGuardedWhileLoop(
    const absl::flat_hash_set<std::string>& parent_zero_vars, CompoundStatement* branch_comp,
    const std::vector<std::unique_ptr<CStatement>>& outer_enclosing_stmts, size_t outer_stmt_idx) {
  if (branch_comp == nullptr || parent_zero_vars.empty()) return false;
  auto& b_stmts = branch_comp->MutableStatements();
  size_t if_idx = 0;
  if (b_stmts.size() >= 2 && b_stmts[0] != nullptr && b_stmts[1] != nullptr &&
      b_stmts[1]->Kind() == CStatementKind::kIfStatement) {
    const auto& cand_if = static_cast<const IfStatement&>(*b_stmts[1]);
    if (IsRedundantLatchComparisonAssignment(*b_stmts[0], cand_if.Condition())) {
      if_idx = 1;
    }
  }
  if (if_idx >= b_stmts.size() || b_stmts[if_idx] == nullptr ||
      b_stmts[if_idx]->Kind() != CStatementKind::kIfStatement) {
    return false;
  }
  const auto& ifs = static_cast<const IfStatement&>(*b_stmts[if_idx]);
  if (ifs.ElseBranch() != nullptr ||
      ifs.ThenBranch().Kind() != CStatementKind::kCompoundStatement) {
    return false;
  }
  const auto& then_body = static_cast<const CompoundStatement&>(ifs.ThenBranch());
  if (then_body.Statements().empty() || then_body.Statements().back() == nullptr ||
      then_body.Statements().back()->Kind() != CStatementKind::kDoWhileStatement) {
    return false;
  }
  const DoWhileStatement* do_while =
      static_cast<const DoWhileStatement*>(then_body.Statements().back().get());
  std::vector<PreheaderGivInfo> givs;
  absl::flat_hash_set<std::string> giv_vars;
  for (size_t gi = 0; gi + 1 < then_body.Statements().size(); ++gi) {
    if (then_body.Statements()[gi] == nullptr) return false;
    std::string giv_lhs;
    const CExpression* giv_rhs = nullptr;
    if (!ExtractSimpleVarAssign(*then_body.Statements()[gi], &giv_lhs, &giv_rhs) ||
        giv_rhs == nullptr || !IsPureExpression(*giv_rhs) ||
        ExpressionUsesVariable(*giv_rhs, giv_lhs) || giv_vars.contains(giv_lhs) ||
        !(IsScratchGprOrArgVar(giv_lhs) || giv_lhs == "at" || giv_lhs == "temp_at")) {
      return false;
    }
    for (const auto& prev_giv : givs) {
      if (ExpressionUsesVariable(*giv_rhs, prev_giv.var)) return false;
    }
    for (size_t after = if_idx + 1; after < b_stmts.size(); ++after) {
      if (b_stmts[after] != nullptr && StatementReferencesVariable(*b_stmts[after], giv_lhs)) {
        return false;
      }
    }
    for (size_t after = outer_stmt_idx + 1; after < outer_enclosing_stmts.size(); ++after) {
      if (outer_enclosing_stmts[after] != nullptr &&
          StatementReferencesVariable(*outer_enclosing_stmts[after], giv_lhs)) {
        return false;
      }
    }
    giv_vars.insert(giv_lhs);
    givs.push_back(PreheaderGivInfo{.var = std::move(giv_lhs), .init_expr = giv_rhs});
  }

  const auto& loop_cond = do_while->Condition();
  if (loop_cond.Kind() != CExpressionKind::kBinaryExpression ||
      do_while->Body().Kind() != CStatementKind::kCompoundStatement) {
    return false;
  }
  const auto& bin_cond = static_cast<const BinaryExpression&>(loop_cond);
  std::string_view loop_var_sv = ExtractForLoopConditionVar(bin_cond);
  if (loop_var_sv.empty() || !parent_zero_vars.contains(std::string(loop_var_sv)) ||
      giv_vars.contains(std::string(loop_var_sv))) {
    return false;
  }
  std::string loop_var(loop_var_sv);

  std::unique_ptr<CExpression> normalized_guard = ifs.Condition().Clone();
  for (const auto& zv : parent_zero_vars) {
    normalized_guard = SubstituteIdentifier(*normalized_guard, zv, *CExpression::Integer(0));
  }
  if (!IsCompatibleForLoopEntryGuard(*normalized_guard, bin_cond, loop_var,
                                     *CExpression::Integer(0))) {
    return false;
  }

  const auto& loop_compound = static_cast<const CompoundStatement&>(do_while->Body());
  int inc_idx = -1;
  std::optional<ForLoopStepMatch> step_match;
  for (int i = static_cast<int>(loop_compound.Statements().size()) - 1; i >= 0; --i) {
    auto candidate = TryBuildForLoopStep(*loop_compound.Statements()[i], loop_var);
    if (candidate.has_value() &&
        IsStepDirectionCompatibleWithCondition(bin_cond.Op(), candidate->direction)) {
      inc_idx = i;
      step_match = std::move(candidate);
      break;
    }
  }
  if (inc_idx < 0) return false;

  for (size_t i = 0; i < loop_compound.Statements().size(); ++i) {
    if (static_cast<int>(i) == inc_idx) continue;
    const CStatement& s = *loop_compound.Statements()[i];
    if (IsRedundantLatchComparisonAssignment(s, loop_cond)) continue;
    if (!givs.empty() && static_cast<int>(i) > inc_idx) {
      std::string s_lhs;
      const CExpression* s_rhs = nullptr;
      bool matched_giv_step = false;
      if (ExtractSimpleVarAssign(s, &s_lhs, &s_rhs) && s_rhs != nullptr &&
          s_rhs->Kind() == CExpressionKind::kBinaryExpression) {
        const auto& b = static_cast<const BinaryExpression&>(*s_rhs);
        if (b.Op() == "+" && b.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(b.Lhs()).Name() == s_lhs &&
            b.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(b.Rhs()).Value() > 1) {
          for (auto& g : givs) {
            if (g.var == s_lhs && g.inc_idx < 0) {
              g.inc_idx = static_cast<int>(i);
              g.stride = static_cast<const IntegerLiteralExpression&>(b.Rhs()).Value();
              matched_giv_step = true;
              break;
            }
          }
        }
      }
      if (matched_giv_step) continue;
    }
    if (!givs.empty()) {
      if (StatementContainsContinue(s)) return false;
      for (const auto& g : givs) {
        if (StatementModifiesVariable(s, g.var)) return false;
      }
    }
    if (StatementModifiesVariable(s, loop_var) ||
        (static_cast<int>(i) > inc_idx &&
         (StatementReferencesVariable(s, loop_var) || StatementContainsCall(s)))) {
      return false;
    }
  }
  if (!givs.empty()) {
    if (!step_match.has_value() || step_match->direction != 1 ||
        step_match->step_expr->ToString() != (loop_var + "++")) {
      return false;
    }
    for (const auto& g : givs) {
      if (g.inc_idx < 0 || g.stride <= 1 || g.init_expr == nullptr ||
          ExpressionUsesVariable(*g.init_expr, loop_var)) {
        return false;
      }
      std::vector<std::string> giv_init_vars;
      absl::flat_hash_set<std::string> giv_init_seen;
      CollectVariablesInExpression(*g.init_expr, &giv_init_vars, &giv_init_seen);
      for (const auto& gv : giv_init_vars) {
        if (StatementModifiesVariable(loop_compound, gv)) return false;
      }
    }
  }

  auto while_body = std::make_unique<CompoundStatement>();
  std::vector<std::pair<std::string, std::unique_ptr<CExpression>>> giv_replacements;
  absl::flat_hash_set<int> giv_inc_indices;
  for (const auto& g : givs) {
    giv_inc_indices.insert(g.inc_idx);
    auto scaled_loop_var =
        CExpression::Binary("*", CExpression::Identifier(loop_var), CExpression::Integer(g.stride));
    std::unique_ptr<CExpression> rep;
    if (g.init_expr != nullptr &&
        (g.init_expr->Kind() != CExpressionKind::kIntegerLiteral ||
         static_cast<const IntegerLiteralExpression&>(*g.init_expr).Value() != 0)) {
      rep = CExpression::Binary("+", g.init_expr->Clone(), std::move(scaled_loop_var));
    } else {
      rep = std::move(scaled_loop_var);
    }
    giv_replacements.emplace_back(g.var, std::move(rep));
  }
  for (size_t i = 0; i < loop_compound.Statements().size(); ++i) {
    if (giv_inc_indices.contains(static_cast<int>(i))) continue;
    if (IsRedundantLatchComparisonAssignment(*loop_compound.Statements()[i], loop_cond)) continue;
    auto cloned_stmt = loop_compound.Statements()[i]->Clone();
    for (const auto& [gvar, grep] : giv_replacements) {
      SubstituteExpressionInStatementMutator mutator(gvar, *grep);
      mutator.MutateStatement(&cloned_stmt);
    }
    while_body->AddStatement(std::move(cloned_stmt));
  }

  b_stmts[if_idx] = CStatement::While(bin_cond.Clone(), std::move(while_body));
  if (if_idx == 1) {
    std::string latch_lhs;
    const CExpression* latch_rhs = nullptr;
    if (ExtractSimpleVarAssign(*b_stmts[0], &latch_lhs, &latch_rhs) &&
        !StatementReferencesVariable(*b_stmts[1], latch_lhs)) {
      bool reads_before_kill = false;
      bool killed = false;
      for (size_t after = 2; after < b_stmts.size(); ++after) {
        if (b_stmts[after] == nullptr) continue;
        if (StatementDefinitelyKillsVarBeforeRead(*b_stmts[after], latch_lhs, &reads_before_kill)) {
          killed = true;
          break;
        }
        if (reads_before_kill) break;
      }
      if (!reads_before_kill && killed) {
        b_stmts.erase(b_stmts.begin());
      }
    }
  }
  return true;
}

bool SynthesizeForLoopsInBlock(CompoundStatement* body) {
  if (body == nullptr) return false;
  bool any_changed = false;
  for (auto& stmt : body->MutableStatements()) {
    if (stmt == nullptr) continue;
    ForEachSubCompoundStatement(stmt.get(), [&](CompoundStatement* sub) {
      if (SynthesizeForLoopsInBlock(sub)) {
        any_changed = true;
      }
    });
  }

  CompoundStatement rebuilt;
  CompoundStatement empty_else;
  bool local_changed = false;
  const auto& stmts = body->Statements();
  for (size_t idx = 0; idx < stmts.size(); ++idx) {
    const auto& stmt = stmts[idx];
    if (stmt == nullptr) continue;
    if (stmt->Kind() == CStatementKind::kIfStatement) {
      const auto& ifs = static_cast<const IfStatement&>(*stmt);
      const CompoundStatement* then_comp =
          ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement
              ? static_cast<const CompoundStatement*>(&ifs.ThenBranch())
              : nullptr;
      const CompoundStatement* else_comp =
          (ifs.ElseBranch() != nullptr &&
           ifs.ElseBranch()->Kind() == CStatementKind::kCompoundStatement)
              ? static_cast<const CompoundStatement*>(ifs.ElseBranch())
              : &empty_else;
      if (ifs.ElseBranch() == nullptr && then_comp != nullptr &&
          TrySynthesizeGuardedForLoop(ifs.Condition(), *then_comp, *else_comp, stmts, idx,
                                      &rebuilt)) {
        local_changed = true;
        continue;
      }
      absl::flat_hash_set<std::string> zero_vars;
      for (int si = static_cast<int>(rebuilt.Statements().size()) - 1; si >= 0; --si) {
        std::string z_lhs;
        const CExpression* z_rhs = nullptr;
        if (!ExtractSimpleVarAssign(*rebuilt.Statements()[si], &z_lhs, &z_rhs) ||
            z_rhs == nullptr || z_rhs->Kind() != CExpressionKind::kIntegerLiteral ||
            static_cast<const IntegerLiteralExpression&>(*z_rhs).Value() != 0) {
          continue;
        }
        bool modified_after = false;
        for (size_t k = static_cast<size_t>(si + 1); k < rebuilt.Statements().size(); ++k) {
          if (StatementModifiesVariable(*rebuilt.Statements()[k], z_lhs)) {
            modified_after = true;
            break;
          }
        }
        if (!modified_after) {
          zero_vars.insert(std::move(z_lhs));
        }
      }
      if (!zero_vars.empty()) {
        auto cloned_if = stmt->Clone();
        auto* mut_if = static_cast<IfStatement*>(cloned_if.get());
        bool branch_changed = false;
        if (mut_if->MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
            TrySynthesizeBranchGuardedWhileLoop(
                zero_vars, static_cast<CompoundStatement*>(mut_if->MutableThenBranch()), stmts,
                idx)) {
          branch_changed = true;
        }
        if (mut_if->MutableElseBranch() != nullptr &&
            mut_if->MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
            TrySynthesizeBranchGuardedWhileLoop(
                zero_vars, static_cast<CompoundStatement*>(mut_if->MutableElseBranch()), stmts,
                idx)) {
          branch_changed = true;
        }
        if (branch_changed) {
          rebuilt.AddStatement(std::move(cloned_if));
          local_changed = true;
          continue;
        }
      }
    }
    rebuilt.AddStatement(stmt->Clone());
  }
  if (local_changed) {
    *body = std::move(rebuilt);
    any_changed = true;
  }
  return any_changed;
}

bool SinkLoopInvariantAssignments(CompoundStatement* body) {
  if (body == nullptr) return false;
  bool any_changed = false;

  std::function<void(CompoundStatement*)> process_block = [&](CompoundStatement* comp) {
    if (comp == nullptr) return;
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) continue;
      ForEachSubCompoundStatement(stmts[i].get(),
                                  [&](CompoundStatement* sub) { process_block(sub); });
    }

    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) continue;

      // 1. Fold trailing `reg = (cond); } while ((reg != 0));` on outer/nested do-while loops when
      // `reg` is not used after the loop and is killed before read on loop entry.
      if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement) {
        auto* dw = static_cast<DoWhileStatement*>(stmts[i].get());
        if (FoldDoWhileTrailingLatchComparison(dw, stmts, i, body)) {
          any_changed = true;
        }
      } else if (stmts[i]->Kind() == CStatementKind::kWhileStatement) {
        auto* ws = static_cast<WhileStatement*>(stmts[i].get());
        if (FoldWhileTrueLeadingBreakCondition(ws, stmts, i)) {
          any_changed = true;
        }
      }

      // Extract direct loop (`do-while`, `while`, `for`) or single-loop guarded `if (guard) { loop
      // }`.
      CStatement* loop_stmt = nullptr;
      CExpression* guard_cond = nullptr;
      if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement ||
          stmts[i]->Kind() == CStatementKind::kWhileStatement ||
          stmts[i]->Kind() == CStatementKind::kForStatement) {
        loop_stmt = stmts[i].get();
      } else if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
        auto* ifs = static_cast<IfStatement*>(stmts[i].get());
        if (ifs->ElseBranch() == nullptr &&
            ifs->ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
          auto* then_comp = static_cast<CompoundStatement*>(ifs->MutableThenBranch());
          if (then_comp->Statements().size() == 1 && then_comp->Statements()[0] != nullptr) {
            CStatementKind tk = then_comp->Statements()[0]->Kind();
            if (tk == CStatementKind::kDoWhileStatement || tk == CStatementKind::kWhileStatement ||
                tk == CStatementKind::kForStatement) {
              loop_stmt = then_comp->MutableStatements()[0].get();
              guard_cond = ifs->MutableCondition().get();
            }
          }
        }
      }
      if (loop_stmt == nullptr) continue;

      // Scan preceding pure assignments `reg = rhs;` before `stmts[i]`.
      for (int scan = static_cast<int>(i) - 1; scan >= 0; --scan) {
        if (stmts[scan] == nullptr) break;
        std::string reg;
        const CExpression* rhs = nullptr;
        if (!ExtractSimpleVarAssign(*stmts[scan], &reg, &rhs) || rhs == nullptr) {
          break;
        }
        if (!IsScratchGprOrArgVar(reg)) {
          continue;
        }

        const bool is_int_const = (rhs->Kind() == CExpressionKind::kIntegerLiteral);
        const CExpression* sym_addr = nullptr;
        const bool is_sym_addr = ExtractSymbolAddressOperand(*rhs, &sym_addr);
        const bool is_giv_mult =
            (rhs->Kind() == CExpressionKind::kBinaryExpression &&
             (static_cast<const BinaryExpression&>(*rhs).Op() == "*" ||
              static_cast<const BinaryExpression&>(*rhs).Op() == "<<") &&
             static_cast<const BinaryExpression&>(*rhs).Lhs().Kind() ==
                 CExpressionKind::kIdentifier &&
             static_cast<const BinaryExpression&>(*rhs).Rhs().Kind() ==
                 CExpressionKind::kIntegerLiteral &&
             !StatementModifiesVariable(*stmts[i],
                                        static_cast<const IdentifierExpression&>(
                                            static_cast<const BinaryExpression&>(*rhs).Lhs())
                                            .Name()));
        if (!is_int_const && !is_sym_addr && !is_giv_mult) {
          continue;
        }

        // Verify intervening statements `[scan + 1, i - 1]` are pure assignments that do not
        // reference `reg`, contain calls/labels/jumps, or modify `rhs`.
        bool intervening_ok = true;
        for (size_t k = static_cast<size_t>(scan + 1); k < i; ++k) {
          std::string mid_lhs;
          const CExpression* mid_rhs = nullptr;
          if (stmts[k] == nullptr || !ExtractSimpleVarAssign(*stmts[k], &mid_lhs, &mid_rhs) ||
              !IsPureExpression(*mid_rhs) || StatementContainsCall(*stmts[k]) ||
              StatementReferencesVariable(*stmts[k], reg)) {
            intervening_ok = false;
            break;
          }
        }
        if (!intervening_ok) continue;

        // `reg` must not be modified or address-taken in `stmts[i]` and must be dead after
        // `stmts[i]`.
        if (StatementModifiesVariable(*stmts[i], reg) || StatementTakesAddressOf(*stmts[i], reg)) {
          continue;
        }
        bool used_after = false;
        for (size_t after = i + 1; after < stmts.size(); ++after) {
          if (stmts[after] != nullptr && StatementReferencesVariable(*stmts[after], reg)) {
            used_after = true;
            break;
          }
        }
        if (used_after) continue;

        int total_reads = CountVariableReadUsesInStatement(*stmts[i], reg);
        if (total_reads == 0) {
          // Dead pre-loop constant/symbol-address assignment left over by constant propagation.
          stmts.erase(stmts.begin() + scan);
          --i;
          any_changed = true;
          continue;
        }

        // Case A: `reg = const;` where `reg` is read in the loop condition (or is a 32-bit `lui`
        // constant read inside a call-free loop body).
        if (is_int_const) {
          CExpression* loop_cond_ptr = nullptr;
          const CStatement* loop_body_ptr = nullptr;
          if (loop_stmt->Kind() == CStatementKind::kDoWhileStatement) {
            auto* dw = static_cast<DoWhileStatement*>(loop_stmt);
            loop_cond_ptr = dw->MutableCondition().get();
            loop_body_ptr = &dw->Body();
          } else if (loop_stmt->Kind() == CStatementKind::kWhileStatement) {
            auto* ws = static_cast<WhileStatement*>(loop_stmt);
            loop_cond_ptr = ws->MutableCondition().get();
            loop_body_ptr = &ws->Body();
          } else if (loop_stmt->Kind() == CStatementKind::kForStatement) {
            auto* fs = static_cast<ForStatement*>(loop_stmt);
            loop_cond_ptr = fs->MutableConditionPtr().get();
            loop_body_ptr = &fs->Body();
          }
          if (loop_cond_ptr != nullptr && loop_body_ptr != nullptr &&
              !StatementReferencesVariable(*loop_body_ptr, reg) &&
              ExpressionUsesVariable(*loop_cond_ptr, reg)) {
            auto replacement = rhs->Clone();
            if (loop_stmt->Kind() == CStatementKind::kDoWhileStatement) {
              auto* dw = static_cast<DoWhileStatement*>(loop_stmt);
              dw->MutableCondition() = SubstituteIdentifier(dw->Condition(), reg, *replacement);
            } else if (loop_stmt->Kind() == CStatementKind::kWhileStatement) {
              auto* ws = static_cast<WhileStatement*>(loop_stmt);
              ws->MutableCondition() = SubstituteIdentifier(ws->Condition(), reg, *replacement);
            } else if (loop_stmt->Kind() == CStatementKind::kForStatement) {
              auto* fs = static_cast<ForStatement*>(loop_stmt);
              fs->MutableConditionPtr() = SubstituteIdentifier(*fs->Condition(), reg, *replacement);
            }
            if (guard_cond != nullptr && ExpressionUsesVariable(*guard_cond, reg)) {
              auto* ifs = static_cast<IfStatement*>(stmts[i].get());
              ifs->MutableCondition() = SubstituteIdentifier(ifs->Condition(), reg, *replacement);
            }
            stmts.erase(stmts.begin() + scan);
            --i;
            any_changed = true;
            continue;
          }
          const int64_t int_val = static_cast<const IntegerLiteralExpression&>(*rhs).Value();
          if (!StatementContainsCall(*stmts[i]) && (int_val < -32768 || int_val > 65535)) {
            auto replacement = rhs->Clone();
            SubstituteExpressionInStatementMutator mutator(reg, *replacement);
            mutator.MutateStatement(&stmts[i]);
            if (!StatementReferencesVariable(*stmts[i], reg)) {
              stmts.erase(stmts.begin() + scan);
              --i;
              any_changed = true;
              continue;
            }
          }
        }

        // Case B: `reg = &D_...;` or `reg = outer_iv * stride;` used inside a call-free loop.
        if (((is_sym_addr && sym_addr != nullptr) || (is_giv_mult && total_reads == 1)) &&
            !StatementContainsCall(*stmts[i])) {
          auto replacement =
              is_sym_addr ? CExpression::Cast(CType::S32(), sym_addr->Clone()) : rhs->Clone();
          SubstituteExpressionInStatementMutator mutator(reg, *replacement);
          mutator.MutateStatement(&stmts[i]);
          if (!StatementReferencesVariable(*stmts[i], reg)) {
            stmts.erase(stmts.begin() + scan);
            --i;
            any_changed = true;
            continue;
          }
        }
      }
    }
  };

  process_block(body);
  return any_changed;
}

bool SwitchGotoCaseCollapserPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  return CollapseAliasSwitchGotoCases(body);
}

bool CrossJumpedCallTailInlinerPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  InlineCrossJumpedCallTails(body, allow_diamond_tails_);
  return body->ToString() != before;
}

bool CommonBranchTailSinkerPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  SinkCommonBranchTailArgAssignments(body);
  return body->ToString() != before;
}

bool GotoAndLabelCleanupPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  InlineCrossJumpedCallTails(body, /*allow_diamond_tails=*/false);
  SinkCommonBranchTailArgAssignments(body);
  if (allow_diamond_tails_) {
    InlineCrossJumpedCallTails(body, /*allow_diamond_tails=*/true);
    SinkCommonBranchTailArgAssignments(body);
  }
  return body->ToString() != before;
}

bool ForLoopSynthesizerPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  return SynthesizeForLoopsInBlock(body);
}

bool LoopInvariantSinkerPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  bool changed = SynthesizeForLoopsInBlock(body);
  if (SinkLoopInvariantAssignments(body)) {
    changed = true;
    if (SynthesizeForLoopsInBlock(body)) {
      SinkLoopInvariantAssignments(body);
    }
  }
  return changed;
}

namespace {

const CExpression* UnwrapSwitchControlExpression(const CExpression* expr) {
  while (expr != nullptr) {
    if (expr->Kind() == CExpressionKind::kCastExpression) {
      const auto* cast = static_cast<const CastExpression*>(expr);
      std::string t = cast->TargetType().ToString();
      if (t == "s32" || t == "u32") {
        expr = &cast->Operand();
      } else {
        break;
      }
    } else {
      break;
    }
  }
  return expr;
}

bool ExtractSignedConstantForSwitch(const CExpression& expr, int64_t* out_val) {
  const CExpression* cur = &expr;
  if (cur == nullptr) return false;
  if (cur->Kind() == CExpressionKind::kCastExpression) {
    const auto* cast = static_cast<const CastExpression*>(cur);
    std::string t = cast->TargetType().ToString();
    if (t == "s32" || t == "u32" || t == "s16" || t == "u16" || t == "s8" || t == "u8") {
      return ExtractSignedConstantForSwitch(cast->Operand(), out_val);
    }
    return false;
  }
  if (cur->Kind() == CExpressionKind::kIntegerLiteral) {
    *out_val = static_cast<int64_t>(
        static_cast<int32_t>(static_cast<const IntegerLiteralExpression*>(cur)->Value()));
    return true;
  }
  if (cur->Kind() == CExpressionKind::kUnaryExpression) {
    const auto* un = static_cast<const UnaryExpression*>(cur);
    int64_t inner = 0;
    if (un->Op() == "-" && ExtractSignedConstantForSwitch(un->Operand(), &inner)) {
      *out_val = -inner;
      return true;
    }
  }
  return false;
}

bool ContainsTopLevelBreakStatement(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kBreakStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      for (const auto& s : comp.Statements()) {
        if (s != nullptr && ContainsTopLevelBreakStatement(*s)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ContainsTopLevelBreakStatement(ifs.ThenBranch())) return true;
      if (ifs.ElseBranch() != nullptr && ContainsTopLevelBreakStatement(*ifs.ElseBranch())) {
        return true;
      }
      return false;
    }
    case CStatementKind::kWhileStatement:
    case CStatementKind::kDoWhileStatement:
    case CStatementKind::kForStatement:
    case CStatementKind::kSwitchStatement:
      return false;
    default:
      return false;
  }
}

bool StatementAlwaysTerminatesControlFlow(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kReturnStatement:
    case CStatementKind::kBreakStatement:
    case CStatementKind::kContinueStatement:
    case CStatementKind::kGotoStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      if (comp.Statements().empty() || comp.Statements().back() == nullptr) return false;
      return StatementAlwaysTerminatesControlFlow(*comp.Statements().back());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ifs.ElseBranch() == nullptr) return false;
      return StatementAlwaysTerminatesControlFlow(ifs.ThenBranch()) &&
             StatementAlwaysTerminatesControlFlow(*ifs.ElseBranch());
    }
    default:
      return false;
  }
}

std::unique_ptr<CompoundStatement> WrapSwitchCaseBodyWithBreak(const CStatement& stmt) {
  auto body = std::make_unique<CompoundStatement>();
  if (stmt.Kind() == CStatementKind::kCompoundStatement) {
    const auto& comp = static_cast<const CompoundStatement&>(stmt);
    for (const auto& s : comp.Statements()) {
      if (s != nullptr) body->AddStatement(s->Clone());
    }
  } else {
    body->AddStatement(stmt.Clone());
  }
  if (body->Statements().empty() ||
      !StatementAlwaysTerminatesControlFlow(*body->Statements().back())) {
    body->AddStatement(CStatement::Break());
  }
  return body;
}

struct SparseDecisionTreeState {
  std::unique_ptr<CExpression> control_expr;
  std::string control_expr_str;
  std::map<int64_t, std::unique_ptr<CompoundStatement>> cases;
  int range_pivot_count = 0;
  bool has_empty_default = false;
  std::unique_ptr<CompoundStatement> non_empty_default;

  bool MatchOrSetControlExpr(const CExpression& candidate) {
    const CExpression* unwrapped = UnwrapSwitchControlExpression(&candidate);
    if (unwrapped == nullptr || !IsPureExpression(*unwrapped)) return false;
    if (unwrapped->Kind() == CExpressionKind::kIntegerLiteral) return false;
    std::string s = unwrapped->ToString();
    if (control_expr == nullptr) {
      control_expr = unwrapped->Clone();
      control_expr_str = std::move(s);
      return true;
    }
    return control_expr_str == s;
  }

  bool RecordDefaultLeaf(const CStatement* default_stmt) {
    if (default_stmt == nullptr) {
      has_empty_default = true;
      return non_empty_default == nullptr;
    }
    if (default_stmt->Kind() == CStatementKind::kCompoundStatement &&
        static_cast<const CompoundStatement*>(default_stmt)->IsEmpty()) {
      has_empty_default = true;
      return non_empty_default == nullptr;
    }
    if (ContainsTopLevelBreakStatement(*default_stmt)) return false;
    auto wrapped = WrapSwitchCaseBodyWithBreak(*default_stmt);
    if (has_empty_default) return false;
    if (non_empty_default == nullptr) {
      non_empty_default = std::move(wrapped);
      return true;
    }
    return non_empty_default->ToString() == wrapped->ToString();
  }

  bool AddCase(int64_t val, const CStatement& case_stmt, std::optional<int64_t> low_bound,
               std::optional<int64_t> high_bound) {
    if (low_bound.has_value() && val < *low_bound) return false;
    if (high_bound.has_value() && val > *high_bound) return false;
    if (cases.count(val) > 0) return false;
    if (ContainsTopLevelBreakStatement(case_stmt)) return false;
    cases.emplace(val, WrapSwitchCaseBodyWithBreak(case_stmt));
    return true;
  }

  bool AddCaseRange(int64_t low_val, int64_t high_val, const CStatement& case_stmt,
                    std::optional<int64_t> low_bound, std::optional<int64_t> high_bound) {
    if (low_val > high_val || high_val - low_val > 16) return false;
    for (int64_t v = low_val; v <= high_val; ++v) {
      if (cases.count(v) > 0) return false;
    }
    for (int64_t v = low_val; v <= high_val; ++v) {
      if (!AddCase(v, case_stmt, low_bound, high_bound)) return false;
    }
    return true;
  }
};

std::string InvertComparisonOperator(std::string_view op) {
  if (op == "==") return "!=";
  if (op == "!=") return "==";
  if (op == "<") return ">=";
  if (op == "<=") return ">";
  if (op == ">") return "<=";
  if (op == ">=") return "<";
  return "";
}

std::string FlipComparisonDirection(std::string_view op) {
  if (op == "==" || op == "!=") return std::string(op);
  if (op == "<") return ">";
  if (op == "<=") return ">=";
  if (op == ">") return "<";
  if (op == ">=") return "<=";
  return "";
}

bool ExtractNormalizedSwitchComparison(const CExpression& raw_cond,
                                       const CExpression** out_lhs_expr, std::string* out_op,
                                       int64_t* out_const_val) {
  const CExpression* cur = &raw_cond;
  bool negated = false;
  while (cur != nullptr) {
    if (cur->Kind() == CExpressionKind::kUnaryExpression) {
      const auto* un = static_cast<const UnaryExpression*>(cur);
      if (un->Op() == "!") {
        negated = !negated;
        cur = &un->Operand();
        continue;
      }
    }
    if (cur->Kind() == CExpressionKind::kBinaryExpression) {
      const auto* bin = static_cast<const BinaryExpression*>(cur);
      int64_t zero_check = -1;
      if ((bin->Op() == "==" || bin->Op() == "!=") &&
          ExtractSignedConstantForSwitch(bin->Rhs(), &zero_check) && zero_check == 0) {
        const CExpression* inner = &bin->Lhs();
        if (inner->Kind() == CExpressionKind::kBinaryExpression) {
          const auto* inner_bin = static_cast<const BinaryExpression*>(inner);
          if (!InvertComparisonOperator(inner_bin->Op()).empty()) {
            if (bin->Op() == "==") negated = !negated;
            cur = inner;
            continue;
          }
        }
      }
    }
    break;
  }
  if (cur == nullptr || cur->Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto* bin = static_cast<const BinaryExpression*>(cur);
  std::string op = bin->Op();
  if (InvertComparisonOperator(op).empty()) return false;
  if (negated) {
    op = InvertComparisonOperator(op);
  }

  int64_t cval = 0;
  if (ExtractSignedConstantForSwitch(bin->Rhs(), &cval)) {
    *out_lhs_expr = &bin->Lhs();
    *out_op = op;
    *out_const_val = cval;
    return true;
  }
  if (ExtractSignedConstantForSwitch(bin->Lhs(), &cval)) {
    *out_lhs_expr = &bin->Rhs();
    *out_op = FlipComparisonDirection(op);
    *out_const_val = cval;
    return true;
  }
  return false;
}

bool ParseDecisionTreeNode(const CStatement* stmt, std::optional<int64_t> low_bound,
                           std::optional<int64_t> high_bound, SparseDecisionTreeState* state) {
  if (stmt == nullptr) {
    return state->RecordDefaultLeaf(nullptr);
  }

  if (stmt->Kind() == CStatementKind::kCompoundStatement) {
    const auto* comp = static_cast<const CompoundStatement*>(stmt);
    if (comp->IsEmpty()) {
      return state->RecordDefaultLeaf(nullptr);
    }
    if (comp->Statements().size() == 1 && comp->Statements()[0] != nullptr) {
      return ParseDecisionTreeNode(comp->Statements()[0].get(), low_bound, high_bound, state);
    }

    // Handle sequential early-terminating `if (x == C) { return ...; }` statements followed by
    // a remaining decision tree or default block.
    size_t idx = 0;
    bool matched_any_prefix = false;
    while (idx < comp->Statements().size()) {
      const CStatement* cur_s = comp->Statements()[idx].get();
      if (cur_s == nullptr || cur_s->Kind() != CStatementKind::kIfStatement) break;
      const auto* ifs = static_cast<const IfStatement*>(cur_s);
      if (ifs->ElseBranch() != nullptr) break;
      const CExpression* lhs = nullptr;
      std::string op;
      int64_t cval = 0;
      if (!ExtractNormalizedSwitchComparison(ifs->Condition(), &lhs, &op, &cval) || op != "==" ||
          !StatementAlwaysTerminatesControlFlow(ifs->ThenBranch()) ||
          !state->MatchOrSetControlExpr(*lhs) ||
          !state->AddCase(cval, ifs->ThenBranch(), low_bound, high_bound)) {
        break;
      }
      if (low_bound.has_value() && cval == *low_bound) low_bound = cval + 1;
      if (high_bound.has_value() && cval == *high_bound) high_bound = cval - 1;
      matched_any_prefix = true;
      ++idx;
    }

    if (matched_any_prefix) {
      if (idx == comp->Statements().size()) {
        return state->RecordDefaultLeaf(nullptr);
      }
      if (idx + 1 == comp->Statements().size()) {
        return ParseDecisionTreeNode(comp->Statements()[idx].get(), low_bound, high_bound, state);
      }
      auto remaining_default = std::make_unique<CompoundStatement>();
      for (size_t k = idx; k < comp->Statements().size(); ++k) {
        if (comp->Statements()[k] != nullptr) {
          remaining_default->AddStatement(comp->Statements()[k]->Clone());
        }
      }
      return state->RecordDefaultLeaf(remaining_default.get());
    }

    // Check if this compound statement is a bounded value or contiguous range child.
    if (low_bound.has_value() && high_bound.has_value() && *low_bound <= *high_bound &&
        *high_bound - *low_bound <= 16 &&
        state->AddCaseRange(*low_bound, *high_bound, *comp, low_bound, high_bound)) {
      return true;
    }
    return state->RecordDefaultLeaf(comp);
  }

  if (stmt->Kind() != CStatementKind::kIfStatement) {
    if (low_bound.has_value() && high_bound.has_value() && *low_bound <= *high_bound &&
        *high_bound - *low_bound <= 16 &&
        state->AddCaseRange(*low_bound, *high_bound, *stmt, low_bound, high_bound)) {
      return true;
    }
    return state->RecordDefaultLeaf(stmt);
  }

  const auto* ifs = static_cast<const IfStatement*>(stmt);
  const CExpression* lhs = nullptr;
  std::string op;
  int64_t cval = 0;
  if (!ExtractNormalizedSwitchComparison(ifs->Condition(), &lhs, &op, &cval) ||
      !state->MatchOrSetControlExpr(*lhs)) {
    if (state->control_expr != nullptr) {
      if (low_bound.has_value() && high_bound.has_value() && *low_bound <= *high_bound &&
          *high_bound - *low_bound <= 16 &&
          state->AddCaseRange(*low_bound, *high_bound, *stmt, low_bound, high_bound)) {
        return true;
      }
      return state->RecordDefaultLeaf(stmt);
    }
    return false;
  }

  if (op == "==" || op == "!=") {
    const CStatement* case_branch = (op == "==") ? &ifs->ThenBranch() : ifs->ElseBranch();
    const CStatement* other_branch = (op == "==") ? ifs->ElseBranch() : &ifs->ThenBranch();
    if (case_branch == nullptr) return false;
    if (!state->AddCase(cval, *case_branch, low_bound, high_bound)) return false;
    if (low_bound.has_value() && cval == *low_bound) low_bound = cval + 1;
    if (high_bound.has_value() && cval == *high_bound) high_bound = cval - 1;
    if (other_branch == nullptr) {
      return state->RecordDefaultLeaf(nullptr);
    }
    // Note: after `x == cval`, an `else` branch that is not itself an `IfStatement` testing `x` is
    // a `default` branch, not an implicit bounded case.
    const CStatement* unwrapped_other = other_branch;
    if (unwrapped_other->Kind() == CStatementKind::kCompoundStatement) {
      const auto* ocomp = static_cast<const CompoundStatement*>(unwrapped_other);
      if (ocomp->Statements().size() == 1 && ocomp->Statements()[0] != nullptr) {
        unwrapped_other = ocomp->Statements()[0].get();
      }
    }
    if (unwrapped_other->Kind() != CStatementKind::kIfStatement &&
        unwrapped_other->Kind() != CStatementKind::kCompoundStatement) {
      return state->RecordDefaultLeaf(other_branch);
    }
    return ParseDecisionTreeNode(other_branch, low_bound, high_bound, state);
  }

  // Range split pivot (`<`, `<=`, `>`, `>=`).
  int64_t split_val = (op == "<" || op == ">=") ? cval : (cval + 1);
  const CStatement* less_stmt = (op == "<" || op == "<=") ? &ifs->ThenBranch() : ifs->ElseBranch();
  const CStatement* ge_stmt = (op == "<" || op == "<=") ? ifs->ElseBranch() : &ifs->ThenBranch();

  std::optional<int64_t> left_low = low_bound;
  std::optional<int64_t> left_high =
      high_bound.has_value() ? std::min(*high_bound, split_val - 1) : (split_val - 1);
  while (left_high.has_value() && state->cases.count(*left_high) > 0) {
    left_high = *left_high - 1;
  }
  while (left_low.has_value() && state->cases.count(*left_low) > 0) {
    left_low = *left_low + 1;
  }

  std::optional<int64_t> right_low =
      low_bound.has_value() ? std::max(*low_bound, split_val) : split_val;
  std::optional<int64_t> right_high = high_bound;
  while (right_low.has_value() && state->cases.count(*right_low) > 0) {
    right_low = *right_low + 1;
  }
  while (right_high.has_value() && state->cases.count(*right_high) > 0) {
    right_high = *right_high - 1;
  }

  if (left_low.has_value() && left_high.has_value() && *left_low > *left_high) return false;
  if (right_low.has_value() && right_high.has_value() && *right_low > *right_high) return false;

  ++state->range_pivot_count;
  size_t cases_before_left = state->cases.size();
  if (!ParseDecisionTreeNode(less_stmt, left_low, left_high, state)) return false;
  size_t cases_after_left = state->cases.size();

  if (!ParseDecisionTreeNode(ge_stmt, right_low, right_high, state)) return false;
  size_t cases_after_right = state->cases.size();

  // At least one side of the range split must contain a case constant, and if both branches are
  // non-null, both should contribute at least one case constant.
  if (cases_after_right == cases_before_left) return false;
  if (less_stmt != nullptr && ge_stmt != nullptr &&
      (cases_after_left == cases_before_left || cases_after_right == cases_after_left)) {
    return false;
  }
  return true;
}

std::unique_ptr<CStatement> TryBuildSparseSwitchFromState(SparseDecisionTreeState* state) {
  if (state == nullptr || state->control_expr == nullptr || state->range_pivot_count < 1 ||
      state->cases.size() < 3) {
    return nullptr;
  }
  if (state->has_empty_default && state->non_empty_default != nullptr) {
    return nullptr;
  }

  std::vector<SwitchCase> switch_cases;
  switch_cases.reserve(state->cases.size() + (state->non_empty_default != nullptr ? 1 : 0));
  std::string prev_body_str;
  for (auto& [val, body] : state->cases) {
    std::string cur_body_str = body->ToString();
    if (!switch_cases.empty() && !switch_cases.back().case_values.empty() &&
        switch_cases.back().case_values.back() + 1 == val && cur_body_str == prev_body_str) {
      switch_cases.back().case_values.push_back(val);
    } else {
      SwitchCase sc;
      sc.case_values = {val};
      sc.is_default = false;
      sc.body = std::move(body);
      switch_cases.push_back(std::move(sc));
      prev_body_str = std::move(cur_body_str);
    }
  }

  // Verify the grouped case set matches GCC 2.7.2 `stmt.c:4818` decision-tree criteria (`count < 5`
  // or `span > 10 * count`, evaluated after `group_case_nodes`) so recompiling the `switch`
  // produces a decision tree rather than a jump table.
  const size_t count = switch_cases.size();
  const int64_t min_val = state->cases.begin()->first;
  const int64_t max_val = state->cases.rbegin()->first;
  const uint64_t span = static_cast<uint64_t>(max_val) - static_cast<uint64_t>(min_val);
  if (count < 2 || (count >= 5 && span <= 10 * count)) {
    return nullptr;
  }

  if (state->non_empty_default != nullptr) {
    SwitchCase def_sc;
    def_sc.is_default = true;
    def_sc.body = std::move(state->non_empty_default);
    switch_cases.push_back(std::move(def_sc));
  }
  return CStatement::Switch(std::move(state->control_expr), std::move(switch_cases));
}

}  // namespace

bool ReconstructSparseSwitchDecisionTrees(CompoundStatement* body) {
  if (body == nullptr) return false;
  bool any_changed = false;

  auto& stmts = body->MutableStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i] == nullptr) continue;
    ForEachSubCompoundStatement(stmts[i].get(), [&](CompoundStatement* sub) {
      if (ReconstructSparseSwitchDecisionTrees(sub)) {
        any_changed = true;
      }
    });
  }

  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kIfStatement) continue;

    // 1. Check if `stmts[i]` is an early-terminating `if (x == P) { ... }` (without `else`)
    // immediately followed by a range-split `IfStatement` `stmts[i + 1]` on `x`.
    const auto* ifs = static_cast<const IfStatement*>(stmts[i].get());
    if (ifs->ElseBranch() == nullptr && StatementAlwaysTerminatesControlFlow(ifs->ThenBranch()) &&
        i + 1 < stmts.size() && stmts[i + 1] != nullptr &&
        stmts[i + 1]->Kind() == CStatementKind::kIfStatement) {
      auto combined_if = CStatement::If(ifs->Condition().Clone(), ifs->ThenBranch().Clone(),
                                        stmts[i + 1]->Clone());
      SparseDecisionTreeState state;
      if (ParseDecisionTreeNode(combined_if.get(), std::nullopt, std::nullopt, &state)) {
        if (auto sw = TryBuildSparseSwitchFromState(&state)) {
          stmts[i] = std::move(sw);
          stmts.erase(stmts.begin() + static_cast<ptrdiff_t>(i + 1));
          any_changed = true;
          continue;
        }
      }
    }

    // 2. Check if `stmts[i]` is directly a sparse switch binary-search decision tree.
    SparseDecisionTreeState state;
    if (ParseDecisionTreeNode(stmts[i].get(), std::nullopt, std::nullopt, &state)) {
      if (auto sw = TryBuildSparseSwitchFromState(&state)) {
        stmts[i] = std::move(sw);
        any_changed = true;
      }
    }
  }

  return any_changed;
}

bool SparseSwitchDecisionTreePass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  return ReconstructSparseSwitchDecisionTrees(body);
}

}  // namespace rom_nom_nom
