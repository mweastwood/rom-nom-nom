#include "lifter/ast_expression_folder.h"

#include <cctype>
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
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"

namespace rom_nom_nom {

namespace {

struct FolderContext {
  CompoundStatement* comp;
  const CompoundStatement* root_body;
  const absl::flat_hash_set<std::string>* dead_at_end_vars;
  bool is_switch_case_body;
  int root_has_label_cached;

  static bool MatchesVarIdentifier(std::string_view id_name, std::string_view var) {
    if (id_name == var) return true;
    if (absl::StartsWith(id_name, "((") && absl::EndsWith(id_name, ")")) {
      std::string suffix = absl::StrCat(")", var, ")");
      if (absl::EndsWith(id_name, suffix)) return true;
    }
    return false;
  }

  static int CountUses(const CStatement& s, std::string_view var) {
    return CountVariableReadUsesInStatement(s, var);
  }

  static bool IsPureExpr(const CExpression& expr) { return IsPureExpression(expr); }

  static bool HasAddressOf(const CExpression& expr) {
    bool found = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (found) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          return;
        }
        if (un.Op().find('&') != std::string::npos) {
          found = true;
          return;
        }
        check(un.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check(static_cast<const BinaryExpression&>(e).Lhs());
        check(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        check(static_cast<const ArrayIndexExpression&>(e).Array());
        check(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        check(static_cast<const MemberAccessExpression&>(e).Object());
      }
    };
    check(expr);
    return found;
  }

  static std::string_view ExtractFloatBitcastLhsVar(const CExpression& lhs) {
    return ::rom_nom_nom::ExtractFloatBitcastLhsVar(lhs);
  }

  static std::string_view ExtractAssignedLhsVar(const CExpression& lhs) {
    return ::rom_nom_nom::ExtractAssignedLhsVar(lhs);
  }

  static bool HasAddressOfVar(const CExpression& expr, std::string_view var) {
    return ExpressionTakesAddressOf(expr, var, /*ignore_assigned_float_bitcast_lhs=*/true);
  }

  static bool UsedInAdditionOrSubtraction(const CExpression& expr, std::string_view var) {
    bool in_add_sub = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (in_add_sub) return;
      if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        if (bin.Op() == "+" || bin.Op() == "-") {
          auto check_side = [&](const CExpression& side) {
            if (side.Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(side).Name() == var) {
              in_add_sub = true;
            }
          };
          check_side(bin.Lhs());
          check_side(bin.Rhs());
        }
        check(bin.Lhs());
        check(bin.Rhs());
      } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(e);
        if (assign.Op() == "+=" || assign.Op() == "-=") {
          if (assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(assign.Rhs()).Name() == var) {
            in_add_sub = true;
          }
        }
        check(assign.Lhs());
        check(assign.Rhs());
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        check(static_cast<const UnaryExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        check(static_cast<const ArrayIndexExpression&>(e).Array());
        check(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        check(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check(static_cast<const TernaryExpression&>(e).Condition());
        check(static_cast<const TernaryExpression&>(e).TrueExpression());
        check(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        check(static_cast<const CallExpression&>(e).Callee());
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check(*arg);
        }
      }
    };
    check(expr);
    return in_add_sub;
  }

  static int CountExprUses(const CExpression& e, std::string_view var) {
    return CountVariableReadUsesInExpression(e, var);
  }

  static bool HasAddressOfVarInStmt(const CStatement& root_s, std::string_view var) {
    return StatementTakesAddressOf(root_s, var, /*ignore_assigned_float_bitcast_lhs=*/true);
  }

  static bool HasLabelInStmt(const CStatement& root_s) { return StatementContainsLabel(root_s); }

  static bool HasControlJumpInStmt(const CStatement& root_s) {
    return StatementContainsControlFlowJump(root_s);
  }

  static bool IsStackParamOrLocalName(std::string_view id_name) {
    if (absl::StartsWith(id_name, "var_sp_")) return true;
    if (id_name.size() >= 4 && absl::StartsWith(id_name, "arg") && id_name != "arg0" &&
        id_name != "arg1" && id_name != "arg2" && id_name != "arg3") {
      return true;
    }
    return false;
  }

