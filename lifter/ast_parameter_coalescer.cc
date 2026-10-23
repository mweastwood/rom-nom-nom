#include "lifter/ast_parameter_coalescer.h"

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

void CoalescePrologueParameterCopies(CompoundStatement* comp,
                                     const std::vector<CParameter>& parameters,
                                     uint32_t frame_size) {
  if (comp == nullptr) return;
  absl::flat_hash_set<std::string> param_names;
  absl::flat_hash_set<std::string> float_param_names;
  absl::flat_hash_map<std::string, std::string> f32_param_homing_slots;
  uint32_t param_slot = 0;
  for (const auto& p : parameters) {
    param_names.insert(p.name);
    std::string p_type = p.type.ToString();
    if (p_type == "f64") {
      if (param_slot % 2 != 0) ++param_slot;
      param_slot += 2;
    } else {
      if (p_type == "f32") {
        f32_param_homing_slots[p.name] = absl::StrCat("var_sp_", frame_size + param_slot * 4);
        if (absl::StartsWith(p.name, "arg")) {
          float_param_names.insert(p.name);
        }
      }
      ++param_slot;
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
  absl::flat_hash_map<std::string, std::unique_ptr<CExpression>> coalesced_expr_aliases;
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
            bool has_other_s_regs = false;
            for (int s_idx = 1; s_idx <= 7; ++s_idx) {
              if (StatementReferencesVariable(*comp, absl::StrCat("temp_s", s_idx))) {
                has_other_s_regs = true;
                break;
              }
            }
            bool later_param_used = false;
            for (size_t p_idx = 1; p_idx < parameters.size(); ++p_idx) {
              if (StatementReferencesVariable(*comp, parameters[p_idx].name)) {
                later_param_used = true;
                break;
              }
            }
            bool s_reg_mutated_param_coalesce_ok =
                is_s_reg && i == 0 && !lhs_addr_taken && lhs == "temp_s0" && rhs == "arg0" &&
                later_param_used && !has_other_s_regs && StatementHasCall(*comp) &&
                CountIdentifierOccurrencesInStatement(*comp, rhs) == 1;
            if (!is_assigned(rhs) && v_copy_ok &&
                (is_homed_param_slot ? homed_param_coalesce_ok
                                     : ((count_assignments(lhs) == 1 && !lhs_addr_taken) ||
                                        s_reg_mutated_param_coalesce_ok)) &&
                !absl::StrContains(body_text, absl::StrCat("(*)())", lhs))) {
              coalesced_aliases[lhs] = rhs;
              stmts_to_erase.push_back(i);
            }
          }
        } else if (i < 6 && a.Op() == "=" && a.Rhs().Kind() == CExpressionKind::kIdentifier) {
          auto extract_f32_store_slot_var = [](const CExpression& lhs_expr) -> std::string_view {
            if (lhs_expr.Kind() != CExpressionKind::kUnaryExpression) return "";
            const auto& un = static_cast<const UnaryExpression&>(lhs_expr);
            if ((un.Op() == "*(f32*)&" || un.Op() == "*(f32 *)&") &&
                un.Operand().Kind() == CExpressionKind::kIdentifier) {
              return static_cast<const IdentifierExpression&>(un.Operand()).Name();
            }
            if (un.Op() == "*" && un.Operand().Kind() == CExpressionKind::kCastExpression) {
              const auto& cast = static_cast<const CastExpression&>(un.Operand());
              if (cast.TargetType().name == "f32" && cast.TargetType().pointer_depth == 1 &&
                  cast.Operand().Kind() == CExpressionKind::kUnaryExpression) {
                const auto& addr_un = static_cast<const UnaryExpression&>(cast.Operand());
                if (addr_un.Op() == "&" &&
                    addr_un.Operand().Kind() == CExpressionKind::kIdentifier) {
                  return static_cast<const IdentifierExpression&>(addr_un.Operand()).Name();
                }
              }
            }
            return "";
          };
          std::string rhs = static_cast<const IdentifierExpression&>(a.Rhs()).Name();
          std::string_view slot_var = extract_f32_store_slot_var(a.Lhs());
          auto it = f32_param_homing_slots.find(rhs);
          if (!slot_var.empty() && it != f32_param_homing_slots.end() && it->second == slot_var &&
              !is_assigned(rhs) && count_assignments(slot_var) == 0) {
            bool slot_only_homed_here = true;
            for (size_t k = 0; k < comp->Statements().size(); ++k) {
              if (k == i || comp->Statements()[k] == nullptr) continue;
              if (StatementModifiesVariable(*comp->Statements()[k], slot_var) ||
                  StatementTakesAddressOf(*comp->Statements()[k], slot_var)) {
                slot_only_homed_here = false;
                break;
              }
            }
            if (slot_only_homed_here) {
              coalesced_expr_aliases[std::string(slot_var)] =
                  CExpression::Unary("*(s32*)&", CExpression::Identifier(rhs));
              stmts_to_erase.push_back(i);
            }
          }
        }
      }
    }
  }

  if (coalesced_aliases.empty() && coalesced_expr_aliases.empty()) return;

  auto apply_aliases_to_expr =
      [&](std::unique_ptr<CExpression> expr) -> std::unique_ptr<CExpression> {
    for (const auto& [from, to] : coalesced_aliases) {
      expr = SubstituteIdentifier(*expr, from, *CExpression::Identifier(to));
    }
    for (const auto& [from, to_expr] : coalesced_expr_aliases) {
      expr = SubstituteIdentifier(*expr, from, *to_expr);
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

void SplitReusedFloatGprParameters(CompoundStatement* comp,
                                   const std::vector<CParameter>& parameters) {
  if (comp == nullptr) return;

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
        stmts[k] = SubstituteIdentifierInStatement(std::move(stmts[k]), p.name, temp_name);
      }
    }
  }
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
  size_t matching_s_splits = 0;
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
    } else if (IsCalleeSavedGprVarName(prologue_copy_lhs)) {
      if (has_goto && parameters.size() == 1) {
        any_s_split_in_single_param = true;
      }
      if (prologue_copy_lhs == absl::StrCat("temp_s", param.name.substr(3))) {
        ++matching_s_splits;
      }
    }
  }
  if (matching_s_splits == parameters.size()) {
    bool has_higher_s_reg = false;
    for (size_t s_idx = parameters.size(); s_idx <= 7; ++s_idx) {
      if (StatementReferencesVariable(*body, absl::StrCat("temp_s", s_idx))) {
        has_higher_s_reg = true;
        break;
      }
    }
    if (!has_higher_s_reg) {
      any_s_split_in_single_param = true;
    }
  }
  if (any_homed_split || any_s_split_in_single_param) {
    CoalescePrologueParameterCopies(body, parameters, frame_size);
  }
  return any_s_split_in_single_param;
}

}  // namespace rom_nom_nom
