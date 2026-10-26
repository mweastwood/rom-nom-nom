#include "lifter/ast_abi_rewriter.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"

namespace rom_nom_nom {
namespace {

bool IsCalleeSavedGprVarName(std::string_view v) {
  return v == "fp" ||
         (v.size() == 7 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7');
}

}  // namespace

void SimplifyReturnStatements(CompoundStatement* comp, bool returns_v0, bool returns_f0) {
  if (comp == nullptr) return;

  for (auto& s : comp->MutableStatements()) {
    ForEachSubCompoundStatement(s.get(), [&](CompoundStatement* sub) {
      SimplifyReturnStatements(sub, returns_v0, returns_f0);
    });
  }

  auto get_id_name = [](const CExpression& expr) -> std::string {
    if (expr.Kind() == CExpressionKind::kIdentifier) {
      return static_cast<const IdentifierExpression&>(expr).Name();
    }
    return "";
  };

  std::function<std::unique_ptr<CExpression>(const CExpression&, std::string_view,
                                             const CExpression&)>
      subst_id = [&](const CExpression& expr, std::string_view target,
                     const CExpression& replacement) -> std::unique_ptr<CExpression> {
    if (expr.Kind() == CExpressionKind::kIdentifier) {
      if (static_cast<const IdentifierExpression&>(expr).Name() == target) {
        return replacement.Clone();
      }
      return expr.Clone();
    }
    if (expr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(bin.Op(), subst_id(bin.Lhs(), target, replacement),
                                 subst_id(bin.Rhs(), target, replacement));
    }
    if (expr.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(un.Op(), subst_id(un.Operand(), target, replacement),
                                un.IsPrefix());
    }
    if (expr.Kind() == CExpressionKind::kCastExpression) {
      const auto& cast = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(cast.TargetType(), subst_id(cast.Operand(), target, replacement));
    }
    if (expr.Kind() == CExpressionKind::kArrayIndexExpression) {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(subst_id(arr.Array(), target, replacement),
                                     subst_id(arr.Index(), target, replacement));
    }
    if (expr.Kind() == CExpressionKind::kMemberAccessExpression) {
      const auto& mem = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(subst_id(mem.Object(), target, replacement),
                                       mem.MemberName(), mem.IsArrow());
    }
    return expr.Clone();
  };

  std::vector<std::unique_ptr<CStatement>> new_stmts;
  auto& old_stmts = comp->MutableStatements();
  for (size_t i = 0; i < old_stmts.size(); ++i) {
    if (i + 2 < old_stmts.size() && old_stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
        old_stmts[i + 1]->Kind() == CStatementKind::kIfStatement &&
        old_stmts[i + 2]->Kind() == CStatementKind::kReturnStatement) {
      const auto& expr_s0 = static_cast<const ExpressionStatement&>(*old_stmts[i]);
      const auto& if_s = static_cast<const IfStatement&>(*old_stmts[i + 1]);
      const auto& ret_s = static_cast<const ReturnStatement&>(*old_stmts[i + 2]);

      if (expr_s0.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
          (if_s.ElseBranch() == nullptr ||
           (if_s.ElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
            static_cast<const CompoundStatement*>(if_s.ElseBranch())->IsEmpty()))) {
        const auto& assign0 = static_cast<const AssignmentExpression&>(expr_s0.Expression());
        std::string var_name = get_id_name(assign0.Lhs());
        if ((var_name == "v0" || var_name == "f0" || var_name == "f0_d") && assign0.Op() == "=") {
          bool ret_matches =
              (ret_s.ReturnValue() == nullptr || get_id_name(*ret_s.ReturnValue()) == var_name);
          if (ret_matches && if_s.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
            const auto& then_comp = static_cast<const CompoundStatement&>(if_s.ThenBranch());
            if (then_comp.Statements().size() == 1 &&
                then_comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement) {
              const auto& then_expr_s =
                  static_cast<const ExpressionStatement&>(*then_comp.Statements()[0]);
              if (then_expr_s.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                const auto& then_assign =
                    static_cast<const AssignmentExpression&>(then_expr_s.Expression());
                if (get_id_name(then_assign.Lhs()) == var_name && then_assign.Op() == "=") {
                  const auto& default_expr = assign0.Rhs();
                  const auto& raw_then_expr = then_assign.Rhs();
                  const auto& raw_cond = if_s.Condition();

                  bool uses_var = absl::StrContains(raw_then_expr.ToString(), var_name) ||
                                  absl::StrContains(raw_cond.ToString(), var_name);
                  if (!uses_var) {
                    new_stmts.push_back(std::move(old_stmts[i]));
                    continue;
                  }

                  auto final_then_expr = subst_id(raw_then_expr, var_name, default_expr);
                  auto final_cond = subst_id(raw_cond, var_name, default_expr);

                  if (final_cond->Kind() == CExpressionKind::kBinaryExpression) {
                    const auto& bin_cond = static_cast<const BinaryExpression&>(*final_cond);
                    if (bin_cond.Op() == ">=") {
                      std::string lhs_id = get_id_name(bin_cond.Lhs());
                      std::string rhs_id = get_id_name(bin_cond.Rhs());
                      std::string then_id = get_id_name(*final_then_expr);
                      std::string def_id = get_id_name(default_expr);
                      if (!lhs_id.empty() && !rhs_id.empty() && !then_id.empty() &&
                          !def_id.empty()) {
                        if (lhs_id == then_id && rhs_id == def_id) {
                          // MaxTwo pattern: if (arg0 > arg1) return arg0; return arg1;
                          auto inverted_cond = CExpression::Binary(">", default_expr.Clone(),
                                                                   final_then_expr->Clone());
                          auto then_return = std::make_unique<CompoundStatement>();
                          then_return->AddStatement(CStatement::Return(default_expr.Clone()));
                          new_stmts.push_back(
                              CStatement::If(std::move(inverted_cond), std::move(then_return)));
                          new_stmts.push_back(CStatement::Return(std::move(final_then_expr)));
                          i += 2;
                          continue;
                        } else if (lhs_id == def_id && rhs_id == then_id) {
                          // MinTwo pattern: if (arg0 < arg1) return arg0; return arg1;
                          auto inverted_cond = CExpression::Binary("<", default_expr.Clone(),
                                                                   final_then_expr->Clone());
                          auto then_return = std::make_unique<CompoundStatement>();
                          then_return->AddStatement(CStatement::Return(default_expr.Clone()));
                          new_stmts.push_back(
                              CStatement::If(std::move(inverted_cond), std::move(then_return)));
                          new_stmts.push_back(CStatement::Return(std::move(final_then_expr)));
                          i += 2;
                          continue;
                        }
                      }
                    }
                  }

                  auto then_return = std::make_unique<CompoundStatement>();
                  then_return->AddStatement(CStatement::Return(std::move(final_then_expr)));
                  new_stmts.push_back(
                      CStatement::If(std::move(final_cond), std::move(then_return)));
                  new_stmts.push_back(CStatement::Return(default_expr.Clone()));
                  i += 2;
                  continue;
                }
              }
            }
          }
        }
      }
    }

    if (i + 1 < old_stmts.size() && old_stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
        old_stmts[i + 1]->Kind() == CStatementKind::kReturnStatement) {
      const auto& expr_s = static_cast<const ExpressionStatement&>(*old_stmts[i]);
      const auto& ret_s = static_cast<const ReturnStatement&>(*old_stmts[i + 1]);
      if (expr_s.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr_s.Expression());
        std::string var_name = get_id_name(assign.Lhs());
        if ((var_name == "v0" || var_name == "f0" || var_name == "f0_d") && assign.Op() == "=") {
          bool ret_matches =
              (ret_s.ReturnValue() == nullptr || get_id_name(*ret_s.ReturnValue()) == var_name);
          if (ret_matches) {
            new_stmts.push_back(CStatement::Return(assign.Rhs().Clone()));
            i += 1;
            continue;
          }
        }
      }
    }

    new_stmts.push_back(std::move(old_stmts[i]));
  }
  old_stmts = std::move(new_stmts);
}

void SimplifyDecoupledDelaySlotEarlyReturns(CompoundStatement* comp) {
  if (comp == nullptr) return;

  for (auto& s : comp->MutableStatements()) {
    ForEachSubCompoundStatement(
        s.get(), [](CompoundStatement* sub) { SimplifyDecoupledDelaySlotEarlyReturns(sub); });
  }

  auto get_id_name = [](const CExpression& expr) -> std::string {
    if (expr.Kind() == CExpressionKind::kIdentifier) {
      return static_cast<const IdentifierExpression&>(expr).Name();
    }
    return "";
  };

  std::function<std::unique_ptr<CStatement>(const IfStatement&, std::string_view,
                                            const CExpression&)>
      try_extract = [&](const IfStatement& if_s, std::string_view var_name,
                        const CExpression& default_val) -> std::unique_ptr<CStatement> {
    if (absl::StrContains(if_s.Condition().ToString(), var_name)) return nullptr;
    if (if_s.ThenBranch().Kind() != CStatementKind::kCompoundStatement ||
        if_s.ElseBranch() == nullptr ||
        if_s.ElseBranch()->Kind() != CStatementKind::kCompoundStatement) {
      return nullptr;
    }
    const auto& then_comp = static_cast<const CompoundStatement&>(if_s.ThenBranch());
    const auto& else_comp = static_cast<const CompoundStatement&>(*if_s.ElseBranch());
    if (then_comp.Statements().size() != 2 || else_comp.Statements().size() != 1) {
      return nullptr;
    }
    if (then_comp.Statements()[0]->Kind() != CStatementKind::kExpressionStatement ||
        else_comp.Statements()[0]->Kind() != CStatementKind::kExpressionStatement) {
      return nullptr;
    }
    const auto& then_es0 = static_cast<const ExpressionStatement&>(*then_comp.Statements()[0]);
    const auto& else_es0 = static_cast<const ExpressionStatement&>(*else_comp.Statements()[0]);
    if (then_es0.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
        else_es0.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
      return nullptr;
    }
    const auto& then_a0 = static_cast<const AssignmentExpression&>(then_es0.Expression());
    const auto& else_a0 = static_cast<const AssignmentExpression&>(else_es0.Expression());
    if (then_a0.Op() != "=" || else_a0.Op() != "=" || get_id_name(then_a0.Lhs()) != var_name ||
        get_id_name(else_a0.Lhs()) != var_name ||
        then_a0.Rhs().ToString() != default_val.ToString() ||
        else_a0.Rhs().ToString() != default_val.ToString()) {
      return nullptr;
    }

    const CStatement& tail_stmt = *then_comp.Statements()[1];
    if (tail_stmt.Kind() == CStatementKind::kExpressionStatement) {
      const auto& tail_es = static_cast<const ExpressionStatement&>(tail_stmt);
      if (tail_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& tail_a = static_cast<const AssignmentExpression&>(tail_es.Expression());
        if (tail_a.Op() == "=" && get_id_name(tail_a.Lhs()) == var_name &&
            !absl::StrContains(tail_a.Rhs().ToString(), var_name)) {
          auto new_then = std::make_unique<CompoundStatement>();
          new_then->AddStatement(CStatement::Return(tail_a.Rhs().Clone()));
          return CStatement::If(if_s.Condition().Clone(), std::move(new_then));
        }
      }
    }
    if (tail_stmt.Kind() == CStatementKind::kIfStatement) {
      auto extracted_nested =
          try_extract(static_cast<const IfStatement&>(tail_stmt), var_name, default_val);
      if (extracted_nested != nullptr) {
        auto new_then = std::make_unique<CompoundStatement>();
        new_then->AddStatement(std::move(extracted_nested));
        return CStatement::If(if_s.Condition().Clone(), std::move(new_then));
      }
    }
    if (!absl::StrContains(tail_stmt.ToString(), var_name)) {
      auto new_then = std::make_unique<CompoundStatement>();
      new_then->AddStatement(tail_stmt.Clone());
      return CStatement::If(if_s.Condition().Clone(), std::move(new_then));
    }
    return nullptr;
  };

  std::vector<std::unique_ptr<CStatement>> new_stmts;
  auto& old_stmts = comp->MutableStatements();
  for (size_t i = 0; i < old_stmts.size(); ++i) {
    if (i + 1 < old_stmts.size() && old_stmts[i] != nullptr && old_stmts[i + 1] != nullptr &&
        old_stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
        old_stmts[i + 1]->Kind() == CStatementKind::kIfStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*old_stmts[i]);
      auto* next_if = static_cast<IfStatement*>(old_stmts[i + 1].get());
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
          next_if->Condition().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        const auto& cond_bin = static_cast<const BinaryExpression&>(next_if->Condition());
        std::string lhs_var = get_id_name(a.Lhs());
        if (a.Op() == "=" && IsCalleeSavedGprVarName(lhs_var) &&
            (cond_bin.Op() == "==" || cond_bin.Op() == "!=") &&
            cond_bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(cond_bin.Rhs()).Value() == 0 &&
            cond_bin.Lhs().ToString() == a.Rhs().ToString()) {
          old_stmts[i + 1] = CStatement::If(
              CExpression::Binary(cond_bin.Op(), CExpression::Identifier(lhs_var),
                                  cond_bin.Rhs().Clone()),
              next_if->ThenBranch().Clone(),
              next_if->ElseBranch() != nullptr ? next_if->ElseBranch()->Clone() : nullptr);
        }
      }
    }
    if (i + 1 < old_stmts.size() && old_stmts[i]->Kind() == CStatementKind::kIfStatement &&
        old_stmts[i + 1]->Kind() == CStatementKind::kReturnStatement) {
      const auto& if_s = static_cast<const IfStatement&>(*old_stmts[i]);
      const auto& ret_s = static_cast<const ReturnStatement&>(*old_stmts[i + 1]);
      if (ret_s.ReturnValue() != nullptr) {
        std::string var_name = get_id_name(*ret_s.ReturnValue());
        if ((var_name == "v0" || var_name == "f0" || var_name == "f0_d") &&
            if_s.ElseBranch() != nullptr &&
            if_s.ElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          const auto& else_comp = static_cast<const CompoundStatement&>(*if_s.ElseBranch());
          if (else_comp.Statements().size() == 1 &&
              else_comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement) {
            const auto& else_es0 =
                static_cast<const ExpressionStatement&>(*else_comp.Statements()[0]);
            if (else_es0.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
              const auto& else_a0 = static_cast<const AssignmentExpression&>(else_es0.Expression());
              if (else_a0.Op() == "=" && get_id_name(else_a0.Lhs()) == var_name) {
                auto extracted = try_extract(if_s, var_name, else_a0.Rhs());
                if (extracted != nullptr) {
                  new_stmts.push_back(std::move(extracted));
                  new_stmts.push_back(CStatement::Return(else_a0.Rhs().Clone()));
                  i += 1;
                  continue;
                }
              }
            }
          }
        }
      }
    }
    new_stmts.push_back(std::move(old_stmts[i]));
  }
  old_stmts = std::move(new_stmts);
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

  // Rule C: Remove any LabelStatement in `comp` that has no remaining GotoStatement targeting it.
  absl::flat_hash_set<std::string> used_labels;
  std::function<void(const CStatement&)> collect_gotos = [&](const CStatement& s) {
    if (s.Kind() == CStatementKind::kGotoStatement) {
      used_labels.insert(std::string(static_cast<const GotoStatement&>(s).Label()));
    } else if (s.Kind() == CStatementKind::kCompoundStatement) {
      for (const auto& ch : static_cast<const CompoundStatement&>(s).Statements()) {
        if (ch != nullptr) collect_gotos(*ch);
      }
    } else if (s.Kind() == CStatementKind::kIfStatement) {
      const auto& ifs = static_cast<const IfStatement&>(s);
      collect_gotos(ifs.ThenBranch());
      if (ifs.ElseBranch() != nullptr) collect_gotos(*ifs.ElseBranch());
    } else if (s.Kind() == CStatementKind::kSwitchStatement) {
      for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
        if (sc.body != nullptr) collect_gotos(*sc.body);
      }
    } else if (s.Kind() == CStatementKind::kWhileStatement) {
      collect_gotos(static_cast<const WhileStatement&>(s).Body());
    } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
      collect_gotos(static_cast<const DoWhileStatement&>(s).Body());
    } else if (s.Kind() == CStatementKind::kForStatement) {
      collect_gotos(static_cast<const ForStatement&>(s).Body());
    }
  };
  collect_gotos(*comp);

  std::function<void(CompoundStatement*)> prune_unused_labels = [&](CompoundStatement* cur_comp) {
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
          continue;
        }
      } else {
        ForEachSubCompoundStatement(stmts[i].get(), prune_unused_labels);
      }
      ++i;
    }
  };
  prune_unused_labels(comp);
}

