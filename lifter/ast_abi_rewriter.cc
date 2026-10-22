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

bool StatementHasCall(const CStatement& s) {
  return StatementContainsCall(s, /*ignore_math_intrinsics=*/true);
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

void CoalescePrologueParameterCopies(CompoundStatement* comp,
                                     const std::vector<CParameter>& parameters,
                                     uint32_t frame_size) {
  if (comp == nullptr) return;
  absl::flat_hash_set<std::string> param_names;
  absl::flat_hash_set<std::string> float_param_names;
  for (const auto& p : parameters) {
    param_names.insert(p.name);
    if (p.type.ToString() == "f32" && absl::StartsWith(p.name, "arg")) {
      float_param_names.insert(p.name);
    }
  }

  auto is_assigned = [&](std::string_view name) {
    bool assigned = false;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& e) {
      if (assigned) return;
      if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          if (static_cast<const IdentifierExpression&>(a.Lhs()).Name() == name) {
            assigned = true;
            return;
          }
        }
        check_expr(a.Lhs());
        check_expr(a.Rhs());
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if ((u.Op() == "++" || u.Op() == "--") &&
            u.Operand().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(u.Operand()).Name() == name) {
          assigned = true;
          return;
        }
        check_expr(u.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check_expr(static_cast<const BinaryExpression&>(e).Lhs());
        check_expr(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check_expr(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check_expr(static_cast<const TernaryExpression&>(e).Condition());
        check_expr(static_cast<const TernaryExpression&>(e).TrueExpression());
        check_expr(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check_expr(*arg);
        }
      }
    };

    std::function<void(const CStatement&)> check_stmt = [&](const CStatement& s) {
      if (assigned) return;
      if (s.Kind() == CStatementKind::kExpressionStatement) {
        check_expr(static_cast<const ExpressionStatement&>(s).Expression());
      } else if (s.Kind() == CStatementKind::kCompoundStatement) {
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          check_stmt(*child);
        }
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        check_expr(ifs.Condition());
        check_stmt(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) check_stmt(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kWhileStatement) {
        check_expr(static_cast<const WhileStatement&>(s).Condition());
        check_stmt(static_cast<const WhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
        check_stmt(static_cast<const DoWhileStatement&>(s).Body());
        check_expr(static_cast<const DoWhileStatement&>(s).Condition());
      } else if (s.Kind() == CStatementKind::kForStatement) {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) check_stmt(*fs.Init());
        if (fs.Condition() != nullptr) check_expr(*fs.Condition());
        if (fs.Step() != nullptr) check_expr(*fs.Step());
        check_stmt(fs.Body());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        check_expr(ss.Condition());
        for (const auto& sc : ss.Cases()) {
          if (sc.body != nullptr) check_stmt(*sc.body);
        }
      }
    };
    check_stmt(*comp);
    return assigned;
  };

  absl::flat_hash_map<std::string, std::string> coalesced_aliases;
  std::vector<size_t> stmts_to_erase;

  auto count_assignments = [&](std::string_view name) {
    int count = 0;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& e) {
      if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        if (a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(a.Lhs()).Name() == name) {
          count++;
        }
        check_expr(a.Lhs());
        check_expr(a.Rhs());
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if ((u.Op() == "++" || u.Op() == "--") &&
            u.Operand().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(u.Operand()).Name() == name) {
          count++;
        }
        check_expr(u.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check_expr(static_cast<const BinaryExpression&>(e).Lhs());
        check_expr(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check_expr(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check_expr(static_cast<const TernaryExpression&>(e).Condition());
        check_expr(static_cast<const TernaryExpression&>(e).TrueExpression());
        check_expr(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check_expr(*arg);
        }
      }
    };

    std::function<void(const CStatement&)> check_stmt = [&](const CStatement& s) {
      if (s.Kind() == CStatementKind::kExpressionStatement) {
        check_expr(static_cast<const ExpressionStatement&>(s).Expression());
      } else if (s.Kind() == CStatementKind::kCompoundStatement) {
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          check_stmt(*child);
        }
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        check_expr(ifs.Condition());
        check_stmt(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) check_stmt(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kWhileStatement) {
        check_expr(static_cast<const WhileStatement&>(s).Condition());
        check_stmt(static_cast<const WhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
        check_stmt(static_cast<const DoWhileStatement&>(s).Body());
        check_expr(static_cast<const DoWhileStatement&>(s).Condition());
      } else if (s.Kind() == CStatementKind::kForStatement) {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) check_stmt(*fs.Init());
        if (fs.Condition() != nullptr) check_expr(*fs.Condition());
        if (fs.Step() != nullptr) check_expr(*fs.Step());
        check_stmt(fs.Body());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        check_expr(ss.Condition());
        for (const auto& sc : ss.Cases()) {
          if (sc.body != nullptr) check_stmt(*sc.body);
        }
      }
    };
    check_stmt(*comp);
    return count;
  };

  for (size_t i = 0; i < comp->Statements().size(); ++i) {
    const auto& s = comp->Statements()[i];
    if (s->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*s);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            a.Rhs().Kind() == CExpressionKind::kIdentifier) {
          std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          std::string rhs = static_cast<const IdentifierExpression&>(a.Rhs()).Name();
          bool is_s_reg = (absl::StartsWith(lhs, "temp_s") || lhs == "fp" ||
                           (lhs.size() == 2 && lhs[0] == 's' && std::isdigit(lhs[1])));
          bool is_f_reg = (lhs.size() >= 2 && lhs[0] == 'f' && std::isdigit(lhs[1]));
          bool is_callee_saved_f_reg = (lhs == "f20" || lhs == "f22" || lhs == "f24" ||
                                        lhs == "f26" || lhs == "f28" || lhs == "f30");
          bool is_homed_param_slot = false;
          if (frame_size > 0 && rhs.size() == 4 && absl::StartsWith(rhs, "arg") && rhs[3] >= '0' &&
              rhs[3] <= '3') {
            uint32_t expected_off = frame_size + static_cast<uint32_t>(rhs[3] - '0') * 4;
            if (lhs == absl::StrCat("var_sp_", expected_off)) {
              is_homed_param_slot = true;
            }
          }
          bool is_single_assign_prologue_v_copy =
              (i < 3 && (lhs == "v0" || lhs == "v1") &&
               (rhs == "arg1" || rhs == "arg2" || rhs == "arg3") &&
               i + 1 < comp->Statements().size() &&
               (comp->Statements()[i + 1]->Kind() == CStatementKind::kIfStatement ||
                (i + 2 < comp->Statements().size() &&
                 comp->Statements()[i + 2]->Kind() == CStatementKind::kIfStatement)));
          if (param_names.contains(rhs) &&
              (is_s_reg || is_f_reg || is_homed_param_slot || is_single_assign_prologue_v_copy) &&
              !(float_param_names.contains(rhs) &&
                (is_s_reg || is_homed_param_slot || is_single_assign_prologue_v_copy ||
                 is_callee_saved_f_reg))) {
            std::string body_text = comp->ToString();
            bool lhs_addr_taken = StatementTakesAddressOf(*comp, lhs);
            bool v_copy_ok = true;
            if (is_single_assign_prologue_v_copy) {
              const CStatement& branch_s =
                  (comp->Statements()[i + 1]->Kind() == CStatementKind::kIfStatement)
                      ? *comp->Statements()[i + 1]
                      : *comp->Statements()[i + 2];
              std::function<bool(const CExpression&)> expr_uses_in_call_arg =
                  [&](const CExpression& e) -> bool {
                if (e.Kind() == CExpressionKind::kCallExpression) {
                  const auto& call = static_cast<const CallExpression&>(e);
                  if (!IsInlineAsmIntrinsicCall(call)) {
                    for (const auto& arg : call.Arguments()) {
                      if (arg != nullptr && ExpressionUsesVariable(*arg, lhs)) {
                        return true;
                      }
                    }
                  }
                  for (const auto& arg : call.Arguments()) {
                    if (arg != nullptr && expr_uses_in_call_arg(*arg)) return true;
                  }
                  return false;
                }
                if (e.Kind() == CExpressionKind::kUnaryExpression) {
                  return expr_uses_in_call_arg(static_cast<const UnaryExpression&>(e).Operand());
                }
                if (e.Kind() == CExpressionKind::kBinaryExpression) {
                  const auto& b = static_cast<const BinaryExpression&>(e);
                  return expr_uses_in_call_arg(b.Lhs()) || expr_uses_in_call_arg(b.Rhs());
                }
                if (e.Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& a = static_cast<const AssignmentExpression&>(e);
                  return expr_uses_in_call_arg(a.Lhs()) || expr_uses_in_call_arg(a.Rhs());
                }
                if (e.Kind() == CExpressionKind::kCastExpression) {
                  return expr_uses_in_call_arg(static_cast<const CastExpression&>(e).Operand());
                }
                if (e.Kind() == CExpressionKind::kTernaryExpression) {
                  const auto& t = static_cast<const TernaryExpression&>(e);
                  return expr_uses_in_call_arg(t.Condition()) ||
                         expr_uses_in_call_arg(t.TrueExpression()) ||
                         expr_uses_in_call_arg(t.FalseExpression());
                }
                return false;
              };
              std::function<bool(const CStatement&)> stmt_uses_in_call_arg =
                  [&](const CStatement& st) -> bool {
                if (st.Kind() == CStatementKind::kExpressionStatement) {
                  return expr_uses_in_call_arg(
                      static_cast<const ExpressionStatement&>(st).Expression());
                }
                if (st.Kind() == CStatementKind::kCompoundStatement) {
                  for (const auto& child : static_cast<const CompoundStatement&>(st).Statements()) {
                    if (child != nullptr && stmt_uses_in_call_arg(*child)) return true;
                  }
                } else if (st.Kind() == CStatementKind::kIfStatement) {
                  const auto& ifs = static_cast<const IfStatement&>(st);
                  if (expr_uses_in_call_arg(ifs.Condition()) ||
                      stmt_uses_in_call_arg(ifs.ThenBranch())) {
                    return true;
                  }
                  if (ifs.ElseBranch() != nullptr && stmt_uses_in_call_arg(*ifs.ElseBranch())) {
                    return true;
                  }
                }
                return false;
              };
              std::function<bool(const CStatement&)> stmt_returns_lhs =
                  [&](const CStatement& st) -> bool {
                if (st.Kind() == CStatementKind::kReturnStatement) {
                  const auto* rv = static_cast<const ReturnStatement&>(st).ReturnValue();
                  return rv != nullptr && ExpressionUsesVariable(*rv, lhs);
                }
                if (st.Kind() == CStatementKind::kCompoundStatement) {
                  for (const auto& child : static_cast<const CompoundStatement&>(st).Statements()) {
                    if (child != nullptr && stmt_returns_lhs(*child)) return true;
                  }
                } else if (st.Kind() == CStatementKind::kIfStatement) {
                  const auto& ifs = static_cast<const IfStatement&>(st);
                  if (stmt_returns_lhs(ifs.ThenBranch())) return true;
                  if (ifs.ElseBranch() != nullptr && stmt_returns_lhs(*ifs.ElseBranch())) {
                    return true;
                  }
                } else if (st.Kind() == CStatementKind::kWhileStatement) {
                  return stmt_returns_lhs(static_cast<const WhileStatement&>(st).Body());
                } else if (st.Kind() == CStatementKind::kDoWhileStatement) {
                  return stmt_returns_lhs(static_cast<const DoWhileStatement&>(st).Body());
                } else if (st.Kind() == CStatementKind::kForStatement) {
                  return stmt_returns_lhs(static_cast<const ForStatement&>(st).Body());
                } else if (st.Kind() == CStatementKind::kSwitchStatement) {
                  for (const auto& sc : static_cast<const SwitchStatement&>(st).Cases()) {
                    if (sc.body != nullptr && stmt_returns_lhs(*sc.body)) return true;
                  }
                }
                return false;
              };
              v_copy_ok = stmt_uses_in_call_arg(branch_s) && !stmt_returns_lhs(*comp);
            }
            bool homed_param_coalesce_ok = false;
            if (is_homed_param_slot && !is_assigned(rhs) &&
                !absl::StrContains(body_text, absl::StrCat("&", lhs, " +")) &&
                !absl::StrContains(body_text, absl::StrCat("&", lhs, ") +"))) {
              if (count_assignments(lhs) == 1) {
                homed_param_coalesce_ok = true;
              } else if (i < 6 && !lhs_addr_taken) {
                size_t first_rhs = body_text.find(rhs);
                if (first_rhs != std::string::npos &&
                    body_text.find(rhs, first_rhs + rhs.size()) == std::string::npos) {
                  homed_param_coalesce_ok = true;
                }
              }
            }
            if (!is_assigned(rhs) && v_copy_ok &&
                (is_homed_param_slot ? homed_param_coalesce_ok
                                     : (count_assignments(lhs) == 1 && !lhs_addr_taken)) &&
                !absl::StrContains(body_text, absl::StrCat("(*)())", lhs))) {
              coalesced_aliases[lhs] = rhs;
              stmts_to_erase.push_back(i);
            }
          }
        }
      }
    }
  }

  if (coalesced_aliases.empty()) return;

  auto apply_aliases_to_expr =
      [&](std::unique_ptr<CExpression> expr) -> std::unique_ptr<CExpression> {
    for (const auto& [from, to] : coalesced_aliases) {
      expr = SubstituteIdentifier(*expr, from, *CExpression::Identifier(to));
    }
    return expr;
  };

  std::function<std::unique_ptr<CStatement>(std::unique_ptr<CStatement>)> rewrite_stmt =
      [&](std::unique_ptr<CStatement> s) -> std::unique_ptr<CStatement> {
    if (s == nullptr) return nullptr;
    switch (s->Kind()) {
      case CStatementKind::kCompoundStatement: {
        auto* comp_stmt = static_cast<CompoundStatement*>(s.get());
        for (auto& child : comp_stmt->MutableStatements()) {
          child = rewrite_stmt(std::move(child));
        }
        return s;
      }
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(*s);
        return CStatement::Expression(apply_aliases_to_expr(es.Expression().Clone()));
      }
      case CStatementKind::kReturnStatement: {
        const auto& ret = static_cast<const ReturnStatement&>(*s);
        if (ret.ReturnValue() != nullptr) {
          return CStatement::Return(apply_aliases_to_expr(ret.ReturnValue()->Clone()));
        }
        return CStatement::Return(nullptr);
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(*s);
        auto new_cond = apply_aliases_to_expr(ifs.Condition().Clone());
        auto new_then = rewrite_stmt(ifs.ThenBranch().Clone());
        std::unique_ptr<CStatement> new_else = nullptr;
        if (ifs.ElseBranch() != nullptr) {
          new_else = rewrite_stmt(ifs.ElseBranch()->Clone());
        }
        return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(*s);
        auto new_cond = apply_aliases_to_expr(ws.Condition().Clone());
        auto new_body = rewrite_stmt(ws.Body().Clone());
        return CStatement::While(std::move(new_cond), std::move(new_body));
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(*s);
        auto new_cond = apply_aliases_to_expr(dws.Condition().Clone());
        auto new_body = rewrite_stmt(dws.Body().Clone());
        return CStatement::DoWhile(std::move(new_body), std::move(new_cond));
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(*s);
        std::unique_ptr<CStatement> new_init = nullptr;
        if (fs.Init() != nullptr) {
          new_init = rewrite_stmt(fs.Init()->Clone());
        }
        std::unique_ptr<CExpression> new_cond = nullptr;
        if (fs.Condition() != nullptr) {
          new_cond = apply_aliases_to_expr(fs.Condition()->Clone());
        }
        std::unique_ptr<CExpression> new_step = nullptr;
        if (fs.Step() != nullptr) {
          new_step = apply_aliases_to_expr(fs.Step()->Clone());
        }
        auto new_body = rewrite_stmt(fs.Body().Clone());
        return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                               std::move(new_body));
      }
      case CStatementKind::kVariableDeclarationStatement: {
        const auto& vds = static_cast<const VariableDeclarationStatement&>(*s);
        std::unique_ptr<CExpression> new_init = nullptr;
        if (vds.Initializer() != nullptr) {
          new_init = apply_aliases_to_expr(vds.Initializer()->Clone());
        }
        return CStatement::VariableDeclaration(vds.Type(), vds.Name(), std::move(new_init),
                                               vds.ArraySize());
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(*s);
        auto new_cond = apply_aliases_to_expr(ss.Condition().Clone());
        std::vector<SwitchCase> new_cases;
        for (const auto& c : ss.Cases()) {
          SwitchCase sc = c.Clone();
          if (c.body != nullptr) {
            auto rewritten_body = rewrite_stmt(c.body->Clone());
            sc.body = std::unique_ptr<CompoundStatement>(
                static_cast<CompoundStatement*>(rewritten_body.release()));
          }
          new_cases.push_back(std::move(sc));
        }
        return CStatement::Switch(std::move(new_cond), std::move(new_cases));
      }
      default:
        return s;
    }
  };

  std::vector<std::unique_ptr<CStatement>> new_stmts;
  absl::flat_hash_set<size_t> erase_set(stmts_to_erase.begin(), stmts_to_erase.end());
  for (size_t i = 0; i < comp->Statements().size(); ++i) {
    if (!erase_set.contains(i)) {
      new_stmts.push_back(rewrite_stmt(std::move(comp->MutableStatements()[i])));
    }
  }
  comp->MutableStatements() = std::move(new_stmts);
}

