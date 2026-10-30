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
#include "core/c_ast_visitor.h"

namespace rom_nom_nom {
namespace {

bool IsCalleeSavedGprVarName(std::string_view v) {
  return v == "fp" ||
         (v.size() == 7 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7');
}

class DefinedVarsCollectorVisitor : public AstVisitor {
 public:
  explicit DefinedVarsCollectorVisitor(absl::flat_hash_set<std::string>* defined_vars)
      : defined_vars_(defined_vars) {}

  void VisitAssignmentExpression(const AssignmentExpression& expr) override {
    if (expr.Lhs().Kind() == CExpressionKind::kIdentifier) {
      defined_vars_->insert(static_cast<const IdentifierExpression&>(expr.Lhs()).Name());
    } else {
      VisitExpression(expr.Lhs());
    }
    VisitExpression(expr.Rhs());
  }

  void VisitUnaryExpression(const UnaryExpression& expr) override {
    if (expr.Op() == "&" && expr.Operand().Kind() == CExpressionKind::kIdentifier) {
      defined_vars_->insert(static_cast<const IdentifierExpression&>(expr.Operand()).Name());
    }
    AstVisitor::VisitUnaryExpression(expr);
  }

 private:
  absl::flat_hash_set<std::string>* defined_vars_;
};

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

void RewriteFloatInGprAndStackCallArguments(CompoundStatement* comp,
                                            const std::vector<CParameter>& parameters) {
  if (comp == nullptr) return;
  absl::flat_hash_set<std::string> defined_vars;
  for (const auto& p : parameters) {
    defined_vars.insert(p.name);
  }
  DefinedVarsCollectorVisitor collector(&defined_vars);
  collector.VisitCompoundStatement(*comp);

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
    std::string existing_ret_type;
    if (absl::StartsWith(callee_name, "((")) {
      size_t params_open = callee_name.find("(*)(");
      size_t params_close = callee_name.rfind("))");
      if (params_open == std::string::npos || params_close == std::string::npos ||
          params_close <= params_open + 4) {
        return nullptr;
      }
      if (params_open > 2) {
        existing_ret_type = callee_name.substr(2, params_open - 2);
        while (!existing_ret_type.empty() && existing_ret_type.back() == ' ') {
          existing_ret_type.pop_back();
        }
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
    std::string_view final_ret_type = ret_type;
    if (!existing_ret_type.empty() && ret_type == "s32") {
      final_ret_type = existing_ret_type;
    }
    std::string cast_callee = absl::StrFormat("((%s (*)(%s))%s)", final_ret_type, sig, callee_name);
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
        if (a.Op() == "=") {
          if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
            std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
            if (is_fp_var_name(lhs_name)) {
              rhs_ret_type = absl::EndsWith(lhs_name, "_d") ? "f64" : "f32";
            }
          } else if (a.Lhs().Kind() == CExpressionKind::kUnaryExpression) {
            const auto& u = static_cast<const UnaryExpression&>(a.Lhs());
            if (u.Op() == "*(f32*)&" || u.Op() == "*(f32 *)&") {
              rhs_ret_type = "f32";
            } else if (u.Op() == "*(f64*)&" || u.Op() == "*(f64 *)&") {
              rhs_ret_type = "f64";
            } else if (u.Op() == "*" && u.Operand().Kind() == CExpressionKind::kCastExpression) {
              const auto& ct = static_cast<const CastExpression&>(u.Operand()).TargetType();
              if (ct.pointer_depth == 1 && (ct.name == "f32" || ct.name == "f64")) {
                rhs_ret_type = ct.name == "f64" ? "f64" : "f32";
              }
            }
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
  DefinedVarsCollectorVisitor collector(&defined_vars);
  collector.VisitCompoundStatement(*body);

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
          bool before_prefix = in_entry_linear_prefix;
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_comp(static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
          }
          auto after_then = defined_so_far;
          bool after_then_prefix = in_entry_linear_prefix;
          defined_so_far = before_if;
          in_entry_linear_prefix = before_prefix;
          if (ifs.MutableElseBranch() != nullptr &&
              ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
            rewrite_comp(static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
          }
          in_entry_linear_prefix = after_then_prefix && in_entry_linear_prefix;
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

namespace {

template <typename Fn>
bool RunAndDetectChange(CompoundStatement* body, Fn&& fn) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  fn();
  return body->ToString() != before;
}

}  // namespace

bool ReturnStatementSimplifierPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  return RunAndDetectChange(
      body, [&]() { SimplifyReturnStatements(body, ctx.returns_v0, ctx.returns_f0); });
}

bool DecoupledDelaySlotEarlyReturnPass::Run(CompoundStatement* body,
                                            const AstPassContext& /*ctx*/) {
  return RunAndDetectChange(body, [&]() { SimplifyDecoupledDelaySlotEarlyReturns(body); });
}

bool FloatCallArgumentRewriterPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  return RunAndDetectChange(
      body, [&]() { RewriteFloatInGprAndStackCallArguments(body, ctx.parameters); });
}

bool UnassignedTrailingCallArgTrimmerPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  return RunAndDetectChange(body, [&]() {
    TrimUnassignedTrailingCallArgRegisters(body, ctx.parameters, ctx.function_parameter_counts);
  });
}

}  // namespace rom_nom_nom