void InlineCrossJumpedCallTails(CompoundStatement* root_body) {
  if (root_body == nullptr) return;

  struct LabelSite {
    CompoundStatement* comp = nullptr;
    std::string label_name;
    std::vector<std::unique_ptr<CStatement>> tail_stmts;
    bool ends_with_break = false;
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

  std::vector<LabelSite> candidates;
  std::function<void(CompoundStatement*, bool, bool)> collect_labels =
      [&](CompoundStatement* comp, bool is_root, bool parent_ends_with_break) {
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
            size_t tail_count = end_idx - (i + 1);
            if (tail_count >= 1 && tail_count <= 3) {
              bool has_call = false;
              bool all_valid = true;
              std::vector<std::unique_ptr<CStatement>> tail;
              for (size_t k = i + 1; k < end_idx; ++k) {
                if (!is_simple_call_or_assign_stmt(*stmts[k], &has_call)) {
                  all_valid = false;
                  break;
                }
                tail.push_back(stmts[k]->Clone());
              }
              if (all_valid && has_call) {
                candidates.push_back(LabelSite{.comp = comp,
                                               .label_name = std::move(lbl),
                                               .tail_stmts = std::move(tail),
                                               .ends_with_break = ends_with_break});
              }
            }
          } else if (s->Kind() == CStatementKind::kIfStatement) {
            bool branch_ends_with_break =
                (i + 1 == stmts.size() && (parent_ends_with_break || is_root)) ||
                (i + 2 == stmts.size() &&
                 (stmts[i + 1]->Kind() == CStatementKind::kBreakStatement ||
                  stmts[i + 1]->Kind() == CStatementKind::kReturnStatement));
            auto& ifs = static_cast<IfStatement&>(*s);
            if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
              collect_labels(static_cast<CompoundStatement*>(ifs.MutableThenBranch()), false,
                             branch_ends_with_break);
            }
            if (ifs.MutableElseBranch() != nullptr &&
                ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
              collect_labels(static_cast<CompoundStatement*>(ifs.MutableElseBranch()), false,
                             branch_ends_with_break);
            }
          } else if (s->Kind() == CStatementKind::kSwitchStatement) {
            for (auto& sc : static_cast<SwitchStatement&>(*s).MutableCases()) {
              if (sc.body != nullptr) {
                bool sc_ends_with_break =
                    !sc.body->Statements().empty() &&
                    sc.body->Statements().back()->Kind() == CStatementKind::kBreakStatement;
                collect_labels(sc.body.get(), false, sc_ends_with_break);
              }
            }
          } else if (s->Kind() == CStatementKind::kCompoundStatement) {
            bool sub_ends_with_break =
                (i + 1 == stmts.size() && (parent_ends_with_break || is_root)) ||
                (i + 2 == stmts.size() &&
                 (stmts[i + 1]->Kind() == CStatementKind::kBreakStatement ||
                  stmts[i + 1]->Kind() == CStatementKind::kReturnStatement));
            collect_labels(static_cast<CompoundStatement*>(s), false, sub_ends_with_break);
          }
        }
      };
  collect_labels(root_body, true, true);

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

  absl::flat_hash_set<std::string> remaining_gotos;
  std::function<void(const CStatement&)> collect_remaining_gotos = [&](const CStatement& s) {
    if (s.Kind() == CStatementKind::kGotoStatement) {
      remaining_gotos.insert(std::string(static_cast<const GotoStatement&>(s).Label()));
    } else if (s.Kind() == CStatementKind::kCompoundStatement) {
      for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
        if (child != nullptr) collect_remaining_gotos(*child);
      }
    } else if (s.Kind() == CStatementKind::kIfStatement) {
      const auto& ifs = static_cast<const IfStatement&>(s);
      collect_remaining_gotos(ifs.ThenBranch());
      if (ifs.ElseBranch() != nullptr) collect_remaining_gotos(*ifs.ElseBranch());
    } else if (s.Kind() == CStatementKind::kSwitchStatement) {
      for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
        if (sc.body != nullptr) collect_remaining_gotos(*sc.body);
      }
    } else if (s.Kind() == CStatementKind::kWhileStatement) {
      collect_remaining_gotos(static_cast<const WhileStatement&>(s).Body());
    } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
      collect_remaining_gotos(static_cast<const DoWhileStatement&>(s).Body());
    } else if (s.Kind() == CStatementKind::kForStatement) {
      collect_remaining_gotos(static_cast<const ForStatement&>(s).Body());
    }
  };
  collect_remaining_gotos(*root_body);

  std::function<void(CompoundStatement*)> prune_dead_case_labels = [&](CompoundStatement* comp) {
    if (comp == nullptr) return;
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size();) {
      if (stmts[i] == nullptr) {
        ++i;
        continue;
      }
      if (stmts[i]->Kind() == CStatementKind::kLabelStatement) {
        std::string lbl(static_cast<const LabelStatement&>(*stmts[i]).Label());
        if (!remaining_gotos.contains(lbl)) {
          stmts.erase(stmts.begin() + i);
          continue;
        }
      } else {
        ForEachSubCompoundStatement(stmts[i].get(), prune_dead_case_labels);
      }
      ++i;
    }
  };
  prune_dead_case_labels(root_body);
}