void SinkCommonBranchTailArgAssignments(CompoundStatement* comp) {
  if (comp == nullptr) return;
  auto extract_arg_assign = [](const CStatement& s, std::string* lhs_out,
                               std::string* rhs_str_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
    std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
    if (lhs != "arg0" && lhs != "arg1" && lhs != "arg2" && lhs != "arg3" &&
        !absl::StartsWith(lhs, "var_sp_")) {
      return false;
    }
    if (a.Rhs().Kind() != CExpressionKind::kIdentifier &&
        a.Rhs().Kind() != CExpressionKind::kIntegerLiteral &&
        a.Rhs().Kind() != CExpressionKind::kFloatLiteral &&
        !(a.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
          (static_cast<const UnaryExpression&>(a.Rhs()).Op() == "*(s32*)&" ||
           static_cast<const UnaryExpression&>(a.Rhs()).Op() == "*(s32 *)&"))) {
      return false;
    }
    *lhs_out = std::move(lhs);
    *rhs_str_out = a.Rhs().ToString();
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

  auto& stmts = comp->MutableStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
      auto& ifs = static_cast<IfStatement&>(*stmts[i]);
      if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(
            static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
      }
      if (ifs.MutableElseBranch() != nullptr &&
          ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(
            static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
      }
      if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
          ifs.MutableElseBranch() != nullptr &&
          ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
        auto* then_comp = static_cast<CompoundStatement*>(ifs.MutableThenBranch());
        auto* else_comp = static_cast<CompoundStatement*>(ifs.MutableElseBranch());
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
            if (!extract_arg_assign(*then_stmts[ti - 1], &t_lhs, &t_rhs)) continue;
            bool t_blocked = false;
            for (size_t tk = ti; tk < then_stmts.size(); ++tk) {
              std::string after_lhs;
              if (!is_simple_reg_assign(*then_stmts[tk], &after_lhs) || after_lhs == t_lhs ||
                  absl::StrContains(then_stmts[tk]->ToString(), t_lhs) ||
                  absl::StrContains(t_rhs, after_lhs)) {
                t_blocked = true;
                break;
              }
            }
            if (t_blocked) continue;

            for (size_t ei = else_stmts.size(); ei > else_min; --ei) {
              std::string e_lhs;
              std::string e_rhs;
              if (!extract_arg_assign(*else_stmts[ei - 1], &e_lhs, &e_rhs)) continue;
              if (e_lhs != t_lhs || e_rhs != t_rhs) continue;
              bool e_blocked = false;
              for (size_t ek = ei; ek < else_stmts.size(); ++ek) {
                std::string after_lhs;
                if (!is_simple_reg_assign(*else_stmts[ek], &after_lhs) || after_lhs == e_lhs ||
                    absl::StrContains(else_stmts[ek]->ToString(), e_lhs) ||
                    absl::StrContains(e_rhs, after_lhs)) {
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
      }
    } else if (stmts[i]->Kind() == CStatementKind::kSwitchStatement) {
      auto& sw = static_cast<SwitchStatement&>(*stmts[i]);
      for (auto& sc : sw.MutableCases()) {
        if (sc.body != nullptr) SinkCommonBranchTailArgAssignments(sc.body.get());
      }
    } else if (stmts[i]->Kind() == CStatementKind::kWhileStatement) {
      auto& ws = static_cast<WhileStatement&>(*stmts[i]);
      if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(ws.MutableBody()));
      }
    } else if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement) {
      auto& dws = static_cast<DoWhileStatement&>(*stmts[i]);
      if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(dws.MutableBody()));
      }
    } else if (stmts[i]->Kind() == CStatementKind::kForStatement) {
      auto& fs = static_cast<ForStatement&>(*stmts[i]);
      if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(fs.MutableBody()));
      }
    } else if (stmts[i]->Kind() == CStatementKind::kCompoundStatement) {
      SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(stmts[i].get()));
    }
  }
}

