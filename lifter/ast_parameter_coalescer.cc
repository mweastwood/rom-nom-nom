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
#include "core/c_ast_visitor.h"

namespace rom_nom_nom {
namespace {

bool IsCalleeSavedGprVarName(std::string_view v) {
  return v == "fp" ||
         (v.size() == 7 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7');
}

bool StatementHasCall(const CStatement& s) {
  return StatementContainsCall(s, /*ignore_math_intrinsics=*/true);
}

bool IsFloat32Type(const CType& type) {
  return type.pointer_depth == 0 && type.name == "f32";
}

bool IsFloat64Type(const CType& type) {
  return type.pointer_depth == 0 && type.name == "f64";
}

class AddressOfInPointerArithmeticVisitor : public AstVisitor {
 public:
  explicit AddressOfInPointerArithmeticVisitor(std::string_view target_var)
      : target_var_(target_var) {}

  bool Found() const { return found_; }

  void VisitBinaryExpression(const BinaryExpression& expr) override {
    if ((expr.Op() == "+" || expr.Op() == "-") &&
        (ExpressionTakesAddressOf(expr.Lhs(), target_var_) ||
         ExpressionTakesAddressOf(expr.Rhs(), target_var_))) {
      found_ = true;
      return;
    }
    AstVisitor::VisitBinaryExpression(expr);
  }

 private:
  std::string_view target_var_;
  bool found_ = false;
};

bool StatementUsesAddressOfInPointerArithmetic(const CStatement& stmt,
                                               std::string_view target_var) {
  AddressOfInPointerArithmeticVisitor visitor(target_var);
  visitor.VisitStatement(stmt);
  return visitor.Found();
}

class FunctionPointerCallVisitor : public AstVisitor {
 public:
  explicit FunctionPointerCallVisitor(std::string_view target_var) : target_var_(target_var) {}

  bool Found() const { return found_; }

  void VisitCallExpression(const CallExpression& call) override {
    if (ExpressionUsesVariable(call.Callee(), target_var_)) {
      found_ = true;
      return;
    }
    if (call.Callee().Kind() == CExpressionKind::kIdentifier) {
      std::string_view id_name = static_cast<const IdentifierExpression&>(call.Callee()).Name();
      std::string_view underlying;
      if (ExtractFunctionPointerCastCalleeIdentifier(id_name, nullptr, &underlying) &&
          underlying == target_var_) {
        found_ = true;
        return;
      }
    }
    AstVisitor::VisitCallExpression(call);
  }

 private:
  std::string_view target_var_;
  bool found_ = false;
};

bool StatementCallsThroughFunctionPointerVar(const CStatement& stmt, std::string_view target_var) {
  FunctionPointerCallVisitor visitor(target_var);
  visitor.VisitStatement(stmt);
  return visitor.Found();
}

class GotoFinderVisitor : public AstVisitor {
 public:
  bool Found() const { return found_; }
  void VisitGotoStatement(const GotoStatement& /*stmt*/) override { found_ = true; }

 private:
  bool found_ = false;
};

bool StatementContainsGoto(const CStatement& stmt) {
  GotoFinderVisitor visitor;
  visitor.VisitStatement(stmt);
  return visitor.Found();
}

bool IsArgParamIdentifier(std::string_view id, int* out_arg_idx = nullptr) {
  if (id.size() < 4 || !absl::StartsWith(id, "arg")) {
    return false;
  }
  int idx = -1;
  if (!absl::SimpleAtoi(id.substr(3), &idx) || idx < 0) {
    return false;
  }
  if (out_arg_idx != nullptr) {
    *out_arg_idx = idx;
  }
  return true;
}

bool ExtractPrologueNarrowParamCopy(const CExpression& rhs_expr, std::string* param_name,
                                    CType* narrow_type) {
  if (rhs_expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(rhs_expr);
    if (bin.Op() == "&" && bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
        bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
      std::string_view id = static_cast<const IdentifierExpression&>(bin.Lhs()).Name();
      int64_t mask = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
      if (IsArgParamIdentifier(id)) {
        if (mask == 255) {
          *param_name = std::string(id);
          *narrow_type = CType::U8();
          return true;
        }
        if (mask == 65535) {
          *param_name = std::string(id);
          *narrow_type = CType::U16();
          return true;
        }
      }
    } else if (bin.Op() == ">>" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
               bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
      int64_t rshift = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
      const auto& lbin = static_cast<const BinaryExpression&>(bin.Lhs());
      if (lbin.Op() == "<<" && lbin.Lhs().Kind() == CExpressionKind::kIdentifier &&
          lbin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        std::string_view id = static_cast<const IdentifierExpression&>(lbin.Lhs()).Name();
        int64_t lshift = static_cast<const IntegerLiteralExpression&>(lbin.Rhs()).Value();
        if (IsArgParamIdentifier(id)) {
          if (lshift == 24 && rshift == 24) {
            *param_name = std::string(id);
            *narrow_type = CType::S8();
            return true;
          }
          if (lshift == 16 && rshift == 16) {
            *param_name = std::string(id);
            *narrow_type = CType::S16();
            return true;
          }
        }
      }
    }
  } else if (rhs_expr.Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(rhs_expr);
    if (cast.Operand().Kind() == CExpressionKind::kIdentifier &&
        cast.TargetType().pointer_depth == 0) {
      std::string_view id = static_cast<const IdentifierExpression&>(cast.Operand()).Name();
      std::string_view tname = cast.TargetType().name;
      if (IsArgParamIdentifier(id) &&
          (tname == "u8" || tname == "s8" || tname == "u16" || tname == "s16")) {
        *param_name = std::string(id);
        *narrow_type = cast.TargetType();
        return true;
      }
    }
  }
  return false;
}

bool IsVariableLiveAcrossCall(const CStatement& stmt, std::string_view var) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      bool seen_call = false;
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child == nullptr) continue;
        if (seen_call && StatementReferencesVariable(*child, var)) {
          return true;
        }
        if (IsVariableLiveAcrossCall(*child, var)) {
          return true;
        }
        if (StatementHasCall(*child)) {
          seen_call = true;
        }
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ExpressionContainsCall(ifs.Condition(), /*ignore_math_intrinsics=*/true) &&
          (StatementReferencesVariable(ifs.ThenBranch(), var) ||
           (ifs.ElseBranch() != nullptr && StatementReferencesVariable(*ifs.ElseBranch(), var)))) {
        return true;
      }
      return IsVariableLiveAcrossCall(ifs.ThenBranch(), var) ||
             (ifs.ElseBranch() != nullptr && IsVariableLiveAcrossCall(*ifs.ElseBranch(), var));
    }
    case CStatementKind::kWhileStatement:
    case CStatementKind::kDoWhileStatement:
    case CStatementKind::kForStatement:
      return StatementHasCall(stmt) && StatementReferencesVariable(stmt, var);
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      for (const auto& sc : ss.Cases()) {
        if (sc.body != nullptr && IsVariableLiveAcrossCall(*sc.body, var)) {
          return true;
        }
      }
      return false;
    }
    default:
      return false;
  }
}