void RewriteFloatInGprAndStackCallArguments(CompoundStatement* comp,
                                            const std::vector<CParameter>& parameters) {
  if (comp == nullptr) return;
  absl::flat_hash_set<std::string> defined_vars;
  for (const auto& p : parameters) {
    defined_vars.insert(p.name);
  }
  std::function<void(const CExpression&)> collect_defined_e = [&](const CExpression& e) {
    switch (e.Kind()) {
      case CExpressionKind::kAssignmentExpression: {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          defined_vars.insert(static_cast<const IdentifierExpression&>(a.Lhs()).Name());
        } else {
          collect_defined_e(a.Lhs());
        }
        collect_defined_e(a.Rhs());
        break;
      }
      case CExpressionKind::kUnaryExpression: {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if (u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
          defined_vars.insert(static_cast<const IdentifierExpression&>(u.Operand()).Name());
        }
        collect_defined_e(u.Operand());
        break;
      }
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(e);
        collect_defined_e(b.Lhs());
        collect_defined_e(b.Rhs());
        break;
      }
      case CExpressionKind::kCastExpression:
        collect_defined_e(static_cast<const CastExpression&>(e).Operand());
        break;
      case CExpressionKind::kCallExpression: {
        const auto& c = static_cast<const CallExpression&>(e);
        collect_defined_e(c.Callee());
        for (const auto& arg : c.Arguments()) {
          if (arg != nullptr) collect_defined_e(*arg);
        }
        break;
      }
      default:
        break;
    }
  };
  std::function<void(const CStatement&)> collect_defined_s = [&](const CStatement& s) {
    switch (s.Kind()) {
      case CStatementKind::kExpressionStatement:
        collect_defined_e(static_cast<const ExpressionStatement&>(s).Expression());
        break;
      case CStatementKind::kReturnStatement: {
        const auto& r = static_cast<const ReturnStatement&>(s);
        if (r.ReturnValue() != nullptr) collect_defined_e(*r.ReturnValue());
        break;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        collect_defined_e(ifs.Condition());
        collect_defined_s(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) collect_defined_s(*ifs.ElseBranch());
        break;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(s);
        collect_defined_e(ws.Condition());
        collect_defined_s(ws.Body());
        break;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(s);
        collect_defined_s(dws.Body());
        collect_defined_e(dws.Condition());
        break;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) collect_defined_s(*fs.Init());
        if (fs.Condition() != nullptr) collect_defined_e(*fs.Condition());
        if (fs.Step() != nullptr) collect_defined_e(*fs.Step());
        collect_defined_s(fs.Body());
        break;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& sw = static_cast<const SwitchStatement&>(s);
        collect_defined_e(sw.Condition());
        for (const auto& sc : sw.Cases()) {
          if (sc.body != nullptr) collect_defined_s(*sc.body);
        }
        break;
      }
      case CStatementKind::kCompoundStatement:
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          if (child != nullptr) collect_defined_s(*child);
        }
        break;
      default:
        break;
    }
  };
  collect_defined_s(*comp);

  auto is_fp_var_name = [](std::string_view name) -> bool {
    if (name.size() < 2 || name[0] != 'f' || !std::isdigit(name[1])) return false;
    for (size_t i = 1; i < name.size(); ++i) {
      if (!std::isdigit(name[i])) {
        return i == name.size() - 2 && name[i] == '_' && name[i + 1] == 'd';
      }
    }
    return true;
  };

  std::function<bool(const CExpression&)> is_float_arg0 = [&](const CExpression& arg0) -> bool {
    if (arg0.Kind() == CExpressionKind::kFloatLiteral) return true;
    if (arg0.Kind() == CExpressionKind::kIdentifier) {
      return is_fp_var_name(static_cast<const IdentifierExpression&>(arg0).Name());
    }
    if (arg0.Kind() == CExpressionKind::kCastExpression) {
      const auto& ct = static_cast<const CastExpression&>(arg0).TargetType();
      return ct.pointer_depth == 0 && (ct.name == "f32" || ct.name == "f64");
    }
    if (arg0.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(arg0);
      if (un.Op() == "-" || un.Op() == "*(f32*)&" || un.Op() == "*(f32 *)&" ||
          un.Op() == "*(f64*)&" || un.Op() == "*(f64 *)&") {
        return true;
      }
      if (un.Op() == "*" && un.Operand().Kind() == CExpressionKind::kCastExpression) {
        const auto& ct = static_cast<const CastExpression&>(un.Operand()).TargetType();
        return ct.pointer_depth == 1 && (ct.name == "f32" || ct.name == "f64");
      }
    }
    if (arg0.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(arg0);
      if (bin.Op() == "+" || bin.Op() == "-" || bin.Op() == "*" || bin.Op() == "/") {
        return is_float_arg0(bin.Lhs()) || is_float_arg0(bin.Rhs());
      }
    }
    return false;
  };

  auto extract_s32_bitcast_of_f32 = [&](const CExpression& arg) -> std::unique_ptr<CExpression> {
    if (arg.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(arg);
      if (un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") {
        const CExpression& op = un.Operand();
        if (op.Kind() == CExpressionKind::kIdentifier) {
          std::string_view op_name = static_cast<const IdentifierExpression&>(op).Name();
          if ((is_fp_var_name(op_name) && !absl::EndsWith(op_name, "_d")) ||
              (!op_name.empty() && !std::isalpha(static_cast<unsigned char>(op_name[0])) &&
               op_name[0] != '_')) {
            return op.Clone();
          }
        } else if (op.Kind() == CExpressionKind::kFloatLiteral) {
          return op.Clone();
        } else if (op.Kind() == CExpressionKind::kCastExpression) {
          const auto& ct = static_cast<const CastExpression&>(op).TargetType();
          if (ct.pointer_depth == 0 && ct.name == "f32") {
            return op.Clone();
          }
        } else if (op.Kind() == CExpressionKind::kUnaryExpression) {
          const auto& inner_u = static_cast<const UnaryExpression&>(op);
          if (inner_u.Op() == "*(f32*)&" || inner_u.Op() == "*(f32 *)&") {
            return op.Clone();
          }
          if (inner_u.Op() == "*" && inner_u.Operand().Kind() == CExpressionKind::kCastExpression) {
            const auto& ct = static_cast<const CastExpression&>(inner_u.Operand()).TargetType();
            if (ct.pointer_depth == 1 &&
                (ct.name == "f32" || ct.name == "const f32" || ct.name == "volatile f32")) {
              return op.Clone();
            }
          }
        } else if (op.Kind() == CExpressionKind::kBinaryExpression) {
          return op.Clone();
        }
      }
    }
    return nullptr;
  };

  auto rewrite_call = [&](const CallExpression& call,
                          std::string_view ret_type) -> std::unique_ptr<CExpression> {
    if (call.Callee().Kind() != CExpressionKind::kIdentifier) return nullptr;
    std::string callee_name = static_cast<const IdentifierExpression&>(call.Callee()).Name();
    if (callee_name.empty() || callee_name == "N64Sqrtf" || callee_name == "N64Sqrt" ||
        callee_name == "fabsf" || callee_name == "fabs" || callee_name == "roundf" ||
        callee_name == "round" || callee_name == "ceilf" || callee_name == "ceil" ||
        callee_name == "floorf" || callee_name == "floor" || callee_name == "truncf" ||
        callee_name == "trunc") {
      return nullptr;
    }
    const auto& args = call.Arguments();
    if (args.empty()) return nullptr;
    const bool arg0_is_float = is_float_arg0(*args[0]);

    std::vector<std::string> existing_param_types;
    if (absl::StartsWith(callee_name, "((")) {
      size_t params_open = callee_name.find("(*)(");
      size_t params_close = callee_name.rfind("))");
      if (params_open == std::string::npos || params_close == std::string::npos ||
          params_close <= params_open + 4) {
        return nullptr;
      }
      std::string params_str =
          callee_name.substr(params_open + 4, params_close - (params_open + 4));
      size_t pos = 0;
      while (pos <= params_str.size()) {
        size_t comma = params_str.find(',', pos);
        size_t end = (comma == std::string::npos) ? params_str.size() : comma;
        while (pos < end && params_str[pos] == ' ') ++pos;
        size_t r = end;
        while (r > pos && params_str[r - 1] == ' ') --r;
        existing_param_types.push_back(params_str.substr(pos, r - pos));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      if (existing_param_types.size() != args.size()) {
        return nullptr;
      }
      callee_name = callee_name.substr(params_close + 2);
      if (!callee_name.empty() && callee_name.back() == ')') {
        callee_name.pop_back();
      }
    }

    std::function<std::string(const CExpression&)> infer_fp_arg_type =
        [&](const CExpression& e) -> std::string {
      if (e.Kind() == CExpressionKind::kIdentifier &&
          absl::EndsWith(static_cast<const IdentifierExpression&>(e).Name(), "_d")) {
        return "f64";
      }
      if (e.Kind() == CExpressionKind::kCastExpression &&
          static_cast<const CastExpression&>(e).TargetType().name == "f64") {
        return "f64";
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op() == "*(f64*)&" || un.Op() == "*(f64 *)&") return "f64";
        if (un.Op() == "*" && un.Operand().Kind() == CExpressionKind::kCastExpression &&
            static_cast<const CastExpression&>(un.Operand()).TargetType().name == "f64") {
          return "f64";
        }
        return infer_fp_arg_type(un.Operand());
      }
      if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        if (infer_fp_arg_type(bin.Lhs()) == "f64" || infer_fp_arg_type(bin.Rhs()) == "f64") {
          return "f64";
        }
      }
      return "f32";
    };

    auto extract_s64_bitcast_of_f64 = [&](const CExpression& arg) -> std::unique_ptr<CExpression> {
      if (arg.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(arg);
        if (un.Op() == "*(s64*)&" || un.Op() == "*(s64 *)&") {
          return un.Operand().Clone();
        }
      }
      return nullptr;
    };

    auto extract_fpr_half_reg = [&](const CExpression& arg) -> int {
      if (arg.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(arg);
        if ((un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") &&
            un.Operand().Kind() == CExpressionKind::kIdentifier) {
          std::string_view name = static_cast<const IdentifierExpression&>(un.Operand()).Name();
          if (name.size() >= 2 && name[0] == 'f' && !absl::EndsWith(name, "_d")) {
            int reg = -1;
            if (absl::SimpleAtoi(name.substr(1), &reg) && reg >= 0) {
              return reg;
            }
          }
        }
      }
      return -1;
    };

    bool has_float_arg = false;
    std::vector<std::string> param_types;
    std::vector<std::unique_ptr<CExpression>> new_args;
    param_types.reserve(args.size());
    new_args.reserve(args.size());

    param_types.push_back(existing_param_types.empty()
                              ? (arg0_is_float ? infer_fp_arg_type(*args[0]) : "s32")
                              : existing_param_types[0]);
    if (arg0_is_float && existing_param_types.empty() && param_types[0] == "f32") {
      has_float_arg = true;
    }
    new_args.push_back(args[0]->Clone());
    int abi_slot = (param_types[0] == "f64") ? 2 : 1;

    for (size_t k = 1; k < args.size(); ++k) {
      if (k == 1 && arg0_is_float) {
        if (is_float_arg0(*args[1])) {
          std::string p1_type =
              existing_param_types.empty() ? infer_fp_arg_type(*args[1]) : existing_param_types[1];
          if (existing_param_types.empty() && p1_type == "f32") {
            has_float_arg = true;
          }
          if (p1_type == "f64") {
            if (abi_slot % 2 != 0) ++abi_slot;
            abi_slot += 2;
          } else {
            ++abi_slot;
          }
          param_types.push_back(std::move(p1_type));
          new_args.push_back(args[1]->Clone());
          continue;
        }
      }

      if (abi_slot >= 2 && (abi_slot % 2 == 0) && k + 1 < args.size()) {
        std::unique_ptr<CExpression> f64_expr;
        if (abi_slot >= 4) {
          auto s64_unwrapped = extract_s64_bitcast_of_f64(*args[k]);
          std::string hi_stack_name = absl::StrFormat("var_sp_%d", 16 + (abi_slot + 1 - 4) * 4);
          if (s64_unwrapped != nullptr && args[k + 1]->Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(*args[k + 1]).Name() == hi_stack_name) {
            f64_expr = std::move(s64_unwrapped);
          }
        }
        if (f64_expr == nullptr) {
          int odd_reg = extract_fpr_half_reg(*args[k]);
          int even_reg = extract_fpr_half_reg(*args[k + 1]);
          if (even_reg >= 0 && (even_reg % 2 == 0) && odd_reg == even_reg + 1) {
            f64_expr = CExpression::Identifier(absl::StrFormat("f%d_d", even_reg));
          }
        }
        if (f64_expr != nullptr) {
          if (!new_args.empty() && new_args.back()->Kind() == CExpressionKind::kIdentifier) {
            std::string_view prev_name =
                static_cast<const IdentifierExpression&>(*new_args.back()).Name();
            bool is_unassigned_odd_padding = false;
            if (abi_slot >= 6) {
              std::string pad_stack_name =
                  absl::StrFormat("var_sp_%d", 16 + (abi_slot - 1 - 4) * 4);
              if (prev_name == pad_stack_name) {
                is_unassigned_odd_padding = true;
              }
            } else if (abi_slot == 2 || abi_slot == 4) {
              std::string pad_arg_name = absl::StrFormat("arg%d", abi_slot - 1);
              if (prev_name == pad_arg_name) {
                is_unassigned_odd_padding = true;
              }
            }
            if (is_unassigned_odd_padding) {
              new_args.pop_back();
              param_types.pop_back();
            }
          }
          has_float_arg = true;
          param_types.push_back("f64");
          new_args.push_back(std::move(f64_expr));
          ++k;
          abi_slot += 2;
          continue;
        }
      }

      auto unwrapped = extract_s32_bitcast_of_f32(*args[k]);
      if (unwrapped != nullptr) {
        has_float_arg = true;
        param_types.push_back("f32");
        new_args.push_back(std::move(unwrapped));
        ++abi_slot;
      } else {
        std::string p_type = existing_param_types.empty() ? "s32" : existing_param_types[k];
        if (p_type == "f64") {
          if (abi_slot % 2 != 0) ++abi_slot;
          abi_slot += 2;
        } else {
          ++abi_slot;
        }
        param_types.push_back(std::move(p_type));
        new_args.push_back(args[k]->Clone());
      }
    }
    if (!has_float_arg) return nullptr;

    std::string sig;
    for (size_t k = 0; k < param_types.size(); ++k) {
      if (k > 0) sig += ", ";
      sig += param_types[k];
    }
    std::string cast_callee = absl::StrFormat("((%s (*)(%s))%s)", ret_type, sig, callee_name);
    return CExpression::Call(CExpression::Identifier(std::move(cast_callee)), std::move(new_args));
  };

  std::function<std::unique_ptr<CExpression>(const CExpression&, std::string_view)> rewrite_expr =
      [&](const CExpression& expr,
          std::string_view default_ret_type) -> std::unique_ptr<CExpression> {
    switch (expr.Kind()) {
      case CExpressionKind::kCallExpression: {
        const auto& call = static_cast<const CallExpression&>(expr);
        auto rewritten = rewrite_call(call, default_ret_type);
        if (rewritten != nullptr) return rewritten;
        std::vector<std::unique_ptr<CExpression>> args;
        args.reserve(call.Arguments().size());
        for (const auto& arg : call.Arguments()) {
          args.push_back(rewrite_expr(*arg, "s32"));
        }
        return CExpression::Call(call.Callee().Clone(), std::move(args));
      }
      case CExpressionKind::kAssignmentExpression: {
        const auto& a = static_cast<const AssignmentExpression&>(expr);
        std::string_view rhs_ret_type = "s32";
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          if (lhs_name == "f0") {
            rhs_ret_type = "f32";
          } else if (lhs_name == "f0_d") {
            rhs_ret_type = "f64";
          }
        }
        return CExpression::Assignment(a.Op(), rewrite_expr(a.Lhs(), "s32"),
                                       rewrite_expr(a.Rhs(), rhs_ret_type));
      }
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(expr);
        return CExpression::Binary(b.Op(), rewrite_expr(b.Lhs(), "s32"),
                                   rewrite_expr(b.Rhs(), "s32"));
      }
      case CExpressionKind::kUnaryExpression: {
        const auto& u = static_cast<const UnaryExpression&>(expr);
        return CExpression::Unary(u.Op(), rewrite_expr(u.Operand(), "s32"), u.IsPrefix());
      }
      case CExpressionKind::kCastExpression: {
        const auto& c = static_cast<const CastExpression&>(expr);
        return CExpression::Cast(c.TargetType(), rewrite_expr(c.Operand(), "s32"));
      }
      case CExpressionKind::kArrayIndexExpression: {
        const auto& ai = static_cast<const ArrayIndexExpression&>(expr);
        return CExpression::ArrayIndex(rewrite_expr(ai.Array(), "s32"),
                                       rewrite_expr(ai.Index(), "s32"));
      }
      case CExpressionKind::kMemberAccessExpression: {
        const auto& ma = static_cast<const MemberAccessExpression&>(expr);
        return CExpression::MemberAccess(rewrite_expr(ma.Object(), "s32"), ma.MemberName(),
                                         ma.IsArrow());
      }
      case CExpressionKind::kTernaryExpression: {
        const auto& t = static_cast<const TernaryExpression&>(expr);
        return CExpression::Ternary(rewrite_expr(t.Condition(), "s32"),
                                    rewrite_expr(t.TrueExpression(), "s32"),
                                    rewrite_expr(t.FalseExpression(), "s32"));
      }
      default:
        return expr.Clone();
    }
  };

  for (auto& stmt : comp->MutableStatements()) {
    if (stmt->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*stmt);
      std::string_view top_ret =
          (es.Expression().Kind() == CExpressionKind::kCallExpression) ? "void" : "s32";
      stmt = CStatement::Expression(rewrite_expr(es.Expression(), top_ret));
    } else if (stmt->Kind() == CStatementKind::kReturnStatement) {
      const auto& rs = static_cast<const ReturnStatement&>(*stmt);
      if (rs.ReturnValue() != nullptr) {
        stmt = CStatement::Return(rewrite_expr(*rs.ReturnValue(), "s32"));
      }
    } else if (stmt->Kind() == CStatementKind::kIfStatement) {
      auto& ifs = static_cast<IfStatement&>(*stmt);
      if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(
            static_cast<CompoundStatement*>(ifs.MutableThenBranch()), parameters);
      }
      if (ifs.MutableElseBranch() != nullptr) {
        if (ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          RewriteFloatInGprAndStackCallArguments(
              static_cast<CompoundStatement*>(ifs.MutableElseBranch()), parameters);
        } else if (ifs.MutableElseBranch()->Kind() == CStatementKind::kIfStatement) {
          CompoundStatement wrapper;
          wrapper.AddStatement(ifs.MutableElseBranch()->Clone());
          RewriteFloatInGprAndStackCallArguments(&wrapper, parameters);
          if (wrapper.Statements().size() == 1) {
            stmt = CStatement::If(rewrite_expr(ifs.Condition(), "s32"), ifs.ThenBranch().Clone(),
                                  std::move(wrapper.MutableStatements()[0]));
            continue;
          }
        }
      }
      stmt = CStatement::If(rewrite_expr(ifs.Condition(), "s32"), ifs.ThenBranch().Clone(),
                            ifs.ElseBranch() != nullptr ? ifs.ElseBranch()->Clone() : nullptr);
    } else {
      ForEachSubCompoundStatement(stmt.get(), [&](CompoundStatement* sub) {
        RewriteFloatInGprAndStackCallArguments(sub, parameters);
      });
    }
  }
}