void InlineCrossJumpedCallTails(CompoundStatement* root_body) {
  if (root_body == nullptr) return;

  struct LabelSite {
    CompoundStatement* comp = nullptr;
    std::string label_name;
    std::vector<std::unique_ptr<CStatement>> tail_stmts;
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
  std::function<void(CompoundStatement*, bool)> collect_labels = [&](CompoundStatement* comp,
                                                                     bool is_root) {
    if (comp == nullptr) return;
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      CStatement* s = stmts[i].get();
      if (s == nullptr) continue;
      if (!is_root && s->Kind() == CStatementKind::kLabelStatement) {
        std::string lbl(static_cast<const LabelStatement&>(*s).Label());
        size_t end_idx = stmts.size();
        if (end_idx > i + 1 && stmts[end_idx - 1]->Kind() == CStatementKind::kBreakStatement) {
          --end_idx;
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
            candidates.push_back(LabelSite{
                .comp = comp, .label_name = std::move(lbl), .tail_stmts = std::move(tail)});
          }
        }
      } else if (s->Kind() == CStatementKind::kIfStatement) {
        auto& ifs = static_cast<IfStatement&>(*s);
        if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
          collect_labels(static_cast<CompoundStatement*>(ifs.MutableThenBranch()), false);
        }
        if (ifs.MutableElseBranch() != nullptr &&
            ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          collect_labels(static_cast<CompoundStatement*>(ifs.MutableElseBranch()), false);
        }
      } else if (s->Kind() == CStatementKind::kSwitchStatement) {
        for (auto& sc : static_cast<SwitchStatement&>(*s).MutableCases()) {
          if (sc.body != nullptr) collect_labels(sc.body.get(), false);
        }
      } else if (s->Kind() == CStatementKind::kCompoundStatement) {
        collect_labels(static_cast<CompoundStatement*>(s), false);
      }
    }
  };
  collect_labels(root_body, true);

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

    if (total_gotos == 0 || total_gotos != valid_tail_gotos) continue;

    int replaced_gotos = 0;
    std::function<void(CompoundStatement*)> replace_gotos = [&](CompoundStatement* comp) {
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
          ++replaced_gotos;
          return;
        }
        if (s->Kind() == CStatementKind::kIfStatement) {
          auto& ifs = static_cast<IfStatement&>(*s);
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
          }
          if (ifs.MutableElseBranch() != nullptr &&
              ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
          }
        } else if (s->Kind() == CStatementKind::kSwitchStatement) {
          for (auto& sc : static_cast<SwitchStatement&>(*s).MutableCases()) {
            if (sc.body != nullptr) replace_gotos(sc.body.get());
          }
        } else if (s->Kind() == CStatementKind::kWhileStatement) {
          auto& ws = static_cast<WhileStatement&>(*s);
          if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(ws.MutableBody()));
          }
        } else if (s->Kind() == CStatementKind::kDoWhileStatement) {
          auto& dws = static_cast<DoWhileStatement&>(*s);
          if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(dws.MutableBody()));
          }
        } else if (s->Kind() == CStatementKind::kForStatement) {
          auto& fs = static_cast<ForStatement&>(*s);
          if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(fs.MutableBody()));
          }
        } else if (s->Kind() == CStatementKind::kCompoundStatement) {
          replace_gotos(static_cast<CompoundStatement*>(s));
        }
      }
    };
    replace_gotos(root_body);

    total_gotos = 0;
    check_gotos(*root_body);
    if (total_gotos == 0 && replaced_gotos > 0) {
      auto& label_stmts = site.comp->MutableStatements();
      for (size_t i = 0; i < label_stmts.size(); ++i) {
        if (label_stmts[i]->Kind() == CStatementKind::kLabelStatement &&
            static_cast<const LabelStatement&>(*label_stmts[i]).Label() == site.label_name) {
          label_stmts.erase(label_stmts.begin() + i);
          break;
        }
      }
    }
  }
}