bool IsMatchingNarrowPrologueStoreOfParam(const CStatement& stmt, std::string_view param_name,
                                          const CType& narrow_type) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& es = static_cast<const ExpressionStatement&>(stmt);
  if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
  if (a.Op() != "=" || a.Rhs().Kind() != CExpressionKind::kIdentifier ||
      static_cast<const IdentifierExpression&>(a.Rhs()).Name() != param_name ||
      ExpressionUsesVariable(a.Lhs(), param_name)) {
    return false;
  }
  if (a.Lhs().Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& un = static_cast<const UnaryExpression&>(a.Lhs());
  if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
  const auto& cast = static_cast<const CastExpression&>(un.Operand());
  if (cast.TargetType().pointer_depth != 1) return false;
  if (narrow_type.name == "u8" || narrow_type.name == "s8") {
    return cast.TargetType().name == "u8" || cast.TargetType().name == "s8";
  }
  if (narrow_type.name == "u16" || narrow_type.name == "s16") {
    return cast.TargetType().name == "u16" || cast.TargetType().name == "s16";
  }
  return false;
}

std::string_view ExtractF32StoreSlotVar(const CExpression& expr) {
  if (expr.Kind() != CExpressionKind::kUnaryExpression) return "";
  const auto& un = static_cast<const UnaryExpression&>(expr);
  if ((un.Op() == "*(f32*)&" || un.Op() == "*(f32 *)&") &&
      un.Operand().Kind() == CExpressionKind::kIdentifier) {
    return static_cast<const IdentifierExpression&>(un.Operand()).Name();
  }
  if (un.Op() == "*" && un.Operand().Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if (cast.TargetType().name == "f32" && cast.TargetType().pointer_depth == 1 &&
        cast.Operand().Kind() == CExpressionKind::kUnaryExpression) {
      const auto& addr_un = static_cast<const UnaryExpression&>(cast.Operand());
      if (addr_un.Op() == "&" && addr_un.Operand().Kind() == CExpressionKind::kIdentifier) {
        return static_cast<const IdentifierExpression&>(addr_un.Operand()).Name();
      }
    }
  }
  return "";
}

int CountF32SlotUsesInStatement(const CStatement& root, std::string_view slot_var) {
  int count = 0;
  std::function<void(const CExpression&)> visit_expr = [&](const CExpression& e) {
    if (ExtractF32StoreSlotVar(e) == slot_var) {
      ++count;
      return;
    }
    switch (e.Kind()) {
      case CExpressionKind::kUnaryExpression:
        visit_expr(static_cast<const UnaryExpression&>(e).Operand());
        break;
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(e);
        visit_expr(b.Lhs());
        visit_expr(b.Rhs());
        break;
      }
      case CExpressionKind::kAssignmentExpression: {
        const auto& as = static_cast<const AssignmentExpression&>(e);
        visit_expr(as.Lhs());
        visit_expr(as.Rhs());
        break;
      }
      case CExpressionKind::kCallExpression: {
        const auto& c = static_cast<const CallExpression&>(e);
        visit_expr(c.Callee());
        for (const auto& arg : c.Arguments()) {
          if (arg != nullptr) visit_expr(*arg);
        }
        break;
      }
      case CExpressionKind::kCastExpression:
        visit_expr(static_cast<const CastExpression&>(e).Operand());
        break;
      case CExpressionKind::kMemberAccessExpression:
        visit_expr(static_cast<const MemberAccessExpression&>(e).Object());
        break;
      case CExpressionKind::kArrayIndexExpression: {
        const auto& ar = static_cast<const ArrayIndexExpression&>(e);
        visit_expr(ar.Array());
        visit_expr(ar.Index());
        break;
      }
      case CExpressionKind::kTernaryExpression: {
        const auto& t = static_cast<const TernaryExpression&>(e);
        visit_expr(t.Condition());
        visit_expr(t.TrueExpression());
        visit_expr(t.FalseExpression());
        break;
      }
      default:
        break;
    }
  };
  std::function<void(const CStatement&)> visit_stmt = [&](const CStatement& st) {
    switch (st.Kind()) {
      case CStatementKind::kCompoundStatement:
        for (const auto& ch : static_cast<const CompoundStatement&>(st).Statements()) {
          if (ch != nullptr) visit_stmt(*ch);
        }
        break;
      case CStatementKind::kExpressionStatement:
        visit_expr(static_cast<const ExpressionStatement&>(st).Expression());
        break;
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(st);
        visit_expr(ifs.Condition());
        visit_stmt(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) visit_stmt(*ifs.ElseBranch());
        break;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(st);
        visit_expr(ws.Condition());
        visit_stmt(ws.Body());
        break;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(st);
        visit_stmt(dws.Body());
        visit_expr(dws.Condition());
        break;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(st);
        if (fs.Init() != nullptr) visit_stmt(*fs.Init());
        if (fs.Condition() != nullptr) visit_expr(*fs.Condition());
        if (fs.Step() != nullptr) visit_expr(*fs.Step());
        visit_stmt(fs.Body());
        break;
      }
      case CStatementKind::kReturnStatement: {
        const auto& rs = static_cast<const ReturnStatement&>(st);
        if (rs.ReturnValue() != nullptr) visit_expr(*rs.ReturnValue());
        break;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(st);
        visit_expr(ss.Condition());
        for (const auto& c : ss.Cases()) {
          if (c.body != nullptr) visit_stmt(*c.body);
        }
        break;
      }
      default:
        break;
    }
  };
  visit_stmt(root);
  return count;
}

std::unique_ptr<CExpression> RewriteF32SlotAliasesInExpression(
    const CExpression& e, const absl::flat_hash_map<std::string, std::string>& aliases) {
  std::string_view sv = ExtractF32StoreSlotVar(e);
  if (!sv.empty()) {
    auto fit = aliases.find(std::string(sv));
    if (fit != aliases.end()) {
      return CExpression::Identifier(fit->second);
    }
  }
  if (e.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& un = static_cast<const UnaryExpression&>(e);
    return CExpression::Unary(un.Op(), RewriteF32SlotAliasesInExpression(un.Operand(), aliases),
                              un.IsPrefix());
  }
  if (e.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& b = static_cast<const BinaryExpression&>(e);
    return CExpression::Binary(b.Op(), RewriteF32SlotAliasesInExpression(b.Lhs(), aliases),
                               RewriteF32SlotAliasesInExpression(b.Rhs(), aliases));
  }
  if (e.Kind() == CExpressionKind::kAssignmentExpression) {
    const auto& as = static_cast<const AssignmentExpression&>(e);
    return CExpression::Assignment(as.Op(), RewriteF32SlotAliasesInExpression(as.Lhs(), aliases),
                                   RewriteF32SlotAliasesInExpression(as.Rhs(), aliases));
  }
  if (e.Kind() == CExpressionKind::kCallExpression) {
    const auto& c = static_cast<const CallExpression&>(e);
    std::vector<std::unique_ptr<CExpression>> args;
    args.reserve(c.Arguments().size());
    for (const auto& arg : c.Arguments()) {
      args.push_back(arg != nullptr ? RewriteF32SlotAliasesInExpression(*arg, aliases) : nullptr);
    }
    return CExpression::Call(RewriteF32SlotAliasesInExpression(c.Callee(), aliases),
                             std::move(args));
  }
  if (e.Kind() == CExpressionKind::kCastExpression) {
    const auto& c = static_cast<const CastExpression&>(e);
    return CExpression::Cast(c.TargetType(),
                             RewriteF32SlotAliasesInExpression(c.Operand(), aliases));
  }
  if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
    const auto& m = static_cast<const MemberAccessExpression&>(e);
    return CExpression::MemberAccess(RewriteF32SlotAliasesInExpression(m.Object(), aliases),
                                     m.MemberName(), m.IsArrow());
  }
  if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
    const auto& ar = static_cast<const ArrayIndexExpression&>(e);
    return CExpression::ArrayIndex(RewriteF32SlotAliasesInExpression(ar.Array(), aliases),
                                   RewriteF32SlotAliasesInExpression(ar.Index(), aliases));
  }
  if (e.Kind() == CExpressionKind::kTernaryExpression) {
    const auto& t = static_cast<const TernaryExpression&>(e);
    return CExpression::Ternary(RewriteF32SlotAliasesInExpression(t.Condition(), aliases),
                                RewriteF32SlotAliasesInExpression(t.TrueExpression(), aliases),
                                RewriteF32SlotAliasesInExpression(t.FalseExpression(), aliases));
  }
  return e.Clone();
}

