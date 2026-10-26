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

bool FoldDoWhileTrailingLatchComparison(DoWhileStatement* do_while,
                                        bool v0_used_after_in_enclosing_block) {
  if (do_while == nullptr || v0_used_after_in_enclosing_block) {
    return false;
  }
  const CExpression& cond = do_while->Condition();
  if (cond.Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  const auto& bin_cond = static_cast<const BinaryExpression&>(cond);
  if (bin_cond.Op() != "!=" || bin_cond.Lhs().Kind() != CExpressionKind::kIdentifier ||
      bin_cond.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
      static_cast<const IntegerLiteralExpression&>(bin_cond.Rhs()).Value() != 0) {
    return false;
  }
  std::string cond_var(static_cast<const IdentifierExpression&>(bin_cond.Lhs()).Name());
  if (cond_var != "v0" && cond_var != "v1" && cond_var != "at") {
    return false;
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
  for (int offset = 0; offset <= 1; ++offset) {
    int cand_idx = static_cast<int>(body_stmts.size()) - 1 - offset;
    if (cand_idx < 0 || body_stmts[cand_idx] == nullptr) continue;
    std::string lhs;
    const CExpression* rhs = nullptr;
    if (!ExtractSimpleVarAssign(*body_stmts[cand_idx], &lhs, &rhs) || lhs != cond_var ||
        rhs == nullptr || !IsRelationalOrEqualityBinary(*rhs) || !IsPureExpression(*rhs) ||
        ExpressionUsesVariable(*rhs, cond_var)) {
      continue;
    }
    if (offset == 1) {
      const CStatement& tail_stmt = *body_stmts.back();
      std::string tail_lhs;
      const CExpression* tail_rhs = nullptr;
      if (!ExtractSimpleVarAssign(tail_stmt, &tail_lhs, &tail_rhs) || tail_lhs == cond_var ||
          !IsPureExpression(*tail_rhs) || StatementReferencesVariable(tail_stmt, cond_var)) {
        continue;
      }
      bool rhs_mentions_tail_lhs = ExpressionUsesVariable(*rhs, tail_lhs);
      if (rhs_mentions_tail_lhs) continue;
    }
    assign_idx = cand_idx;
    latch_rhs = rhs;
    break;
  }
  if (assign_idx < 0 || latch_rhs == nullptr) {
    return false;
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
  if (!killed_on_entry || reads_before_kill) {
    return false;
  }

  auto new_cond = latch_rhs->Clone();
  body_stmts.erase(body_stmts.begin() + assign_idx);
  do_while->MutableCondition() = std::move(new_cond);
  return true;
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
  if (lhs_name != "v0" && lhs_name != "v1" && lhs_name != "at") {
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

  auto is_simple_call_or_assign_stmt = [](const CStatement& s, bool* out_has_call) -> bool {
    if (s.Kind() == CStatementKind::kReturnStatement) {
      const auto& ret = static_cast<const ReturnStatement&>(s);
      if (ret.ReturnValue() == nullptr) return true;
      return ret.ReturnValue()->Kind() == CExpressionKind::kIdentifier ||
             ret.ReturnValue()->Kind() == CExpressionKind::kIntegerLiteral;
    }
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() == CExpressionKind::kCallExpression) {
      *out_has_call = true;
      return true;
    }
    if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
        if (a.Rhs().Kind() == CExpressionKind::kCallExpression) {
          *out_has_call = true;
          return true;
        }
        if (a.Rhs().Kind() == CExpressionKind::kIdentifier ||
            a.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
          return true;
        }
      }
    }
    return false;
  };

  auto is_switch_tail_stmt = [](const CStatement& s, bool* out_writes_memory) -> bool {
    if (s.Kind() == CStatementKind::kReturnStatement) {
      const auto& ret = static_cast<const ReturnStatement&>(s);
      return ret.ReturnValue() == nullptr || !ExpressionContainsCall(*ret.ReturnValue());
    }
    if (s.Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(s);
      if (es.Expression().Kind() == CExpressionKind::kCallExpression) {
        *out_writes_memory = true;
        return true;
      }
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() != "=") return false;
        if (a.Lhs().Kind() == CExpressionKind::kUnaryExpression ||
            a.Lhs().Kind() == CExpressionKind::kArrayIndexExpression ||
            a.Lhs().Kind() == CExpressionKind::kMemberAccessExpression) {
          *out_writes_memory = true;
        } else if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          if (absl::StartsWith(lhs_name, "g_") || absl::StartsWith(lhs_name, "D_") ||
              absl::StartsWith(lhs_name, "s_")) {
            *out_writes_memory = true;
          }
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
      if (a.Lhs().Kind() == CExpressionKind::kUnaryExpression ||
          a.Lhs().Kind() == CExpressionKind::kArrayIndexExpression ||
          a.Lhs().Kind() == CExpressionKind::kMemberAccessExpression ||
          (a.Lhs().Kind() == CExpressionKind::kIdentifier &&
           (absl::StartsWith(static_cast<const IdentifierExpression&>(a.Lhs()).Name(), "g_") ||
            absl::StartsWith(static_cast<const IdentifierExpression&>(a.Lhs()).Name(), "D_")))) {
        *out_writes_memory = true;
      }
      return true;
    }
    return false;
  };

  std::vector<LabelSite> candidates;
  std::function<void(CompoundStatement*, const CStatement*, bool, bool, bool, bool)>
      collect_labels = [&](CompoundStatement* comp, const CStatement* scope_root, bool is_root,
                           bool parent_ends_with_break, bool is_switch_case_body, bool in_switch) {
        if (comp == nullptr) return;
        auto& stmts = comp->MutableStatements();
        for (size_t i = 0; i < stmts.size(); ++i) {
          CStatement* s = stmts[i].get();
          if (s == nullptr) continue;
          if (!is_root && s->Kind() == CStatementKind::kLabelStatement) {
            std::string lbl(static_cast<const LabelStatement&>(*s).Label());
            size_t end_idx = stmts.size();
            bool ends_with_break = parent_ends_with_break;
            if (end_idx > i + 1 && stmts[end_idx - 1]->Kind() == CStatementKind::kBreakStatement) {
              --end_idx;
              ends_with_break = true;
            }
            std::vector<std::unique_ptr<CStatement>> tail;
            bool has_call = false;
            bool all_call_valid = true;
            bool writes_memory = false;
            bool all_tail_stmts_valid = i > 0;
            bool all_switch_tail_valid = is_switch_case_body && i > 0 && ends_with_break;
            for (size_t k = i + 1; k < end_idx; ++k) {
              if (stmts[k] == nullptr) {
                all_call_valid = false;
                all_tail_stmts_valid = false;
                all_switch_tail_valid = false;
                break;
              }
              if (stmts[k]->Kind() == CStatementKind::kLabelStatement) {
                all_call_valid = false;
                continue;
              }
              if (!is_simple_call_or_assign_stmt(*stmts[k], &has_call)) {
                all_call_valid = false;
              }
              if (!is_switch_tail_stmt(*stmts[k], &writes_memory)) {
                all_tail_stmts_valid = false;
                all_switch_tail_valid = false;
              }
              tail.push_back(stmts[k]->Clone());
            }
            bool valid_call_tail = all_call_valid && has_call && tail.size() <= 3;
            bool valid_switch_tail = all_switch_tail_valid && writes_memory && tail.size() <= 8;
            bool valid_diamond_tail =
                allow_diamond_tails && all_tail_stmts_valid && writes_memory && tail.size() <= 4;
            if (!tail.empty() && (valid_call_tail || valid_switch_tail || valid_diamond_tail)) {
              candidates.push_back(LabelSite{
                  .comp = comp,
                  .goto_scope = scope_root,
                  .label_name = std::move(lbl),
                  .tail_stmts = std::move(tail),
                  .ends_with_break = ends_with_break,
                  .requires_tail_goto =
                      !valid_call_tail && !valid_switch_tail && !(in_switch && ends_with_break)});
            }
          } else if (s->Kind() == CStatementKind::kIfStatement) {
            bool stmt_at_tail = (i + 1 == stmts.size()) ||
                                (i + 2 == stmts.size() &&
                                 (stmts[i + 1]->Kind() == CStatementKind::kBreakStatement ||
                                  stmts[i + 1]->Kind() == CStatementKind::kReturnStatement));
            bool branch_ends_with_break = stmt_at_tail && (parent_ends_with_break || is_root);
            const CStatement* child_scope = stmt_at_tail ? scope_root : s;
            auto& ifs = static_cast<IfStatement&>(*s);
            if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
              collect_labels(static_cast<CompoundStatement*>(ifs.MutableThenBranch()), child_scope,
                             false, branch_ends_with_break, false, in_switch);
            }
            if (ifs.MutableElseBranch() != nullptr &&
                ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
              collect_labels(static_cast<CompoundStatement*>(ifs.MutableElseBranch()), child_scope,
                             false, branch_ends_with_break, false, in_switch);
            }
          } else if (s->Kind() == CStatementKind::kSwitchStatement) {
            for (auto& sc : static_cast<SwitchStatement&>(*s).MutableCases()) {
              if (sc.body != nullptr) {
                bool sc_ends_with_break =
                    !sc.body->Statements().empty() &&
                    (sc.body->Statements().back()->Kind() == CStatementKind::kBreakStatement ||
                     sc.body->Statements().back()->Kind() == CStatementKind::kReturnStatement);
                collect_labels(sc.body.get(), sc.body.get(), false, sc_ends_with_break, true, true);
              }
            }
          } else if (s->Kind() == CStatementKind::kCompoundStatement) {
            bool stmt_at_tail = (i + 1 == stmts.size()) ||
                                (i + 2 == stmts.size() &&
                                 (stmts[i + 1]->Kind() == CStatementKind::kBreakStatement ||
                                  stmts[i + 1]->Kind() == CStatementKind::kReturnStatement));
            bool sub_ends_with_break = stmt_at_tail && (parent_ends_with_break || is_root);
            const CStatement* child_scope = stmt_at_tail ? scope_root : s;
            collect_labels(static_cast<CompoundStatement*>(s), child_scope, false,
                           sub_ends_with_break, false, in_switch);
          }
        }
      };
  collect_labels(root_body, root_body, true, true, false, false);

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
          if ((last->Kind() == CStatementKind::kBreakStatement ||
               last->Kind() == CStatementKind::kReturnStatement) &&
              cs.size() >= 2 && cs[cs.size() - 2] != nullptr) {
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
      std::function<bool(CompoundStatement*)> remove_label = [&](CompoundStatement* comp) -> bool {
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
    }
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
        // `block_N: <0..4 sinkable ExpressionStatements>`, sink `block_N: <tail>` after the
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
              if (!is_sinkable_tail_stmt(*then_stmts[k - 1]) || (then_stmts.size() - (k - 1)) > 4) {
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
            size_t then_min = then_stmts.size() > 4 ? then_stmts.size() - 4 : 0;
            size_t else_min = else_stmts.size() > 4 ? else_stmts.size() - 4 : 0;
            for (size_t ti = then_stmts.size(); ti > then_min; --ti) {
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

              for (size_t ei = else_stmts.size(); ei > else_min; --ei) {
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

bool TrySynthesizeGuardedForLoop(const CExpression& guard_cond, const CompoundStatement& then_body,
                                 const CompoundStatement& else_body,
                                 CompoundStatement* target_block) {
  if (target_block == nullptr || !else_body.Statements().empty() ||
      then_body.Statements().size() != 1 ||
      then_body.Statements()[0]->Kind() != CStatementKind::kDoWhileStatement) {
    return false;
  }
  const auto* do_while = static_cast<const DoWhileStatement*>(then_body.Statements()[0].get());
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
    if (loop_var_view.empty()) continue;
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

    bool step_placement_valid = (inc_idx >= 0);
    for (size_t i = 0; step_placement_valid && i < loop_compound.Statements().size(); ++i) {
      if (static_cast<int>(i) == inc_idx) continue;
      const CStatement& s = *loop_compound.Statements()[i];
      if (IsRedundantLatchComparisonAssignment(s, loop_cond) ||
          IsRedundantLatchComparisonAssignment(s, bin_cond)) {
        continue;
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

    int init_idx = -1;
    const CExpression* init_rhs = nullptr;
    for (int si = static_cast<int>(target_block->Statements().size()) - 1; si >= 0; --si) {
      if (static_cast<size_t>(si + 4) < target_block->Statements().size()) {
        break;
      }
      const CExpression* rhs =
          MatchSimpleInitializerAssignment(*target_block->Statements()[si], loop_var);
      if (rhs != nullptr) {
        init_idx = si;
        init_rhs = rhs;
        break;
      }
    }

    bool init_placement_valid =
        (init_idx >= 0 && init_rhs != nullptr &&
         IsCompatibleForLoopEntryGuard(guard_cond, bin_cond, loop_var, *init_rhs));
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
      auto& target_stmts = target_block->MutableStatements();
      target_stmts.erase(target_stmts.begin() + init_idx);

      auto for_body = std::make_unique<CompoundStatement>();
      for (size_t i = 0; i < loop_compound.Statements().size(); ++i) {
        if (static_cast<int>(i) == inc_idx) continue;
        if (IsRedundantLatchComparisonAssignment(*loop_compound.Statements()[i], loop_cond) ||
            IsRedundantLatchComparisonAssignment(*loop_compound.Statements()[i], bin_cond)) {
          continue;
        }
        for_body->AddStatement(loop_compound.Statements()[i]->Clone());
      }
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
  for (const auto& stmt : body->Statements()) {
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
          TrySynthesizeGuardedForLoop(ifs.Condition(), *then_comp, *else_comp, &rebuilt)) {
        local_changed = true;
        continue;
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

  std::function<void(CompoundStatement*, bool)> process_block = [&](CompoundStatement* comp,
                                                                    bool in_loop) {
    if (comp == nullptr) return;
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) continue;
      bool child_in_loop = in_loop || stmts[i]->Kind() == CStatementKind::kDoWhileStatement ||
                           stmts[i]->Kind() == CStatementKind::kWhileStatement ||
                           stmts[i]->Kind() == CStatementKind::kForStatement;
      ForEachSubCompoundStatement(
          stmts[i].get(), [&](CompoundStatement* sub) { process_block(sub, child_in_loop); });
    }

    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) continue;

      // 1. Fold trailing `v0 = (cond); } while ((v0 != 0));` on outer/nested do-while loops when
      // `v0` is not used after the loop and is killed before read on loop entry.
      if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement) {
        auto* dw = static_cast<DoWhileStatement*>(stmts[i].get());
        bool v0_used_after = false;
        for (size_t after = i + 1; after < stmts.size(); ++after) {
          if (stmts[after] != nullptr && StatementReferencesVariable(*stmts[after], "v0")) {
            v0_used_after = true;
            break;
          }
        }
        if (FoldDoWhileTrailingLatchComparison(dw, v0_used_after)) {
          any_changed = true;
        }
      }

      if (in_loop) continue;

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

      // Scan up to 4 preceding pure assignments `reg = rhs;` before `stmts[i]`.
      for (int scan = static_cast<int>(i) - 1; scan >= 0 && scan >= static_cast<int>(i) - 4;
           --scan) {
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
        if (!is_int_const && !is_sym_addr) {
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

        // Case A: `reg = const;` where `reg` is read ONLY in the loop condition (and optional
        // entry guard condition), and not inside the loop body.
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
        }

        // Case B: `reg = &D_...;` where `reg` is a symbol-address invariant used inside a loop
        // without calls, and substituting `(s32)&D_...` allows `loop.c:move_movables` to hoist
        // the symbol address naturally.
        if (is_sym_addr && sym_addr != nullptr && !StatementContainsCall(*stmts[i])) {
          auto replacement = CExpression::Cast(CType::S32(), sym_addr->Clone());
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

  process_block(body, /*in_loop=*/false);
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
  InlineCrossJumpedCallTails(body, allow_diamond_tails_);
  SinkCommonBranchTailArgAssignments(body);
  return body->ToString() != before;
}

bool ForLoopSynthesizerPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  return SynthesizeForLoopsInBlock(body);
}

bool LoopInvariantSinkerPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  return SinkLoopInvariantAssignments(body);
}

}  // namespace rom_nom_nom