void SplitReusedFloatGprParameters(CompoundStatement* comp,
                                   const std::vector<CParameter>& parameters) {
  if (comp == nullptr) return;

  std::function<std::unique_ptr<CStatement>(std::unique_ptr<CStatement>, std::string_view,
                                            std::string_view)>
      rewrite_stmt = [&](std::unique_ptr<CStatement> s, std::string_view from,
                         std::string_view to) -> std::unique_ptr<CStatement> {
    if (s == nullptr) return nullptr;
    auto to_id = CExpression::Identifier(std::string(to));
    switch (s->Kind()) {
      case CStatementKind::kCompoundStatement: {
        auto* c = static_cast<CompoundStatement*>(s.get());
        for (auto& child : c->MutableStatements()) {
          child = rewrite_stmt(std::move(child), from, to);
        }
        return s;
      }
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(*s);
        return CStatement::Expression(SubstituteIdentifier(es.Expression(), from, *to_id));
      }
      case CStatementKind::kReturnStatement: {
        const auto& ret = static_cast<const ReturnStatement&>(*s);
        if (ret.ReturnValue() != nullptr) {
          return CStatement::Return(SubstituteIdentifier(*ret.ReturnValue(), from, *to_id));
        }
        return s;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(*s);
        auto new_cond = SubstituteIdentifier(ifs.Condition(), from, *to_id);
        auto new_then = rewrite_stmt(ifs.ThenBranch().Clone(), from, to);
        std::unique_ptr<CStatement> new_else =
            ifs.ElseBranch() ? rewrite_stmt(ifs.ElseBranch()->Clone(), from, to) : nullptr;
        return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(*s);
        return CStatement::While(SubstituteIdentifier(ws.Condition(), from, *to_id),
                                 rewrite_stmt(ws.Body().Clone(), from, to));
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(*s);
        return CStatement::DoWhile(rewrite_stmt(dws.Body().Clone(), from, to),
                                   SubstituteIdentifier(dws.Condition(), from, *to_id));
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(*s);
        auto new_init = fs.Init() ? rewrite_stmt(fs.Init()->Clone(), from, to) : nullptr;
        auto new_cond =
            fs.Condition() ? SubstituteIdentifier(*fs.Condition(), from, *to_id) : nullptr;
        auto new_step = fs.Step() ? SubstituteIdentifier(*fs.Step(), from, *to_id) : nullptr;
        return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                               rewrite_stmt(fs.Body().Clone(), from, to));
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(*s);
        auto new_cond = SubstituteIdentifier(ss.Condition(), from, *to_id);
        std::vector<SwitchCase> new_cases;
        for (const auto& c : ss.Cases()) {
          SwitchCase sc = c.Clone();
          if (c.body != nullptr) {
            auto rb = rewrite_stmt(c.body->Clone(), from, to);
            sc.body =
                std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(rb.release()));
          }
          new_cases.push_back(std::move(sc));
        }
        return CStatement::Switch(std::move(new_cond), std::move(new_cases));
      }
      default:
        return s;
    }
  };

  auto& stmts = comp->MutableStatements();
  for (const auto& p : parameters) {
    if (p.type.ToString() != "f32" || (p.name != "arg1" && p.name != "arg2" && p.name != "arg3")) {
      continue;
    }
    bool seen_float_read = false;
    std::optional<size_t> split_start_idx;
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es = static_cast<const ExpressionStatement&>(*stmts[i]);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(a.Lhs()).Name() == p.name) {
            if (seen_float_read && !absl::StrContains(a.Rhs().ToString(), p.name)) {
              split_start_idx = i;
            }
            break;
          }
        }
      }
      if (absl::StrContains(stmts[i]->ToString(), p.name)) {
        seen_float_read = true;
      }
    }
    if (split_start_idx.has_value()) {
      std::string temp_name = absl::StrCat("temp_a", p.name.substr(3));
      for (size_t k = *split_start_idx; k < stmts.size(); ++k) {
        stmts[k] = rewrite_stmt(std::move(stmts[k]), p.name, temp_name);
      }
    }
  }
}