  static bool HasMemoryDeref(const CExpression& expr) {
    bool found = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (found) return;
      if (e.Kind() == CExpressionKind::kIdentifier) {
        std::string_view id_name = static_cast<const IdentifierExpression&>(e).Name();
        if (IsStackParamOrLocalName(id_name)) {
          found = true;
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          if ((un.Op() == "*(s32*)&" || un.Op() == "*(u32*)&" || un.Op() == "*(f32*)&" ||
               un.Op() == "*(s32 *)&" || un.Op() == "*(u32 *)&" || un.Op() == "*(f32 *)&") &&
              un.Operand().Kind() == CExpressionKind::kIdentifier) {
            std::string_view id_name =
                static_cast<const IdentifierExpression&>(un.Operand()).Name();
            if (!IsStackParamOrLocalName(id_name) && !absl::StartsWith(id_name, "D_") &&
                !absl::StartsWith(id_name, "g_") && !absl::StartsWith(id_name, "RO_") &&
                !absl::StartsWith(id_name, "B_")) {
              return;
            }
          }
          found = true;
          return;
        }
        check(un.Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        found = true;
        return;
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        if (static_cast<const MemberAccessExpression&>(e).IsArrow()) {
          found = true;
          return;
        }
        check(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check(static_cast<const BinaryExpression&>(e).Lhs());
        check(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check(static_cast<const TernaryExpression&>(e).Condition());
        check(static_cast<const TernaryExpression&>(e).TrueExpression());
        check(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        check(static_cast<const CallExpression&>(e).Callee());
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check(*arg);
        }
      }
    };
    check(expr);
    return found;
  }

  static bool IsCallOrWrappedCall(const CExpression& expr) {
    int call_count = 0;
    bool invalid = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (invalid) return;
      if (e.Kind() == CExpressionKind::kCallExpression) {
        call_count++;
        if (call_count > 1) {
          invalid = true;
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kAssignmentExpression ||
          e.Kind() == CExpressionKind::kArrayIndexExpression) {
        invalid = true;
        return;
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          invalid = true;
          return;
        }
        check(un.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        if (bin.Op() == "&&" || bin.Op() == "||") {
          invalid = true;
          return;
        }
        check(bin.Lhs());
        check(bin.Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        invalid = true;
      }
    };
    check(expr);
    return !invalid && call_count == 1;
  }

  static bool IsUnconditionallyEvaluatedFirst(const CExpression& expr, std::string_view var) {
    bool seen_var = false;
    bool seen_side_effect_before_var = false;
    bool in_conditional_branch = false;
    std::function<void(const CExpression&, bool)> walk = [&](const CExpression& e,
                                                             bool cond_branch) {
      if (seen_var) return;
      if (e.Kind() == CExpressionKind::kIdentifier) {
        if (MatchesVarIdentifier(static_cast<const IdentifierExpression&>(e).Name(), var)) {
          seen_var = true;
          in_conditional_branch = cond_branch;
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kCallExpression ||
          e.Kind() == CExpressionKind::kAssignmentExpression) {
        seen_side_effect_before_var = true;
        return;
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          seen_side_effect_before_var = true;
        }
        walk(un.Operand(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        walk(bin.Lhs(), cond_branch);
        bool rhs_cond = cond_branch || (bin.Op() == "&&" || bin.Op() == "||");
        walk(bin.Rhs(), rhs_cond);
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        walk(static_cast<const CastExpression&>(e).Operand(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        seen_side_effect_before_var = true;
        walk(static_cast<const ArrayIndexExpression&>(e).Array(), cond_branch);
        walk(static_cast<const ArrayIndexExpression&>(e).Index(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        if (static_cast<const MemberAccessExpression&>(e).IsArrow()) {
          seen_side_effect_before_var = true;
        }
        walk(static_cast<const MemberAccessExpression&>(e).Object(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        const auto& tern = static_cast<const TernaryExpression&>(e);
        walk(tern.Condition(), cond_branch);
        walk(tern.TrueExpression(), true);
        walk(tern.FalseExpression(), true);
      }
    };
    walk(expr, false);
    return seen_var && !in_conditional_branch && !seen_side_effect_before_var;
  }

  static bool IsScratchVar(std::string_view var) {
    return var == "v0" || var == "v1" || var == "at" || var == "temp_at" || var == "fcond" ||
           var == "f0" || var == "f0_d" || var == "f2" || var == "f2_d" || var == "f4" ||
           var == "f4_d" || var == "f6" || var == "f6_d" || var == "f8" || var == "f8_d" ||
           var == "f10" || var == "f10_d" || var == "f16" || var == "f16_d" || var == "f18" ||
           var == "f18_d" || absl::StartsWith(var, "temp_t") ||
           (var.size() >= 2 && var[0] == 't' && std::isdigit(var[1]));
  }

  static bool IsOutgoingStackArgVar(std::string_view var) {
    if (!absl::StartsWith(var, "var_sp_")) return false;
    int32_t offset = 0;
    if (!absl::SimpleAtoi(var.substr(7), &offset)) return false;
    return offset >= 16 && (offset % 4 == 0) && offset < 128;
  }

  static bool IsCallArgVar(std::string_view var) {
    return var == "arg0" || var == "arg1" || var == "arg2" || var == "arg3";
  }

  enum class VarLiveness {
    kTransparent,
    kReadBeforeWrite,
    kKilledBeforeRead,
  };

  bool root_has_label = false;
  const CStatement* active_ignored_def_stmt = nullptr;
  const CStatement* active_ignored_use_stmt = nullptr;

  VarLiveness AnalyzeVarLiveness(const CStatement& s, std::string_view var, bool in_switch_case_top,
                                 int goto_depth) {
    if (&s == active_ignored_def_stmt || &s == active_ignored_use_stmt) {
      return VarLiveness::kTransparent;
    }
    switch (s.Kind()) {
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(s);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          std::string_view lhs_var = (a.Op() == "=") ? ExtractAssignedLhsVar(a.Lhs()) : "";
          if (!lhs_var.empty() && lhs_var == var) {
            return CountExprUses(a.Rhs(), var) == 0 ? VarLiveness::kKilledBeforeRead
                                                    : VarLiveness::kReadBeforeWrite;
          }
        }
        return CountExprUses(es.Expression(), var) > 0 ? VarLiveness::kReadBeforeWrite
                                                       : VarLiveness::kTransparent;
      }
      case CStatementKind::kReturnStatement:
        return CountUses(s, var) == 0 ? VarLiveness::kKilledBeforeRead
                                      : VarLiveness::kReadBeforeWrite;
      case CStatementKind::kBreakStatement:
        return in_switch_case_top ? VarLiveness::kTransparent : VarLiveness::kReadBeforeWrite;
      case CStatementKind::kContinueStatement:
        return VarLiveness::kReadBeforeWrite;
      case CStatementKind::kGotoStatement: {
        if (goto_depth > 4 || root_body == nullptr) {
          return VarLiveness::kReadBeforeWrite;
        }
        const auto& gs = static_cast<const GotoStatement&>(s);
        return AnalyzeFromLabel(gs.Label(), var, goto_depth + 1);
      }
      case CStatementKind::kLabelStatement:
        return VarLiveness::kTransparent;
      case CStatementKind::kCompoundStatement: {
        const auto& children = static_cast<const CompoundStatement&>(s).Statements();
        for (size_t c = 0; c < children.size(); ++c) {
          const auto& child = children[c];
          if (child->Kind() == CStatementKind::kBreakStatement && in_switch_case_top) {
            return VarLiveness::kTransparent;
          }
          VarLiveness res = AnalyzeVarLiveness(*child, var, in_switch_case_top, goto_depth);
          if (res == VarLiveness::kReadBeforeWrite) return VarLiveness::kReadBeforeWrite;
          if (res == VarLiveness::kKilledBeforeRead) {
            return VarLiveness::kKilledBeforeRead;
          }
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        if (CountExprUses(ifs.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        VarLiveness then_res =
            AnalyzeVarLiveness(ifs.ThenBranch(), var, in_switch_case_top, goto_depth);
        VarLiveness else_res =
            (ifs.ElseBranch() != nullptr)
                ? AnalyzeVarLiveness(*ifs.ElseBranch(), var, in_switch_case_top, goto_depth)
                : VarLiveness::kTransparent;
        if (then_res == VarLiveness::kReadBeforeWrite ||
            else_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        if (then_res == VarLiveness::kKilledBeforeRead &&
            else_res == VarLiveness::kKilledBeforeRead) {
          return VarLiveness::kKilledBeforeRead;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        if (CountExprUses(ss.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        bool has_default = false;
        bool all_killed = !ss.Cases().empty();
        for (const auto& sc : ss.Cases()) {
          if (sc.is_default) has_default = true;
          VarLiveness case_res =
              (sc.body != nullptr)
                  ? AnalyzeVarLiveness(*sc.body, var, /*in_switch_case_top=*/true, goto_depth)
                  : VarLiveness::kTransparent;
          if (case_res == VarLiveness::kReadBeforeWrite) {
            return VarLiveness::kReadBeforeWrite;
          }
          if (case_res != VarLiveness::kKilledBeforeRead) {
            all_killed = false;
          }
        }
        if (has_default && all_killed) {
          return VarLiveness::kKilledBeforeRead;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(s);
        if (CountExprUses(ws.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        VarLiveness body_res = AnalyzeVarLiveness(ws.Body(), var, false, goto_depth);
        if (body_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(s);
        VarLiveness body_res = AnalyzeVarLiveness(dws.Body(), var, false, goto_depth);
        if (body_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        if (body_res == VarLiveness::kKilledBeforeRead && !HasLabelInStmt(dws.Body())) {
          return VarLiveness::kKilledBeforeRead;
        }
        if (CountExprUses(dws.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) {
          VarLiveness init_res = AnalyzeVarLiveness(*fs.Init(), var, false, goto_depth);
          if (init_res == VarLiveness::kReadBeforeWrite) return VarLiveness::kReadBeforeWrite;
          if (init_res == VarLiveness::kKilledBeforeRead) return VarLiveness::kKilledBeforeRead;
        }
        if (fs.Condition() != nullptr && CountExprUses(*fs.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        VarLiveness body_res = AnalyzeVarLiveness(fs.Body(), var, false, goto_depth);
        if (body_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        if (body_res != VarLiveness::kKilledBeforeRead && fs.Step() != nullptr &&
            CountExprUses(*fs.Step(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        return VarLiveness::kTransparent;
      }
      default:
        return (CountUses(s, var) > 0 || (root_has_label && HasControlJumpInStmt(s)))
                   ? VarLiveness::kReadBeforeWrite
                   : VarLiveness::kTransparent;
    }
  }

  VarLiveness AnalyzeFromLabel(std::string_view target_label, std::string_view var,
                               int goto_depth) {
    if (goto_depth > 4 || root_body == nullptr) {
      return VarLiveness::kReadBeforeWrite;
    }
    const CompoundStatement* found_comp = nullptr;
    size_t found_idx = 0;
    std::function<void(const CStatement&)> find_label = [&](const CStatement& s) {
      if (found_comp != nullptr) return;
      if (s.Kind() == CStatementKind::kCompoundStatement) {
        const auto& cs = static_cast<const CompoundStatement&>(s);
        for (size_t idx = 0; idx < cs.Statements().size(); ++idx) {
          const CStatement* cur = cs.Statements()[idx].get();
          if (cur != nullptr && cur->Kind() == CStatementKind::kLabelStatement) {
            const auto& ls = static_cast<const LabelStatement&>(*cur);
            if (ls.Label() == target_label) {
              found_comp = &cs;
              found_idx = idx;
              return;
            }
          }
          find_label(*cs.Statements()[idx]);
          if (found_comp != nullptr) return;
        }
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        find_label(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) find_label(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
          if (sc.body != nullptr) find_label(*sc.body);
        }
      } else if (s.Kind() == CStatementKind::kWhileStatement) {
        find_label(static_cast<const WhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
        find_label(static_cast<const DoWhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kForStatement) {
        find_label(static_cast<const ForStatement&>(s).Body());
      }
    };
    find_label(*root_body);
    if (found_comp == nullptr) {
      return VarLiveness::kReadBeforeWrite;
    }
    for (size_t m = found_idx; m < found_comp->Statements().size(); ++m) {
      VarLiveness res = AnalyzeVarLiveness(*found_comp->Statements()[m], var, false, goto_depth);
      if (res == VarLiveness::kReadBeforeWrite) return VarLiveness::kReadBeforeWrite;
      if (res == VarLiveness::kKilledBeforeRead) return VarLiveness::kKilledBeforeRead;
    }
    if (found_comp == root_body) {
      return VarLiveness::kKilledBeforeRead;
    }
    return VarLiveness::kReadBeforeWrite;
  }

  static bool IsComparisonExpr(const CExpression& e) {
    if (e.Kind() != CExpressionKind::kBinaryExpression) return false;
    std::string_view op = static_cast<const BinaryExpression&>(e).Op();
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
  }

  static bool HasCallInStmt(const CStatement& root_s) {
    return StatementContainsCall(root_s, /*ignore_math_intrinsics=*/false);
  }

  static bool HasLoopInStmt(const CStatement& root_s) { return StatementContainsLoop(root_s); }

  bool IsDeadAfterStmt(size_t next_idx, std::string_view var, int expected_uses_in_next = 1,
                       const CStatement* def_stmt = nullptr) {
    const auto& stmts = comp->Statements();
    if (next_idx >= stmts.size()) {
      return false;
    }
    if (stmts[next_idx]->Kind() == CStatementKind::kReturnStatement) {
      return !HasAddressOfVarInStmt(*stmts[next_idx], var);
    }
    if (stmts[next_idx]->Kind() != CStatementKind::kIfStatement &&
        stmts[next_idx]->Kind() != CStatementKind::kSwitchStatement &&
        HasControlJumpInStmt(*stmts[next_idx])) {
      return false;
    }

    if (IsOutgoingStackArgVar(var) && expected_uses_in_next > 0 &&
        HasCallInStmt(*stmts[next_idx]) && root_body != nullptr &&
        !HasAddressOfVarInStmt(*root_body, var)) {
      int total_uses_in_root = CountUses(*root_body, var);
      if (total_uses_in_root == expected_uses_in_next) {
        return true;
      }
      int uses_in_calls = 0;
      std::function<void(const CExpression&)> count_call_uses_expr = [&](const CExpression& e) {
        if (e.Kind() == CExpressionKind::kCallExpression) {
          for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
            uses_in_calls += CountExprUses(*arg, var);
          }
          return;
        }
        if (e.Kind() == CExpressionKind::kUnaryExpression) {
          count_call_uses_expr(static_cast<const UnaryExpression&>(e).Operand());
        } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
          count_call_uses_expr(static_cast<const BinaryExpression&>(e).Lhs());
          count_call_uses_expr(static_cast<const BinaryExpression&>(e).Rhs());
        } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
          count_call_uses_expr(static_cast<const AssignmentExpression&>(e).Lhs());
          count_call_uses_expr(static_cast<const AssignmentExpression&>(e).Rhs());
        } else if (e.Kind() == CExpressionKind::kCastExpression) {
          count_call_uses_expr(static_cast<const CastExpression&>(e).Operand());
        } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
          const auto& tern = static_cast<const TernaryExpression&>(e);
          count_call_uses_expr(tern.Condition());
          count_call_uses_expr(tern.TrueExpression());
          count_call_uses_expr(tern.FalseExpression());
        }
      };
      bool all_call_uses_locally_defined = true;
      std::function<void(const CStatement&)> count_call_uses_stmt = [&](const CStatement& s) {
        if (s.Kind() == CStatementKind::kExpressionStatement) {
          count_call_uses_expr(static_cast<const ExpressionStatement&>(s).Expression());
        } else if (s.Kind() == CStatementKind::kReturnStatement) {
          const auto& ret = static_cast<const ReturnStatement&>(s);
          if (ret.ReturnValue() != nullptr) count_call_uses_expr(*ret.ReturnValue());
        } else if (s.Kind() == CStatementKind::kCompoundStatement) {
          const auto& cs_stmts = static_cast<const CompoundStatement&>(s).Statements();
          bool defined_in_comp = false;
          for (const auto& child : cs_stmts) {
            if (child == nullptr) continue;
            if (child->Kind() == CStatementKind::kLabelStatement) {
              defined_in_comp = false;
            }
            if (child->Kind() == CStatementKind::kExpressionStatement) {
              const auto& es = static_cast<const ExpressionStatement&>(*child);
              if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
                if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                    static_cast<const IdentifierExpression&>(a.Lhs()).Name() == var &&
                    CountExprUses(a.Rhs(), var) == 0) {
                  defined_in_comp = true;
                  continue;
                }
              }
            }
            if (child.get() != stmts[next_idx].get() && CountUses(*child, var) > 0 &&
                !defined_in_comp) {
              all_call_uses_locally_defined = false;
            }
            count_call_uses_stmt(*child);
          }
        } else if (s.Kind() == CStatementKind::kIfStatement) {
          const auto& ifs = static_cast<const IfStatement&>(s);
          count_call_uses_expr(ifs.Condition());
          count_call_uses_stmt(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) count_call_uses_stmt(*ifs.ElseBranch());
        } else if (s.Kind() == CStatementKind::kSwitchStatement) {
          const auto& ss = static_cast<const SwitchStatement&>(s);
          count_call_uses_expr(ss.Condition());
          for (const auto& sc : ss.Cases()) {
            if (sc.body != nullptr) count_call_uses_stmt(*sc.body);
          }
        } else if (s.Kind() == CStatementKind::kWhileStatement) {
          const auto& ws = static_cast<const WhileStatement&>(s);
          count_call_uses_expr(ws.Condition());
          count_call_uses_stmt(ws.Body());
        } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
          const auto& dws = static_cast<const DoWhileStatement&>(s);
          count_call_uses_stmt(dws.Body());
          count_call_uses_expr(dws.Condition());
        } else if (s.Kind() == CStatementKind::kForStatement) {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr) count_call_uses_stmt(*fs.Init());
          if (fs.Condition() != nullptr) count_call_uses_expr(*fs.Condition());
          if (fs.Step() != nullptr) count_call_uses_expr(*fs.Step());
          count_call_uses_stmt(fs.Body());
        }
      };
      count_call_uses_stmt(*root_body);
      if (total_uses_in_root == uses_in_calls && all_call_uses_locally_defined) {
        return true;
      }
    }

    active_ignored_def_stmt = def_stmt;
    active_ignored_use_stmt = (expected_uses_in_next > 0) ? stmts[next_idx].get() : nullptr;
    for (size_t k = next_idx + 1; k < stmts.size(); ++k) {
      const auto& s = *stmts[k];
      if (s.Kind() == CStatementKind::kBreakStatement && is_switch_case_body) {
        break;
      }
      VarLiveness res = AnalyzeVarLiveness(s, var, is_switch_case_body, 0);
      if (res == VarLiveness::kKilledBeforeRead) {
        active_ignored_def_stmt = nullptr;
        active_ignored_use_stmt = nullptr;
        return true;
      }
      if (res == VarLiveness::kReadBeforeWrite) {
        active_ignored_def_stmt = nullptr;
        active_ignored_use_stmt = nullptr;
        return false;
      }
    }
    active_ignored_def_stmt = nullptr;
    active_ignored_use_stmt = nullptr;
    if (comp == root_body || (dead_at_end_vars != nullptr && dead_at_end_vars->contains(var))) {
      return true;
    }
    if (expected_uses_in_next == 0 && root_body != nullptr && var != "v0" && var != "f0" &&
        var != "f0_d" && CountUses(*root_body, var) == 0 &&
        !HasAddressOfVarInStmt(*root_body, var)) {
      return true;
    }
    bool has_label_between_def_and_use = true;
    if (def_stmt != nullptr) {
      bool seen_def = false;
      for (size_t k = 0; k <= next_idx && k < stmts.size(); ++k) {
        if (stmts[k].get() == def_stmt) {
          seen_def = true;
          has_label_between_def_and_use = false;
        } else if (seen_def && HasLabelInStmt(*stmts[k])) {
          has_label_between_def_and_use = true;
          break;
        }
      }
    }
    if (expected_uses_in_next > 0 && def_stmt != nullptr && root_body != nullptr &&
        (!root_has_label || (!has_label_between_def_and_use && IsCallArgVar(var))) && var != "v0" &&
        var != "f0" && var != "f0_d" && !HasLoopInStmt(*stmts[next_idx]) &&
        CountUses(*root_body, var) == expected_uses_in_next &&
        !HasAddressOfVarInStmt(*root_body, var)) {
      return true;
    }
    if ((IsOutgoingStackArgVar(var) ||
         (IsCallArgVar(var) && expected_uses_in_next > 0 && HasCallInStmt(*stmts[next_idx]))) &&
        !root_has_label) {
      return true;
    }
    return false;
  }

  static bool IsDuplicableArgRhs(const CExpression& rhs) {
    if (rhs.Kind() == CExpressionKind::kIdentifier ||
        rhs.Kind() == CExpressionKind::kIntegerLiteral ||
        rhs.Kind() == CExpressionKind::kFloatLiteral) {
      return true;
    }
    if (rhs.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(rhs);
      if ((un.Op() == "&" || un.Op() == "-" || un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") &&
          (un.Operand().Kind() == CExpressionKind::kIdentifier ||
           un.Operand().Kind() == CExpressionKind::kIntegerLiteral)) {
        return true;
      }
    }
    return false;
  }

  static bool ExtractSimpleDerefRange(const CExpression& e, std::string_view* out_base,
                                      int64_t* out_offset, int64_t* out_size,
                                      bool* out_is_addr_of = nullptr) {
    if (out_is_addr_of != nullptr) *out_is_addr_of = false;
    if (e.Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(e);
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    std::string ts = cast.TargetType().ToString();
    int64_t size = 4;
    if (ts == "s8*" || ts == "u8*" || ts == "s8 *" || ts == "u8 *") {
      size = 1;
    } else if (ts == "s16*" || ts == "u16*" || ts == "s16 *" || ts == "u16 *") {
      size = 2;
    } else if (ts == "s64*" || ts == "u64*" || ts == "f64*" || ts == "s64 *" || ts == "u64 *" ||
               ts == "f64 *") {
      size = 8;
    }
    const CExpression& addr = cast.Operand();
    if (addr.Kind() == CExpressionKind::kIdentifier) {
      *out_base = static_cast<const IdentifierExpression&>(addr).Name();
      *out_offset = 0;
      *out_size = size;
      return true;
    }
    if (addr.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& addr_un = static_cast<const UnaryExpression&>(addr);
      if (addr_un.Op() == "&" && addr_un.Operand().Kind() == CExpressionKind::kIdentifier) {
        *out_base = static_cast<const IdentifierExpression&>(addr_un.Operand()).Name();
        *out_offset = 0;
        *out_size = size;
        if (out_is_addr_of != nullptr) *out_is_addr_of = true;
        return true;
      }
    }
    if (addr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(addr);
      if ((bin.Op() == "+" || bin.Op() == "-") &&
          bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t off = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        if (bin.Lhs().Kind() == CExpressionKind::kIdentifier) {
          *out_base = static_cast<const IdentifierExpression&>(bin.Lhs()).Name();
          *out_offset = (bin.Op() == "-") ? -off : off;
          *out_size = size;
          return true;
        }
        if (bin.Lhs().Kind() == CExpressionKind::kUnaryExpression) {
          const auto& lhs_un = static_cast<const UnaryExpression&>(bin.Lhs());
          if (lhs_un.Op() == "&" && lhs_un.Operand().Kind() == CExpressionKind::kIdentifier) {
            *out_base = static_cast<const IdentifierExpression&>(lhs_un.Operand()).Name();
            *out_offset = (bin.Op() == "-") ? -off : off;
            *out_size = size;
            if (out_is_addr_of != nullptr) *out_is_addr_of = true;
            return true;
          }
        }
      }
    }
    return false;
  }

  static bool AreProvablyDisjointDerefs(const CExpression& a, const CExpression& b) {
    std::string_view base_a;
    std::string_view base_b;
    int64_t off_a = 0;
    int64_t off_b = 0;
    int64_t size_a = 0;
    int64_t size_b = 0;
    bool addr_of_a = false;
    bool addr_of_b = false;
    if (!ExtractSimpleDerefRange(a, &base_a, &off_a, &size_a, &addr_of_a) ||
        !ExtractSimpleDerefRange(b, &base_b, &off_b, &size_b, &addr_of_b)) {
      return false;
    }
    if (base_a == base_b) {
      return (off_a + size_a <= off_b) || (off_b + size_b <= off_a);
    }
    if (addr_of_a && addr_of_b) {
      return true;
    }
    if ((addr_of_a != addr_of_b) && size_a != size_b) {
      return true;
    }
    return false;
  }

  static bool AreAllDerefsProvablyDisjoint(const CExpression& expr, const CExpression& store_lhs) {
    bool found_deref = false;
    bool all_disjoint = true;
    std::function<void(const CExpression&)> walk = [&](const CExpression& e) {
      if (!all_disjoint) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op() == "*") {
          found_deref = true;
          if (!AreProvablyDisjointDerefs(e, store_lhs)) {
            all_disjoint = false;
          }
          return;
        }
        if (un.Op() == "*(s32*)&" || un.Op() == "*(u32*)&" || un.Op() == "*(f32*)&" ||
            un.Op() == "*(s32 *)&" || un.Op() == "*(u32 *)&" || un.Op() == "*(f32 *)&") {
          if (un.Operand().Kind() == CExpressionKind::kIdentifier) {
            std::string_view id_name =
                static_cast<const IdentifierExpression&>(un.Operand()).Name();
            if (!absl::StartsWith(id_name, "D_") && !absl::StartsWith(id_name, "g_") &&
                !absl::StartsWith(id_name, "RO_") && !absl::StartsWith(id_name, "B_")) {
              return;
            }
            all_disjoint = false;
            return;
          }
          walk(un.Operand());
          return;
        }
        walk(un.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        walk(bin.Lhs());
        walk(bin.Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        walk(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kIdentifier ||
                 e.Kind() == CExpressionKind::kIntegerLiteral ||
                 e.Kind() == CExpressionKind::kFloatLiteral) {
        return;
      } else {
        all_disjoint = false;
      }
    };
    walk(expr);
    return found_deref && all_disjoint;
  }

  // NOLINTNEXTLINE(google-readability-function-size)
  void FoldStatementsInBlock() {
    bool changed = true;
    while (changed) {
      changed = false;
      auto& stmts = comp->MutableStatements();
      for (size_t i = 0; i + 1 < stmts.size(); ++i) {
        if (stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
          const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
          if (es0.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& assign = static_cast<const AssignmentExpression&>(es0.Expression());
            std::string_view extracted_var =
                (assign.Op() == "=") ? ExtractAssignedLhsVar(assign.Lhs()) : "";
            bool is_float_bitcast_lhs =
                (!extracted_var.empty() && assign.Lhs().Kind() != CExpressionKind::kIdentifier);
            if (!extracted_var.empty() &&
                (!is_float_bitcast_lhs || IsOutgoingStackArgVar(extracted_var))) {
              std::string var(extracted_var);
              bool is_scratch = IsScratchVar(var);
              bool is_stack_arg = IsOutgoingStackArgVar(var);
              bool is_call_arg = IsCallArgVar(var);
              bool is_fp_param_in_place_before_return =
                  ((var == "f12" || var == "f14" || var == "f12_d" || var == "f14_d") &&
                   !is_float_bitcast_lhs && i + 1 < stmts.size() &&
                   stmts[i + 1]->Kind() == CStatementKind::kReturnStatement &&
                   IsPureExpr(assign.Rhs()) && !HasMemoryDeref(assign.Rhs()));
              if (is_fp_param_in_place_before_return) {
                const auto& ret_stmt = static_cast<const ReturnStatement&>(*stmts[i + 1]);
                if (ret_stmt.ReturnValue() != nullptr &&
                    CountExprUses(*ret_stmt.ReturnValue(), var) == 1 &&
                    IsPureExpr(*ret_stmt.ReturnValue()) &&
                    !HasMemoryDeref(*ret_stmt.ReturnValue()) &&
                    !HasAddressOfVar(*ret_stmt.ReturnValue(), var)) {
                  auto new_ret = SubstituteIdentifier(*ret_stmt.ReturnValue(), var, assign.Rhs());
                  stmts[i + 1] = CStatement::Return(std::move(new_ret));
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }
              auto is_float_literal_expr = [](const CExpression& e) -> bool {
                if (e.Kind() == CExpressionKind::kFloatLiteral) return true;
                if (e.Kind() == CExpressionKind::kIdentifier) {
                  std::string_view name = static_cast<const IdentifierExpression&>(e).Name();
                  return !name.empty() &&
                         (std::isdigit(static_cast<unsigned char>(name[0])) ||
                          (name.size() > 1 && name[0] == '-' &&
                           std::isdigit(static_cast<unsigned char>(name[1])))) &&
                         absl::StrContains(name, '.');
                }
                return false;
              };
              if (var.size() >= 2 && var[0] == 'f' && std::isdigit(var[1]) &&
                  !is_float_bitcast_lhs && IsPureExpr(assign.Rhs())) {
                // 1. Subword half of a 64-bit double constant load: `f2 = <lit>; f2_d = <lit>;`
                // or `f2_d = <lit>; f2_d = <lit>;`
                if (i + 1 < stmts.size() &&
                    stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
                    (is_float_literal_expr(assign.Rhs()) ||
                     assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral)) {
                  const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                  if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& next_a =
                        static_cast<const AssignmentExpression&>(next_es.Expression());
                    if (next_a.Op() == "=" && next_a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        (is_float_literal_expr(next_a.Rhs()) ||
                         next_a.Rhs().Kind() == CExpressionKind::kIntegerLiteral)) {
                      std::string_view next_lhs =
                          static_cast<const IdentifierExpression&>(next_a.Lhs()).Name();
                      bool is_odd_half_of_next_double = false;
                      int var_reg = -1;
                      if (absl::SimpleAtoi(var.substr(1), &var_reg) && (var_reg % 2 == 1) &&
                          var_reg >= 1) {
                        if (next_lhs == absl::StrFormat("f%d_d", var_reg - 1)) {
                          is_odd_half_of_next_double = true;
                        }
                      }
                      if (next_lhs == var || next_lhs == absl::StrCat(var, "_d") ||
                          is_odd_half_of_next_double) {
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                        break;
                      }
                    }
                  }
                }
                // 2. Completely unread FPR literal assignment (e.g., dead odd/even half of a
                // synthesized double literal).
                bool is_literal_rhs = (assign.Rhs().Kind() == CExpressionKind::kFloatLiteral ||
                                       assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral);
                bool can_drop_unread_fpr =
                    is_literal_rhs &&
                    ((var != "f0" && var != "f0_d") ||
                     (dead_at_end_vars != nullptr && dead_at_end_vars->contains(var)));
                if (can_drop_unread_fpr && root_body != nullptr &&
                    CountUses(*root_body, var) == 0 && !HasAddressOfVarInStmt(*root_body, var)) {
                  bool half_referenced = false;
                  if (absl::EndsWith(var, "_d")) {
                    int even_reg = -1;
                    if (absl::SimpleAtoi(var.substr(1, var.size() - 3), &even_reg) &&
                        even_reg >= 0) {
                      std::string even_name = absl::StrFormat("f%d", even_reg);
                      std::string odd_name = absl::StrFormat("f%d", even_reg + 1);
                      if (CountUses(*root_body, even_name) > 0 ||
                          HasAddressOfVarInStmt(*root_body, even_name) ||
                          CountUses(*root_body, odd_name) > 0 ||
                          HasAddressOfVarInStmt(*root_body, odd_name)) {
                        half_referenced = true;
                      }
                    }
                  }
                  if (!half_referenced) {
                    stmts.erase(stmts.begin() + i);
                    changed = true;
                    break;
                  }
                }
              }
              if (!is_scratch && !is_stack_arg && !is_call_arg) continue;

              auto is_c_var_identifier = [](const CExpression& e) -> bool {
                if (e.Kind() != CExpressionKind::kIdentifier) return false;
                std::string_view name = static_cast<const IdentifierExpression&>(e).Name();
                return !name.empty() &&
                       (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
              };

              std::unique_ptr<CExpression> effective_rhs_holder;
              if (is_float_bitcast_lhs) {
                if (!IsPureExpr(assign.Rhs())) continue;
                bool is_f64_lhs =
                    assign.Lhs().Kind() == CExpressionKind::kUnaryExpression &&
                    static_cast<const UnaryExpression&>(assign.Lhs()).Operand().Kind() ==
                        CExpressionKind::kCastExpression &&
                    static_cast<const CastExpression&>(
                        static_cast<const UnaryExpression&>(assign.Lhs()).Operand())
                            .TargetType()
                            .name == "f64";
                effective_rhs_holder =
                    CExpression::Unary(is_f64_lhs ? "*(s64*)&" : "*(s32*)&", assign.Rhs().Clone());
              }
              const CExpression& effective_rhs =
                  is_float_bitcast_lhs ? *effective_rhs_holder : assign.Rhs();

              // 0. Dead pre-call argument/stack-arg store elimination:
              // When `arg0 = ...` or `var_sp_16 = ...` is pure and dead after stmt i (e.g., because
              // LiftCallInstruction already substituted its value into the call arguments), erase
              // it immediately so it does not artificially inflate use counts of `v0`/`v1`!
              if ((is_call_arg || is_stack_arg) && IsPureExpr(effective_rhs)) {
                bool followed_by_call = false;
                for (size_t k = i + 1; k < stmts.size() && k <= i + 40; ++k) {
                  if (HasCallInStmt(*stmts[k])) {
                    followed_by_call = true;
                    break;
                  }
                }
                bool dead_at_end_arg =
                    is_call_arg && dead_at_end_vars != nullptr && dead_at_end_vars->contains(var);
                if ((followed_by_call || dead_at_end_arg) &&
                    (IsDeadAfterStmt(i, var, 0, stmts[i].get()) ||
                     (dead_at_end_arg && root_body != nullptr && CountUses(*root_body, var) == 0 &&
                      !HasAddressOfVarInStmt(*root_body, var)))) {
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }

              bool killed_in_same_block = false;
              if (!is_switch_case_body) {
                for (size_t k = i + 1; k < stmts.size(); ++k) {
                  if (stmts[k]->Kind() == CStatementKind::kSwitchStatement ||
                      (stmts[k]->Kind() == CStatementKind::kDoWhileStatement &&
                       AnalyzeVarLiveness(*stmts[k], var, false, 0) ==
                           VarLiveness::kKilledBeforeRead)) {
                    killed_in_same_block = true;
                    break;
                  }
                  if (stmts[k]->Kind() == CStatementKind::kExpressionStatement) {
                    const auto& kes = static_cast<const ExpressionStatement&>(*stmts[k]);
                    if (kes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                      const auto& ka = static_cast<const AssignmentExpression&>(kes.Expression());
                      if (ka.Op() == "=" && ka.Lhs().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(ka.Lhs()).Name() == var) {
                        killed_in_same_block = true;
                        break;
                      }
                    }
                  }
                }
              }
              if ((var == "temp_at" ||
                   (var == "v0" && !is_float_bitcast_lhs &&
                    ((dead_at_end_vars != nullptr && dead_at_end_vars->contains(var)) ||
                     comp == root_body || killed_in_same_block) &&
                    (assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral ||
                     IsComparisonExpr(effective_rhs) ||
                     (effective_rhs.Kind() == CExpressionKind::kBinaryExpression &&
                      static_cast<const BinaryExpression&>(effective_rhs).Op() == "&")))) &&
                  IsPureExpr(effective_rhs) && IsDeadAfterStmt(i, var, 0, stmts[i].get())) {
                stmts.erase(stmts.begin() + i);
                changed = true;
                break;
              }

              // Dead scratch assignment overwritten later in the same CompoundStatement
              // across an intervening function call without any intervening read, or dead before a
              // call in the same block.
              if (is_scratch && !is_float_bitcast_lhs && IsPureExpr(effective_rhs)) {
                bool overwritten_before_use = false;
                bool saw_intervening_call = false;
                for (size_t k = i + 1; k < stmts.size(); ++k) {
                  if (stmts[k]->Kind() == CStatementKind::kDoWhileStatement &&
                      AnalyzeVarLiveness(*stmts[k], var, false, 0) ==
                          VarLiveness::kKilledBeforeRead) {
                    if (saw_intervening_call) {
                      overwritten_before_use = true;
                    } else {
                      for (size_t m = k; m < stmts.size(); ++m) {
                        if (HasCallInStmt(*stmts[m])) {
                          overwritten_before_use = true;
                          break;
                        }
                      }
                    }
                    break;
                  }
                  if (stmts[k]->Kind() != CStatementKind::kExpressionStatement) break;
                  const auto& kes = static_cast<const ExpressionStatement&>(*stmts[k]);
                  if (kes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& ka = static_cast<const AssignmentExpression&>(kes.Expression());
                    if (ka.Op() == "=" && ka.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        static_cast<const IdentifierExpression&>(ka.Lhs()).Name() == var &&
                        CountExprUses(ka.Rhs(), var) == 0) {
                      if (saw_intervening_call) {
                        overwritten_before_use = true;
                      } else {
                        for (size_t m = k + 1; m < stmts.size(); ++m) {
                          if (HasCallInStmt(*stmts[m])) {
                            overwritten_before_use = true;
                            break;
                          }
                        }
                      }
                      break;
                    }
                  }
                  if (CountUses(*stmts[k], var) > 0 || HasAddressOfVarInStmt(*stmts[k], var)) {
                    break;
                  }
                  if (absl::EndsWith(var, "_d")) {
                    int even_reg = -1;
                    if (absl::SimpleAtoi(var.substr(1, var.size() - 3), &even_reg) &&
                        even_reg >= 0) {
                      std::string even_name = absl::StrFormat("f%d", even_reg);
                      std::string odd_name = absl::StrFormat("f%d", even_reg + 1);
                      if (CountUses(*stmts[k], even_name) > 0 ||
                          HasAddressOfVarInStmt(*stmts[k], even_name) ||
                          CountUses(*stmts[k], odd_name) > 0 ||
                          HasAddressOfVarInStmt(*stmts[k], odd_name)) {
                        break;
                      }
                    }
                  }
                  if (HasCallInStmt(*stmts[k])) {
                    saw_intervening_call = true;
                    bool call_has_stack_args = false;
                    if (stmts[k]->Kind() == CStatementKind::kExpressionStatement) {
                      const auto& k_expr =
                          static_cast<const ExpressionStatement&>(*stmts[k]).Expression();
                      if (k_expr.Kind() == CExpressionKind::kCallExpression) {
                        call_has_stack_args =
                            static_cast<const CallExpression&>(k_expr).Arguments().size() >= 5;
                      } else if (k_expr.Kind() == CExpressionKind::kAssignmentExpression) {
                        const auto& k_rhs = static_cast<const AssignmentExpression&>(k_expr).Rhs();
                        if (k_rhs.Kind() == CExpressionKind::kCallExpression) {
                          call_has_stack_args =
                              static_cast<const CallExpression&>(k_rhs).Arguments().size() >= 5;
                        }
                      }
                    }
                    if (call_has_stack_args && IsDeadAfterStmt(k, var, 0, stmts[i].get())) {
                      overwritten_before_use = true;
                      break;
                    }
                  }
                }
                if (overwritten_before_use) {
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }

              // Fold `argX = <pure_expr>; *(T*)<addr> = argX;` when `argX` is used as a dead
              // scratch register for a memory store right before return.
              if (is_call_arg && !is_float_bitcast_lhs && IsPureExpr(assign.Rhs()) &&
                  i + 2 < stmts.size() &&
                  stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
                  stmts[i + 2]->Kind() == CStatementKind::kReturnStatement) {
                const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& next_assign =
                      static_cast<const AssignmentExpression&>(next_es.Expression());
                  if (next_assign.Op() == "=" &&
                      next_assign.Lhs().Kind() != CExpressionKind::kIdentifier &&
                      CountExprUses(next_assign.Lhs(), var) == 0 &&
                      next_assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
                      static_cast<const IdentifierExpression&>(next_assign.Rhs()).Name() == var &&
                      IsDeadAfterStmt(i + 1, var, 1, stmts[i].get())) {
                    stmts[i + 1] = CStatement::Expression(CExpression::Assignment(
                        "=", next_assign.Lhs().Clone(), assign.Rhs().Clone()));
                    stmts.erase(stmts.begin() + i);
                    changed = true;
                    break;
                  }
                }
              }

              // Atomically fold `v0 = &sym; dest = ((idx << 2) + v0);` (or
              // `v0 = &sym; dest = (idx << 2); dest = (dest + v0);`) directly into the
              // immediately following statement when `dest` is dereferenced at least twice
              // (`*(T*)dest` read + write) and is dead afterwards.
              if (is_scratch && !is_float_bitcast_lhs &&
                  assign.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
                  static_cast<const UnaryExpression&>(assign.Rhs()).Op() == "&") {
                auto extract_word_index = [](const CExpression& expr) -> const CExpression* {
                  if (expr.Kind() == CExpressionKind::kBinaryExpression) {
                    const auto& b = static_cast<const BinaryExpression&>(expr);
                    if (b.Op() == "<<" && b.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
                        static_cast<const IntegerLiteralExpression&>(b.Rhs()).Value() == 2 &&
                        b.Lhs().Kind() == CExpressionKind::kIdentifier) {
                      return &b.Lhs();
                    }
                  }
                  return nullptr;
                };
                std::string dest_var;
                const CExpression* word_idx = nullptr;
                size_t use_stmt_idx = 0;
                // Pattern 1: `v0 = &sym; dest = ((idx << 2) + v0); <use_stmt>`
                if (i + 2 < stmts.size() &&
                    stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
                  const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                  if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& next_assign =
                        static_cast<const AssignmentExpression&>(next_es.Expression());
                    if (next_assign.Op() == "=" &&
                        next_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        next_assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                      const auto& bin = static_cast<const BinaryExpression&>(next_assign.Rhs());
                      const CExpression* candidate_idx = extract_word_index(bin.Lhs());
                      if (bin.Op() == "+" && candidate_idx != nullptr &&
                          bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(bin.Rhs()).Name() == var &&
                          CountExprUses(*candidate_idx, var) == 0 &&
                          IsDeadAfterStmt(i + 1, var, 1, stmts[i].get())) {
                        dest_var = std::string(
                            static_cast<const IdentifierExpression&>(next_assign.Lhs()).Name());
                        word_idx = candidate_idx;
                        use_stmt_idx = i + 2;
                      }
                    }
                  }
                }
                // Pattern 2: `v0 = &sym; dest = (idx << 2); dest = (dest + v0); <use_stmt>`
                if (word_idx == nullptr && i + 3 < stmts.size() &&
                    stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
                    stmts[i + 2]->Kind() == CStatementKind::kExpressionStatement) {
                  const auto& mid_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                  const auto& add_es = static_cast<const ExpressionStatement&>(*stmts[i + 2]);
                  if (mid_es.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
                      add_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& mid_assign =
                        static_cast<const AssignmentExpression&>(mid_es.Expression());
                    const auto& add_assign =
                        static_cast<const AssignmentExpression&>(add_es.Expression());
                    const CExpression* candidate_idx = extract_word_index(mid_assign.Rhs());
                    if (mid_assign.Op() == "=" && add_assign.Op() == "=" &&
                        candidate_idx != nullptr &&
                        mid_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        add_assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
                      std::string_view candidate_dest =
                          static_cast<const IdentifierExpression&>(mid_assign.Lhs()).Name();
                      if (candidate_dest ==
                              static_cast<const IdentifierExpression&>(add_assign.Lhs()).Name() &&
                          candidate_dest != var && CountExprUses(*candidate_idx, var) == 0 &&
                          add_assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                        const auto& bin = static_cast<const BinaryExpression&>(add_assign.Rhs());
                        if (bin.Op() == "+" && bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
                            static_cast<const IdentifierExpression&>(bin.Lhs()).Name() ==
                                candidate_dest &&
                            bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
                            static_cast<const IdentifierExpression&>(bin.Rhs()).Name() == var &&
                            IsDeadAfterStmt(i + 2, var, 1, stmts[i].get())) {
                          dest_var = std::string(candidate_dest);
                          word_idx = candidate_idx;
                          use_stmt_idx = i + 3;
                        }
                      }
                    }
                  }
                }

                if (word_idx != nullptr && (IsScratchVar(dest_var) || IsCallArgVar(dest_var))) {
                  int total_dest_uses = CountUses(*stmts[use_stmt_idx], dest_var);
                  int deref_dest_uses = 0;
                  std::function<void(const CExpression&)> count_deref_uses = [&](const CExpression&
                                                                                     e) {
                    if (e.Kind() == CExpressionKind::kUnaryExpression) {
                      const auto& u = static_cast<const UnaryExpression&>(e);
                      if (u.Op() == "*" && u.Operand().Kind() == CExpressionKind::kCastExpression) {
                        const auto& inner_cast = static_cast<const CastExpression&>(u.Operand());
                        if (inner_cast.Operand().Kind() == CExpressionKind::kIdentifier &&
                            static_cast<const IdentifierExpression&>(inner_cast.Operand()).Name() ==
                                dest_var) {
                          ++deref_dest_uses;
                          return;
                        }
                      }
                      count_deref_uses(u.Operand());
                    } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
                      const auto& b = static_cast<const BinaryExpression&>(e);
                      count_deref_uses(b.Lhs());
                      count_deref_uses(b.Rhs());
                    } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
                      const auto& a = static_cast<const AssignmentExpression&>(e);
                      count_deref_uses(a.Lhs());
                      count_deref_uses(a.Rhs());
                    } else if (e.Kind() == CExpressionKind::kCastExpression) {
                      count_deref_uses(static_cast<const CastExpression&>(e).Operand());
                    }
                  };
                  bool safe_stmt_shape = false;
                  if (stmts[use_stmt_idx]->Kind() == CStatementKind::kExpressionStatement) {
                    count_deref_uses(
                        static_cast<const ExpressionStatement&>(*stmts[use_stmt_idx]).Expression());
                    safe_stmt_shape = (total_dest_uses >= 2 && deref_dest_uses == total_dest_uses);
                  } else if (stmts[use_stmt_idx]->Kind() == CStatementKind::kIfStatement) {
                    const auto& ifs = static_cast<const IfStatement&>(*stmts[use_stmt_idx]);
                    count_deref_uses(ifs.Condition());
                    if (ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
                      for (const auto& cs :
                           static_cast<const CompoundStatement&>(ifs.ThenBranch()).Statements()) {
                        if (cs->Kind() == CStatementKind::kExpressionStatement) {
                          const auto& ces = static_cast<const ExpressionStatement&>(*cs);
                          if (HasCallInStmt(*cs)) break;
                          count_deref_uses(ces.Expression());
                        } else {
                          break;
                        }
                      }
                    }
                    safe_stmt_shape = (total_dest_uses >= 2 && deref_dest_uses == total_dest_uses);
                  }
                  if (safe_stmt_shape && IsDeadAfterStmt(use_stmt_idx, dest_var, total_dest_uses,
                                                         stmts[use_stmt_idx - 1].get())) {
                    auto sum_expr =
                        CExpression::Binary("+", assign.Rhs().Clone(), word_idx->Clone());
                    if (stmts[use_stmt_idx]->Kind() == CStatementKind::kExpressionStatement) {
                      const auto& next_es =
                          static_cast<const ExpressionStatement&>(*stmts[use_stmt_idx]);
                      auto new_expr =
                          SubstituteIdentifier(next_es.Expression(), dest_var, *sum_expr);
                      stmts[use_stmt_idx] = CStatement::Expression(std::move(new_expr));
                    } else if (stmts[use_stmt_idx]->Kind() == CStatementKind::kIfStatement) {
                      const auto& ifs = static_cast<const IfStatement&>(*stmts[use_stmt_idx]);
                      auto new_cond = SubstituteIdentifier(ifs.Condition(), dest_var, *sum_expr);
                      auto new_then = ifs.ThenBranch().Clone();
                      if (new_then->Kind() == CStatementKind::kCompoundStatement) {
                        auto& then_stmts =
                            static_cast<CompoundStatement*>(new_then.get())->MutableStatements();
                        for (auto& ts : then_stmts) {
                          if (ts->Kind() == CStatementKind::kExpressionStatement &&
                              CountUses(*ts, dest_var) > 0) {
                            const auto& tes = static_cast<const ExpressionStatement&>(*ts);
                            ts = CStatement::Expression(
                                SubstituteIdentifier(tes.Expression(), dest_var, *sum_expr));
                          }
                        }
                      }
                      std::unique_ptr<CStatement> new_else =
                          (ifs.ElseBranch() != nullptr) ? ifs.ElseBranch()->Clone() : nullptr;
                      stmts[use_stmt_idx] = CStatement::If(std::move(new_cond), std::move(new_then),
                                                           std::move(new_else));
                    }
                    stmts.erase(stmts.begin() + i, stmts.begin() + use_stmt_idx);
                    changed = true;
                    break;
                  }
                }
              }

              // 1. Fold into ExpressionStatement (or ReturnStatement for call args)
              if (stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement ||
                  ((is_stack_arg || is_call_arg) &&
                   stmts[i + 1]->Kind() == CStatementKind::kReturnStatement)) {
                bool is_fp_scratch = (var.size() >= 2 && var[0] == 'f' &&
                                      std::isdigit(static_cast<unsigned char>(var[1])));
                bool block_has_later_call = false;
                for (size_t k = i + 1; k < stmts.size() && k <= i + 48; ++k) {
                  if (HasCallInStmt(*stmts[k])) {
                    block_has_later_call = true;
                    break;
                  }
                }
                bool block_has_earlier_call = false;
                for (size_t k = 0; k < i; ++k) {
                  if (HasCallInStmt(*stmts[k])) {
                    block_has_earlier_call = true;
                    break;
                  }
                }
                bool block_has_any_call = block_has_later_call || block_has_earlier_call;

                // Case A: In-place redefinition (var = ...; var = ... var ...)
                size_t max_case_a_lookahead =
                    (is_call_arg || block_has_any_call) ? 10 : (is_fp_scratch ? 2 : 1);
                for (size_t j = i + 1; j < stmts.size() && j <= i + max_case_a_lookahead; ++j) {
                  if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
                  const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                  if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) break;
                  const auto& cand_assign =
                      static_cast<const AssignmentExpression&>(cand_es.Expression());
                  if (cand_assign.Op() == "=" &&
                      cand_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      static_cast<const IdentifierExpression&>(cand_assign.Lhs()).Name() == var) {
                    if (j > i + 1 && !is_call_arg && !block_has_any_call &&
                        HasMemoryDeref(assign.Rhs())) {
                      break;
                    }
                    int uses_in_rhs = CountExprUses(cand_assign.Rhs(), var);
                    bool can_fold_in_place = false;
                    if (IsPureExpr(assign.Rhs()) && !IsComparisonExpr(assign.Rhs())) {
                      can_fold_in_place = true;
                    } else if (IsCallOrWrappedCall(assign.Rhs()) && j == i + 1 &&
                               IsPureExpr(cand_assign.Rhs()) &&
                               !HasMemoryDeref(cand_assign.Rhs())) {
                      can_fold_in_place = true;
                    }
                    if (can_fold_in_place && is_fp_scratch && var != "f0" && var != "f0_d" &&
                        assign.Rhs().Kind() == CExpressionKind::kBinaryExpression &&
                        cand_assign.Rhs().Kind() == CExpressionKind::kBinaryExpression && i > 0 &&
                        CountUses(*stmts[i - 1], var) > 0) {
                      can_fold_in_place = false;
                    }
                    if (can_fold_in_place && uses_in_rhs == 1 &&
                        !HasAddressOfVar(cand_assign.Rhs(), var) &&
                        (!HasAddressOf(assign.Rhs()) ||
                         !UsedInAdditionOrSubtraction(cand_assign.Rhs(), var))) {
                      auto new_rhs = SubstituteIdentifier(cand_assign.Rhs(), var, assign.Rhs());
                      if (CountExprUses(*new_rhs, var) == 0) {
                        stmts[j] = CStatement::Expression(CExpression::Assignment(
                            "=", cand_assign.Lhs().Clone(), std::move(new_rhs)));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                        break;
                      }
                    }
                    break;
                  }
                  if (CountUses(*stmts[j], var) > 0) break;
                  if (cand_assign.Op() != "=" || !IsPureExpr(cand_assign.Rhs())) break;
                  std::string_view mid_lhs_sv = ExtractAssignedLhsVar(cand_assign.Lhs());
                  if (!mid_lhs_sv.empty()) {
                    std::string mid_lhs(mid_lhs_sv);
                    if (mid_lhs == var || CountExprUses(assign.Rhs(), mid_lhs) > 0) break;
                  } else if (!is_call_arg ||
                             (HasMemoryDeref(assign.Rhs()) &&
                              !AreAllDerefsProvablyDisjoint(assign.Rhs(), cand_assign.Lhs())) ||
                             !IsPureExpr(cand_assign.Lhs())) {
                    break;
                  }
                }
                if (changed) break;

                if (CountExprUses(effective_rhs, var) > 0) continue;

                if (is_stack_arg || is_call_arg) {
                  if (!IsPureExpr(effective_rhs)) continue;
                  std::vector<size_t> intermediate_use_stmts;
                  // Look ahead across pure stack/register argument setup statements to the call
                  for (size_t j = i + 1; j < stmts.size() && j <= i + 40; ++j) {
                    const CallExpression* target_call = nullptr;
                    if (stmts[j]->Kind() == CStatementKind::kExpressionStatement) {
                      const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                      if (cand_es.Expression().Kind() == CExpressionKind::kCallExpression) {
                        target_call = static_cast<const CallExpression*>(&cand_es.Expression());
                      } else if (cand_es.Expression().Kind() ==
                                 CExpressionKind::kAssignmentExpression) {
                        const auto& ca =
                            static_cast<const AssignmentExpression&>(cand_es.Expression());
                        if (ca.Op() == "=" && ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                            ca.Rhs().Kind() == CExpressionKind::kCallExpression) {
                          target_call = static_cast<const CallExpression*>(&ca.Rhs());
                        }
                      }
                    } else if (stmts[j]->Kind() == CStatementKind::kReturnStatement) {
                      const auto& ret_s = static_cast<const ReturnStatement&>(*stmts[j]);
                      if (ret_s.ReturnValue() != nullptr &&
                          ret_s.ReturnValue()->Kind() == CExpressionKind::kCallExpression) {
                        target_call = static_cast<const CallExpression*>(ret_s.ReturnValue());
                      }
                    } else {
                      break;
                    }
                    if (target_call != nullptr) {
                      int uses_in_call = CountExprUses(*target_call, var);
                      int uses_in_stmt = CountUses(*stmts[j], var);
                      bool can_fold_arg_uses =
                          (uses_in_call == 1 && uses_in_stmt == 1 &&
                           (intermediate_use_stmts.empty() ||
                            (is_call_arg && IsDuplicableArgRhs(effective_rhs)))) ||
                          (is_call_arg && uses_in_call > 1 && uses_in_stmt == uses_in_call &&
                           IsDuplicableArgRhs(effective_rhs));
                      if (can_fold_arg_uses &&
                          (!HasAddressOfVar(*target_call, var) ||
                           is_c_var_identifier(effective_rhs)) &&
                          (!HasAddressOf(effective_rhs) ||
                           !UsedInAdditionOrSubtraction(*target_call, var)) &&
                          IsDeadAfterStmt(j, var, uses_in_call, stmts[i].get())) {
                        bool all_intermediate_updated = true;
                        for (size_t mid_idx : intermediate_use_stmts) {
                          const auto& mid_es =
                              static_cast<const ExpressionStatement&>(*stmts[mid_idx]);
                          auto new_mid_expr =
                              SubstituteIdentifier(mid_es.Expression(), var, effective_rhs);
                          if (CountExprUses(*new_mid_expr, var) != 0) {
                            all_intermediate_updated = false;
                            break;
                          }
                        }
                        if (all_intermediate_updated) {
                          for (size_t mid_idx : intermediate_use_stmts) {
                            const auto& mid_es =
                                static_cast<const ExpressionStatement&>(*stmts[mid_idx]);
                            stmts[mid_idx] = CStatement::Expression(
                                SubstituteIdentifier(mid_es.Expression(), var, effective_rhs));
                          }
                          if (stmts[j]->Kind() == CStatementKind::kReturnStatement) {
                            const auto& ret_s = static_cast<const ReturnStatement&>(*stmts[j]);
                            auto new_ret =
                                SubstituteIdentifier(*ret_s.ReturnValue(), var, effective_rhs);
                            if (CountExprUses(*new_ret, var) == 0) {
                              stmts[j] = CStatement::Return(std::move(new_ret));
                              stmts.erase(stmts.begin() + i);
                              changed = true;
                            }
                          } else {
                            const auto& cand_es =
                                static_cast<const ExpressionStatement&>(*stmts[j]);
                            auto new_expr =
                                SubstituteIdentifier(cand_es.Expression(), var, effective_rhs);
                            if (CountExprUses(*new_expr, var) == 0) {
                              stmts[j] = CStatement::Expression(std::move(new_expr));
                              stmts.erase(stmts.begin() + i);
                              changed = true;
                            }
                          }
                        }
                      }
                      break;
                    }
                    if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
                    const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                    // Check if intermediate statement j is a safe argument setup that doesn't touch
                    // var or modify any variable referenced in effective_rhs
                    if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
                      break;
                    }
                    const auto& mid_assign =
                        static_cast<const AssignmentExpression&>(cand_es.Expression());
                    if (mid_assign.Op() != "=" || !IsPureExpr(mid_assign.Rhs())) {
                      break;
                    }
                    int mid_uses = CountUses(*stmts[j], var);
                    if (mid_uses > 0) {
                      if (!is_call_arg || !IsDuplicableArgRhs(effective_rhs) ||
                          HasAddressOf(effective_rhs) ||
                          HasAddressOfVar(cand_es.Expression(), var)) {
                        break;
                      }
                      intermediate_use_stmts.push_back(j);
                    }
                    std::string_view mid_lhs_sv = ExtractAssignedLhsVar(mid_assign.Lhs());
                    if (!mid_lhs_sv.empty()) {
                      std::string mid_lhs(mid_lhs_sv);
                      if (mid_lhs == var || CountExprUses(effective_rhs, mid_lhs) > 0) {
                        break;
                      }
                    } else if ((HasMemoryDeref(effective_rhs) &&
                                !AreAllDerefsProvablyDisjoint(effective_rhs, mid_assign.Lhs())) ||
                               !IsPureExpr(mid_assign.Lhs())) {
                      break;
                    }
                  }
                  if (changed) break;
                  if (is_stack_arg) continue;
                  if (!block_has_any_call) continue;
                }

                // Case B: Regular single-use folding into next (or across pure setup) expression
                // statement
                bool is_direct_call_store_rhs = (var == "v0" || var == "f0" || var == "f0_d") &&
                                                IsCallOrWrappedCall(assign.Rhs());
                bool f0_before_later_call =
                    (var == "f0" && !is_direct_call_store_rhs && block_has_any_call);
                size_t max_case_b_lookahead = ((var == "f0" && !f0_before_later_call) ||
                                               var == "f0_d" || is_direct_call_store_rhs)
                                                  ? 1
                                                  : (block_has_any_call ? 6 : 2);
                if (IsPureExpr(assign.Rhs()) || is_direct_call_store_rhs) {
                  for (size_t j = i + 1; j < stmts.size() && j <= i + max_case_b_lookahead; ++j) {
                    size_t max_case_b_deref_lookahead = block_has_any_call ? 10 : 1;
                    if (j > i + max_case_b_deref_lookahead && HasMemoryDeref(assign.Rhs())) {
                      break;
                    }
                    if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
                    const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                    if (j > i + 1 && f0_before_later_call) {
                      if (cand_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                        const auto& ca =
                            static_cast<const AssignmentExpression&>(cand_es.Expression());
                        std::string_view ca_lhs = ExtractAssignedLhsVar(ca.Lhs());
                        if (!IsOutgoingStackArgVar(ca_lhs) && CountUses(*stmts[j], var) > 0) {
                          break;
                        }
                      }
                    }
                    if (is_direct_call_store_rhs) {
                      if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
                        break;
                      }
                      const auto& ca =
                          static_cast<const AssignmentExpression&>(cand_es.Expression());
                      bool is_mem_store_rhs =
                          ca.Op() == "=" && ca.Lhs().Kind() != CExpressionKind::kIdentifier &&
                          IsPureExpr(ca.Lhs()) && CountExprUses(ca.Lhs(), var) == 0 &&
                          ca.Rhs().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(ca.Rhs()).Name() == var;
                      bool is_saved_reg_copy_before_call =
                          var == "v0" && ca.Op() == "=" &&
                          ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                          absl::StartsWith(
                              static_cast<const IdentifierExpression&>(ca.Lhs()).Name(),
                              "temp_s") &&
                          ca.Rhs().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(ca.Rhs()).Name() == var &&
                          j + 1 < stmts.size() && HasCallInStmt(*stmts[j + 1]);
                      if (!is_mem_store_rhs && !is_saved_reg_copy_before_call) {
                        break;
                      }
                    }
                    if (j > i + 1 && is_fp_scratch &&
                        assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                      break;
                    }
                    if (j > i + 1 && is_fp_scratch && var != "f0" && var != "f0_d" &&
                        HasMemoryDeref(assign.Rhs())) {
                      if (cand_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                        const auto& ca =
                            static_cast<const AssignmentExpression&>(cand_es.Expression());
                        std::string_view ca_lhs = ExtractAssignedLhsVar(ca.Lhs());
                        if (IsOutgoingStackArgVar(ca_lhs)) {
                          break;
                        }
                      } else if (cand_es.Expression().Kind() == CExpressionKind::kCallExpression &&
                                 absl::EndsWith(var, "_d")) {
                        break;
                      }
                    }
                    int uses_in_cand = CountUses(*stmts[j], var);
                    if (uses_in_cand == 1) {
                      if ((var == "f0" || var == "f0_d") && comp == root_body &&
                          j + 1 == stmts.size() && !HasCallInStmt(*stmts[j])) {
                        break;
                      }
                      if (HasAddressOf(assign.Rhs()) &&
                          UsedInAdditionOrSubtraction(cand_es.Expression(), var)) {
                        break;
                      }
                      if (!is_fp_scratch &&
                          assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                        auto expr_has_u32_cast = [](const CExpression& root_e) -> bool {
                          auto impl = [&](auto& self, const CExpression& e) -> bool {
                            if (e.Kind() == CExpressionKind::kIdentifier) {
                              return absl::StartsWith(
                                  static_cast<const IdentifierExpression&>(e).Name(), "((u32)");
                            }
                            if (e.Kind() == CExpressionKind::kCastExpression) {
                              const auto& ce = static_cast<const CastExpression&>(e);
                              if (ce.TargetType().ToString() == "u32") return true;
                              return self(self, ce.Operand());
                            }
                            if (e.Kind() == CExpressionKind::kUnaryExpression) {
                              const auto& un = static_cast<const UnaryExpression&>(e);
                              return self(self, un.Operand());
                            }
                            if (e.Kind() == CExpressionKind::kBinaryExpression) {
                              const auto& b = static_cast<const BinaryExpression&>(e);
                              return self(self, b.Lhs()) || self(self, b.Rhs());
                            }
                            return false;
                          };
                          return impl(impl, root_e);
                        };
                        auto expr_casts_var_to_float = [&](const CExpression& root_e) -> bool {
                          auto impl = [&](auto& self, const CExpression& e) -> bool {
                            if (e.Kind() == CExpressionKind::kCastExpression) {
                              const auto& ce = static_cast<const CastExpression&>(e);
                              std::string ts = ce.TargetType().ToString();
                              if ((ts == "f32" || ts == "f64") &&
                                  CountExprUses(ce.Operand(), var) > 0) {
                                return true;
                              }
                              return self(self, ce.Operand());
                            }
                            if (e.Kind() == CExpressionKind::kUnaryExpression) {
                              const auto& un = static_cast<const UnaryExpression&>(e);
                              return self(self, un.Operand());
                            }
                            if (e.Kind() == CExpressionKind::kBinaryExpression) {
                              const auto& b = static_cast<const BinaryExpression&>(e);
                              return self(self, b.Lhs()) || self(self, b.Rhs());
                            }
                            if (e.Kind() == CExpressionKind::kAssignmentExpression) {
                              const auto& a = static_cast<const AssignmentExpression&>(e);
                              return self(self, a.Lhs()) || self(self, a.Rhs());
                            }
                            if (e.Kind() == CExpressionKind::kCallExpression) {
                              const auto& c = static_cast<const CallExpression&>(e);
                              for (const auto& arg : c.Arguments()) {
                                if (arg != nullptr && self(self, *arg)) return true;
                              }
                              return false;
                            }
                            return false;
                          };
                          return impl(impl, root_e);
                        };
                        if (expr_has_u32_cast(assign.Rhs()) &&
                            expr_casts_var_to_float(cand_es.Expression())) {
                          break;
                        }
                      }
                      if (HasAddressOfVar(cand_es.Expression(), var) &&
                          !is_c_var_identifier(assign.Rhs())) {
                        bool is_float_arg_bitcast_assign = false;
                        if (is_fp_scratch && !absl::EndsWith(var, "_d") &&
                            assign.Rhs().Kind() == CExpressionKind::kUnaryExpression) {
                          const auto& rhs_un = static_cast<const UnaryExpression&>(assign.Rhs());
                          if (rhs_un.Op() == "*" &&
                              rhs_un.Operand().Kind() == CExpressionKind::kCastExpression) {
                            const auto& rhs_cast =
                                static_cast<const CastExpression&>(rhs_un.Operand());
                            if ((rhs_cast.TargetType().name == "f32" ||
                                 rhs_cast.TargetType().name == "const f32" ||
                                 rhs_cast.TargetType().name == "volatile f32") &&
                                rhs_cast.TargetType().pointer_depth == 1) {
                              is_float_arg_bitcast_assign = true;
                            }
                          }
                        }
                        if (is_fp_scratch &&
                            assign.Rhs().Kind() != CExpressionKind::kFloatLiteral) {
                          if (cand_es.Expression().Kind() ==
                              CExpressionKind::kAssignmentExpression) {
                            const auto& ca =
                                static_cast<const AssignmentExpression&>(cand_es.Expression());
                            if (ca.Op() == "=" && ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                                IsCallArgVar(
                                    static_cast<const IdentifierExpression&>(ca.Lhs()).Name()) &&
                                ca.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
                                (static_cast<const UnaryExpression&>(ca.Rhs()).Op() == "*(s32*)&" ||
                                 static_cast<const UnaryExpression&>(ca.Rhs()).Op() ==
                                     "*(s32 *)&")) {
                              std::string arg_lhs_name(
                                  static_cast<const IdentifierExpression&>(ca.Lhs()).Name());
                              if (CountExprUses(assign.Rhs(), arg_lhs_name) == 0) {
                                for (size_t k = j + 1; k < stmts.size() && k <= j + 16; ++k) {
                                  if (stmts[k]->Kind() != CStatementKind::kExpressionStatement) {
                                    break;
                                  }
                                  const auto& kes =
                                      static_cast<const ExpressionStatement&>(*stmts[k]);
                                  const CallExpression* k_call = nullptr;
                                  if (kes.Expression().Kind() == CExpressionKind::kCallExpression) {
                                    k_call = static_cast<const CallExpression*>(&kes.Expression());
                                  } else if (kes.Expression().Kind() ==
                                             CExpressionKind::kAssignmentExpression) {
                                    const auto& ka =
                                        static_cast<const AssignmentExpression&>(kes.Expression());
                                    if (ka.Op() == "=" &&
                                        ka.Lhs().Kind() == CExpressionKind::kIdentifier &&
                                        ka.Rhs().Kind() == CExpressionKind::kCallExpression) {
                                      k_call = static_cast<const CallExpression*>(&ka.Rhs());
                                    }
                                  }
                                  if (k_call != nullptr) {
                                    bool arg0_is_fp =
                                        !k_call->Arguments().empty() &&
                                        k_call->Arguments()[0] != nullptr &&
                                        (k_call->Arguments()[0]->Kind() ==
                                             CExpressionKind::kFloatLiteral ||
                                         (k_call->Arguments()[0]->Kind() ==
                                              CExpressionKind::kIdentifier &&
                                          IsScratchVar(static_cast<const IdentifierExpression&>(
                                                           *k_call->Arguments()[0])
                                                           .Name()) &&
                                          static_cast<const IdentifierExpression&>(
                                              *k_call->Arguments()[0])
                                                  .Name()[0] == 'f') ||
                                         (k_call->Arguments()[0]->Kind() ==
                                              CExpressionKind::kIdentifier &&
                                          absl::StartsWith(static_cast<const IdentifierExpression&>(
                                                               *k_call->Arguments()[0])
                                                               .Name(),
                                                           "f12")));
                                    bool valid_slot =
                                        !(arg0_is_fp && arg_lhs_name == "arg1") &&
                                        (!k_call->Arguments().empty() &&
                                         CountExprUses(*k_call->Arguments()[0], arg_lhs_name) == 0);
                                    if (valid_slot && CountExprUses(*k_call, arg_lhs_name) == 1 &&
                                        CountUses(*stmts[k], arg_lhs_name) == 1 &&
                                        IsDeadAfterStmt(k, arg_lhs_name, 1, stmts[j].get())) {
                                      is_float_arg_bitcast_assign = true;
                                    }
                                    break;
                                  }
                                  if (HasCallInStmt(*stmts[k]) ||
                                      CountUses(*stmts[k], arg_lhs_name) > 0 ||
                                      kes.Expression().Kind() !=
                                          CExpressionKind::kAssignmentExpression) {
                                    break;
                                  }
                                  const auto& mid_a =
                                      static_cast<const AssignmentExpression&>(kes.Expression());
                                  std::string_view mid_lhs_sv =
                                      (mid_a.Op() == "=") ? ExtractAssignedLhsVar(mid_a.Lhs()) : "";
                                  if (mid_lhs_sv.empty() || !IsPureExpr(mid_a.Rhs())) {
                                    break;
                                  }
                                  std::string mid_lhs(mid_lhs_sv);
                                  if (mid_lhs == arg_lhs_name ||
                                      CountExprUses(assign.Rhs(), mid_lhs) > 0) {
                                    break;
                                  }
                                }
                              }
                            } else if (ca.Op() == "=" &&
                                       ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                                       ca.Rhs().Kind() == CExpressionKind::kCallExpression) {
                              const auto& ce = static_cast<const CallExpression&>(ca.Rhs());
                              for (size_t ai = 1; ai < ce.Arguments().size(); ++ai) {
                                const auto& arg = ce.Arguments()[ai];
                                if (arg != nullptr &&
                                    arg->Kind() == CExpressionKind::kUnaryExpression) {
                                  const auto& ue = static_cast<const UnaryExpression&>(*arg);
                                  if ((ue.Op() == "*(s32*)&" || ue.Op() == "*(s32 *)&") &&
                                      ue.Operand().Kind() == CExpressionKind::kIdentifier &&
                                      static_cast<const IdentifierExpression&>(ue.Operand())
                                              .Name() == var) {
                                    is_float_arg_bitcast_assign = true;
                                    break;
                                  }
                                }
                              }
                            }
                          } else if (cand_es.Expression().Kind() ==
                                     CExpressionKind::kCallExpression) {
                            const auto& ce =
                                static_cast<const CallExpression&>(cand_es.Expression());
                            for (size_t ai = 1; ai < ce.Arguments().size(); ++ai) {
                              const auto& arg = ce.Arguments()[ai];
                              if (arg != nullptr &&
                                  arg->Kind() == CExpressionKind::kUnaryExpression) {
                                const auto& ue = static_cast<const UnaryExpression&>(*arg);
                                if ((ue.Op() == "*(s32*)&" || ue.Op() == "*(s32 *)&") &&
                                    ue.Operand().Kind() == CExpressionKind::kIdentifier &&
                                    static_cast<const IdentifierExpression&>(ue.Operand()).Name() ==
                                        var) {
                                  is_float_arg_bitcast_assign = true;
                                  break;
                                }
                              }
                            }
                          }
                        }
                        if (!is_float_arg_bitcast_assign) {
                          break;
                        }
                      }
                      if (cand_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                        const auto& cand_assign =
                            static_cast<const AssignmentExpression&>(cand_es.Expression());
                        auto is_stack_param_or_local_expr = [&](const CExpression& e) -> bool {
                          if (e.Kind() == CExpressionKind::kIdentifier) {
                            return IsStackParamOrLocalName(
                                static_cast<const IdentifierExpression&>(e).Name());
                          }
                          if (e.Kind() == CExpressionKind::kUnaryExpression) {
                            const auto& un = static_cast<const UnaryExpression&>(e);
                            if ((un.Op() == "*(f32*)&" || un.Op() == "*(f32 *)&") &&
                                un.Operand().Kind() == CExpressionKind::kIdentifier) {
                              return IsStackParamOrLocalName(
                                  static_cast<const IdentifierExpression&>(un.Operand()).Name());
                            }
                          }
                          return false;
                        };
                        if (is_fp_scratch && var != "f0" && var != "f0_d" &&
                            assign.Rhs().Kind() == CExpressionKind::kCastExpression &&
                            static_cast<const CastExpression&>(assign.Rhs()).TargetType().name ==
                                "f64" &&
                            cand_assign.Rhs().Kind() == CExpressionKind::kCastExpression &&
                            static_cast<const CastExpression&>(cand_assign.Rhs())
                                    .TargetType()
                                    .name == "f32") {
                          const auto& f64_op =
                              static_cast<const CastExpression&>(assign.Rhs()).Operand();
                          bool is_u32_f64 =
                              f64_op.Kind() == CExpressionKind::kCastExpression &&
                              static_cast<const CastExpression&>(f64_op).TargetType().name ==
                                  "u32" &&
                              static_cast<const CastExpression&>(f64_op)
                                      .TargetType()
                                      .pointer_depth == 0;
                          if (!is_u32_f64) {
                            break;
                          }
                        }
                        if (is_fp_scratch &&
                            cand_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                            is_stack_param_or_local_expr(assign.Rhs())) {
                          std::string_view ca_lhs =
                              static_cast<const IdentifierExpression&>(cand_assign.Lhs()).Name();
                          if (ca_lhs != var && CountExprUses(cand_assign.Rhs(), ca_lhs) == 0) {
                            break;
                          }
                        }
                        if (cand_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                            static_cast<const IdentifierExpression&>(cand_assign.Lhs()).Name() ==
                                var) {
                          break;
                        }
                        if (cand_assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
                          if (CountExprUses(cand_assign.Lhs(), var) > 0 &&
                              !IsPureExpr(cand_assign.Rhs())) {
                            break;
                          }
                        }
                      }
                      if (IsDeadAfterStmt(j, var, 1, stmts[i].get())) {
                        auto new_expr =
                            SubstituteIdentifier(cand_es.Expression(), var, assign.Rhs());
                        if (CountExprUses(*new_expr, var) == 0) {
                          stmts[j] = CStatement::Expression(std::move(new_expr));
                          stmts.erase(stmts.begin() + i);
                          changed = true;
                        }
                      }
                      break;
                    }
                    if (uses_in_cand > 1) {
                      if (j == i + 1 && is_scratch && !is_fp_scratch &&
                          assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
                          IsCallArgVar(
                              static_cast<const IdentifierExpression&>(assign.Rhs()).Name()) &&
                          HasCallInStmt(*stmts[j]) && !HasAddressOfVar(cand_es.Expression(), var) &&
                          IsDeadAfterStmt(j, var, uses_in_cand, stmts[i].get())) {
                        auto new_expr =
                            SubstituteIdentifier(cand_es.Expression(), var, assign.Rhs());
                        if (CountExprUses(*new_expr, var) == 0) {
                          stmts[j] = CStatement::Expression(std::move(new_expr));
                          stmts.erase(stmts.begin() + i);
                          changed = true;
                        }
                      }
                      break;
                    }
                    if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
                      break;
                    }
                    const auto& mid_assign =
                        static_cast<const AssignmentExpression&>(cand_es.Expression());
                    if (mid_assign.Op() != "=" || !IsPureExpr(mid_assign.Rhs())) {
                      break;
                    }
                    std::string_view mid_lhs_sv = ExtractAssignedLhsVar(mid_assign.Lhs());
                    if (!mid_lhs_sv.empty()) {
                      std::string mid_lhs(mid_lhs_sv);
                      if (mid_lhs == var || CountExprUses(assign.Rhs(), mid_lhs) > 0) {
                        break;
                      }
                    } else if ((HasMemoryDeref(assign.Rhs()) &&
                                !AreAllDerefsProvablyDisjoint(assign.Rhs(), mid_assign.Lhs())) ||
                               !IsPureExpr(mid_assign.Lhs())) {
                      break;
                    }
                  }
                  if (changed) break;
                }
              }

              // 2, 3, 4. Fold into IfStatement, ReturnStatement, or SwitchStatement
              // (possibly across 1-3 pure register assignments like branch delay slots!)
              for (size_t j = i + 1; j < stmts.size() && j <= i + 4; ++j) {
                if (j > i + 1 && !IsPureExpr(assign.Rhs())) break;
                if (j > i + 1 && HasMemoryDeref(assign.Rhs())) break;

                if (stmts[j]->Kind() == CStatementKind::kIfStatement) {
                  if (var == "f0" || var == "f0_d") {
                    bool is_f0_float_literal =
                        (var == "f0" && assign.Rhs().Kind() == CExpressionKind::kFloatLiteral);
                    if (!IsCallOrWrappedCall(assign.Rhs()) && !is_f0_float_literal) {
                      break;
                    }
                  }
                  const auto& if_stmt = static_cast<const IfStatement&>(*stmts[j]);
                  // If `argX = <pure_rhs>;` (from a branch delay slot) precedes an `if`
                  // whose `then` or `else` branch calls a function using `argX` once (and the
                  // other branch does not touch `argX`), and `argX` is dead after the `if`,
                  // fold `argX` into that branch's call statement.
                  if (j == i + 1 && is_call_arg && CountExprUses(if_stmt.Condition(), var) == 0 &&
                      IsPureExpr(assign.Rhs()) && !HasMemoryDeref(assign.Rhs()) &&
                      !HasAddressOf(assign.Rhs()) && IsDeadAfterStmt(j, var, 1, stmts[i].get())) {
                    auto try_fold_into_branch_comp = [&](const CompoundStatement& branch_comp)
                        -> std::unique_ptr<CompoundStatement> {
                      for (size_t k = 0; k < branch_comp.Statements().size() && k < 4; ++k) {
                        const CStatement& bs = *branch_comp.Statements()[k];
                        int k_uses = CountUses(bs, var);
                        if (k_uses == 1) {
                          if (bs.Kind() != CStatementKind::kExpressionStatement ||
                              !HasCallInStmt(bs)) {
                            return nullptr;
                          }
                          if (k > 0 && (assign.Rhs().Kind() != CExpressionKind::kIdentifier &&
                                        assign.Rhs().Kind() != CExpressionKind::kIntegerLiteral)) {
                            // Only fold non-trivial expressions into a multi-statement branch
                            // if the branch has no other call after k.
                            for (size_t after_k = k + 1; after_k < branch_comp.Statements().size();
                                 ++after_k) {
                              if (HasCallInStmt(*branch_comp.Statements()[after_k])) {
                                return nullptr;
                              }
                            }
                          }
                          const auto& call_es = static_cast<const ExpressionStatement&>(bs);
                          auto new_call_expr =
                              SubstituteIdentifier(call_es.Expression(), var, assign.Rhs());
                          if (CountExprUses(*new_call_expr, var) != 0) return nullptr;
                          auto new_comp = std::make_unique<CompoundStatement>();
                          for (size_t m = 0; m < branch_comp.Statements().size(); ++m) {
                            if (m == k) {
                              new_comp->AddStatement(
                                  CStatement::Expression(std::move(new_call_expr)));
                            } else {
                              new_comp->AddStatement(branch_comp.Statements()[m]->Clone());
                            }
                          }
                          return new_comp;
                        }
                        if (k_uses > 0 || HasCallInStmt(bs) ||
                            bs.Kind() != CStatementKind::kExpressionStatement) {
                          return nullptr;
                        }
                        const auto& mid_es = static_cast<const ExpressionStatement&>(bs);
                        if (mid_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
                          return nullptr;
                        }
                        const auto& mid_a =
                            static_cast<const AssignmentExpression&>(mid_es.Expression());
                        if (mid_a.Op() != "=" || !IsPureExpr(mid_a.Rhs()) ||
                            !IsPureExpr(mid_a.Lhs())) {
                          return nullptr;
                        }
                        std::string_view mid_lhs_sv = ExtractAssignedLhsVar(mid_a.Lhs());
                        if (!mid_lhs_sv.empty()) {
                          std::string mid_lhs(mid_lhs_sv);
                          if (mid_lhs == var || CountExprUses(assign.Rhs(), mid_lhs) > 0) {
                            return nullptr;
                          }
                        }
                      }
                      return nullptr;
                    };

                    int then_uses = CountUses(if_stmt.ThenBranch(), var);
                    int else_uses = (if_stmt.ElseBranch() != nullptr)
                                        ? CountUses(*if_stmt.ElseBranch(), var)
                                        : 0;
                    if (then_uses == 1 && else_uses == 0 &&
                        if_stmt.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
                        HasCallInStmt(if_stmt.ThenBranch()) &&
                        !HasAddressOfVarInStmt(if_stmt.ThenBranch(), var)) {
                      const auto& then_comp =
                          static_cast<const CompoundStatement&>(if_stmt.ThenBranch());
                      if (auto new_then = try_fold_into_branch_comp(then_comp)) {
                        std::unique_ptr<CStatement> new_else = (if_stmt.ElseBranch() != nullptr)
                                                                   ? if_stmt.ElseBranch()->Clone()
                                                                   : nullptr;
                        stmts[j] = CStatement::If(if_stmt.Condition().Clone(), std::move(new_then),
                                                  std::move(new_else));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                        break;
                      }
                    } else if (then_uses == 0 && else_uses == 1 &&
                               if_stmt.ElseBranch() != nullptr &&
                               if_stmt.ElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
                               HasCallInStmt(*if_stmt.ElseBranch()) &&
                               !HasAddressOfVarInStmt(*if_stmt.ElseBranch(), var)) {
                      const auto& else_comp =
                          static_cast<const CompoundStatement&>(*if_stmt.ElseBranch());
                      if (auto new_else = try_fold_into_branch_comp(else_comp)) {
                        stmts[j] =
                            CStatement::If(if_stmt.Condition().Clone(),
                                           if_stmt.ThenBranch().Clone(), std::move(new_else));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                        break;
                      }
                    }
                  }
                  if (HasAddressOfVar(if_stmt.Condition(), var) &&
                      assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
                    break;
                  }
                  if (!IsPureExpr(assign.Rhs()) &&
                      !IsUnconditionallyEvaluatedFirst(if_stmt.Condition(), var)) {
                    break;
                  }
                  int uses_in_cond = CountExprUses(if_stmt.Condition(), var);
                  active_ignored_def_stmt = stmts[i].get();
                  active_ignored_use_stmt = stmts[j].get();
                  VarLiveness then_live =
                      AnalyzeVarLiveness(if_stmt.ThenBranch(), var, is_switch_case_body, 0);
                  VarLiveness else_live =
                      (if_stmt.ElseBranch() != nullptr)
                          ? AnalyzeVarLiveness(*if_stmt.ElseBranch(), var, is_switch_case_body, 0)
                          : VarLiveness::kTransparent;
                  active_ignored_def_stmt = nullptr;
                  active_ignored_use_stmt = nullptr;
                  if (uses_in_cond == 1 && then_live != VarLiveness::kReadBeforeWrite &&
                      else_live != VarLiveness::kReadBeforeWrite) {
                    bool killed_by_both = (then_live == VarLiveness::kKilledBeforeRead &&
                                           else_live == VarLiveness::kKilledBeforeRead);
                    if (killed_by_both || IsDeadAfterStmt(j, var, 1, stmts[i].get())) {
                      auto new_cond = SubstituteIdentifier(if_stmt.Condition(), var, assign.Rhs());
                      if (CountExprUses(*new_cond, var) == 0) {
                        auto new_then = if_stmt.ThenBranch().Clone();
                        std::unique_ptr<CStatement> new_else = (if_stmt.ElseBranch() != nullptr)
                                                                   ? if_stmt.ElseBranch()->Clone()
                                                                   : nullptr;
                        stmts[j] = CStatement::If(std::move(new_cond), std::move(new_then),
                                                  std::move(new_else));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                      }
                    }
                  }
                  break;
                }

                if (stmts[j]->Kind() == CStatementKind::kReturnStatement) {
                  const auto& ret_stmt = static_cast<const ReturnStatement&>(*stmts[j]);
                  if (ret_stmt.ReturnValue() != nullptr) {
                    int uses_in_ret = CountExprUses(*ret_stmt.ReturnValue(), var);
                    bool is_direct_id =
                        (ret_stmt.ReturnValue()->Kind() == CExpressionKind::kIdentifier &&
                         static_cast<const IdentifierExpression&>(*ret_stmt.ReturnValue()).Name() ==
                             var);
                    bool can_fold_ret =
                        is_direct_id ||
                        (uses_in_ret == 1 && !IsComparisonExpr(assign.Rhs()) &&
                         (IsPureExpr(assign.Rhs()) || (IsCallOrWrappedCall(assign.Rhs()) &&
                                                       IsPureExpr(*ret_stmt.ReturnValue()) &&
                                                       !HasMemoryDeref(*ret_stmt.ReturnValue()))) &&
                         !HasAddressOfVar(*ret_stmt.ReturnValue(), var) &&
                         (!HasAddressOf(assign.Rhs()) ||
                          !UsedInAdditionOrSubtraction(*ret_stmt.ReturnValue(), var)));
                    if (can_fold_ret && IsDeadAfterStmt(j, var, 1, stmts[i].get())) {
                      auto new_ret =
                          SubstituteIdentifier(*ret_stmt.ReturnValue(), var, assign.Rhs());
                      if (CountExprUses(*new_ret, var) == 0) {
                        stmts[j] = CStatement::Return(std::move(new_ret));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                      }
                    }
                  }
                  break;
                }

                if (stmts[j]->Kind() == CStatementKind::kSwitchStatement) {
                  const auto& sw_stmt = static_cast<const SwitchStatement&>(*stmts[j]);
                  if (HasAddressOfVar(sw_stmt.Condition(), var) &&
                      assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
                    break;
                  }
                  if (!IsPureExpr(assign.Rhs()) &&
                      !IsUnconditionallyEvaluatedFirst(sw_stmt.Condition(), var)) {
                    break;
                  }
                  int uses_in_cond = CountExprUses(sw_stmt.Condition(), var);
                  bool read_in_cases = false;
                  active_ignored_def_stmt = stmts[i].get();
                  active_ignored_use_stmt = stmts[j].get();
                  for (const auto& sc : sw_stmt.Cases()) {
                    if (sc.body != nullptr &&
                        AnalyzeVarLiveness(*sc.body, var, /*in_switch_case_top=*/true, 0) ==
                            VarLiveness::kReadBeforeWrite) {
                      read_in_cases = true;
                      break;
                    }
                  }
                  active_ignored_def_stmt = nullptr;
                  active_ignored_use_stmt = nullptr;
                  if (uses_in_cond == 1 && !read_in_cases) {
                    if (IsDeadAfterStmt(j, var, 1, stmts[i].get())) {
                      auto new_cond = SubstituteIdentifier(sw_stmt.Condition(), var, assign.Rhs());
                      if (CountExprUses(*new_cond, var) == 0) {
                        std::vector<SwitchCase> new_cases;
                        for (const auto& sc : sw_stmt.Cases()) {
                          new_cases.push_back(sc.Clone());
                        }
                        stmts[j] = CStatement::Switch(std::move(new_cond), std::move(new_cases));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                      }
                    }
                  }
                  break;
                }

                // Intermediate statement j must be a pure assignment to a different variable
                if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
                const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                if (CountUses(*stmts[j], var) > 0) break;
                if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) break;
                const auto& mid_assign =
                    static_cast<const AssignmentExpression&>(cand_es.Expression());
                std::string_view mid_lhs_sv =
                    (mid_assign.Op() == "=") ? ExtractAssignedLhsVar(mid_assign.Lhs()) : "";
                if (mid_lhs_sv.empty() || !IsPureExpr(mid_assign.Rhs())) {
                  break;
                }
                std::string mid_lhs(mid_lhs_sv);
                if (mid_lhs == var || CountExprUses(assign.Rhs(), mid_lhs) > 0) {
                  break;
                }
              }
              if (changed) break;
            }
          }
        }
      }
    }
  }

  void FoldChildBlocks() {
    bool changed = false;
    if (root_has_label_cached == -1) {
      root_has_label_cached = (root_body != nullptr && HasLabelInStmt(*root_body)) ? 1 : 0;
    }
    static constexpr std::string_view kTrackedVars[] = {
        "v0",        "v1",        "temp_t0",   "temp_t1",   "temp_t2",   "temp_t3",   "temp_t4",
        "temp_t5",   "temp_t6",   "temp_t7",   "temp_t8",   "temp_t9",   "temp_at",   "fcond",
        "f0",        "f0_d",      "f2",        "f2_d",      "f4",        "f4_d",      "f6",
        "f6_d",      "f8",        "f8_d",      "f10",       "f10_d",     "f12",       "f12_d",
        "f14",       "f14_d",     "f16",       "f16_d",     "f18",       "f18_d",     "arg0",
        "arg1",      "arg2",      "arg3",      "var_sp_16", "var_sp_20", "var_sp_24", "var_sp_28",
        "var_sp_32", "var_sp_36", "var_sp_40", "var_sp_44", "var_sp_48", "var_sp_52", "var_sp_56",
        "var_sp_60",
    };
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i]->Kind() == CStatementKind::kIfStatement ||
          stmts[i]->Kind() == CStatementKind::kSwitchStatement) {
        absl::flat_hash_set<std::string> child_dead_vars;
        for (std::string_view tv : kTrackedVars) {
          std::string tv_str(tv);
          if (IsDeadAfterStmt(i, tv_str, 0)) {
            child_dead_vars.insert(std::move(tv_str));
          }
        }
        if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
          auto& ifs = static_cast<IfStatement&>(*stmts[i]);
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            FoldSingleUseExpressions(static_cast<CompoundStatement*>(ifs.MutableThenBranch()),
                                     root_body, &child_dead_vars, false, root_has_label_cached);
          }
          if (ifs.MutableElseBranch() != nullptr) {
            if (ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
              FoldSingleUseExpressions(static_cast<CompoundStatement*>(ifs.MutableElseBranch()),
                                       root_body, &child_dead_vars, false, root_has_label_cached);
            } else if (ifs.MutableElseBranch()->Kind() == CStatementKind::kIfStatement) {
              CompoundStatement wrapper;
              wrapper.AddStatement(ifs.MutableElseBranch()->Clone());
              FoldSingleUseExpressions(&wrapper, root_body, &child_dead_vars, false,
                                       root_has_label_cached);
              if (wrapper.Statements().size() == 1) {
                stmts[i] = CStatement::If(ifs.Condition().Clone(), ifs.ThenBranch().Clone(),
                                          std::move(wrapper.MutableStatements()[0]));
              }
            }
          }
          // Hoist identical single-statement `else` branch duplicated by branch-likely (`beql`)
          // target-stealing when `then` contains a function call (`if (c) { ...; S; } else { S; }`
          // -> `if (c) { ... } S;`).
          auto& cur_ifs = static_cast<IfStatement&>(*stmts[i]);
          if (cur_ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
              cur_ifs.MutableElseBranch() != nullptr &&
              cur_ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
              HasCallInStmt(*cur_ifs.MutableThenBranch())) {
            auto& then_comp = static_cast<CompoundStatement&>(*cur_ifs.MutableThenBranch());
            auto& else_comp = static_cast<CompoundStatement&>(*cur_ifs.MutableElseBranch());
            if (then_comp.Statements().size() >= 2 && else_comp.Statements().size() == 1 &&
                else_comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement &&
                !HasCallInStmt(*else_comp.Statements()[0]) &&
                then_comp.Statements().back()->ToString() ==
                    else_comp.Statements()[0]->ToString()) {
              auto hoisted_stmt = std::move(else_comp.MutableStatements()[0]);
              then_comp.MutableStatements().pop_back();
              stmts[i] = CStatement::If(cur_ifs.Condition().Clone(),
                                        cur_ifs.MutableThenBranch()->Clone(), nullptr);
              stmts.insert(stmts.begin() + i + 1, std::move(hoisted_stmt));
              changed = true;
              break;
            }
          }
        } else {
          auto& sw = static_cast<SwitchStatement&>(*stmts[i]);
          for (auto& sc : sw.MutableCases()) {
            if (sc.body != nullptr) {
              FoldSingleUseExpressions(sc.body.get(), root_body, &child_dead_vars, true,
                                       root_has_label_cached);
            }
          }
        }
      } else if (stmts[i]->Kind() == CStatementKind::kWhileStatement) {
        auto& ws = static_cast<WhileStatement&>(*stmts[i]);
        if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          auto* body_comp = static_cast<CompoundStatement*>(ws.MutableBody());
          absl::flat_hash_set<std::string> loop_dead_vars;
          if (HasCallInStmt(*body_comp)) {
            for (std::string_view tv : kTrackedVars) {
              std::string tv_str(tv);
              if (CountExprUses(ws.Condition(), tv_str) == 0 &&
                  AnalyzeVarLiveness(*body_comp, tv_str, false, 0) ==
                      VarLiveness::kKilledBeforeRead &&
                  IsDeadAfterStmt(i, tv_str, 0)) {
                loop_dead_vars.insert(std::move(tv_str));
              }
            }
          }
          FoldSingleUseExpressions(body_comp, root_body,
                                   loop_dead_vars.empty() ? nullptr : &loop_dead_vars, false,
                                   root_has_label_cached);
        }
      } else if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement) {
        auto& dws = static_cast<DoWhileStatement&>(*stmts[i]);
        if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          auto* body_comp = static_cast<CompoundStatement*>(dws.MutableBody());
          absl::flat_hash_set<std::string> loop_dead_vars;
          if (HasCallInStmt(*body_comp)) {
            for (std::string_view tv : kTrackedVars) {
              std::string tv_str(tv);
              if (CountExprUses(dws.Condition(), tv_str) == 0 &&
                  AnalyzeVarLiveness(*body_comp, tv_str, false, 0) ==
                      VarLiveness::kKilledBeforeRead &&
                  IsDeadAfterStmt(i, tv_str, 0)) {
                loop_dead_vars.insert(std::move(tv_str));
              }
            }
          }
          FoldSingleUseExpressions(body_comp, root_body,
                                   loop_dead_vars.empty() ? nullptr : &loop_dead_vars, false,
                                   root_has_label_cached);
          // Remove redundant trailing comparison assignment (`v0 = (i < n);` or `v1 = (i < n);`)
          // at the end of a `do { ... } while (i < n);` body (or immediately preceding a pure
          // branch-delay-slot statement at the end of the body) when the variable is dead on both
          // the loop back-edge and loop exit.
          if (!body_comp->Statements().empty()) {
            size_t body_sz = body_comp->Statements().size();
            for (size_t offset : {size_t{1}, size_t{2}}) {
              if (body_sz < offset) continue;
              size_t cand_idx = body_sz - offset;
              if (body_comp->Statements()[cand_idx]->Kind() !=
                  CStatementKind::kExpressionStatement) {
                continue;
              }
              const auto& cand_es =
                  static_cast<const ExpressionStatement&>(*body_comp->Statements()[cand_idx]);
              if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) continue;
              const auto& cand_assign =
                  static_cast<const AssignmentExpression&>(cand_es.Expression());
              if (cand_assign.Op() != "=" ||
                  cand_assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
                  !IsPureExpr(cand_assign.Rhs())) {
                continue;
              }
              if (offset == 2 && !IsComparisonExpr(cand_assign.Rhs())) {
                continue;
              }
              const std::string& last_var =
                  static_cast<const IdentifierExpression&>(cand_assign.Lhs()).Name();
              if (offset == 2 &&
                  (CountUses(*body_comp->Statements().back(), last_var) > 0 ||
                   HasAddressOfVarInStmt(*body_comp->Statements().back(), last_var) ||
                   HasCallInStmt(*body_comp->Statements().back()) ||
                   HasControlJumpInStmt(*body_comp->Statements().back()))) {
                continue;
              }
              if ((last_var == "v0" || last_var == "v1") &&
                  CountExprUses(dws.Condition(), last_var) == 0 &&
                  IsDeadAfterStmt(i, last_var, 0)) {
                active_ignored_def_stmt = body_comp->Statements()[cand_idx].get();
                VarLiveness prefix_live = AnalyzeVarLiveness(*body_comp, last_var, false, 0);
                active_ignored_def_stmt = nullptr;
                if (prefix_live != VarLiveness::kReadBeforeWrite &&
                    (root_has_label_cached == 0 || !HasLabelInStmt(*body_comp))) {
                  body_comp->MutableStatements().erase(body_comp->MutableStatements().begin() +
                                                       cand_idx);
                  if (offset == 2) {
                    FoldSingleUseExpressions(body_comp, root_body,
                                             loop_dead_vars.empty() ? nullptr : &loop_dead_vars,
                                             false, root_has_label_cached);
                  }
                  break;
                }
              }
            }
          }
        }
      } else if (stmts[i]->Kind() == CStatementKind::kForStatement) {
        auto& fs = static_cast<ForStatement&>(*stmts[i]);
        if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          FoldSingleUseExpressions(static_cast<CompoundStatement*>(fs.MutableBody()), root_body,
                                   nullptr, false, root_has_label_cached);
        }
      } else if (stmts[i]->Kind() == CStatementKind::kCompoundStatement) {
        FoldSingleUseExpressions(static_cast<CompoundStatement*>(stmts[i].get()), root_body,
                                 dead_at_end_vars, is_switch_case_body, root_has_label_cached);
      }
    }
    (void)changed;
  }

  void Run() {
    if (root_has_label_cached == -1) {
      root_has_label_cached = (root_body != nullptr && HasLabelInStmt(*root_body)) ? 1 : 0;
    }
    root_has_label = (root_has_label_cached != 0);
    FoldStatementsInBlock();
    FoldChildBlocks();
  }
};

}  // namespace

void FoldSingleUseExpressions(CompoundStatement* comp, const CompoundStatement* root_body,
                              const absl::flat_hash_set<std::string>* dead_at_end_vars,
                              bool is_switch_case_body, int root_has_label_cached) {
  if (comp == nullptr) return;
  if (root_body == nullptr) root_body = comp;
  FolderContext ctx{comp, root_body, dead_at_end_vars, is_switch_case_body, root_has_label_cached};
  ctx.Run();
}

}  // namespace rom_nom_nom