bool TryCoalescePartialWebPrologueParameterCopy(CompoundStatement* body, size_t prologue_copy_idx,
                                                std::string_view lhs, std::string_view rhs) {
  if (body == nullptr || !IsCalleeSavedGprVarName(lhs) || lhs == "fp" ||
      StatementTakesAddressOf(*body, lhs) || StatementContainsLabel(*body) ||
      StatementContainsGoto(*body) || StatementCallsThroughFunctionPointerVar(*body, lhs)) {
    return false;
  }
  if (StatementModifiesVariable(*body, rhs) ||
      CountIdentifierOccurrencesInStatement(*body, rhs) != 1) {
    return false;
  }
  auto& stmts = body->MutableStatements();
  if (prologue_copy_idx >= stmts.size()) return false;

  std::optional<size_t> redef_idx;
  for (size_t k = prologue_copy_idx + 1; k < stmts.size(); ++k) {
    if (stmts[k] == nullptr || !StatementModifiesVariable(*stmts[k], lhs)) {
      continue;
    }
    if (redef_idx.has_value() || stmts[k]->Kind() != CStatementKind::kExpressionStatement) {
      return false;
    }
    const auto& es = static_cast<const ExpressionStatement&>(*stmts[k]);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
      return false;
    }
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier ||
        static_cast<const IdentifierExpression&>(a.Lhs()).Name() != lhs ||
        ExpressionUsesVariable(a.Rhs(), lhs)) {
      return false;
    }
    redef_idx = k;
  }
  if (!redef_idx.has_value() || *redef_idx + 1 >= stmts.size() ||
      stmts[*redef_idx + 1] == nullptr) {
    return false;
  }
  const CStatement& call_stmt = *stmts[*redef_idx + 1];
  if (!StatementHasCall(call_stmt) || !StatementReferencesVariable(call_stmt, lhs)) {
    return false;
  }
  bool has_blocked_stack_arg_setup = false;
  for (size_t k = prologue_copy_idx + 1; k < *redef_idx; ++k) {
    if (stmts[k] == nullptr || stmts[k]->Kind() != CStatementKind::kExpressionStatement) {
      continue;
    }
    const auto& es = static_cast<const ExpressionStatement&>(*stmts[k]);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
      continue;
    }
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=") continue;
    std::string_view slot_var = ExtractAssignedLhsVar(a.Lhs());
    if (absl::StartsWith(slot_var, "var_sp_") && ExpressionUsesVariable(a.Rhs(), lhs) &&
        StatementReferencesVariable(call_stmt, slot_var)) {
      has_blocked_stack_arg_setup = true;
      break;
    }
  }
  if (!has_blocked_stack_arg_setup) {
    return false;
  }
  for (size_t k = prologue_copy_idx + 1; k < *redef_idx; ++k) {
    if (stmts[k] != nullptr) {
      stmts[k] = SubstituteIdentifierInStatement(std::move(stmts[k]), lhs, rhs);
    }
  }
  stmts.erase(stmts.begin() + prologue_copy_idx);
  return true;
}

}  // namespace