void RewriteFloatInGprAndStackCallArguments(CompoundStatement* comp) {
  if (comp == nullptr) return;
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
      if (un.Op() == "-" || un.Op() == "(f32)" || un.Op() == "(f64)" || un.Op() == "*(f32*)&" ||
          un.Op() == "*(f32 *)&" || un.Op() == "*(f64*)&" || un.Op() == "*(f64 *)&") {
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
          if (inner_u.Op() == "(f32)" || inner_u.Op() == "*(f32*)&" ||
              inner_u.Op() == "*(f32 *)&") {
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
        if (un.Op() == "(f64)" || un.Op() == "*(f64*)&" || un.Op() == "*(f64 *)&") return "f64";
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

    for (size_t k = 1; k < args.size(); ++k) {
      if (k == 1 && arg0_is_float) {
        if (is_float_arg0(*args[1])) {
          std::string p1_type =
              existing_param_types.empty() ? infer_fp_arg_type(*args[1]) : existing_param_types[1];
          if (existing_param_types.empty() && p1_type == "f32") {
            has_float_arg = true;
          }
          param_types.push_back(std::move(p1_type));
        } else {
          param_types.push_back(existing_param_types.empty() ? "s32" : existing_param_types[1]);
        }
        new_args.push_back(args[1]->Clone());
        continue;
      }
      auto unwrapped = extract_s32_bitcast_of_f32(*args[k]);
      if (unwrapped != nullptr) {
        has_float_arg = true;
        param_types.push_back("f32");
        new_args.push_back(std::move(unwrapped));
      } else {
        param_types.push_back(existing_param_types.empty() ? "s32" : existing_param_types[k]);
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
            static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
      }
      if (ifs.MutableElseBranch() != nullptr) {
        if (ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          RewriteFloatInGprAndStackCallArguments(
              static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
        } else if (ifs.MutableElseBranch()->Kind() == CStatementKind::kIfStatement) {
          CompoundStatement wrapper;
          wrapper.AddStatement(ifs.MutableElseBranch()->Clone());
          RewriteFloatInGprAndStackCallArguments(&wrapper);
          if (wrapper.Statements().size() == 1) {
            stmt = CStatement::If(rewrite_expr(ifs.Condition(), "s32"), ifs.ThenBranch().Clone(),
                                  std::move(wrapper.MutableStatements()[0]));
            continue;
          }
        }
      }
      stmt = CStatement::If(rewrite_expr(ifs.Condition(), "s32"), ifs.ThenBranch().Clone(),
                            ifs.ElseBranch() != nullptr ? ifs.ElseBranch()->Clone() : nullptr);
    } else if (stmt->Kind() == CStatementKind::kSwitchStatement) {
      auto& sw = static_cast<SwitchStatement&>(*stmt);
      for (auto& sc : sw.MutableCases()) {
        if (sc.body != nullptr) RewriteFloatInGprAndStackCallArguments(sc.body.get());
      }
    } else if (stmt->Kind() == CStatementKind::kWhileStatement) {
      auto& ws = static_cast<WhileStatement&>(*stmt);
      if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(ws.MutableBody()));
      }
    } else if (stmt->Kind() == CStatementKind::kDoWhileStatement) {
      auto& dws = static_cast<DoWhileStatement&>(*stmt);
      if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(dws.MutableBody()));
      }
    } else if (stmt->Kind() == CStatementKind::kForStatement) {
      auto& fs = static_cast<ForStatement&>(*stmt);
      if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(fs.MutableBody()));
      }
    } else if (stmt->Kind() == CStatementKind::kCompoundStatement) {
      RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(stmt.get()));
    }
  }
}