void TrimUnassignedTrailingCallArgRegisters(
    CompoundStatement* body, const std::vector<CParameter>& parameters,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts) {
  if (body == nullptr) return;
  absl::flat_hash_set<std::string> defined_vars;
  for (const auto& p : parameters) {
    defined_vars.insert(p.name);
  }
  std::function<void(const CExpression&)> collect_e = [&](const CExpression& e) {
    switch (e.Kind()) {
      case CExpressionKind::kAssignmentExpression: {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          defined_vars.insert(static_cast<const IdentifierExpression&>(a.Lhs()).Name());
        } else {
          collect_e(a.Lhs());
        }
        collect_e(a.Rhs());
        break;
      }
      case CExpressionKind::kUnaryExpression: {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if (u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
          defined_vars.insert(static_cast<const IdentifierExpression&>(u.Operand()).Name());
        }
        collect_e(u.Operand());
        break;
      }
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(e);
        collect_e(b.Lhs());
        collect_e(b.Rhs());
        break;
      }
      case CExpressionKind::kCastExpression:
        collect_e(static_cast<const CastExpression&>(e).Operand());
        break;
      case CExpressionKind::kCallExpression: {
        const auto& c = static_cast<const CallExpression&>(e);
        collect_e(c.Callee());
        for (const auto& arg : c.Arguments()) {
          if (arg != nullptr) collect_e(*arg);
        }
        break;
      }
      default:
        break;
    }
  };
  std::function<void(const CStatement&)> collect_s = [&](const CStatement& s) {
    switch (s.Kind()) {
      case CStatementKind::kExpressionStatement:
        collect_e(static_cast<const ExpressionStatement&>(s).Expression());
        break;
      case CStatementKind::kReturnStatement: {
        const auto& r = static_cast<const ReturnStatement&>(s);
        if (r.ReturnValue() != nullptr) collect_e(*r.ReturnValue());
        break;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        collect_e(ifs.Condition());
        collect_s(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) collect_s(*ifs.ElseBranch());
        break;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(s);
        collect_e(ws.Condition());
        collect_s(ws.Body());
        break;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(s);
        collect_s(dws.Body());
        collect_e(dws.Condition());
        break;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) collect_s(*fs.Init());
        if (fs.Condition() != nullptr) collect_e(*fs.Condition());
        if (fs.Step() != nullptr) collect_e(*fs.Step());
        collect_s(fs.Body());
        break;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& sw = static_cast<const SwitchStatement&>(s);
        collect_e(sw.Condition());
        for (const auto& sc : sw.Cases()) {
          if (sc.body != nullptr) collect_s(*sc.body);
        }
        break;
      }
      case CStatementKind::kCompoundStatement:
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          if (child != nullptr) collect_s(*child);
        }
        break;
      default:
        break;
    }
  };
  collect_s(*body);

  absl::flat_hash_set<std::string> defined_so_far;
  for (const auto& p : parameters) {
    defined_so_far.insert(p.name);
  }
  bool in_entry_linear_prefix = true;

  std::function<std::unique_ptr<CExpression>(const CExpression&, std::string_view)> rewrite_e =
      [&](const CExpression& e, std::string_view ret_type) -> std::unique_ptr<CExpression> {
    switch (e.Kind()) {
      case CExpressionKind::kCallExpression: {
        const auto& c = static_cast<const CallExpression&>(e);
        std::vector<std::unique_ptr<CExpression>> args;
        for (const auto& arg : c.Arguments()) {
          args.push_back(arg != nullptr ? rewrite_e(*arg, "s32") : nullptr);
        }
        std::string callee_name = (c.Callee().Kind() == CExpressionKind::kIdentifier)
                                      ? static_cast<const IdentifierExpression&>(c.Callee()).Name()
                                      : "";
        std::string raw_callee_name = callee_name;
        if (absl::StartsWith(raw_callee_name, "((") && absl::EndsWith(raw_callee_name, ")")) {
          size_t proto_end = raw_callee_name.rfind("))");
          if (proto_end != std::string::npos && proto_end + 2 < raw_callee_name.size() - 1) {
            raw_callee_name =
                raw_callee_name.substr(proto_end + 2, raw_callee_name.size() - (proto_end + 2) - 1);
          }
        }
        bool known_param_count =
            (!raw_callee_name.empty() && function_parameter_counts != nullptr &&
             function_parameter_counts->contains(raw_callee_name));
        const bool has_explicit_prototype =
            absl::StartsWith(callee_name, "((") && !absl::StrContains(callee_name, "(*)()");
        const size_t orig_args_size = args.size();
        if (!has_explicit_prototype && args.size() <= 4) {
          while (!args.empty() && args.back() != nullptr &&
                 args.back()->Kind() == CExpressionKind::kIdentifier) {
            const std::string& aname =
                static_cast<const IdentifierExpression&>(*args.back()).Name();
            bool is_arg_reg = (!known_param_count && (aname == "arg0" || aname == "temp_a0")) ||
                              (aname == "arg1" || aname == "arg2" || aname == "arg3" ||
                               aname == "temp_a1" || aname == "temp_a2" || aname == "temp_a3");
            if (is_arg_reg && (!defined_vars.contains(aname) ||
                               (in_entry_linear_prefix && !defined_so_far.contains(aname)))) {
              args.pop_back();
            } else {
              break;
            }
          }
        }
        auto callee_expr = rewrite_e(c.Callee(), "s32");
        if (known_param_count && args.size() < orig_args_size) {
          std::string sig = args.empty() ? "void" : "";
          for (size_t k = 0; k < args.size(); ++k) {
            if (k > 0) sig += ", ";
            sig += "s32";
          }
          callee_expr = CExpression::Identifier(
              absl::StrFormat("((%s (*)(%s))%s)", ret_type, sig, raw_callee_name));
        }
        return CExpression::Call(std::move(callee_expr), std::move(args));
      }
      case CExpressionKind::kAssignmentExpression: {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        std::string_view rhs_ret_type = "s32";
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          if (lhs_name == "f0") {
            rhs_ret_type = "f32";
          } else if (lhs_name == "f0_d") {
            rhs_ret_type = "f64";
          }
        }
        auto rhs = rewrite_e(a.Rhs(), rhs_ret_type);
        auto lhs = rewrite_e(a.Lhs(), "s32");
        if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          defined_so_far.insert(static_cast<const IdentifierExpression&>(a.Lhs()).Name());
        }
        return CExpression::Assignment(a.Op(), std::move(lhs), std::move(rhs));
      }
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(e);
        return CExpression::Binary(b.Op(), rewrite_e(b.Lhs(), "s32"), rewrite_e(b.Rhs(), "s32"));
      }
      case CExpressionKind::kUnaryExpression: {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if (u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
          defined_so_far.insert(static_cast<const IdentifierExpression&>(u.Operand()).Name());
        }
        return CExpression::Unary(u.Op(), rewrite_e(u.Operand(), "s32"), u.IsPrefix());
      }
      case CExpressionKind::kCastExpression: {
        const auto& c = static_cast<const CastExpression&>(e);
        return CExpression::Cast(c.TargetType(), rewrite_e(c.Operand(), "s32"));
      }
      default:
        return e.Clone();
    }
  };
  std::function<void(CompoundStatement*)> rewrite_comp = [&](CompoundStatement* comp) {
    if (comp == nullptr) return;
    auto& stmts = comp->MutableStatements();
    for (auto& s : stmts) {
      if (s == nullptr) continue;
      switch (s->Kind()) {
        case CStatementKind::kLabelStatement:
          in_entry_linear_prefix = false;
          break;
        case CStatementKind::kExpressionStatement: {
          const auto& es = static_cast<const ExpressionStatement&>(*s);
          s = CStatement::Expression(rewrite_e(es.Expression(), "s32"));
          break;
        }
        case CStatementKind::kReturnStatement: {
          const auto& rs = static_cast<const ReturnStatement&>(*s);
          if (rs.ReturnValue() != nullptr) {
            s = CStatement::Return(rewrite_e(*rs.ReturnValue(), "s32"));
          }
          break;
        }
        case CStatementKind::kIfStatement: {
          auto& ifs = static_cast<IfStatement&>(*s);
          auto cond = rewrite_e(ifs.Condition(), "s32");
          auto before_if = defined_so_far;
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_comp(static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
          }
          auto after_then = defined_so_far;
          defined_so_far = before_if;
          if (ifs.MutableElseBranch() != nullptr &&
              ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_comp(static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
          }
          for (const auto& v : after_then) {
            defined_so_far.insert(v);
          }
          s = CStatement::If(std::move(cond), ifs.ThenBranch().Clone(),
                             ifs.ElseBranch() != nullptr ? ifs.ElseBranch()->Clone() : nullptr);
          break;
        }
        case CStatementKind::kWhileStatement: {
          in_entry_linear_prefix = false;
          auto& ws = static_cast<WhileStatement&>(*s);
          if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_comp(static_cast<CompoundStatement*>(ws.MutableBody()));
          }
          s = CStatement::While(rewrite_e(ws.Condition(), "s32"), ws.Body().Clone());
          break;
        }
        case CStatementKind::kDoWhileStatement: {
          in_entry_linear_prefix = false;
          auto& dws = static_cast<DoWhileStatement&>(*s);
          if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_comp(static_cast<CompoundStatement*>(dws.MutableBody()));
          }
          s = CStatement::DoWhile(dws.Body().Clone(), rewrite_e(dws.Condition(), "s32"));
          break;
        }
        case CStatementKind::kForStatement: {
          in_entry_linear_prefix = false;
          auto& fs = static_cast<ForStatement&>(*s);
          if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_comp(static_cast<CompoundStatement*>(fs.MutableBody()));
          }
          break;
        }
        case CStatementKind::kSwitchStatement: {
          auto& sw = static_cast<SwitchStatement&>(*s);
          for (auto& sc : sw.MutableCases()) {
            if (sc.body != nullptr) rewrite_comp(sc.body.get());
          }
          break;
        }
        case CStatementKind::kCompoundStatement:
          rewrite_comp(static_cast<CompoundStatement*>(s.get()));
          break;
        default:
          break;
      }
    }
  };
  rewrite_comp(body);
}

}  // namespace rom_nom_nom