void CoalescePrologueParameterCopies(CompoundStatement* comp,
                                     const std::vector<CParameter>& parameters, uint32_t frame_size,
                                     std::vector<CParameter>* mutable_parameters) {
  if (comp == nullptr) return;
  absl::flat_hash_set<std::string> param_names;
  absl::flat_hash_set<std::string> float_param_names;
  absl::flat_hash_map<std::string, std::string> f32_param_homing_slots;
  uint32_t param_slot = 0;
  for (const auto& p : parameters) {
    param_names.insert(p.name);
    if (IsFloat64Type(p.type)) {
      if (param_slot % 2 != 0) ++param_slot;
      if (absl::StartsWith(p.name, "arg")) {
        float_param_names.insert(p.name);
      }
      param_slot += 2;
    } else {
      if (IsFloat32Type(p.type)) {
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
  absl::flat_hash_map<std::string, std::string> coalesced_f32_slot_aliases;
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
        std::string narrow_rhs;
        CType narrow_param_type = CType::S32();
        const bool is_narrow_param_copy =
            (mutable_parameters != nullptr && a.Op() == "=" &&
             a.Lhs().Kind() == CExpressionKind::kIdentifier &&
             ExtractPrologueNarrowParamCopy(a.Rhs(), &narrow_rhs, &narrow_param_type));
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            (a.Rhs().Kind() == CExpressionKind::kIdentifier || is_narrow_param_copy)) {
          std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          std::string rhs = is_narrow_param_copy
                                ? narrow_rhs
                                : static_cast<const IdentifierExpression&>(a.Rhs()).Name();
          bool is_s_reg = (absl::StartsWith(lhs, "temp_s") || lhs == "fp" ||
                           (lhs.size() == 2 && lhs[0] == 's' && std::isdigit(lhs[1])));
          bool is_f_reg = (lhs.size() >= 2 && lhs[0] == 'f' && std::isdigit(lhs[1]));
          bool is_callee_saved_f_reg =
              (lhs == "f20" || lhs == "f22" || lhs == "f24" || lhs == "f26" || lhs == "f28" ||
               lhs == "f30" || lhs == "f20_d" || lhs == "f22_d" || lhs == "f24_d" ||
               lhs == "f26_d" || lhs == "f28_d" || lhs == "f30_d");
          bool is_homed_param_slot = false;
          if ((frame_size > 0 || StatementTakesAddressOf(*comp, lhs)) && rhs.size() == 4 &&
              absl::StartsWith(rhs, "arg") && rhs[3] >= '0' && rhs[3] <= '3') {
            uint32_t expected_off = frame_size + static_cast<uint32_t>(rhs[3] - '0') * 4;
            if (lhs == absl::StrCat("var_sp_", expected_off)) {
              is_homed_param_slot = true;
            }
          }
          bool is_in_call_free_prologue = true;
          for (size_t pre = 0; pre < i; ++pre) {
            if (comp->Statements()[pre] == nullptr) continue;
            if (comp->Statements()[pre]->Kind() != CStatementKind::kExpressionStatement ||
                StatementHasCall(*comp->Statements()[pre]) ||
                StatementModifiesVariable(*comp->Statements()[pre], rhs) ||
                StatementReferencesVariable(*comp->Statements()[pre], lhs)) {
              is_in_call_free_prologue = false;
              break;
            }
          }
          const CStatement* following_branch_stmt = nullptr;
          for (size_t post = i + 1; is_in_call_free_prologue && post < comp->Statements().size();
               ++post) {
            const CStatement* post_s = comp->Statements()[post].get();
            if (post_s == nullptr) continue;
            if (post_s->Kind() == CStatementKind::kIfStatement ||
                post_s->Kind() == CStatementKind::kSwitchStatement) {
              following_branch_stmt = post_s;
              break;
            }
            if (post_s->Kind() != CStatementKind::kExpressionStatement ||
                StatementHasCall(*post_s)) {
              break;
            }
          }
          bool is_single_assign_prologue_v_copy =
              is_in_call_free_prologue && (lhs == "v0" || lhs == "v1") &&
              param_names.contains(rhs) && following_branch_stmt != nullptr;
          bool allow_gpr_float_param_s_coalesce = false;
          if (is_in_call_free_prologue && is_s_reg && lhs != "fp" &&
              float_param_names.contains(rhs) && !parameters.empty() &&
              !IsFloat32Type(parameters[0].type) && !IsFloat64Type(parameters[0].type) &&
              StatementReferencesVariable(*comp, "fp")) {
            for (size_t k = 0; k < comp->Statements().size(); ++k) {
              const CStatement* ks = comp->Statements()[k].get();
              if (ks == nullptr) continue;
              if (StatementHasCall(*ks)) break;
              if (ks->Kind() == CStatementKind::kExpressionStatement) {
                const auto& kes = static_cast<const ExpressionStatement&>(*ks);
                if (kes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& ka = static_cast<const AssignmentExpression&>(kes.Expression());
                  if (ka.Op() == "=" && ka.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      static_cast<const IdentifierExpression&>(ka.Lhs()).Name() == "fp" &&
                      ExpressionUsesVariable(ka.Rhs(), "sp")) {
                    allow_gpr_float_param_s_coalesce = true;
                    break;
                  }
                }
              }
            }
          }
          if (param_names.contains(rhs) &&
              (is_s_reg || is_f_reg || is_homed_param_slot || is_single_assign_prologue_v_copy) &&
              !(float_param_names.contains(rhs) &&
                ((is_s_reg && !allow_gpr_float_param_s_coalesce) || is_homed_param_slot ||
                 is_single_assign_prologue_v_copy || is_callee_saved_f_reg))) {
            bool lhs_addr_taken = StatementTakesAddressOf(*comp, lhs);
            bool v_copy_ok = true;
            if (is_single_assign_prologue_v_copy) {
              const CStatement& branch_s = *following_branch_stmt;
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
                } else if (st.Kind() == CStatementKind::kSwitchStatement) {
                  const auto& ss = static_cast<const SwitchStatement&>(st);
                  if (expr_uses_in_call_arg(ss.Condition())) return true;
                  for (const auto& sc : ss.Cases()) {
                    if (sc.body != nullptr && stmt_uses_in_call_arg(*sc.body)) return true;
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
            int rhs_arg_idx = -1;
            if (is_homed_param_slot && !is_assigned(rhs) &&
                (!StatementUsesAddressOfInPointerArithmetic(*comp, lhs) ||
                 (IsArgParamIdentifier(rhs, &rhs_arg_idx) && count_assignments(lhs) == 1 &&
                  !StatementReferencesVariable(
                      *comp,
                      absl::StrCat("var_sp_",
                                   frame_size + static_cast<uint32_t>(rhs_arg_idx + 1) * 4))))) {
              if (count_assignments(lhs) == 1) {
                homed_param_coalesce_ok = true;
              } else if (is_in_call_free_prologue &&
                         CountIdentifierOccurrencesInStatement(*comp, rhs) == 1) {
                homed_param_coalesce_ok = true;
              }
            }
            bool has_other_s_regs = false;
            for (int s_idx = 0; s_idx <= 7; ++s_idx) {
              std::string candidate_s = absl::StrCat("temp_s", s_idx);
              if (candidate_s != lhs && !coalesced_aliases.contains(candidate_s) &&
                  StatementReferencesVariable(*comp, candidate_s)) {
                has_other_s_regs = true;
                break;
              }
            }
            if (!has_other_s_regs && lhs != "fp" && !coalesced_aliases.contains("fp") &&
                StatementReferencesVariable(*comp, "fp")) {
              has_other_s_regs = true;
            }
            bool other_param_used = false;
            for (const auto& p : parameters) {
              if (p.name != rhs && StatementReferencesVariable(*comp, p.name)) {
                other_param_used = true;
                break;
              }
            }
            bool all_lower_s_regs_used = false;
            if (lhs.size() == 7 && absl::StartsWith(lhs, "temp_s") && lhs[6] >= '0' &&
                lhs[6] <= '7' && !StatementReferencesVariable(*comp, "fp")) {
              int lhs_s_idx = lhs[6] - '0';
              all_lower_s_regs_used = true;
              for (int s_idx = 0; s_idx < lhs_s_idx; ++s_idx) {
                std::string candidate_s = absl::StrCat("temp_s", s_idx);
                bool used = !coalesced_aliases.contains(candidate_s) &&
                            StatementReferencesVariable(*comp, candidate_s);
                if (!used) {
                  all_lower_s_regs_used = false;
                  break;
                }
              }
            }
            bool used_across_call_before_reassignment = false;
            if (all_lower_s_regs_used && parameters.size() == 1) {
              auto check_bare_deref_and_offset = [&](const CStatement& st, bool* bare_deref,
                                                     bool* offset_add) {
                std::function<void(const CExpression&)> walk_e = [&](const CExpression& e) {
                  if (e.Kind() == CExpressionKind::kUnaryExpression) {
                    const auto& u = static_cast<const UnaryExpression&>(e);
                    if (u.Op() == "*" && u.Operand().Kind() == CExpressionKind::kCastExpression) {
                      const auto& c = static_cast<const CastExpression&>(u.Operand());
                      if (c.Operand().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(c.Operand()).Name() == lhs) {
                        *bare_deref = true;
                      }
                    }
                    walk_e(u.Operand());
                  } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
                    const auto& b = static_cast<const BinaryExpression&>(e);
                    if ((b.Op() == "+" || b.Op() == "-") &&
                        b.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
                        ExpressionUsesVariable(b.Lhs(), lhs)) {
                      *offset_add = true;
                    }
                    walk_e(b.Lhs());
                    walk_e(b.Rhs());
                  } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& a_expr = static_cast<const AssignmentExpression&>(e);
                    walk_e(a_expr.Lhs());
                    walk_e(a_expr.Rhs());
                  } else if (e.Kind() == CExpressionKind::kCastExpression) {
                    walk_e(static_cast<const CastExpression&>(e).Operand());
                  } else if (e.Kind() == CExpressionKind::kCallExpression) {
                    for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
                      if (arg != nullptr) walk_e(*arg);
                    }
                  } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
                    const auto& t = static_cast<const TernaryExpression&>(e);
                    walk_e(t.Condition());
                    walk_e(t.TrueExpression());
                    walk_e(t.FalseExpression());
                  }
                };
                std::function<void(const CStatement&)> walk_s = [&](const CStatement& s_node) {
                  if (s_node.Kind() == CStatementKind::kExpressionStatement) {
                    walk_e(static_cast<const ExpressionStatement&>(s_node).Expression());
                  } else if (s_node.Kind() == CStatementKind::kCompoundStatement) {
                    for (const auto& ch :
                         static_cast<const CompoundStatement&>(s_node).Statements()) {
                      if (ch != nullptr) walk_s(*ch);
                    }
                  } else if (s_node.Kind() == CStatementKind::kIfStatement) {
                    const auto& ifs = static_cast<const IfStatement&>(s_node);
                    walk_e(ifs.Condition());
                    walk_s(ifs.ThenBranch());
                    if (ifs.ElseBranch() != nullptr) walk_s(*ifs.ElseBranch());
                  }
                };
                walk_s(st);
              };
              bool saw_call = false;
              bool saw_bare_deref = false;
              bool saw_offset_add = false;
              for (size_t post = i + 1; post < comp->Statements().size(); ++post) {
                const CStatement* post_s = comp->Statements()[post].get();
                if (post_s == nullptr) continue;
                if (post_s->Kind() == CStatementKind::kExpressionStatement) {
                  const auto& post_es = static_cast<const ExpressionStatement&>(*post_s);
                  if (post_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& post_a =
                        static_cast<const AssignmentExpression&>(post_es.Expression());
                    if (post_a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        static_cast<const IdentifierExpression&>(post_a.Lhs()).Name() == lhs) {
                      break;
                    }
                  }
                }
                if (saw_call && StatementReferencesVariable(*post_s, lhs)) {
                  check_bare_deref_and_offset(*post_s, &saw_bare_deref, &saw_offset_add);
                  if (saw_bare_deref && saw_offset_add) {
                    used_across_call_before_reassignment = true;
                    break;
                  }
                }
                if (StatementHasCall(*post_s)) {
                  saw_call = true;
                }
              }
            }
            bool s_reg_mutated_param_coalesce_ok =
                !is_narrow_param_copy && is_s_reg && is_in_call_free_prologue && !lhs_addr_taken &&
                ((other_param_used && !has_other_s_regs) || used_across_call_before_reassignment) &&
                StatementHasCall(*comp) && CountIdentifierOccurrencesInStatement(*comp, rhs) == 1;
            bool narrow_param_coalesce_ok = true;
            if (is_narrow_param_copy) {
              const int rhs_occurrences = CountIdentifierOccurrencesInStatement(*comp, rhs);
              const bool rhs_uses_ok = (rhs_occurrences == 1) ||
                                       (rhs_occurrences == 2 && i + 1 < comp->Statements().size() &&
                                        comp->Statements()[i + 1] != nullptr &&
                                        IsMatchingNarrowPrologueStoreOfParam(
                                            *comp->Statements()[i + 1], rhs, narrow_param_type));
              const bool is_temp_s_lhs = lhs.size() == 7 && absl::StartsWith(lhs, "temp_s") &&
                                         lhs[6] >= '0' && lhs[6] <= '7';
              bool s_order_ok = false;
              if (is_temp_s_lhs && !StatementReferencesVariable(*comp, "fp")) {
                const int lhs_s_idx = lhs[6] - '0';
                int rhs_arg_idx = -1;
                s_order_ok = IsArgParamIdentifier(rhs, &rhs_arg_idx);
                for (int s_idx = 0; s_order_ok && s_idx < lhs_s_idx; ++s_idx) {
                  std::string candidate_s = absl::StrCat("temp_s", s_idx);
                  auto alias_it = coalesced_aliases.find(candidate_s);
                  if (alias_it != coalesced_aliases.end()) {
                    const std::string& aliased_param = alias_it->second;
                    int aliased_arg_idx = -1;
                    if (!IsArgParamIdentifier(aliased_param, &aliased_arg_idx) ||
                        aliased_arg_idx >= rhs_arg_idx) {
                      s_order_ok = false;
                      break;
                    }
                  } else if (!StatementReferencesVariable(*comp, candidate_s)) {
                    s_order_ok = false;
                    break;
                  }
                }
                for (int arg_idx = 0; s_order_ok && arg_idx < rhs_arg_idx; ++arg_idx) {
                  std::string earlier_param = absl::StrCat("arg", arg_idx);
                  bool coalesced_from_lower_s = false;
                  for (int s_idx = 0; s_idx < lhs_s_idx; ++s_idx) {
                    auto alias_it = coalesced_aliases.find(absl::StrCat("temp_s", s_idx));
                    if (alias_it != coalesced_aliases.end() && alias_it->second == earlier_param) {
                      coalesced_from_lower_s = true;
                      break;
                    }
                  }
                  if (!coalesced_from_lower_s && IsVariableLiveAcrossCall(*comp, earlier_param)) {
                    s_order_ok = false;
                    break;
                  }
                }
              }
              narrow_param_coalesce_ok = is_temp_s_lhs && is_in_call_free_prologue && rhs_uses_ok &&
                                         s_order_ok && IsVariableLiveAcrossCall(*comp, lhs);
            }
            if (!is_assigned(rhs) && v_copy_ok && narrow_param_coalesce_ok &&
                (is_homed_param_slot ? homed_param_coalesce_ok
                                     : ((count_assignments(lhs) == 1 && !lhs_addr_taken) ||
                                        s_reg_mutated_param_coalesce_ok)) &&
                !StatementCallsThroughFunctionPointerVar(*comp, lhs)) {
              coalesced_aliases[lhs] = rhs;
              stmts_to_erase.push_back(i);
              if (is_narrow_param_copy && mutable_parameters != nullptr) {
                for (auto& p : *mutable_parameters) {
                  if (p.name == rhs) {
                    p.type = narrow_param_type;
                    break;
                  }
                }
              }
            }
          }
        } else if (a.Op() == "=" && a.Rhs().Kind() == CExpressionKind::kIdentifier) {
          bool in_call_free_prologue = true;
          for (size_t pre = 0; pre < i; ++pre) {
            if (comp->Statements()[pre] == nullptr) continue;
            if (comp->Statements()[pre]->Kind() != CStatementKind::kExpressionStatement ||
                StatementHasCall(*comp->Statements()[pre])) {
              in_call_free_prologue = false;
              break;
            }
          }
          if (!in_call_free_prologue) continue;
          std::string rhs = static_cast<const IdentifierExpression&>(a.Rhs()).Name();
          std::string_view slot_var = ExtractF32StoreSlotVar(a.Lhs());
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
            } else if (CountIdentifierOccurrencesInStatement(*comp, rhs) == 1 &&
                       CountF32SlotUsesInStatement(*comp, slot_var) ==
                           CountIdentifierOccurrencesInStatement(*comp, slot_var)) {
              coalesced_f32_slot_aliases[std::string(slot_var)] = rhs;
              stmts_to_erase.push_back(i);
            }
          }
        }
      }
    }
  }

  if (coalesced_aliases.empty() && coalesced_expr_aliases.empty() &&
      coalesced_f32_slot_aliases.empty()) {
    return;
  }

  auto apply_aliases_to_expr =
      [&](std::unique_ptr<CExpression> expr) -> std::unique_ptr<CExpression> {
    if (!coalesced_f32_slot_aliases.empty()) {
      expr = RewriteF32SlotAliasesInExpression(*expr, coalesced_f32_slot_aliases);
    }
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
                                               vds.ArraySize(), vds.RegisterAnnotation());
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
    if (!IsFloat32Type(p.type) || (p.name != "arg1" && p.name != "arg2" && p.name != "arg3")) {
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
            if (seen_float_read && !ExpressionUsesVariable(a.Rhs(), p.name)) {
              split_start_idx = i;
            }
            break;
          }
        }
      }
      if (StatementReferencesVariable(*stmts[i], p.name)) {
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

  for (int s_idx = 0; s_idx <= 7; ++s_idx) {
    std::string gpr_name = absl::StrCat("temp_s", s_idx);
    if (!StatementTakesAddressOf(*comp, gpr_name)) continue;
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kExpressionStatement) {
        continue;
      }
      const auto& es = static_cast<const ExpressionStatement&>(*stmts[i]);
      if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) continue;
      const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
      if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier ||
          static_cast<const IdentifierExpression&>(a.Lhs()).Name() != gpr_name ||
          a.Rhs().Kind() != CExpressionKind::kUnaryExpression) {
        continue;
      }
      const auto& rhs_un = static_cast<const UnaryExpression&>(a.Rhs());
      if ((rhs_un.Op() != "*(s32*)&" && rhs_un.Op() != "*(s32 *)&") ||
          rhs_un.Operand().Kind() != CExpressionKind::kIdentifier) {
        continue;
      }
      std::string fpr_src(static_cast<const IdentifierExpression&>(rhs_un.Operand()).Name());
      if (fpr_src.size() < 2 || fpr_src[0] != 'f' || !std::isdigit(fpr_src[1])) continue;
      std::optional<size_t> redef_idx;
      int bitcast_read_count = 0;
      bool has_non_bitcast_use = false;
      for (size_t j = i + 1; j < stmts.size(); ++j) {
        if (stmts[j] == nullptr) continue;
        if (stmts[j]->Kind() == CStatementKind::kExpressionStatement) {
          const auto& jes = static_cast<const ExpressionStatement&>(*stmts[j]);
          if (jes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& ja = static_cast<const AssignmentExpression&>(jes.Expression());
            if (ja.Op() == "=" && ja.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(ja.Lhs()).Name() == gpr_name &&
                !ExpressionUsesVariable(ja.Rhs(), gpr_name)) {
              redef_idx = j;
              break;
            }
            if (ja.Op() == "=" && ja.Lhs().Kind() == CExpressionKind::kIdentifier &&
                ja.Rhs().Kind() == CExpressionKind::kUnaryExpression) {
              const auto& ju = static_cast<const UnaryExpression&>(ja.Rhs());
              if ((ju.Op() == "*(f32*)&" || ju.Op() == "*(f32 *)&") &&
                  ju.Operand().Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression&>(ju.Operand()).Name() == gpr_name &&
                  !ExpressionUsesVariable(ja.Lhs(), gpr_name)) {
                ++bitcast_read_count;
                continue;
              }
            }
          }
        }
        if (StatementReferencesVariable(*stmts[j], gpr_name)) {
          has_non_bitcast_use = true;
          break;
        }
      }
      if (redef_idx.has_value() && bitcast_read_count > 0 && !has_non_bitcast_use) {
        std::string float_carrier_name = absl::StrCat(gpr_name, "_f");
        stmts[i] = CStatement::Expression(CExpression::Assignment(
            "=", CExpression::Identifier(float_carrier_name), CExpression::Identifier(fpr_src)));
        for (size_t j = i + 1; j < *redef_idx; ++j) {
          if (stmts[j] == nullptr || stmts[j]->Kind() != CStatementKind::kExpressionStatement) {
            continue;
          }
          const auto& jes = static_cast<const ExpressionStatement&>(*stmts[j]);
          if (jes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& ja = static_cast<const AssignmentExpression&>(jes.Expression());
            if (ja.Op() == "=" && ja.Lhs().Kind() == CExpressionKind::kIdentifier &&
                ja.Rhs().Kind() == CExpressionKind::kUnaryExpression) {
              const auto& ju = static_cast<const UnaryExpression&>(ja.Rhs());
              if ((ju.Op() == "*(f32*)&" || ju.Op() == "*(f32 *)&") &&
                  ju.Operand().Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression&>(ju.Operand()).Name() == gpr_name) {
                stmts[j] = CStatement::Expression(CExpression::Assignment(
                    "=", ja.Lhs().Clone(), CExpression::Identifier(float_carrier_name)));
              }
            }
          }
        }
        break;
      }
    }
  }
}