void CollapseUnalignedBlockCopies(CompoundStatement* comp) {
  if (comp == nullptr) return;

  auto extract_base_and_offset = [](const CExpression& addr, std::string* base_out,
                                    int64_t* offset_out) -> bool {
    if (addr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(addr);
      if (bin.Op() == "+" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        *base_out = bin.Lhs().ToString();
        *offset_out = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        return true;
      }
    }
    *base_out = addr.ToString();
    *offset_out = 0;
    return true;
  };

  auto extract_typed_load = [&](const CStatement& s, std::string_view expected_type,
                                std::string* lhs_var_out, std::string* base_out,
                                int64_t* offset_out, const CExpression** base_expr_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
    if (a.Rhs().Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(a.Rhs());
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if (cast.TargetType().name != expected_type || cast.TargetType().pointer_depth != 1) {
      return false;
    }
    *lhs_var_out = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
    if (!extract_base_and_offset(cast.Operand(), base_out, offset_out)) return false;
    if (cast.Operand().Kind() == CExpressionKind::kBinaryExpression &&
        static_cast<const BinaryExpression&>(cast.Operand()).Op() == "+" &&
        static_cast<const BinaryExpression&>(cast.Operand()).Rhs().Kind() ==
            CExpressionKind::kIntegerLiteral) {
      *base_expr_out = &static_cast<const BinaryExpression&>(cast.Operand()).Lhs();
    } else {
      *base_expr_out = &cast.Operand();
    }
    return true;
  };

  auto match_u8_byte_store = [&](const CStatement& s, std::string_view expected_src_var,
                                 int64_t expected_shift, std::string* dst_base_out,
                                 int64_t* dst_offset_out,
                                 const CExpression** dst_base_expr_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(a.Lhs());
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if (cast.TargetType().name != "u8" || cast.TargetType().pointer_depth != 1) return false;
    if (expected_shift == 0) {
      if (a.Rhs().Kind() != CExpressionKind::kIdentifier ||
          static_cast<const IdentifierExpression&>(a.Rhs()).Name() != expected_src_var) {
        return false;
      }
    } else {
      if (a.Rhs().Kind() != CExpressionKind::kBinaryExpression) return false;
      const auto& rhs_bin = static_cast<const BinaryExpression&>(a.Rhs());
      if (rhs_bin.Op() != ">>" || rhs_bin.Lhs().Kind() != CExpressionKind::kIdentifier ||
          static_cast<const IdentifierExpression&>(rhs_bin.Lhs()).Name() != expected_src_var ||
          rhs_bin.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
          static_cast<const IntegerLiteralExpression&>(rhs_bin.Rhs()).Value() != expected_shift) {
        return false;
      }
    }
    if (!extract_base_and_offset(cast.Operand(), dst_base_out, dst_offset_out)) return false;
    if (cast.Operand().Kind() == CExpressionKind::kBinaryExpression &&
        static_cast<const BinaryExpression&>(cast.Operand()).Op() == "+" &&
        static_cast<const BinaryExpression&>(cast.Operand()).Rhs().Kind() ==
            CExpressionKind::kIntegerLiteral) {
      *dst_base_expr_out = &static_cast<const BinaryExpression&>(cast.Operand()).Lhs();
    } else {
      *dst_base_expr_out = &cast.Operand();
    }
    return true;
  };

  auto match_s8_byte_store = [&](const CStatement& s, std::string_view expected_src_var,
                                 std::string_view expected_dst_base,
                                 int64_t expected_dst_offset) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(a.Lhs());
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if ((cast.TargetType().name != "s8" && cast.TargetType().name != "u8") ||
        cast.TargetType().pointer_depth != 1) {
      return false;
    }
    if (a.Rhs().Kind() != CExpressionKind::kIdentifier ||
        static_cast<const IdentifierExpression&>(a.Rhs()).Name() != expected_src_var) {
      return false;
    }
    std::string b;
    int64_t off = 0;
    if (!extract_base_and_offset(cast.Operand(), &b, &off)) return false;
    return b == expected_dst_base && off == expected_dst_offset;
  };

  auto make_addr_with_offset = [](const CExpression& base_expr,
                                  int64_t offset) -> std::unique_ptr<CExpression> {
    if (offset == 0) return base_expr.Clone();
    return CExpression::Binary("+", base_expr.Clone(), CExpression::Integer(offset));
  };

  auto make_block_copy_stmt = [&](std::string_view struct_name, const CExpression& dst_base,
                                  int64_t dst_off, const CExpression& src_base,
                                  int64_t src_off) -> std::unique_ptr<CStatement> {
    CType ptr_type = CType::Named(std::string(struct_name)).MakePointer();
    auto lhs = CExpression::Unary(
        "*", CExpression::Cast(ptr_type, make_addr_with_offset(dst_base, dst_off)));
    auto rhs = CExpression::Unary(
        "*", CExpression::Cast(ptr_type, make_addr_with_offset(src_base, src_off)));
    return CStatement::Expression(CExpression::Assignment("=", std::move(lhs), std::move(rhs)));
  };

  auto& stmts = comp->MutableStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    // Try matching 6-byte copy: 1 word load + 2 s8 loads + 4 u8 byte stores + 2 s8 byte stores
    if (i + 9 <= stmts.size()) {
      std::string v0, v1, v2, src_b0, src_b1, src_b2, dst_b0;
      int64_t src_off0 = 0, src_off1 = 0, src_off2 = 0, dst_off0 = 0;
      const CExpression* src_expr0 = nullptr;
      const CExpression* dummy_expr = nullptr;
      const CExpression* dst_expr0 = nullptr;
      if (extract_typed_load(*stmts[i], "s32", &v0, &src_b0, &src_off0, &src_expr0) &&
          extract_typed_load(*stmts[i + 1], "s8", &v1, &src_b1, &src_off1, &dummy_expr) &&
          extract_typed_load(*stmts[i + 2], "s8", &v2, &src_b2, &src_off2, &dummy_expr) &&
          src_b1 == src_b0 && src_b2 == src_b0 && src_off1 == src_off0 + 4 &&
          src_off2 == src_off0 + 5 &&
          match_u8_byte_store(*stmts[i + 3], v0, 24, &dst_b0, &dst_off0, &dst_expr0)) {
        bool all_stores_ok = true;
        for (int b = 1; b < 4; ++b) {
          std::string db;
          int64_t doff = 0;
          if (!match_u8_byte_store(*stmts[i + 3 + b], v0, (3 - b) * 8, &db, &doff, &dummy_expr) ||
              db != dst_b0 || doff != dst_off0 + b) {
            all_stores_ok = false;
            break;
          }
        }
        if (all_stores_ok && match_s8_byte_store(*stmts[i + 7], v1, dst_b0, dst_off0 + 4) &&
            match_s8_byte_store(*stmts[i + 8], v2, dst_b0, dst_off0 + 5)) {
          auto copy_stmt =
              make_block_copy_stmt("UnalignedBlock6", *dst_expr0, dst_off0, *src_expr0, src_off0);
          stmts[i] = std::move(copy_stmt);
          stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 9);
          continue;
        }
      }
    }

    // Try matching N-word copy (N = 4, 3, 2, 1): N s32 loads + 4*N u8 byte stores
    bool matched_n = false;
    for (int n_words : {4, 3, 2, 1}) {
      size_t total_stmts = static_cast<size_t>(n_words * 5);
      if (i + total_stmts > stmts.size()) continue;
      std::vector<std::string> vars(n_words);
      std::string src_base;
      int64_t src_off0 = 0;
      const CExpression* src_base_expr = nullptr;
      bool loads_ok = true;
      for (int w = 0; w < n_words; ++w) {
        std::string b;
        int64_t off = 0;
        const CExpression* be = nullptr;
        if (!extract_typed_load(*stmts[i + w], "s32", &vars[w], &b, &off, &be)) {
          loads_ok = false;
          break;
        }
        if (w == 0) {
          src_base = b;
          src_off0 = off;
          src_base_expr = be;
        } else if (b != src_base || off != src_off0 + w * 4) {
          loads_ok = false;
          break;
        }
      }
      if (!loads_ok) continue;

      std::string dst_base;
      int64_t dst_off0 = 0;
      const CExpression* dst_base_expr = nullptr;
      bool stores_ok = true;
      for (int w = 0; w < n_words && stores_ok; ++w) {
        for (int b = 0; b < 4; ++b) {
          std::string db;
          int64_t doff = 0;
          const CExpression* dbe = nullptr;
          size_t stmt_idx = i + n_words + w * 4 + b;
          if (!match_u8_byte_store(*stmts[stmt_idx], vars[w], (3 - b) * 8, &db, &doff, &dbe)) {
            stores_ok = false;
            break;
          }
          if (w == 0 && b == 0) {
            dst_base = db;
            dst_off0 = doff;
            dst_base_expr = dbe;
          } else if (db != dst_base || doff != dst_off0 + w * 4 + b) {
            stores_ok = false;
            break;
          }
        }
      }
      if (!stores_ok) continue;

      std::string_view block_type = "UnalignedWord";
      if (n_words == 4) {
        block_type = "UnalignedBlock16";
      } else if (n_words == 3) {
        block_type = "UnalignedBlock12";
      } else if (n_words == 2) {
        block_type = "UnalignedBlock8";
      }
      auto copy_stmt =
          make_block_copy_stmt(block_type, *dst_base_expr, dst_off0, *src_base_expr, src_off0);
      stmts[i] = std::move(copy_stmt);
      stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + total_stmts);
      matched_n = true;
      break;
    }
    if (matched_n) {
      if (i > 0 && stmts[i - 1]->Kind() == CStatementKind::kExpressionStatement &&
          stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
        // Merge two consecutive UnalignedBlock8 copies at offset 0 and +8 into UnalignedBlock16!
        const auto& prev_es = static_cast<const ExpressionStatement&>(*stmts[i - 1]);
        const auto& cur_es = static_cast<const ExpressionStatement&>(*stmts[i]);
        if (prev_es.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
            cur_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& pa = static_cast<const AssignmentExpression&>(prev_es.Expression());
          const auto& ca = static_cast<const AssignmentExpression&>(cur_es.Expression());
          auto extract_block8 = [&](const CExpression& e, std::string* b_out, int64_t* off_out,
                                    const CExpression** be_out) -> bool {
            if (e.Kind() != CExpressionKind::kUnaryExpression) return false;
            const auto& u = static_cast<const UnaryExpression&>(e);
            if (u.Op() != "*" || u.Operand().Kind() != CExpressionKind::kCastExpression) {
              return false;
            }
            const auto& c = static_cast<const CastExpression&>(u.Operand());
            if (c.TargetType().name != "UnalignedBlock8" || c.TargetType().pointer_depth != 1) {
              return false;
            }
            if (!extract_base_and_offset(c.Operand(), b_out, off_out)) return false;
            if (c.Operand().Kind() == CExpressionKind::kBinaryExpression &&
                static_cast<const BinaryExpression&>(c.Operand()).Op() == "+" &&
                static_cast<const BinaryExpression&>(c.Operand()).Rhs().Kind() ==
                    CExpressionKind::kIntegerLiteral) {
              *be_out = &static_cast<const BinaryExpression&>(c.Operand()).Lhs();
            } else {
              *be_out = &c.Operand();
            }
            return true;
          };
          std::string pd, ps, cd, cs;
          int64_t pdo = 0, pso = 0, cdo = 0, cso = 0;
          const CExpression* pde = nullptr;
          const CExpression* pse = nullptr;
          const CExpression* dummy = nullptr;
          if (extract_block8(pa.Lhs(), &pd, &pdo, &pde) &&
              extract_block8(pa.Rhs(), &ps, &pso, &pse) &&
              extract_block8(ca.Lhs(), &cd, &cdo, &dummy) &&
              extract_block8(ca.Rhs(), &cs, &cso, &dummy) && pd == cd && ps == cs &&
              cdo == pdo + 8 && cso == pso + 8) {
            stmts[i - 1] = make_block_copy_stmt("UnalignedBlock16", *pde, pdo, *pse, pso);
            stmts.erase(stmts.begin() + i);
            --i;
          }
        }
      }
      continue;
    }
  }

  for (auto& s : stmts) {
    if (s == nullptr) continue;
    switch (s->Kind()) {
      case CStatementKind::kCompoundStatement:
        CollapseUnalignedBlockCopies(static_cast<CompoundStatement*>(s.get()));
        break;
      case CStatementKind::kIfStatement: {
        auto& ifs = static_cast<IfStatement&>(*s);
        if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
          CollapseUnalignedBlockCopies(static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
        }
        if (ifs.MutableElseBranch() != nullptr &&
            ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          CollapseUnalignedBlockCopies(static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
        }
        break;
      }
      case CStatementKind::kWhileStatement: {
        auto& ws = static_cast<WhileStatement&>(*s);
        if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          CollapseUnalignedBlockCopies(static_cast<CompoundStatement*>(ws.MutableBody()));
        }
        break;
      }
      case CStatementKind::kDoWhileStatement: {
        auto& dws = static_cast<DoWhileStatement&>(*s);
        if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          CollapseUnalignedBlockCopies(static_cast<CompoundStatement*>(dws.MutableBody()));
        }
        break;
      }
      case CStatementKind::kForStatement: {
        auto& fs = static_cast<ForStatement&>(*s);
        if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          CollapseUnalignedBlockCopies(static_cast<CompoundStatement*>(fs.MutableBody()));
        }
        break;
      }
      case CStatementKind::kSwitchStatement: {
        auto& sw = static_cast<SwitchStatement&>(*s);
        for (auto& sc : sw.MutableCases()) {
          if (sc.body != nullptr) CollapseUnalignedBlockCopies(sc.body.get());
        }
        break;
      }
      default:
        break;
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
        const size_t orig_args_size = args.size();
        if (args.size() <= 4) {
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

bool SplitScratchReusedParameterRegisters(CompoundStatement* body,
                                          const std::vector<CParameter>& parameters,
                                          uint32_t frame_size) {
  if (body == nullptr || parameters.empty()) return false;
  auto& stmts = body->MutableStatements();
  std::string body_text = body->ToString();
  bool has_goto = absl::StrContains(body_text, "goto ");
  auto is_temp_t_var = [](std::string_view v) {
    return v.size() == 7 && absl::StartsWith(v, "temp_t") && v[6] >= '0' && v[6] <= '9';
  };
  bool any_homed_split = false;
  bool any_s_split_in_single_param = false;
  for (const auto& param : parameters) {
    if (param.name.size() != 4 || !absl::StartsWith(param.name, "arg") || param.name[3] < '0' ||
        param.name[3] > '3' || param.type.ToString() == "f32") {
      continue;
    }
    if (StatementTakesAddressOf(*body, param.name)) {
      continue;
    }
    std::string homed_slot_name =
        (frame_size > 0)
            ? absl::StrCat("var_sp_", frame_size + static_cast<uint32_t>(param.name[3] - '0') * 4)
            : "";
    std::optional<size_t> prologue_copy_idx;
    std::string prologue_copy_lhs;
    bool is_homed_copy = false;
    for (size_t i = 0; i < stmts.size() && i < 8; ++i) {
      if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kExpressionStatement ||
          StatementHasCall(*stmts[i])) {
        break;
      }
      const auto& es = static_cast<const ExpressionStatement&>(*stmts[i]);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            a.Rhs().Kind() == CExpressionKind::kIdentifier) {
          std::string_view lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          std::string_view rhs = static_cast<const IdentifierExpression&>(a.Rhs()).Name();
          if (rhs == param.name && (IsCalleeSavedGprVarName(lhs) || is_temp_t_var(lhs) ||
                                    (!homed_slot_name.empty() && lhs == homed_slot_name))) {
            prologue_copy_idx = i;
            prologue_copy_lhs = std::string(lhs);
            is_homed_copy = (!homed_slot_name.empty() && lhs == homed_slot_name);
            break;
          }
        }
      }
      if (StatementReferencesVariable(*stmts[i], param.name)) {
        break;
      }
    }
    if (!prologue_copy_idx.has_value()) {
      continue;
    }
    bool any_redef_seen = false;
    bool saw_call_before_write = false;
    auto is_direct_overwrite_of_param = [&](const CStatement& s) -> bool {
      if (s.Kind() != CStatementKind::kExpressionStatement) return false;
      const auto& es = static_cast<const ExpressionStatement&>(s);
      if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
      const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
      return a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
             static_cast<const IdentifierExpression&>(a.Lhs()).Name() == param.name &&
             !ExpressionUsesVariable(a.Rhs(), param.name);
    };
    auto is_overwritten_after_label = [&](const CompoundStatement& cs, size_t label_idx) -> bool {
      for (size_t step = 1; step <= 3 && label_idx + step < cs.Statements().size(); ++step) {
        const CStatement* cur = cs.Statements()[label_idx + step].get();
        if (cur == nullptr) return false;
        if (is_direct_overwrite_of_param(*cur) ||
            (cur->Kind() == CStatementKind::kReturnStatement &&
             !StatementReferencesVariable(*cur, param.name))) {
          return true;
        }
        if (cur->Kind() != CStatementKind::kExpressionStatement ||
            StatementReferencesVariable(*cur, param.name) || StatementHasCall(*cur)) {
          return false;
        }
      }
      return false;
    };
    auto label_is_immediately_overwritten = [&](std::string_view target_label) -> bool {
      bool ok = false;
      std::function<void(const CStatement&)> find_lbl = [&](const CStatement& s) {
        if (ok) return;
        if (s.Kind() == CStatementKind::kCompoundStatement) {
          const auto& cs = static_cast<const CompoundStatement&>(s);
          for (size_t idx = 0; idx < cs.Statements().size(); ++idx) {
            if (cs.Statements()[idx] != nullptr &&
                cs.Statements()[idx]->Kind() == CStatementKind::kLabelStatement &&
                static_cast<const LabelStatement&>(*cs.Statements()[idx]).Label() == target_label) {
              if (is_overwritten_after_label(cs, idx)) {
                ok = true;
              }
              return;
            }
            if (cs.Statements()[idx] != nullptr) find_lbl(*cs.Statements()[idx]);
          }
        } else if (s.Kind() == CStatementKind::kIfStatement) {
          const auto& ifs = static_cast<const IfStatement&>(s);
          find_lbl(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) find_lbl(*ifs.ElseBranch());
        } else if (s.Kind() == CStatementKind::kSwitchStatement) {
          for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
            if (sc.body != nullptr) find_lbl(*sc.body);
          }
        } else if (s.Kind() == CStatementKind::kWhileStatement) {
          find_lbl(static_cast<const WhileStatement&>(s).Body());
        } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
          find_lbl(static_cast<const DoWhileStatement&>(s).Body());
        } else if (s.Kind() == CStatementKind::kForStatement) {
          find_lbl(static_cast<const ForStatement&>(s).Body());
        }
      };
      find_lbl(*body);
      return ok;
    };
    std::function<bool(const CStatement&, bool*)> check_entry_not_read =
        [&](const CStatement& s, bool* definitely_written) -> bool {
      if (!has_goto && *definitely_written) return true;
      switch (s.Kind()) {
        case CStatementKind::kExpressionStatement: {
          const auto& es = static_cast<const ExpressionStatement&>(s);
          if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
            if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(a.Lhs()).Name() == param.name) {
              if (!*definitely_written && ExpressionUsesVariable(a.Rhs(), param.name)) {
                return false;
              }
              *definitely_written = true;
              any_redef_seen = true;
              return true;
            }
          }
          if (!*definitely_written && StatementHasCall(s)) {
            saw_call_before_write = true;
          }
          return *definitely_written || !StatementReferencesVariable(s, param.name);
        }
        case CStatementKind::kCompoundStatement: {
          const auto& cs = static_cast<const CompoundStatement&>(s);
          const auto& cs_stmts = cs.Statements();
          for (size_t idx = 0; idx < cs_stmts.size(); ++idx) {
            if (cs_stmts[idx] == nullptr) continue;
            if (cs_stmts[idx]->Kind() == CStatementKind::kLabelStatement) {
              if (!*definitely_written && !is_overwritten_after_label(cs, idx)) {
                return false;
              }
              continue;
            }
            if (!check_entry_not_read(*cs_stmts[idx], definitely_written)) {
              return false;
            }
            if (!has_goto && *definitely_written) return true;
          }
          return true;
        }
        case CStatementKind::kIfStatement: {
          const auto& ifs = static_cast<const IfStatement&>(s);
          if (!*definitely_written && ExpressionUsesVariable(ifs.Condition(), param.name)) {
            return false;
          }
          bool then_written = *definitely_written;
          if (!check_entry_not_read(ifs.ThenBranch(), &then_written)) {
            return false;
          }
          bool else_written = *definitely_written;
          if (ifs.ElseBranch() != nullptr) {
            if (!check_entry_not_read(*ifs.ElseBranch(), &else_written)) {
              return false;
            }
            if (then_written && else_written) {
              *definitely_written = true;
            }
          }
          return true;
        }
        case CStatementKind::kSwitchStatement: {
          if (!has_goto) return !StatementReferencesVariable(s, param.name);
          const auto& ss = static_cast<const SwitchStatement&>(s);
          if (!*definitely_written && ExpressionUsesVariable(ss.Condition(), param.name)) {
            return false;
          }
          for (const auto& sc : ss.Cases()) {
            if (sc.body == nullptr) continue;
            bool case_written = *definitely_written;
            if (!check_entry_not_read(*sc.body, &case_written)) {
              return false;
            }
          }
          return true;
        }
        case CStatementKind::kGotoStatement: {
          if (!has_goto) return false;
          if (*definitely_written) return true;
          const auto& gs = static_cast<const GotoStatement&>(s);
          if (label_is_immediately_overwritten(gs.Label())) {
            *definitely_written = true;
            return true;
          }
          return false;
        }
        case CStatementKind::kLabelStatement:
          return false;
        default:
          return *definitely_written || !StatementReferencesVariable(s, param.name);
      }
    };
    size_t subst_start_idx = *prologue_copy_idx + 1;
    if (!is_homed_copy) {
      for (size_t pre_idx = *prologue_copy_idx + 1; pre_idx < stmts.size() && pre_idx < 8;
           ++pre_idx) {
        if (stmts[pre_idx] == nullptr ||
            stmts[pre_idx]->Kind() != CStatementKind::kExpressionStatement) {
          break;
        }
        const auto& es = static_cast<const ExpressionStatement&>(*stmts[pre_idx]);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          if (a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(a.Lhs()).Name() == param.name) {
            break;
          }
        }
        if (StatementReferencesVariable(*stmts[pre_idx], param.name)) {
          subst_start_idx = pre_idx + 1;
        }
        if (StatementHasCall(*stmts[pre_idx])) {
          saw_call_before_write = true;
          break;
        }
      }
    }
    bool entry_not_read = true;
    bool written_so_far = false;
    for (size_t k = subst_start_idx; k < stmts.size(); ++k) {
      if (stmts[k] == nullptr) continue;
      if (stmts[k]->Kind() == CStatementKind::kLabelStatement) {
        if (!written_so_far && !is_overwritten_after_label(*body, k)) {
          entry_not_read = false;
          break;
        }
        continue;
      }
      if (!check_entry_not_read(*stmts[k], &written_so_far)) {
        entry_not_read = false;
        break;
      }
      if (!has_goto && written_so_far) {
        break;
      }
    }
    if (!entry_not_read || !any_redef_seen ||
        (parameters.size() < 2 && !saw_call_before_write && !is_temp_t_var(prologue_copy_lhs))) {
      continue;
    }
    std::string scratch_name = absl::StrCat("temp_a", param.name.substr(3));
    for (size_t k = subst_start_idx; k < stmts.size(); ++k) {
      stmts[k] = SubstituteIdentifierInStatement(std::move(stmts[k]), param.name, scratch_name);
    }
    if (is_homed_copy) {
      any_homed_split = true;
    } else if (has_goto && parameters.size() == 1 && IsCalleeSavedGprVarName(prologue_copy_lhs)) {
      any_s_split_in_single_param = true;
    }
  }
  if (any_homed_split || any_s_split_in_single_param) {
    CoalescePrologueParameterCopies(body, parameters, frame_size);
  }
  return any_s_split_in_single_param;
}

}  // namespace rom_nom_nom