bool SplitScratchReusedParameterRegisters(CompoundStatement* body,
                                          const std::vector<CParameter>& parameters,
                                          uint32_t frame_size,
                                          std::vector<CParameter>* mutable_parameters) {
  if (body == nullptr || parameters.empty()) return false;
  auto& stmts = body->MutableStatements();
  bool has_goto = StatementContainsGoto(*body);
  absl::flat_hash_set<std::string> param_names;
  for (const auto& p : parameters) {
    param_names.insert(p.name);
  }
  auto is_temp_t_var = [](std::string_view v) {
    return v.size() == 7 && absl::StartsWith(v, "temp_t") && v[6] >= '0' && v[6] <= '9';
  };
  auto is_non_param_arg_var = [&](std::string_view v) {
    return v.size() == 4 && absl::StartsWith(v, "arg") && v[3] >= '0' && v[3] <= '3' &&
           !param_names.contains(std::string(v));
  };
  bool any_homed_split = false;
  bool any_s_split_coalesced = false;
  bool any_partial_web_s_coalesced = false;
  size_t s_saved_params_count = 0;
  size_t matching_s_splits = 0;
  for (const auto& param : parameters) {
    if (param.name.size() != 4 || !absl::StartsWith(param.name, "arg") || param.name[3] < '0' ||
        param.name[3] > '3' || IsFloat32Type(param.type)) {
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
    bool is_narrow_prologue_copy = false;
    for (size_t i = 0; i < stmts.size() && i < 8; ++i) {
      if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kExpressionStatement ||
          StatementHasCall(*stmts[i])) {
        break;
      }
      const auto& es = static_cast<const ExpressionStatement&>(*stmts[i]);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        std::string narrow_rhs;
        CType narrow_type = CType::S32();
        const bool matches_narrow_rhs =
            (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
             ExtractPrologueNarrowParamCopy(a.Rhs(), &narrow_rhs, &narrow_type) &&
             narrow_rhs == param.name);
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            (a.Rhs().Kind() == CExpressionKind::kIdentifier || matches_narrow_rhs)) {
          std::string_view lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          const std::string& rhs = matches_narrow_rhs
                                       ? narrow_rhs
                                       : static_cast<const IdentifierExpression&>(a.Rhs()).Name();
          bool narrow_lhs_single_assign = true;
          if (matches_narrow_rhs) {
            if (mutable_parameters == nullptr || !IsCalleeSavedGprVarName(lhs) || lhs == "fp") {
              narrow_lhs_single_assign = false;
            } else {
              for (size_t k = i + 1; k < stmts.size(); ++k) {
                if (stmts[k] != nullptr && StatementModifiesVariable(*stmts[k], lhs)) {
                  narrow_lhs_single_assign = false;
                  break;
                }
              }
            }
          }
          if (rhs == param.name &&
              (IsCalleeSavedGprVarName(lhs) || is_temp_t_var(lhs) || is_non_param_arg_var(lhs) ||
               (!homed_slot_name.empty() && lhs == homed_slot_name))) {
            prologue_copy_idx = i;
            prologue_copy_lhs = std::string(lhs);
            is_homed_copy = (!homed_slot_name.empty() && lhs == homed_slot_name);
            is_narrow_prologue_copy = matches_narrow_rhs && narrow_lhs_single_assign;
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
    if (IsCalleeSavedGprVarName(prologue_copy_lhs)) {
      ++s_saved_params_count;
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
    std::function<bool(const CStatement&, bool*, int)> can_reach_read_before_write =
        [&](const CStatement& s, bool* definitely_written, int goto_depth) -> bool {
      if (*definitely_written) return false;
      switch (s.Kind()) {
        case CStatementKind::kExpressionStatement: {
          const auto& es = static_cast<const ExpressionStatement&>(s);
          if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
            if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(a.Lhs()).Name() == param.name) {
              if (ExpressionUsesVariable(a.Rhs(), param.name)) {
                return true;
              }
              *definitely_written = true;
              return false;
            }
          }
          return StatementReferencesVariable(s, param.name);
        }
        case CStatementKind::kCompoundStatement: {
          const auto& cs = static_cast<const CompoundStatement&>(s);
          for (const auto& child : cs.Statements()) {
            if (child == nullptr || child->Kind() == CStatementKind::kLabelStatement) continue;
            if (can_reach_read_before_write(*child, definitely_written, goto_depth)) {
              return true;
            }
            if (*definitely_written) return false;
            if (child->Kind() == CStatementKind::kReturnStatement ||
                child->Kind() == CStatementKind::kBreakStatement ||
                child->Kind() == CStatementKind::kContinueStatement ||
                child->Kind() == CStatementKind::kGotoStatement) {
              *definitely_written = true;
              return false;
            }
          }
          return false;
        }
        case CStatementKind::kIfStatement: {
          const auto& ifs = static_cast<const IfStatement&>(s);
          if (ExpressionUsesVariable(ifs.Condition(), param.name)) return true;
          bool then_w = *definitely_written;
          if (can_reach_read_before_write(ifs.ThenBranch(), &then_w, goto_depth)) return true;
          bool else_w = *definitely_written;
          if (ifs.ElseBranch() != nullptr) {
            if (can_reach_read_before_write(*ifs.ElseBranch(), &else_w, goto_depth)) return true;
            if (then_w && else_w) *definitely_written = true;
          }
          return false;
        }
        case CStatementKind::kDoWhileStatement: {
          const auto& dws = static_cast<const DoWhileStatement&>(s);
          if (can_reach_read_before_write(dws.Body(), definitely_written, goto_depth)) {
            return true;
          }
          return !*definitely_written && ExpressionUsesVariable(dws.Condition(), param.name);
        }
        case CStatementKind::kWhileStatement: {
          const auto& ws = static_cast<const WhileStatement&>(s);
          if (ExpressionUsesVariable(ws.Condition(), param.name)) return true;
          bool body_w = *definitely_written;
          return can_reach_read_before_write(ws.Body(), &body_w, goto_depth);
        }
        case CStatementKind::kForStatement: {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr &&
              can_reach_read_before_write(*fs.Init(), definitely_written, goto_depth)) {
            return true;
          }
          if (!*definitely_written && fs.Condition() != nullptr &&
              ExpressionUsesVariable(*fs.Condition(), param.name)) {
            return true;
          }
          bool body_w = *definitely_written;
          if (can_reach_read_before_write(fs.Body(), &body_w, goto_depth)) return true;
          if (!body_w && fs.Step() != nullptr && ExpressionUsesVariable(*fs.Step(), param.name)) {
            return true;
          }
          return false;
        }
        case CStatementKind::kGotoStatement: {
          if (goto_depth > 4) return true;
          const auto& gs = static_cast<const GotoStatement&>(s);
          if (label_is_immediately_overwritten(gs.Label())) {
            *definitely_written = true;
            return false;
          }
          return true;
        }
        default:
          return StatementReferencesVariable(s, param.name);
      }
    };
    auto label_can_reach_read_before_write = [&](std::string_view target_label) -> bool {
      bool found = false;
      bool can_read = false;
      std::function<void(const CStatement&)> find_lbl = [&](const CStatement& s) {
        if (found) return;
        if (s.Kind() == CStatementKind::kCompoundStatement) {
          const auto& cs = static_cast<const CompoundStatement&>(s);
          for (size_t idx = 0; idx < cs.Statements().size(); ++idx) {
            if (cs.Statements()[idx] != nullptr &&
                cs.Statements()[idx]->Kind() == CStatementKind::kLabelStatement &&
                static_cast<const LabelStatement&>(*cs.Statements()[idx]).Label() == target_label) {
              found = true;
              bool written = false;
              for (size_t rem = idx + 1; rem < cs.Statements().size(); ++rem) {
                if (cs.Statements()[rem] == nullptr ||
                    cs.Statements()[rem]->Kind() == CStatementKind::kLabelStatement) {
                  continue;
                }
                if (can_reach_read_before_write(*cs.Statements()[rem], &written, 1)) {
                  can_read = true;
                  return;
                }
                if (written || cs.Statements()[rem]->Kind() == CStatementKind::kReturnStatement ||
                    cs.Statements()[rem]->Kind() == CStatementKind::kBreakStatement ||
                    cs.Statements()[rem]->Kind() == CStatementKind::kContinueStatement ||
                    cs.Statements()[rem]->Kind() == CStatementKind::kGotoStatement) {
                  return;
                }
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
      return !found || can_read;
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
                bool written_after_lbl = false;
                bool reads_before_write_after_lbl = false;
                for (size_t rem = idx + 1; rem < cs_stmts.size(); ++rem) {
                  if (cs_stmts[rem] == nullptr ||
                      cs_stmts[rem]->Kind() == CStatementKind::kLabelStatement) {
                    continue;
                  }
                  if (can_reach_read_before_write(*cs_stmts[rem], &written_after_lbl, 0)) {
                    reads_before_write_after_lbl = true;
                    break;
                  }
                  if (written_after_lbl ||
                      cs_stmts[rem]->Kind() == CStatementKind::kReturnStatement ||
                      cs_stmts[rem]->Kind() == CStatementKind::kBreakStatement ||
                      cs_stmts[rem]->Kind() == CStatementKind::kContinueStatement ||
                      cs_stmts[rem]->Kind() == CStatementKind::kGotoStatement) {
                    break;
                  }
                }
                if (reads_before_write_after_lbl) {
                  return false;
                }
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
        case CStatementKind::kDoWhileStatement: {
          const auto& dws = static_cast<const DoWhileStatement&>(s);
          if (!*definitely_written && !has_goto && parameters.size() <= 4 &&
              StatementHasCall(dws) &&
              prologue_copy_lhs == absl::StrCat("temp_s", param.name.substr(3))) {
            return !StatementReferencesVariable(s, param.name);
          }
          if (!check_entry_not_read(dws.Body(), definitely_written)) {
            return false;
          }
          if (!*definitely_written && ExpressionUsesVariable(dws.Condition(), param.name)) {
            return false;
          }
          return true;
        }
        case CStatementKind::kWhileStatement: {
          const auto& ws = static_cast<const WhileStatement&>(s);
          if (!*definitely_written && !saw_call_before_write && !has_goto &&
              parameters.size() <= 4 && StatementHasCall(ws) &&
              prologue_copy_lhs == absl::StrCat("temp_s", param.name.substr(3))) {
            return !StatementReferencesVariable(s, param.name);
          }
          if (!*definitely_written && ExpressionUsesVariable(ws.Condition(), param.name)) {
            return false;
          }
          bool body_written = *definitely_written;
          return check_entry_not_read(ws.Body(), &body_written);
        }
        case CStatementKind::kForStatement: {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr && !check_entry_not_read(*fs.Init(), definitely_written)) {
            return false;
          }
          if (!*definitely_written && fs.Condition() != nullptr &&
              ExpressionUsesVariable(*fs.Condition(), param.name)) {
            return false;
          }
          bool body_written = *definitely_written;
          if (!check_entry_not_read(fs.Body(), &body_written)) {
            return false;
          }
          if (!body_written && fs.Step() != nullptr &&
              ExpressionUsesVariable(*fs.Step(), param.name)) {
            return false;
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
          if (label_is_immediately_overwritten(gs.Label()) ||
              !label_can_reach_read_before_write(gs.Label())) {
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
    bool entry_not_read = true;
    bool written_so_far = false;
    for (size_t k = subst_start_idx; k < stmts.size(); ++k) {
      if (stmts[k] == nullptr) continue;
      if (stmts[k]->Kind() == CStatementKind::kLabelStatement) {
        if (!written_so_far && !is_overwritten_after_label(*body, k)) {
          bool written_after_lbl = false;
          bool reads_before_write_after_lbl = false;
          for (size_t rem = k + 1; rem < stmts.size(); ++rem) {
            if (stmts[rem] == nullptr || stmts[rem]->Kind() == CStatementKind::kLabelStatement) {
              continue;
            }
            if (can_reach_read_before_write(*stmts[rem], &written_after_lbl, 0)) {
              reads_before_write_after_lbl = true;
              break;
            }
            if (written_after_lbl || stmts[rem]->Kind() == CStatementKind::kReturnStatement ||
                stmts[rem]->Kind() == CStatementKind::kBreakStatement ||
                stmts[rem]->Kind() == CStatementKind::kContinueStatement ||
                stmts[rem]->Kind() == CStatementKind::kGotoStatement) {
              break;
            }
          }
          if (reads_before_write_after_lbl) {
            entry_not_read = false;
            break;
          }
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
    const bool copies_to_non_matching_s_with_calls =
        IsCalleeSavedGprVarName(prologue_copy_lhs) &&
        prologue_copy_lhs != absl::StrCat("temp_s", param.name.substr(3)) &&
        StatementHasCall(*body);
    if (!entry_not_read || !any_redef_seen ||
        (!is_homed_copy && !is_narrow_prologue_copy && !saw_call_before_write &&
         !is_temp_t_var(prologue_copy_lhs) && !is_non_param_arg_var(prologue_copy_lhs) &&
         !has_goto && !copies_to_non_matching_s_with_calls && s_saved_params_count == 1 &&
         parameters.size() == 1)) {
      continue;
    }
    std::string scratch_name = absl::StrCat("temp_a", param.name.substr(3));
    for (size_t k = subst_start_idx; k < stmts.size(); ++k) {
      stmts[k] = SubstituteIdentifierInStatement(std::move(stmts[k]), param.name, scratch_name);
    }
    if (is_homed_copy) {
      any_homed_split = true;
    } else if (IsCalleeSavedGprVarName(prologue_copy_lhs)) {
      if (!is_narrow_prologue_copy &&
          TryCoalescePartialWebPrologueParameterCopy(body, *prologue_copy_idx, prologue_copy_lhs,
                                                     param.name)) {
        any_partial_web_s_coalesced = true;
        continue;
      }
      if (is_narrow_prologue_copy) {
        any_s_split_coalesced = true;
      }
      if (prologue_copy_lhs == absl::StrCat("temp_s", param.name.substr(3))) {
        if (has_goto) {
          any_s_split_coalesced = true;
        }
        ++matching_s_splits;
      }
      if (parameters.size() == 1 && StatementHasCall(*body)) {
        bool lhs_reassigned_later = false;
        for (size_t k = *prologue_copy_idx + 1; k < stmts.size(); ++k) {
          if (stmts[k] != nullptr && StatementModifiesVariable(*stmts[k], prologue_copy_lhs)) {
            lhs_reassigned_later = true;
            break;
          }
        }
        if (lhs_reassigned_later) {
          CoalescePrologueParameterCopies(body, parameters, frame_size, mutable_parameters);
          if (!StatementReferencesVariable(*body, prologue_copy_lhs)) {
            any_s_split_coalesced = true;
          }
        }
      }
    }
  }
  if (matching_s_splits > 0 && matching_s_splits == parameters.size()) {
    bool has_higher_s_reg = false;
    for (size_t s_idx = parameters.size(); s_idx <= 7; ++s_idx) {
      if (StatementReferencesVariable(*body, absl::StrCat("temp_s", s_idx))) {
        has_higher_s_reg = true;
        break;
      }
    }
    if (!has_higher_s_reg) {
      any_s_split_coalesced = true;
    }
  }
  if (any_homed_split || any_s_split_coalesced) {
    CoalescePrologueParameterCopies(body, parameters, frame_size, mutable_parameters);
  }
  return any_s_split_coalesced || any_partial_web_s_coalesced;
}

bool PrologueParameterCoalescerPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  CoalescePrologueParameterCopies(body, ctx.parameters, ctx.frame_size, ctx.mutable_parameters);
  return body->ToString() != before;
}

bool ReusedFloatGprParameterSplitterPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  SplitReusedFloatGprParameters(body, ctx.parameters);
  return body->ToString() != before;
}

bool ScratchReusedParameterSplitterPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  SplitScratchReusedParameterRegisters(body, ctx.parameters, ctx.frame_size,
                                       ctx.mutable_parameters);
  return body->ToString() != before;
}

}  // namespace rom_nom_nom
