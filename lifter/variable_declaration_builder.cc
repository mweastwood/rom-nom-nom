#include "lifter/variable_declaration_builder.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/ast_converter.h"
#include "lifter/expression_builder.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

namespace {

bool ExpressionHasCall(const CExpression& expr) {
  return ExpressionContainsCall(expr, /*ignore_math_intrinsics=*/true);
}

bool StatementHasCall(const CStatement& s) {
  return StatementContainsCall(s, /*ignore_math_intrinsics=*/true);
}

bool IsCalleeSavedGprVarName(std::string_view v) {
  return v == "fp" ||
         (v.size() == 7 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7');
}

bool IsCalleeSavedFprBaseName(std::string_view base) {
  return base == "f20" || base == "f22" || base == "f24" || base == "f26" || base == "f28" ||
         base == "f30";
}

bool IsDedicatedCallerScratchFprBaseName(std::string_view base) {
  return base == "f2" || base == "f4" || base == "f6" || base == "f8" || base == "f10" ||
         base == "f16" || base == "f18";
}

bool IsAbiArgOrReturnFprBaseName(std::string_view base) {
  return base == "f0" || base == "f12" || base == "f14";
}

bool IsFloatingPointReturnType(const CType& type) {
  return type.pointer_depth == 0 && (type.name == "f32" || type.name == "f64");
}

bool ExpressionContainsS64PointerDeref(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      if (un.IsPrefix() && un.Op() == "*" &&
          un.Operand().Kind() == CExpressionKind::kCastExpression) {
        const auto& cast = static_cast<const CastExpression&>(un.Operand());
        if (cast.TargetType().name == "s64" && cast.TargetType().pointer_depth >= 1) {
          return true;
        }
      }
      return ExpressionContainsS64PointerDeref(un.Operand());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return ExpressionContainsS64PointerDeref(bin.Lhs()) ||
             ExpressionContainsS64PointerDeref(bin.Rhs());
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return ExpressionContainsS64PointerDeref(assign.Lhs()) ||
             ExpressionContainsS64PointerDeref(assign.Rhs());
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (ExpressionContainsS64PointerDeref(call.Callee())) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExpressionContainsS64PointerDeref(*arg)) return true;
      }
      return false;
    }
    case CExpressionKind::kCastExpression:
      return ExpressionContainsS64PointerDeref(static_cast<const CastExpression&>(expr).Operand());
    case CExpressionKind::kMemberAccessExpression:
      return ExpressionContainsS64PointerDeref(
          static_cast<const MemberAccessExpression&>(expr).Object());
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return ExpressionContainsS64PointerDeref(arr.Array()) ||
             ExpressionContainsS64PointerDeref(arr.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return ExpressionContainsS64PointerDeref(tern.Condition()) ||
             ExpressionContainsS64PointerDeref(tern.TrueExpression()) ||
             ExpressionContainsS64PointerDeref(tern.FalseExpression());
    }
    default:
      return false;
  }
}

bool StatementContainsS64PointerDeref(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsS64PointerDeref(*child)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement:
      return ExpressionContainsS64PointerDeref(
          static_cast<const ExpressionStatement&>(stmt).Expression());
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return vd.Initializer() != nullptr && ExpressionContainsS64PointerDeref(*vd.Initializer());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExpressionContainsS64PointerDeref(ifs.Condition()) ||
             StatementContainsS64PointerDeref(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StatementContainsS64PointerDeref(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExpressionContainsS64PointerDeref(ws.Condition()) ||
             StatementContainsS64PointerDeref(ws.Body());
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StatementContainsS64PointerDeref(dws.Body()) ||
             ExpressionContainsS64PointerDeref(dws.Condition());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StatementContainsS64PointerDeref(*fs.Init())) ||
             (fs.Condition() != nullptr && ExpressionContainsS64PointerDeref(*fs.Condition())) ||
             (fs.Step() != nullptr && ExpressionContainsS64PointerDeref(*fs.Step())) ||
             StatementContainsS64PointerDeref(fs.Body());
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr && ExpressionContainsS64PointerDeref(*rs.ReturnValue());
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExpressionContainsS64PointerDeref(ss.Condition())) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StatementContainsS64PointerDeref(*c.body)) return true;
      }
      return false;
    }
    default:
      return false;
  }
}

bool ExpressionContainsMaskedSignBitFloatFix(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      if (bin.Op() == "&" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t mask = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        if ((mask == 255 || mask == 65535) &&
            bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& inner = static_cast<const BinaryExpression&>(bin.Lhs());
          if ((inner.Op() == "|" || inner.Op() == "^") &&
              inner.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
              static_cast<uint32_t>(
                  static_cast<const IntegerLiteralExpression&>(inner.Rhs()).Value()) ==
                  0x80000000u &&
              inner.Lhs().Kind() == CExpressionKind::kCastExpression) {
            return true;
          }
        }
      }
      return ExpressionContainsMaskedSignBitFloatFix(bin.Lhs()) ||
             ExpressionContainsMaskedSignBitFloatFix(bin.Rhs());
    }
    case CExpressionKind::kUnaryExpression:
      return ExpressionContainsMaskedSignBitFloatFix(
          static_cast<const UnaryExpression&>(expr).Operand());
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return ExpressionContainsMaskedSignBitFloatFix(assign.Lhs()) ||
             ExpressionContainsMaskedSignBitFloatFix(assign.Rhs());
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (ExpressionContainsMaskedSignBitFloatFix(call.Callee())) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExpressionContainsMaskedSignBitFloatFix(*arg)) return true;
      }
      return false;
    }
    case CExpressionKind::kCastExpression:
      return ExpressionContainsMaskedSignBitFloatFix(
          static_cast<const CastExpression&>(expr).Operand());
    case CExpressionKind::kMemberAccessExpression:
      return ExpressionContainsMaskedSignBitFloatFix(
          static_cast<const MemberAccessExpression&>(expr).Object());
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return ExpressionContainsMaskedSignBitFloatFix(arr.Array()) ||
             ExpressionContainsMaskedSignBitFloatFix(arr.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return ExpressionContainsMaskedSignBitFloatFix(tern.Condition()) ||
             ExpressionContainsMaskedSignBitFloatFix(tern.TrueExpression()) ||
             ExpressionContainsMaskedSignBitFloatFix(tern.FalseExpression());
    }
    default:
      return false;
  }
}

bool StatementContainsMaskedSignBitFloatFix(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsMaskedSignBitFloatFix(*child)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement:
      return ExpressionContainsMaskedSignBitFloatFix(
          static_cast<const ExpressionStatement&>(stmt).Expression());
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return vd.Initializer() != nullptr &&
             ExpressionContainsMaskedSignBitFloatFix(*vd.Initializer());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExpressionContainsMaskedSignBitFloatFix(ifs.Condition()) ||
             StatementContainsMaskedSignBitFloatFix(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr &&
              StatementContainsMaskedSignBitFloatFix(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExpressionContainsMaskedSignBitFloatFix(ws.Condition()) ||
             StatementContainsMaskedSignBitFloatFix(ws.Body());
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StatementContainsMaskedSignBitFloatFix(dws.Body()) ||
             ExpressionContainsMaskedSignBitFloatFix(dws.Condition());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StatementContainsMaskedSignBitFloatFix(*fs.Init())) ||
             (fs.Condition() != nullptr &&
              ExpressionContainsMaskedSignBitFloatFix(*fs.Condition())) ||
             (fs.Step() != nullptr && ExpressionContainsMaskedSignBitFloatFix(*fs.Step())) ||
             StatementContainsMaskedSignBitFloatFix(fs.Body());
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr &&
             ExpressionContainsMaskedSignBitFloatFix(*rs.ReturnValue());
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExpressionContainsMaskedSignBitFloatFix(ss.Condition())) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StatementContainsMaskedSignBitFloatFix(*c.body)) return true;
      }
      return false;
    }
    default:
      return false;
  }
}

uint32_t CountForStatements(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      uint32_t count = 0;
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          count += CountForStatements(*child);
        }
      }
      return count;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CountForStatements(ifs.ThenBranch()) +
             (ifs.ElseBranch() != nullptr ? CountForStatements(*ifs.ElseBranch()) : 0u);
    }
    case CStatementKind::kWhileStatement:
      return CountForStatements(static_cast<const WhileStatement&>(stmt).Body());
    case CStatementKind::kDoWhileStatement:
      return CountForStatements(static_cast<const DoWhileStatement&>(stmt).Body());
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return 1u + CountForStatements(fs.Body());
    }
    case CStatementKind::kSwitchStatement: {
      uint32_t count = 0;
      for (const auto& c : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (c.body != nullptr) {
          count += CountForStatements(*c.body);
        }
      }
      return count;
    }
    default:
      return 0u;
  }
}

int CountDirectAssignmentsInExpression(const CExpression& expr, std::string_view target_var) {
  switch (expr.Kind()) {
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      int self_assign =
          (assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
           static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == target_var)
              ? 1
              : 0;
      return self_assign + CountDirectAssignmentsInExpression(assign.Lhs(), target_var) +
             CountDirectAssignmentsInExpression(assign.Rhs(), target_var);
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      int self_inc = ((un.Op() == "++" || un.Op() == "--") &&
                      un.Operand().Kind() == CExpressionKind::kIdentifier &&
                      static_cast<const IdentifierExpression&>(un.Operand()).Name() == target_var)
                         ? 1
                         : 0;
      return self_inc + CountDirectAssignmentsInExpression(un.Operand(), target_var);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return CountDirectAssignmentsInExpression(bin.Lhs(), target_var) +
             CountDirectAssignmentsInExpression(bin.Rhs(), target_var);
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      int total = CountDirectAssignmentsInExpression(call.Callee(), target_var);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr) {
          total += CountDirectAssignmentsInExpression(*arg, target_var);
        }
      }
      return total;
    }
    case CExpressionKind::kCastExpression:
      return CountDirectAssignmentsInExpression(static_cast<const CastExpression&>(expr).Operand(),
                                                target_var);
    case CExpressionKind::kMemberAccessExpression:
      return CountDirectAssignmentsInExpression(
          static_cast<const MemberAccessExpression&>(expr).Object(), target_var);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CountDirectAssignmentsInExpression(arr.Array(), target_var) +
             CountDirectAssignmentsInExpression(arr.Index(), target_var);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return CountDirectAssignmentsInExpression(tern.Condition(), target_var) +
             CountDirectAssignmentsInExpression(tern.TrueExpression(), target_var) +
             CountDirectAssignmentsInExpression(tern.FalseExpression(), target_var);
    }
    default:
      return 0;
  }
}

int CountDirectAssignmentsInStatement(const CStatement& stmt, std::string_view target_var) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      int total = 0;
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          total += CountDirectAssignmentsInStatement(*child, target_var);
        }
      }
      return total;
    }
    case CStatementKind::kExpressionStatement:
      return CountDirectAssignmentsInExpression(
          static_cast<const ExpressionStatement&>(stmt).Expression(), target_var);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return vd.Initializer() != nullptr
                 ? CountDirectAssignmentsInExpression(*vd.Initializer(), target_var)
                 : 0;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      int total = CountDirectAssignmentsInExpression(ifs.Condition(), target_var) +
                  CountDirectAssignmentsInStatement(ifs.ThenBranch(), target_var);
      if (ifs.ElseBranch() != nullptr) {
        total += CountDirectAssignmentsInStatement(*ifs.ElseBranch(), target_var);
      }
      return total;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CountDirectAssignmentsInExpression(ws.Condition(), target_var) +
             CountDirectAssignmentsInStatement(ws.Body(), target_var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CountDirectAssignmentsInStatement(dws.Body(), target_var) +
             CountDirectAssignmentsInExpression(dws.Condition(), target_var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      int total = CountDirectAssignmentsInStatement(fs.Body(), target_var);
      if (fs.Init() != nullptr) {
        total += CountDirectAssignmentsInStatement(*fs.Init(), target_var);
      }
      if (fs.Condition() != nullptr) {
        total += CountDirectAssignmentsInExpression(*fs.Condition(), target_var);
      }
      if (fs.Step() != nullptr) {
        total += CountDirectAssignmentsInExpression(*fs.Step(), target_var);
      }
      return total;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr
                 ? CountDirectAssignmentsInExpression(*rs.ReturnValue(), target_var)
                 : 0;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      int total = CountDirectAssignmentsInExpression(ss.Condition(), target_var);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) {
          total += CountDirectAssignmentsInStatement(*c.body, target_var);
        }
      }
      return total;
    }
    default:
      return 0;
  }
}

int CountNonReturnVariableReadUsesInStatement(const CStatement& stmt, std::string_view var) {
  switch (stmt.Kind()) {
    case CStatementKind::kReturnStatement:
      return 0;
    case CStatementKind::kCompoundStatement: {
      int total = 0;
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          total += CountNonReturnVariableReadUsesInStatement(*child, var);
        }
      }
      return total;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      int total = CountVariableReadUsesInExpression(ifs.Condition(), var) +
                  CountNonReturnVariableReadUsesInStatement(ifs.ThenBranch(), var);
      if (ifs.ElseBranch() != nullptr) {
        total += CountNonReturnVariableReadUsesInStatement(*ifs.ElseBranch(), var);
      }
      return total;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CountVariableReadUsesInExpression(ws.Condition(), var) +
             CountNonReturnVariableReadUsesInStatement(ws.Body(), var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CountNonReturnVariableReadUsesInStatement(dws.Body(), var) +
             CountVariableReadUsesInExpression(dws.Condition(), var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      int total = CountNonReturnVariableReadUsesInStatement(fs.Body(), var);
      if (fs.Init() != nullptr) {
        total += CountNonReturnVariableReadUsesInStatement(*fs.Init(), var);
      }
      if (fs.Condition() != nullptr) {
        total += CountVariableReadUsesInExpression(*fs.Condition(), var);
      }
      if (fs.Step() != nullptr) {
        total += CountVariableReadUsesInExpression(*fs.Step(), var);
      }
      return total;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      int total = CountVariableReadUsesInExpression(ss.Condition(), var);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) {
          total += CountNonReturnVariableReadUsesInStatement(*c.body, var);
        }
      }
      return total;
    }
    default:
      return CountVariableReadUsesInStatement(stmt, var);
  }
}

// Returns true if a split argument scratch variable (`temp_a0`..`temp_a3`) is modified before a
// subsequent read of the corresponding incoming parameter (`arg0`..`arg3`), meaning their live
// ranges overlap on the same physical argument register (`$4`..`$7`).
bool DoesScratchArgInterfereWithEntryParam(const CompoundStatement& body,
                                           std::string_view temp_a_var) {
  if (temp_a_var.size() != 7 || !absl::StartsWith(temp_a_var, "temp_a") || temp_a_var[6] < '0' ||
      temp_a_var[6] > '3') {
    return false;
  }
  const std::string param_var = absl::StrCat("arg", temp_a_var.substr(6, 1));
  bool temp_modified = false;
  auto check_stmt = [&](auto& self, const CStatement& s) -> bool {
    switch (s.Kind()) {
      case CStatementKind::kCompoundStatement:
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          if (child != nullptr && self(self, *child)) return true;
        }
        return false;
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        if (temp_modified && CountVariableReadUsesInExpression(ifs.Condition(), param_var) > 0) {
          return true;
        }
        if (ExpressionModifiesVariable(ifs.Condition(), temp_a_var)) {
          temp_modified = true;
        }
        bool saved = temp_modified;
        bool then_mod = saved;
        temp_modified = saved;
        if (self(self, ifs.ThenBranch())) return true;
        then_mod = temp_modified;
        bool else_mod = saved;
        if (ifs.ElseBranch() != nullptr) {
          temp_modified = saved;
          if (self(self, *ifs.ElseBranch())) return true;
          else_mod = temp_modified;
        }
        temp_modified = then_mod || else_mod;
        return false;
      }
      case CStatementKind::kWhileStatement:
      case CStatementKind::kDoWhileStatement:
      case CStatementKind::kForStatement: {
        if (StatementModifiesVariable(s, temp_a_var) &&
            CountVariableReadUsesInStatement(s, param_var) > 0) {
          return true;
        }
        if (temp_modified && CountVariableReadUsesInStatement(s, param_var) > 0) {
          return true;
        }
        if (StatementModifiesVariable(s, temp_a_var)) {
          temp_modified = true;
        }
        return false;
      }
      default: {
        if (temp_modified && CountVariableReadUsesInStatement(s, param_var) > 0) {
          return true;
        }
        if (StatementModifiesVariable(s, temp_a_var)) {
          temp_modified = true;
        }
        return false;
      }
    }
  };
  return check_stmt(check_stmt, body);
}

const CallExpression* UnwrapCallOrPureUnaryOnCall(const CExpression& expr) {
  const CExpression* cur = &expr;
  while (true) {
    if (cur->Kind() == CExpressionKind::kCastExpression) {
      cur = &static_cast<const CastExpression*>(cur)->Operand();
      continue;
    }
    if (cur->Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(*cur);
      if (un.Op() == "!" || un.Op() == "~" || un.Op() == "-") {
        cur = &un.Operand();
        continue;
      }
    }
    if (cur->Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(*cur);
      if (!ExpressionHasCall(bin.Rhs())) {
        cur = &bin.Lhs();
        continue;
      }
      if (!ExpressionHasCall(bin.Lhs())) {
        cur = &bin.Rhs();
        continue;
      }
    }
    break;
  }
  if (cur->Kind() == CExpressionKind::kCallExpression) {
    return static_cast<const CallExpression*>(cur);
  }
  return nullptr;
}

bool CheckArgRegInCall(std::string_view r, const CExpression& expr) {
  if (!ExpressionUsesVariable(expr, r)) return true;
  const CallExpression* call = UnwrapCallOrPureUnaryOnCall(expr);
  if (call == nullptr) return false;
  auto expr_uses_var_outside_call = [&](auto& self, const CExpression& e) -> bool {
    if (&e == call) return false;
    switch (e.Kind()) {
      case CExpressionKind::kIdentifier:
        return static_cast<const IdentifierExpression&>(e).Name() == r;
      case CExpressionKind::kIntegerLiteral:
      case CExpressionKind::kFloatLiteral:
      case CExpressionKind::kStringLiteral:
        return false;
      case CExpressionKind::kUnaryExpression:
        return self(self, static_cast<const UnaryExpression&>(e).Operand());
      case CExpressionKind::kCastExpression:
        return self(self, static_cast<const CastExpression&>(e).Operand());
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(e);
        return self(self, b.Lhs()) || self(self, b.Rhs());
      }
      default:
        return true;
    }
  };
  if (expr_uses_var_outside_call(expr_uses_var_outside_call, expr)) return false;
  for (const auto& arg : call->Arguments()) {
    if (arg != nullptr && ExpressionHasCall(*arg)) {
      return false;
    }
  }
  bool is_gpr_arg = (r == "arg0" || r == "arg1" || r == "arg2" || r == "arg3" || r == "temp_a0" ||
                     r == "temp_a1" || r == "temp_a2" || r == "temp_a3");
  bool is_fpr_arg = (r == "f12" || r == "f12_d" || r == "f14" || r == "f14_d");
  if (!is_gpr_arg && !is_fpr_arg) {
    return !(ExpressionUsesVariable(call->Callee(), r) && r != "temp_t9");
  }
  if (ExpressionUsesVariable(call->Callee(), r)) return false;
  size_t slot =
      is_gpr_arg ? static_cast<size_t>(r.back() - '0') : ((r == "f12" || r == "f12_d") ? 0 : 1);
  if (is_gpr_arg) {
    for (size_t i = 0; i < call->Arguments().size(); ++i) {
      if (call->Arguments()[i] == nullptr || !ExpressionUsesVariable(*call->Arguments()[i], r)) {
        continue;
      }
      if (i > slot && slot < call->Arguments().size()) {
        if (call->Arguments()[slot] == nullptr ||
            call->Arguments()[slot]->Kind() != CExpressionKind::kIdentifier ||
            static_cast<const IdentifierExpression&>(*call->Arguments()[slot]).Name() != r) {
          return false;
        }
      }
    }
    return true;
  }
  if (slot >= call->Arguments().size() || call->Arguments()[slot] == nullptr) return false;
  if (call->Arguments()[slot]->Kind() != CExpressionKind::kIdentifier ||
      static_cast<const IdentifierExpression&>(*call->Arguments()[slot]).Name() != r) {
    return false;
  }
  return true;
}

}  // namespace

bool IsCallerSavedSafeAcrossCalls(const CompoundStatement& body, bool is_leaf_func,
                                  std::string_view r) {
  if (is_leaf_func) return true;
  bool clobbered = false;
  bool break_clobbered = false;
  bool continue_clobbered = false;
  absl::flat_hash_map<std::string, bool> label_clobbered;
  auto check_expr = [&](const CExpression& e) -> bool {
    if (e.Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& a = static_cast<const AssignmentExpression&>(e);
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
        std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
        const CExpression& rhs = a.Rhs();
        bool rhs_has_call = ExpressionHasCall(rhs);
        bool rhs_uses_r = ExpressionUsesVariable(rhs, r);
        if (rhs_uses_r && clobbered) return false;
        if (rhs_has_call && rhs_uses_r) {
          if (!CheckArgRegInCall(r, rhs)) return false;
        }
        if (lhs_name == r) {
          if (!rhs_has_call || UnwrapCallOrPureUnaryOnCall(rhs) != nullptr) {
            clobbered = false;
            return true;
          }
          return false;
        }
        if (rhs_has_call) {
          clobbered = true;
        }
        return true;
      }
      if (a.Op() == "=" && !ExpressionHasCall(a.Lhs()) && !ExpressionUsesVariable(a.Lhs(), r)) {
        const CExpression& rhs = a.Rhs();
        bool rhs_has_call = ExpressionHasCall(rhs);
        bool rhs_uses_r = ExpressionUsesVariable(rhs, r);
        if (rhs_uses_r && clobbered) return false;
        if (rhs_has_call && rhs_uses_r) {
          if (!CheckArgRegInCall(r, rhs)) return false;
        }
        if (rhs_has_call) {
          clobbered = true;
        }
        return true;
      }
    }
    bool has_c = ExpressionHasCall(e);
    bool uses_r = ExpressionUsesVariable(e, r);
    if (uses_r && clobbered) return false;
    if (has_c && uses_r) {
      if (!CheckArgRegInCall(r, e)) return false;
    }
    if (has_c) {
      clobbered = true;
    }
    return true;
  };

  auto check_stmt = [&](auto& self, const CStatement& s) -> bool {
    switch (s.Kind()) {
      case CStatementKind::kCompoundStatement:
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          if (child != nullptr && !self(self, *child)) return false;
        }
        return true;
      case CStatementKind::kExpressionStatement:
        return check_expr(static_cast<const ExpressionStatement&>(s).Expression());
      case CStatementKind::kVariableDeclarationStatement: {
        const auto& vd = static_cast<const VariableDeclarationStatement&>(s);
        if (vd.Initializer() != nullptr) {
          return check_expr(*vd.Initializer());
        }
        return true;
      }
      case CStatementKind::kReturnStatement: {
        const auto& rs = static_cast<const ReturnStatement&>(s);
        if (rs.ReturnValue() != nullptr && !check_expr(*rs.ReturnValue())) {
          return false;
        }
        clobbered = false;
        return true;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        if (!check_expr(ifs.Condition())) return false;
        bool before = clobbered;
        bool then_clobbered = before;
        clobbered = before;
        if (!self(self, ifs.ThenBranch())) return false;
        then_clobbered = clobbered;
        bool else_clobbered = before;
        if (ifs.ElseBranch() != nullptr) {
          clobbered = before;
          if (!self(self, *ifs.ElseBranch())) return false;
          else_clobbered = clobbered;
        }
        clobbered = then_clobbered || else_clobbered;
        return true;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(s);
        bool saved_break = break_clobbered;
        bool saved_continue = continue_clobbered;
        break_clobbered = false;
        for (int pass = 0; pass < 2; ++pass) {
          continue_clobbered = false;
          if (!check_expr(ws.Condition())) return false;
          bool cond_clobbered = clobbered;
          if (!self(self, ws.Body())) return false;
          clobbered = clobbered || continue_clobbered || cond_clobbered;
        }
        clobbered = clobbered || break_clobbered;
        break_clobbered = saved_break;
        continue_clobbered = saved_continue;
        return true;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(s);
        bool saved_break = break_clobbered;
        bool saved_continue = continue_clobbered;
        break_clobbered = false;
        for (int pass = 0; pass < 2; ++pass) {
          continue_clobbered = false;
          if (!self(self, dws.Body())) return false;
          clobbered = clobbered || continue_clobbered;
          if (!check_expr(dws.Condition())) return false;
        }
        clobbered = clobbered || break_clobbered;
        break_clobbered = saved_break;
        continue_clobbered = saved_continue;
        return true;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr && !self(self, *fs.Init())) return false;
        bool saved_break = break_clobbered;
        bool saved_continue = continue_clobbered;
        break_clobbered = false;
        for (int pass = 0; pass < 2; ++pass) {
          continue_clobbered = false;
          if (fs.Condition() != nullptr && !check_expr(*fs.Condition())) return false;
          bool cond_clobbered = clobbered;
          if (!self(self, fs.Body())) return false;
          clobbered = clobbered || continue_clobbered;
          if (fs.Step() != nullptr && !check_expr(*fs.Step())) return false;
          clobbered = clobbered || cond_clobbered;
        }
        clobbered = clobbered || break_clobbered;
        break_clobbered = saved_break;
        continue_clobbered = saved_continue;
        return true;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        if (!check_expr(ss.Condition())) return false;
        bool before = clobbered;
        bool has_default = false;
        for (const auto& c : ss.Cases()) {
          if (c.is_default) {
            has_default = true;
            break;
          }
        }
        bool any_clobbered = has_default ? false : before;
        bool saved_break = break_clobbered;
        break_clobbered = false;
        for (const auto& c : ss.Cases()) {
          clobbered = before;
          if (c.body != nullptr && !self(self, *c.body)) return false;
          any_clobbered = any_clobbered || clobbered;
        }
        clobbered = any_clobbered || break_clobbered;
        break_clobbered = saved_break;
        return true;
      }
      case CStatementKind::kBreakStatement:
        break_clobbered = break_clobbered || clobbered;
        clobbered = false;
        return true;
      case CStatementKind::kContinueStatement:
        continue_clobbered = continue_clobbered || clobbered;
        clobbered = false;
        return true;
      case CStatementKind::kGotoStatement: {
        const auto& gs = static_cast<const GotoStatement&>(s);
        if (clobbered) {
          label_clobbered[gs.Label()] = true;
        }
        clobbered = false;
        return true;
      }
      case CStatementKind::kLabelStatement: {
        const auto& ls = static_cast<const LabelStatement&>(s);
        clobbered = clobbered || label_clobbered[ls.Label()];
        return true;
      }
      default:
        return true;
    }
  };

  for (int pass = 0; pass < 2; ++pass) {
    clobbered = false;
    break_clobbered = false;
    continue_clobbered = false;
    if (!check_stmt(check_stmt, body)) return false;
  }
  return true;
}

bool IsNarrowPointerStoreLhs(const CExpression& lhs, int64_t* out_mask) {
  if (lhs.Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& un = static_cast<const UnaryExpression&>(lhs);
  if (!un.IsPrefix() || un.Op() != "*") return false;
  if (un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
  const auto& cast = static_cast<const CastExpression&>(un.Operand());
  if (cast.TargetType().pointer_depth != 1) return false;
  if (cast.TargetType().name == "u8" || cast.TargetType().name == "s8") {
    *out_mask = 255;
    return true;
  }
  if (cast.TargetType().name == "u16" || cast.TargetType().name == "s16") {
    *out_mask = 65535;
    return true;
  }
  return false;
}

bool IsIdentifierStepByConst(const CExpression& rhs, std::string_view expected_var,
                             bool require_unit_step) {
  if (rhs.Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& bin = static_cast<const BinaryExpression&>(rhs);
  if (bin.Op() != "+" && bin.Op() != "-") return false;
  if (bin.Lhs().Kind() != CExpressionKind::kIdentifier ||
      bin.Rhs().Kind() != CExpressionKind::kIntegerLiteral) {
    return false;
  }
  const auto& id = static_cast<const IdentifierExpression&>(bin.Lhs());
  if (!expected_var.empty() && id.Name() != expected_var) {
    return false;
  }
  const int64_t step = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
  return require_unit_step ? (step == 1 || step == -1) : true;
}

void CollectConditionallySteppedVars(
    const CStatement& stmt, absl::flat_hash_set<std::string>* out_vars,
    absl::flat_hash_set<std::string>* out_do_while_post_stepped = nullptr) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          CollectConditionallySteppedVars(*child, out_vars, out_do_while_post_stepped);
        }
      }
      break;
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      const CStatement* single_then = &ifs.ThenBranch();
      if (single_then->Kind() == CStatementKind::kCompoundStatement) {
        const auto& comp = static_cast<const CompoundStatement&>(*single_then);
        single_then = (comp.Statements().size() == 1) ? comp.Statements()[0].get() : nullptr;
      }
      if (single_then != nullptr && single_then->Kind() == CStatementKind::kExpressionStatement) {
        const auto& expr = static_cast<const ExpressionStatement&>(*single_then).Expression();
        if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& assign = static_cast<const AssignmentExpression&>(expr);
          if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
              IsIdentifierStepByConst(assign.Rhs(), /*expected_var=*/"",
                                      /*require_unit_step=*/true)) {
            const std::string& lhs_name =
                static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
            if (ifs.ElseBranch() == nullptr ||
                !StatementModifiesVariable(*ifs.ElseBranch(), lhs_name)) {
              out_vars->insert(lhs_name);
            }
          }
        }
      }
      CollectConditionallySteppedVars(ifs.ThenBranch(), out_vars, out_do_while_post_stepped);
      if (ifs.ElseBranch() != nullptr) {
        CollectConditionallySteppedVars(*ifs.ElseBranch(), out_vars, out_do_while_post_stepped);
      }
      break;
    }
    case CStatementKind::kWhileStatement:
      CollectConditionallySteppedVars(static_cast<const WhileStatement&>(stmt).Body(), out_vars,
                                      out_do_while_post_stepped);
      break;
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      if (out_do_while_post_stepped != nullptr &&
          dws.Body().Kind() == CStatementKind::kCompoundStatement && StatementHasCall(dws.Body())) {
        const auto& comp = static_cast<const CompoundStatement&>(dws.Body());
        if (comp.Statements().size() >= 2) {
          const auto* prev_s = comp.Statements()[comp.Statements().size() - 2].get();
          const auto* last_s = comp.Statements().back().get();
          if (prev_s != nullptr && last_s != nullptr &&
              prev_s->Kind() == CStatementKind::kExpressionStatement &&
              last_s->Kind() == CStatementKind::kExpressionStatement) {
            const auto& prev_e = static_cast<const ExpressionStatement&>(*prev_s).Expression();
            const auto& last_e = static_cast<const ExpressionStatement&>(*last_s).Expression();
            if (prev_e.Kind() == CExpressionKind::kAssignmentExpression &&
                last_e.Kind() == CExpressionKind::kAssignmentExpression) {
              const auto& prev_a = static_cast<const AssignmentExpression&>(prev_e);
              const auto& last_a = static_cast<const AssignmentExpression&>(last_e);
              if (prev_a.Op() == "=" && last_a.Op() == "=" &&
                  prev_a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                  last_a.Lhs().Kind() == CExpressionKind::kIdentifier) {
                const std::string& step_var =
                    static_cast<const IdentifierExpression&>(last_a.Lhs()).Name();
                if (IsIdentifierStepByConst(last_a.Rhs(), step_var, /*require_unit_step=*/true) &&
                    prev_a.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                  const auto& mask_bin = static_cast<const BinaryExpression&>(prev_a.Rhs());
                  if (mask_bin.Op() == "&" &&
                      mask_bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      static_cast<const IdentifierExpression&>(mask_bin.Lhs()).Name() == step_var &&
                      mask_bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
                    int64_t m =
                        static_cast<const IntegerLiteralExpression&>(mask_bin.Rhs()).Value();
                    if (m == 255 || m == 65535) {
                      out_vars->insert(step_var);
                      out_do_while_post_stepped->insert(step_var);
                    }
                  }
                }
              }
            }
          }
        }
      }
      CollectConditionallySteppedVars(dws.Body(), out_vars, out_do_while_post_stepped);
      break;
    }
    case CStatementKind::kForStatement:
      CollectConditionallySteppedVars(static_cast<const ForStatement&>(stmt).Body(), out_vars,
                                      out_do_while_post_stepped);
      break;
    case CStatementKind::kSwitchStatement:
      for (const auto& c : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (c.body != nullptr) {
          CollectConditionallySteppedVars(*c.body, out_vars, out_do_while_post_stepped);
        }
      }
      break;
    default:
      break;
  }
}

void CheckNarrowMaskedReadsInExpr(const CExpression& expr, std::string_view var_name,
                                  int* mask_255_count, int* mask_65535_count,
                                  bool* has_unmasked_read) {
  if (*has_unmasked_read) return;
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier:
      if (static_cast<const IdentifierExpression&>(expr).Name() == var_name) {
        *has_unmasked_read = true;
      }
      return;
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      if (bin.Op() == "&" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t mask = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        if ((mask == 255 || mask == 65535) && bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(bin.Lhs()).Name() == var_name) {
          if (mask == 255) {
            ++(*mask_255_count);
          } else {
            ++(*mask_65535_count);
          }
          return;
        }
        if ((mask == 255 || mask == 65535) &&
            bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& inner = static_cast<const BinaryExpression&>(bin.Lhs());
          if (inner.Op() == "+" || inner.Op() == "-") {
            bool lhs_is_var =
                inner.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(inner.Lhs()).Name() == var_name;
            bool rhs_is_var =
                inner.Rhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(inner.Rhs()).Name() == var_name;
            if (lhs_is_var || rhs_is_var) {
              if (mask == 255) {
                ++(*mask_255_count);
              } else {
                ++(*mask_65535_count);
              }
              if (!lhs_is_var) {
                CheckNarrowMaskedReadsInExpr(inner.Lhs(), var_name, mask_255_count,
                                             mask_65535_count, has_unmasked_read);
              }
              if (!rhs_is_var) {
                CheckNarrowMaskedReadsInExpr(inner.Rhs(), var_name, mask_255_count,
                                             mask_65535_count, has_unmasked_read);
              }
              return;
            }
          }
        }
      }
      CheckNarrowMaskedReadsInExpr(bin.Lhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(bin.Rhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == var_name) {
        if (IsIdentifierStepByConst(assign.Rhs(), var_name, /*require_unit_step=*/false)) {
          return;
        }
        CheckNarrowMaskedReadsInExpr(assign.Rhs(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
        return;
      }
      int64_t store_mask = 0;
      if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assign.Rhs()).Name() == var_name &&
          IsNarrowPointerStoreLhs(assign.Lhs(), &store_mask)) {
        CheckNarrowMaskedReadsInExpr(assign.Lhs(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
        return;
      }
      CheckNarrowMaskedReadsInExpr(assign.Lhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(assign.Rhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CExpressionKind::kUnaryExpression:
      CheckNarrowMaskedReadsInExpr(static_cast<const UnaryExpression&>(expr).Operand(), var_name,
                                   mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CExpressionKind::kCastExpression:
      CheckNarrowMaskedReadsInExpr(static_cast<const CastExpression&>(expr).Operand(), var_name,
                                   mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      CheckNarrowMaskedReadsInExpr(call.Callee(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr) {
          CheckNarrowMaskedReadsInExpr(*arg, var_name, mask_255_count, mask_65535_count,
                                       has_unmasked_read);
        }
      }
      return;
    }
    case CExpressionKind::kMemberAccessExpression:
      CheckNarrowMaskedReadsInExpr(static_cast<const MemberAccessExpression&>(expr).Object(),
                                   var_name, mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      CheckNarrowMaskedReadsInExpr(arr.Array(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(arr.Index(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      CheckNarrowMaskedReadsInExpr(tern.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(tern.TrueExpression(), var_name, mask_255_count,
                                   mask_65535_count, has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(tern.FalseExpression(), var_name, mask_255_count,
                                   mask_65535_count, has_unmasked_read);
      return;
    }
    default:
      return;
  }
}

void CheckNarrowMaskedReadsInStmt(const CStatement& stmt, std::string_view var_name,
                                  int* mask_255_count, int* mask_65535_count,
                                  bool* has_unmasked_read) {
  if (*has_unmasked_read) return;
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          CheckNarrowMaskedReadsInStmt(*child, var_name, mask_255_count, mask_65535_count,
                                       has_unmasked_read);
        }
      }
      return;
    case CStatementKind::kExpressionStatement:
      CheckNarrowMaskedReadsInExpr(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                   var_name, mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      if (vd.Initializer() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*vd.Initializer(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      return;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      CheckNarrowMaskedReadsInExpr(ifs.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInStmt(ifs.ThenBranch(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      if (ifs.ElseBranch() != nullptr) {
        CheckNarrowMaskedReadsInStmt(*ifs.ElseBranch(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      return;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      CheckNarrowMaskedReadsInExpr(ws.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInStmt(ws.Body(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      CheckNarrowMaskedReadsInStmt(dws.Body(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(dws.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr) {
        CheckNarrowMaskedReadsInStmt(*fs.Init(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      if (fs.Condition() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*fs.Condition(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      if (fs.Step() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*fs.Step(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      CheckNarrowMaskedReadsInStmt(fs.Body(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*rs.ReturnValue(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      return;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      CheckNarrowMaskedReadsInExpr(ss.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) {
          CheckNarrowMaskedReadsInStmt(*c.body, var_name, mask_255_count, mask_65535_count,
                                       has_unmasked_read);
        }
      }
      return;
    }
    default:
      return;
  }
}

std::unique_ptr<CExpression> RewriteNarrowCondIncExpr(
    const CExpression& expr, const absl::flat_hash_map<std::string, CType>& narrow_types,
    const absl::flat_hash_set<std::string>& do_while_post_stepped_vars, bool is_return_root) {
  if (is_return_root && expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& lt = static_cast<const BinaryExpression&>(expr);
    if (lt.Op() == "<" && lt.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
        static_cast<const IntegerLiteralExpression&>(lt.Rhs()).Value() == 1 &&
        lt.Lhs().Kind() == CExpressionKind::kCastExpression) {
      const auto& cast = static_cast<const CastExpression&>(lt.Lhs());
      if (cast.TargetType().name == "u32" && cast.TargetType().pointer_depth == 0 &&
          cast.Operand().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& xor_bin = static_cast<const BinaryExpression&>(cast.Operand());
        if (xor_bin.Op() == "^" && xor_bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
            xor_bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& mask_bin = static_cast<const BinaryExpression&>(xor_bin.Lhs());
          if (mask_bin.Op() == "&" && mask_bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
              narrow_types.contains(
                  static_cast<const IdentifierExpression&>(mask_bin.Lhs()).Name())) {
            return CExpression::Binary("==", xor_bin.Lhs().Clone(), xor_bin.Rhs().Clone());
          }
        }
      }
    }
  }
  switch (expr.Kind()) {
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      if (bin.Op() == "&" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
          bin.Lhs().Kind() == CExpressionKind::kIdentifier) {
        const std::string& var_name = static_cast<const IdentifierExpression&>(bin.Lhs()).Name();
        int64_t mask = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        auto it = narrow_types.find(var_name);
        if (it != narrow_types.end() && do_while_post_stepped_vars.contains(var_name) &&
            ((mask == 255 && it->second.name == "u8") ||
             (mask == 65535 && it->second.name == "u16"))) {
          return bin.Lhs().Clone();
        }
      }
      auto rewritten_lhs = RewriteNarrowCondIncExpr(
          bin.Lhs(), narrow_types, do_while_post_stepped_vars, /*is_return_root=*/false);
      auto rewritten_rhs = RewriteNarrowCondIncExpr(
          bin.Rhs(), narrow_types, do_while_post_stepped_vars, /*is_return_root=*/false);
      if (bin.Op() == "&" && rewritten_rhs->Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(*rewritten_rhs).Value() == 65535 &&
          rewritten_lhs->Kind() == CExpressionKind::kBinaryExpression) {
        const auto& inner = static_cast<const BinaryExpression&>(*rewritten_lhs);
        if (inner.Op() == "+" || inner.Op() == "-") {
          bool lhs_u16 = false;
          bool rhs_u16 = false;
          bool lhs_narrow_post_step = false;
          bool rhs_narrow_post_step = false;
          if (inner.Lhs().Kind() == CExpressionKind::kIdentifier) {
            const std::string& lhs_id =
                static_cast<const IdentifierExpression&>(inner.Lhs()).Name();
            auto it = narrow_types.find(lhs_id);
            lhs_u16 = (it != narrow_types.end() && it->second.name == "u16");
            lhs_narrow_post_step =
                (it != narrow_types.end() && do_while_post_stepped_vars.contains(lhs_id));
          }
          if (inner.Rhs().Kind() == CExpressionKind::kIdentifier) {
            const std::string& rhs_id =
                static_cast<const IdentifierExpression&>(inner.Rhs()).Name();
            auto it = narrow_types.find(rhs_id);
            rhs_u16 = (it != narrow_types.end() && it->second.name == "u16");
            rhs_narrow_post_step =
                (it != narrow_types.end() && do_while_post_stepped_vars.contains(rhs_id));
          }
          if (lhs_u16 || rhs_u16 || lhs_narrow_post_step || rhs_narrow_post_step) {
            auto new_lhs = lhs_u16 ? inner.Lhs().Clone()
                                   : CExpression::Cast(CType::U16(), inner.Lhs().Clone());
            auto new_rhs = rhs_u16 ? inner.Rhs().Clone()
                                   : CExpression::Cast(CType::U16(), inner.Rhs().Clone());
            return CExpression::Binary(
                "&",
                CExpression::Cast(CType::U16(), CExpression::Binary(inner.Op(), std::move(new_lhs),
                                                                    std::move(new_rhs))),
                std::move(rewritten_rhs));
          }
        }
      }
      return CExpression::Binary(bin.Op(), std::move(rewritten_lhs), std::move(rewritten_rhs));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return CExpression::Assignment(
          assign.Op(),
          RewriteNarrowCondIncExpr(assign.Lhs(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncExpr(assign.Rhs(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(
          un.Op(),
          RewriteNarrowCondIncExpr(un.Operand(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          un.IsPrefix());
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(
          cast.TargetType(),
          RewriteNarrowCondIncExpr(cast.Operand(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(call.Arguments().size());
      for (const auto& arg : call.Arguments()) {
        args.push_back(arg != nullptr ? RewriteNarrowCondIncExpr(*arg, narrow_types,
                                                                 do_while_post_stepped_vars,
                                                                 /*is_return_root=*/false)
                                      : nullptr);
      }
      return CExpression::Call(
          RewriteNarrowCondIncExpr(call.Callee(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          std::move(args));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          RewriteNarrowCondIncExpr(tern.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncExpr(tern.TrueExpression(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncExpr(tern.FalseExpression(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> RewriteNarrowCondIncStmt(
    const CStatement& stmt, const absl::flat_hash_map<std::string, CType>& narrow_types,
    const absl::flat_hash_set<std::string>& do_while_post_stepped_vars) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      auto out = std::make_unique<CompoundStatement>();
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          out->AddStatement(
              RewriteNarrowCondIncStmt(*child, narrow_types, do_while_post_stepped_vars));
        }
      }
      return out;
    }
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      return CStatement::Expression(RewriteNarrowCondIncExpr(
          es.Expression(), narrow_types, do_while_post_stepped_vars, /*is_return_root=*/false));
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(
          RewriteNarrowCondIncExpr(ifs.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncStmt(ifs.ThenBranch(), narrow_types, do_while_post_stepped_vars),
          ifs.ElseBranch() != nullptr ? RewriteNarrowCondIncStmt(*ifs.ElseBranch(), narrow_types,
                                                                 do_while_post_stepped_vars)
                                      : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(
          RewriteNarrowCondIncExpr(ws.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncStmt(ws.Body(), narrow_types, do_while_post_stepped_vars));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          RewriteNarrowCondIncStmt(dws.Body(), narrow_types, do_while_post_stepped_vars),
          RewriteNarrowCondIncExpr(dws.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          fs.Init() != nullptr
              ? RewriteNarrowCondIncStmt(*fs.Init(), narrow_types, do_while_post_stepped_vars)
              : nullptr,
          fs.Condition() != nullptr
              ? RewriteNarrowCondIncExpr(*fs.Condition(), narrow_types, do_while_post_stepped_vars,
                                         /*is_return_root=*/false)
              : nullptr,
          fs.Step() != nullptr
              ? RewriteNarrowCondIncExpr(*fs.Step(), narrow_types, do_while_post_stepped_vars,
                                         /*is_return_root=*/false)
              : nullptr,
          RewriteNarrowCondIncStmt(fs.Body(), narrow_types, do_while_post_stepped_vars));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return CStatement::Return(rs.ReturnValue() != nullptr
                                    ? RewriteNarrowCondIncExpr(*rs.ReturnValue(), narrow_types,
                                                               do_while_post_stepped_vars,
                                                               /*is_return_root=*/true)
                                    : nullptr);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      std::vector<SwitchCase> cases;
      cases.reserve(ss.Cases().size());
      for (const auto& c : ss.Cases()) {
        SwitchCase sc = c.Clone();
        if (c.body != nullptr) {
          auto rewritten =
              RewriteNarrowCondIncStmt(*c.body, narrow_types, do_while_post_stepped_vars);
          sc.body.reset(static_cast<CompoundStatement*>(rewritten.release()));
        }
        cases.push_back(std::move(sc));
      }
      return CStatement::Switch(
          RewriteNarrowCondIncExpr(ss.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          std::move(cases));
    }
    default:
      return stmt.Clone();
  }
}

absl::Status PrependSynthesizedLocalDeclarations(
    const ControlFlowGraph& cfg, const StackFrameAnalysis& frame_analysis,
    const absl::flat_hash_set<std::string>& double_vars, const std::vector<CParameter>& parameters,
    const CType& return_type, const SymbolIndex* symbol_index, const AstConverterOptions& options,
    std::unique_ptr<CompoundStatement>* body) {
  std::vector<std::string> raw_vars;
  absl::flat_hash_set<std::string> seen;
  for (const auto& param : parameters) {
    seen.insert(param.name);
  }
  CollectUsedVariables(**body, &raw_vars, &seen);

  auto is_valid_c_id = [](std::string_view name) -> bool {
    if (name.empty()) return false;
    if (!std::isalpha(name[0]) && name[0] != '_') return false;
    for (char c : name) {
      if (!std::isalnum(c) && c != '_') return false;
    }
    return true;
  };

  std::vector<std::string> local_vars_to_declare;
  for (const auto& var_name : raw_vars) {
    if (!is_valid_c_id(var_name)) continue;
    // Do not declare parameters
    bool is_param = false;
    for (const auto& param : parameters) {
      if (param.name == var_name) {
        is_param = true;
        break;
      }
    }
    if (is_param) {
      continue;
    }

    // Do not declare globals (prefixed with g_, D_, RO_, B_)
    if (absl::StartsWith(var_name, "g_") || absl::StartsWith(var_name, "D_") ||
        absl::StartsWith(var_name, "RO_") || absl::StartsWith(var_name, "B_")) {
      continue;
    }
    // Do not declare symbols present in symbol registry
    if (symbol_index != nullptr && symbol_index->FindByName(var_name) != nullptr) {
      continue;
    }
    // Do not declare standard C keywords, literals, or types
    if (var_name == "NULL" || var_name == "TRUE" || var_name == "FALSE" || var_name == "s32" ||
        var_name == "u32" || var_name == "s16" || var_name == "u16" || var_name == "s8" ||
        var_name == "u8" || var_name == "f32" || var_name == "f64" || var_name == "s64" ||
        var_name == "u64") {
      continue;
    }
    // Do not declare synthesized or auto function names (func_XXXXXXXX)
    uint32_t auto_addr = 0;
    if (var_name.size() == 13 && absl::StartsWith(var_name, "func_") &&
        absl::SimpleHexAtoi(var_name.substr(5), &auto_addr)) {
      continue;
    }
    // Do not declare known functions from pipeline options
    if (options.function_parameter_counts != nullptr &&
        options.function_parameter_counts->contains(var_name)) {
      continue;
    }
    if (options.function_fp_parameter_counts != nullptr &&
        options.function_fp_parameter_counts->contains(var_name)) {
      continue;
    }
    if (options.functions_returning_v0 != nullptr &&
        options.functions_returning_v0->contains(var_name)) {
      continue;
    }
    if (options.functions_returning_f0 != nullptr &&
        options.functions_returning_f0->contains(var_name)) {
      continue;
    }

    local_vars_to_declare.push_back(var_name);
  }

  {
    std::vector<size_t> sp_var_indices;
    std::vector<std::pair<int64_t, std::string>> sp_vars;
    for (size_t i = 0; i < local_vars_to_declare.size(); ++i) {
      if (absl::StartsWith(local_vars_to_declare[i], "var_sp_")) {
        int64_t off = 0;
        std::string_view sv = local_vars_to_declare[i];
        if (absl::SimpleAtoi(sv.substr(7), &off)) {
          sp_var_indices.push_back(i);
          sp_vars.emplace_back(off, local_vars_to_declare[i]);
        }
      }
    }
    if (sp_vars.size() >= 2) {
      std::sort(sp_vars.begin(), sp_vars.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      for (size_t i = 0; i < sp_var_indices.size(); ++i) {
        local_vars_to_declare[sp_var_indices[i]] = sp_vars[i].second;
      }
    }

    std::vector<size_t> callee_gpr_indices;
    std::vector<std::pair<std::pair<int, int32_t>, std::string>> callee_gpr_vars;
    for (size_t i = 0; i < local_vars_to_declare.size(); ++i) {
      const std::string& v = local_vars_to_declare[i];
      if (!IsCalleeSavedGprVarName(v)) {
        continue;
      }
      Register reg = (v == "fp")
                         ? Register::kFp
                         : static_cast<Register>(static_cast<int>(Register::kS0) + (v[6] - '0'));
      int32_t reg_order = (v == "fp") ? 30 : (16 + (v[6] - '0'));
      auto it = frame_analysis.prologue.saved_reg_offsets.find(reg);
      std::pair<int, int32_t> sort_key = (it != frame_analysis.prologue.saved_reg_offsets.end())
                                             ? std::make_pair(0, it->second)
                                             : std::make_pair(1, reg_order);
      callee_gpr_indices.push_back(i);
      callee_gpr_vars.emplace_back(sort_key, v);
    }
    if (callee_gpr_vars.size() >= 2) {
      std::stable_sort(callee_gpr_vars.begin(), callee_gpr_vars.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      for (size_t i = 0; i < callee_gpr_indices.size(); ++i) {
        local_vars_to_declare[callee_gpr_indices[i]] = callee_gpr_vars[i].second;
      }
    }

    std::vector<size_t> callee_fpr_indices;
    std::vector<std::pair<std::pair<int, int32_t>, std::string>> callee_fpr_vars;
    for (size_t i = 0; i < local_vars_to_declare.size(); ++i) {
      const std::string& v = local_vars_to_declare[i];
      std::string_view sv = v;
      std::string_view base = absl::EndsWith(sv, "_d") ? sv.substr(0, sv.size() - 2) : sv;
      if (!IsCalleeSavedFprBaseName(base)) {
        continue;
      }
      int fpr_num = 0;
      if (!absl::SimpleAtoi(base.substr(1), &fpr_num)) {
        continue;
      }
      FpRegister fp_reg =
          static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (fpr_num - 20));
      auto it = frame_analysis.prologue.saved_fpr_offsets.find(fp_reg);
      std::pair<int, int32_t> sort_key = (it != frame_analysis.prologue.saved_fpr_offsets.end())
                                             ? std::make_pair(0, it->second)
                                             : std::make_pair(1, fpr_num);
      callee_fpr_indices.push_back(i);
      callee_fpr_vars.emplace_back(sort_key, v);
    }
    if (callee_fpr_vars.size() >= 2) {
      std::stable_sort(callee_fpr_vars.begin(), callee_fpr_vars.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      for (size_t i = 0; i < callee_fpr_indices.size(); ++i) {
        local_vars_to_declare[callee_fpr_indices[i]] = callee_fpr_vars[i].second;
      }
    }
  }

  uint32_t unreferenced_sp_gap_size = 0;
  if (!frame_analysis.has_frame_pointer && frame_analysis.sp_buffer_size == 0) {
    uint32_t local_frame_top = 0;
    uint32_t local_frame_bottom = 0;
    if (frame_analysis.prologue.min_saved_reg_offset != 0xFFFFFFFF) {
      local_frame_top = frame_analysis.prologue.min_saved_reg_offset & ~7u;
      local_frame_bottom = (static_cast<uint32_t>(frame_analysis.outgoing_args_size) + 7u) & ~7u;
    } else if (!frame_analysis.has_calls && frame_analysis.frame_size > 0) {
      local_frame_top = static_cast<uint32_t>(frame_analysis.frame_size) & ~7u;
      local_frame_bottom = 0;
    }

    if (local_frame_top > local_frame_bottom) {
      const uint32_t target_local_size = local_frame_top - local_frame_bottom;
      bool has_sp_buffer = false;
      bool any_non_sp_local_addr_taken = false;
      size_t sp_locals_in_gap = 0;
      uint32_t addr_taken_sp_bytes = 0;
      int callee_saved_gprs = 0;

      for (const auto& v : local_vars_to_declare) {
        if (v == "sp") {
          has_sp_buffer = true;
          break;
        }
        if (IsCalleeSavedGprVarName(v)) {
          ++callee_saved_gprs;
        }
        const bool is_addr_taken = StatementTakesAddressOf(**body, v);
        if (absl::StartsWith(v, "var_sp_") && !absl::StartsWith(v, "var_sp_neg_")) {
          int32_t off = 0;
          if (absl::SimpleAtoi(v.substr(7), &off) &&
              off >= static_cast<int32_t>(local_frame_bottom) &&
              off < static_cast<int32_t>(local_frame_top)) {
            ++sp_locals_in_gap;
            if (is_addr_taken) {
              addr_taken_sp_bytes +=
                  (absl::EndsWith(v, "_64") || double_vars.contains(v)) ? 8u : 4u;
            }
          } else if (is_addr_taken) {
            any_non_sp_local_addr_taken = true;
          }
        } else if (is_addr_taken) {
          any_non_sp_local_addr_taken = true;
        }
      }

      if (!has_sp_buffer && !any_non_sp_local_addr_taken &&
          !StatementContainsS64PointerDeref(**body)) {
        const uint32_t for_loop_spill_bytes = 8u * CountForStatements(**body);
        if (sp_locals_in_gap == 0 || (frame_analysis.prologue.min_saved_reg_offset == 0xFFFFFFFF &&
                                      !frame_analysis.has_calls && addr_taken_sp_bytes == 0)) {
          if (target_local_size > for_loop_spill_bytes) {
            unreferenced_sp_gap_size = target_local_size - for_loop_spill_bytes;
          }
        } else if (callee_saved_gprs < 8 && addr_taken_sp_bytes > 0) {
          uint32_t gcc_local_bytes = ((addr_taken_sp_bytes + 7u) & ~7u) + for_loop_spill_bytes;
          if (StatementContainsMaskedSignBitFloatFix(**body)) {
            gcc_local_bytes += 8u;
          }
          if (target_local_size > gcc_local_bytes) {
            unreferenced_sp_gap_size = target_local_size - gcc_local_bytes;
          }
        } else if (callee_saved_gprs == 8 && sp_locals_in_gap <= 2 && addr_taken_sp_bytes == 0) {
          uint32_t max_spill_bytes =
              static_cast<uint32_t>(sp_locals_in_gap) * 8u + for_loop_spill_bytes;
          if (target_local_size > max_spill_bytes) {
            unreferenced_sp_gap_size = target_local_size - max_spill_bytes;
          }
        }
        if (unreferenced_sp_gap_size > 0) {
          local_vars_to_declare.push_back("sp");
        }
      }
    }
  }

  if (local_vars_to_declare.empty()) {
    return absl::OkStatus();
  }

  if (frame_analysis.sp_address_taken && frame_analysis.frame_size == 0) {
    return absl::InternalError(
        absl::StrFormat("Frame analysis failed for function '%s': sp address taken as buffer, "
                        "but no stack allocation detected.",
                        options.function_name));
  }
  uint32_t sp_buffer_size =
      (unreferenced_sp_gap_size > 0) ? unreferenced_sp_gap_size : frame_analysis.sp_buffer_size;

  auto is_fp_reg = [](std::string_view name) -> bool {
    if (name.size() >= 2 && name[0] == 'f' && std::isdigit(name[1])) {
      for (size_t i = 1; i < name.size(); ++i) {
        if (!std::isdigit(name[i])) {
          return i == name.size() - 2 && name[i] == '_' && name[i + 1] == 'd';
        }
      }
      return true;
    }
    return false;
  };

  absl::flat_hash_set<std::string> vars_needing_init;
  constexpr uint32_t kAllGprs = 0xFFFFFFFFu;
  uint32_t entry_defs = (1u << static_cast<size_t>(Register::kZero)) |
                        (1u << static_cast<size_t>(Register::kSp)) |
                        (1u << static_cast<size_t>(Register::kRa));
  for (size_t pi = 0; pi < parameters.size() && pi < 4; ++pi) {
    if (parameters[pi].name == "arg0" || pi == 0) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA0));
    }
    if (parameters[pi].name == "arg1" || pi == 1) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA1));
    }
    if (parameters[pi].name == "arg2" || pi == 2) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA2));
    }
    if (parameters[pi].name == "arg3" || pi == 3) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA3));
    }
  }

  auto is_callee_saved_stack_access = [&](const Instruction& inst) -> bool {
    if (inst.rs != Register::kSp || !inst.rt.has_value()) return false;
    if (inst.opcode != Opcode::kSw && inst.opcode != Opcode::kSd && inst.opcode != Opcode::kLw &&
        inst.opcode != Opcode::kLd) {
      return false;
    }
    if ((inst.opcode == Opcode::kLw || inst.opcode == Opcode::kLd) &&
        frame_analysis.frame_size > 0 &&
        inst.immediate >= static_cast<int32_t>(frame_analysis.frame_size)) {
      return false;
    }
    Register rt = *inst.rt;
    return rt == Register::kRa || rt == Register::kFp ||
           (rt >= Register::kS0 && rt <= Register::kS7);
  };

  auto transfer_block = [&](const BasicBlock& block, uint32_t in_defs) -> uint32_t {
    uint32_t cur = in_defs;
    for (const auto& inst : block.instructions) {
      if (is_callee_saved_stack_access(inst)) continue;
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register d : ud.gpr_defs) {
        if (d != Register::kZero) {
          cur |= (1u << static_cast<size_t>(d));
        }
      }
      if (inst.IsCall()) {
        cur |= (1u << static_cast<size_t>(Register::kV0));
        cur |= (1u << static_cast<size_t>(Register::kRa));
      }
    }
    return cur;
  };

  std::vector<uint32_t> rpo = cfg.ReversePostOrder();
  absl::flat_hash_set<uint32_t> reachable_blocks(rpo.begin(), rpo.end());
  absl::flat_hash_map<uint32_t, uint32_t> def_in;
  absl::flat_hash_map<uint32_t, uint32_t> def_out;

  for (uint32_t b_id : rpo) {
    if (b_id == cfg.EntryBlockId()) {
      def_in[b_id] = entry_defs;
      const auto* eb = cfg.GetBlock(b_id);
      def_out[b_id] = (eb != nullptr) ? transfer_block(*eb, entry_defs) : entry_defs;
    } else {
      def_in[b_id] = kAllGprs;
      def_out[b_id] = kAllGprs;
    }
  }

  bool changed = true;
  int iteration = 0;
  while (changed && iteration < 100) {
    changed = false;
    iteration++;
    for (uint32_t b_id : rpo) {
      if (b_id == cfg.EntryBlockId()) continue;
      const auto* block = cfg.GetBlock(b_id);
      if (block == nullptr) continue;

      uint32_t new_in = kAllGprs;
      int valid_preds = 0;
      for (uint32_t pred_id : block->predecessors) {
        if (!reachable_blocks.contains(pred_id)) continue;
        auto it = def_out.find(pred_id);
        if (it != def_out.end()) {
          new_in &= it->second;
          valid_preds++;
        }
      }
      if (valid_preds == 0) {
        new_in = entry_defs;
      }

      def_in[b_id] = new_in;
      uint32_t new_out = transfer_block(*block, new_in);
      if (def_out[b_id] != new_out) {
        def_out[b_id] = new_out;
        changed = true;
      }
    }
  }

  for (uint32_t b_id : rpo) {
    const auto* block = cfg.GetBlock(b_id);
    if (block == nullptr) continue;

    uint32_t cur = def_in[b_id];
    for (const auto& inst : block->instructions) {
      if (is_callee_saved_stack_access(inst)) continue;
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        if (u != Register::kZero) {
          if ((cur & (1u << static_cast<size_t>(u))) == 0) {
            vars_needing_init.insert(ExpressionBuilder::RegisterVarName(u));
          }
        }
      }
      for (Register d : ud.gpr_defs) {
        if (d != Register::kZero) {
          cur |= (1u << static_cast<size_t>(d));
        }
      }
      if (inst.IsCall()) {
        cur |= (1u << static_cast<size_t>(Register::kV0));
        cur |= (1u << static_cast<size_t>(Register::kRa));
      }
    }
  }

  auto has_address_taken = [&](std::string_view target_var) -> bool {
    return StatementTakesAddressOf(**body, target_var);
  };

  absl::flat_hash_map<std::string, int> fp_base_counts;
  bool has_shared_fp_base = false;
  for (const auto& vn : local_vars_to_declare) {
    if (is_fp_reg(vn)) {
      std::string base = absl::EndsWith(vn, "_d") ? vn.substr(0, vn.size() - 2) : vn;
      if (++fp_base_counts[base] > 1) {
        has_shared_fp_base = true;
      }
    }
  }

  const bool is_leaf_func = !StatementHasCall(**body);
  const bool has_s64_pointer_deref = StatementContainsS64PointerDeref(**body);
  const bool returns_fp = IsFloatingPointReturnType(return_type);

  auto is_caller_saved_safe_across_calls = [&](std::string_view r) -> bool {
    return IsCallerSavedSafeAcrossCalls(**body, is_leaf_func, r);
  };

  // MIPS O32 register file capacities and GCC 2.7.2 reload reserve margins:
  // - 16 even FPRs total ($f0..$f30); reserve 4 even FPRs for expression evaluation / conversions.
  // - 7 dedicated caller-saved scratch FPRs ($f2, $f4, $f6, $f8, $f10, $f16, $f18) before ABI
  //   argument/return FPRs ($f0, $f12, $f14) are needed as extra scratch registers in leaf code.
  // - 16 caller-saved GPRs total ($v0..$v1, $a0..$a3, $t0..$t9); reserve 2 GPRs for reloads.
  // - 18 allocatable GPRs total before hard-register pinning risks spilling fixed registers.
  // - 4 GPR argument registers ($a0..$a3).
  constexpr int kTotalEvenFprs = 16;
  constexpr int kReservedScratchEvenFprs = 4;
  constexpr int kMaxPinnedEvenFprs = kTotalEvenFprs - kReservedScratchEvenFprs;
  constexpr size_t kNumDedicatedCallerScratchFprs = 7;
  constexpr int kTotalCallerSavedGprs = 16;
  constexpr int kReservedScratchGprs = 2;
  constexpr int kMaxCallerGprPins = kTotalCallerSavedGprs - kReservedScratchGprs;
  constexpr size_t kMaxUnpinnedGprLocalsBeforeSpillRisk = 18;
  constexpr size_t kNumGprArgumentRegisters = 4;

  size_t unpinned_gpr_locals_count = 0;
  int callee_saved_gpr_count = 0;
  bool has_v0_local = false;
  bool has_v1_local = false;
  for (const auto& v : local_vars_to_declare) {
    if (v == "v0") has_v0_local = true;
    if (v == "v1") has_v1_local = true;
    if (IsCalleeSavedGprVarName(v)) {
      ++callee_saved_gpr_count;
    }
    if (v != "sp" && v != "lo" && v != "hi" && v != "fcr31" && v != "temp_at" &&
        !absl::StartsWith(v, "var_sp_") && !is_fp_reg(v) && !absl::EndsWith(v, "_64") &&
        !double_vars.contains(v) && !IsCalleeSavedGprVarName(v)) {
      ++unpinned_gpr_locals_count;
    }
  }
  const bool allow_caller_saved_gpr_pins =
      (unpinned_gpr_locals_count <= kMaxUnpinnedGprLocalsBeforeSpillRisk);
  int pinned_caller_gpr_count = 0;
  bool any_param_modified = false;
  for (const auto& param : parameters) {
    if (StatementModifiesVariable(**body, param.name)) {
      any_param_modified = true;
      break;
    }
  }

  const int v0_assigns = has_v0_local ? CountDirectAssignmentsInStatement(**body, "v0") : 0;
  const int v0_reads = has_v0_local ? CountVariableReadUsesInStatement(**body, "v0") : 0;
  const int v0_non_return_reads =
      has_v0_local ? CountNonReturnVariableReadUsesInStatement(**body, "v0") : 0;
  const bool v0_reused_as_working_temp = (v0_assigns >= 2 && v0_reads >= 1);
  const bool is_float_return_leaf_with_v0_v1 = is_leaf_func && returns_fp && has_v1_local;
  const bool is_leaf_with_s_regs = is_leaf_func && callee_saved_gpr_count >= 1;
  const bool is_multi_reg_leaf_with_v0_v1 =
      is_leaf_func && has_v0_local && has_v1_local &&
      (is_leaf_with_s_regs || (!is_float_return_leaf_with_v0_v1 && !has_shared_fp_base &&
                               unpinned_gpr_locals_count >= kNumGprArgumentRegisters &&
                               v0_assigns >= 2 && v0_non_return_reads >= 1));

  absl::flat_hash_set<std::string> float_gpr_var_names;
  for (Register r : AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg)) {
    float_gpr_var_names.insert(ExpressionBuilder::RegisterVarName(r));
  }
  absl::flat_hash_set<std::string> float_local_var_names;
  for (int32_t off : AbiAnalyzer::DetectFloatLocalStackOffsets(cfg)) {
    float_local_var_names.insert(ExpressionBuilder::StackVarName(off));
  }
  absl::flat_hash_set<std::string> s64_local_var_names;
  for (const auto& block : cfg.Blocks()) {
    for (const auto& inst : block.instructions) {
      if ((inst.opcode == Opcode::kSd || inst.opcode == Opcode::kScd ||
           inst.opcode == Opcode::kLd || inst.opcode == Opcode::kLld) &&
          inst.rs.has_value() &&
          (*inst.rs == Register::kSp ||
           (frame_analysis.has_frame_pointer && *inst.rs == Register::kFp)) &&
          !is_callee_saved_stack_access(inst)) {
        s64_local_var_names.insert(ExpressionBuilder::StackVarName(inst.immediate));
      }
    }
  }

  int callee_saved_fp_count = 0;
  for (const auto& [base_name, count] : fp_base_counts) {
    if (IsCalleeSavedFprBaseName(base_name)) {
      callee_saved_fp_count++;
    }
  }
  int pinned_caller_fp_count = 0;
  absl::flat_hash_set<std::string> pinned_fp_bases;

  auto can_pin_abi_fpr_base = [&](std::string_view base) -> bool {
    if (!IsAbiArgOrReturnFprBaseName(base)) {
      return false;
    }
    if (is_leaf_func) {
      return !returns_fp && (has_shared_fp_base || callee_saved_fp_count >= 1 ||
                             fp_base_counts.size() > kNumDedicatedCallerScratchFprs);
    }
    return base == "f12" || base == "f14" || (base == "f0" && !returns_fp);
  };

  absl::flat_hash_set<std::string> unpinned_s_vars;
  if (!is_leaf_func && frame_analysis.frame_size > 0 &&
      !(sp_buffer_size == 0 && has_s64_pointer_deref)) {
    const auto& body_stmts = (*body)->Statements();
    for (size_t i = 0; i < body_stmts.size() && i < 8; ++i) {
      if (body_stmts[i] == nullptr) continue;
      if (StatementHasCall(*body_stmts[i])) break;
      if (body_stmts[i]->Kind() != CStatementKind::kExpressionStatement) continue;
      const auto& es = static_cast<const ExpressionStatement&>(*body_stmts[i]);
      if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) continue;
      const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
          a.Rhs().Kind() == CExpressionKind::kIdentifier) {
        std::string_view lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
        std::string_view rhs = static_cast<const IdentifierExpression&>(a.Rhs()).Name();
        if (lhs == "temp_s0" && rhs.size() == 4 && absl::StartsWith(rhs, "arg") && rhs[3] >= '0' &&
            rhs[3] <= '3' && CountDirectAssignmentsInStatement(**body, lhs) == 1 &&
            !StatementModifiesVariable(**body, rhs) && !has_address_taken(rhs)) {
          unpinned_s_vars.insert(std::string(lhs));
        }
      }
    }
  }

  auto get_callee_saved_register_annotation =
      [&](std::string_view var_name) -> std::optional<std::string> {
    if (has_address_taken(var_name)) {
      return std::nullopt;
    }
    if ((var_name == "temp_k0" || var_name == "temp_k1") && !has_s64_pointer_deref) {
      return (var_name == "temp_k0") ? "$26" : "$27";
    }
    if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_s") && var_name[6] >= '0' &&
        var_name[6] <= '7') {
      Register s_reg = static_cast<Register>(static_cast<int>(Register::kS0) + (var_name[6] - '0'));
      if (frame_analysis.frame_size == 0 || (sp_buffer_size == 0 && has_s64_pointer_deref) ||
          unpinned_s_vars.contains(std::string(var_name)) ||
          (!frame_analysis.prologue.saved_reg_offsets.empty() &&
           !frame_analysis.prologue.saved_reg_offsets.contains(s_reg))) {
        return std::nullopt;
      }
      int reg_num = 16 + (var_name[6] - '0');
      return absl::StrFormat("$%d", reg_num);
    }
    if (is_fp_reg(var_name)) {
      std::string_view base =
          absl::EndsWith(var_name, "_d") ? var_name.substr(0, var_name.size() - 2) : var_name;
      if (pinned_fp_bases.contains(std::string(base))) {
        return std::nullopt;
      }
      if (IsCalleeSavedFprBaseName(base)) {
        const std::string d_name = absl::StrCat(base, "_d");
        const bool prefer_single =
            !has_address_taken(base) &&
            (has_address_taken(d_name) || (!is_caller_saved_safe_across_calls(base) &&
                                           is_caller_saved_safe_across_calls(d_name)));
        const bool is_chosen_variant = (fp_base_counts[std::string(base)] == 1) ||
                                       (prefer_single ? (var_name == base) : (var_name == d_name));
        int fpr_num = 0;
        const bool saved_in_prologue =
            frame_analysis.prologue.saved_fpr_offsets.empty() ||
            (!absl::SimpleAtoi(base.substr(1), &fpr_num) ||
             frame_analysis.prologue.saved_fpr_offsets.contains(
                 static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (fpr_num - 20))));
        if (is_chosen_variant) {
          if (frame_analysis.frame_size == 0 || (sp_buffer_size == 0 && has_s64_pointer_deref) ||
              !saved_in_prologue) {
            return std::nullopt;
          }
          pinned_fp_bases.insert(std::string(base));
          return absl::StrCat("$", base);
        }
      }
      if (fp_base_counts[std::string(base)] == 1 &&
          callee_saved_fp_count + pinned_caller_fp_count < kMaxPinnedEvenFprs &&
          (IsDedicatedCallerScratchFprBaseName(base) || can_pin_abi_fpr_base(base)) &&
          is_caller_saved_safe_across_calls(var_name)) {
        pinned_caller_fp_count++;
        pinned_fp_bases.insert(std::string(base));
        return absl::StrCat("$", base);
      }
      return std::nullopt;
    }
    if (!allow_caller_saved_gpr_pins || pinned_caller_gpr_count >= kMaxCallerGprPins) {
      return std::nullopt;
    }
    std::optional<std::string> caller_gpr_reg;
    const bool v0_safe_in_non_leaf = !is_leaf_func && v0_reused_as_working_temp;
    if ((is_float_return_leaf_with_v0_v1 || is_multi_reg_leaf_with_v0_v1 || is_leaf_with_s_regs ||
         v0_safe_in_non_leaf) &&
        var_name == "v0") {
      caller_gpr_reg = "$2";
    } else if ((is_float_return_leaf_with_v0_v1 || is_multi_reg_leaf_with_v0_v1 ||
                is_leaf_with_s_regs || !is_leaf_func) &&
               var_name == "v1") {
      caller_gpr_reg = "$3";
    } else if ((!is_leaf_func || is_leaf_with_s_regs || is_multi_reg_leaf_with_v0_v1 ||
                (v0_non_return_reads >= 1 &&
                 unpinned_gpr_locals_count >= kNumGprArgumentRegisters &&
                 !is_float_return_leaf_with_v0_v1)) &&
               (!any_param_modified || is_leaf_with_s_regs || is_multi_reg_leaf_with_v0_v1 ||
                (!is_leaf_func &&
                 CountNonReturnVariableReadUsesInStatement(**body, var_name) == 0 &&
                 CountVariableReadUsesInStatement(**body, var_name) >= 1)) &&
               var_name.size() == 4 && absl::StartsWith(var_name, "arg") && var_name[3] >= '0' &&
               var_name[3] <= '3') {
      int reg_num = 4 + (var_name[3] - '0');
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    } else if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_a") && var_name[6] >= '0' &&
               var_name[6] <= '3' &&
               (!is_leaf_func || is_leaf_with_s_regs ||
                !DoesScratchArgInterfereWithEntryParam(**body, var_name))) {
      int reg_num = 4 + (var_name[6] - '0');
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    } else if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_t") && var_name[6] >= '0' &&
               var_name[6] <= '9') {
      int t_idx = var_name[6] - '0';
      int reg_num = (t_idx <= 7) ? (8 + t_idx) : (24 + (t_idx - 8));
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    }
    if (caller_gpr_reg.has_value() && is_caller_saved_safe_across_calls(var_name)) {
      pinned_caller_gpr_count++;
      return caller_gpr_reg;
    }
    return std::nullopt;
  };

  absl::flat_hash_set<std::string> cond_stepped_vars;
  absl::flat_hash_set<std::string> do_while_post_stepped_vars;
  CollectConditionallySteppedVars(**body, &cond_stepped_vars, &do_while_post_stepped_vars);
  absl::flat_hash_map<std::string, CType> narrow_cond_inc_var_types;
  for (const auto& var_name : local_vars_to_declare) {
    if (!cond_stepped_vars.contains(var_name) || var_name == "sp" || is_fp_reg(var_name) ||
        float_gpr_var_names.contains(var_name) || float_local_var_names.contains(var_name) ||
        absl::EndsWith(var_name, "_64") || s64_local_var_names.contains(var_name) ||
        double_vars.contains(var_name) || has_address_taken(var_name)) {
      continue;
    }
    int mask_255_count = 0;
    int mask_65535_count = 0;
    bool has_unmasked_read = false;
    CheckNarrowMaskedReadsInStmt(**body, var_name, &mask_255_count, &mask_65535_count,
                                 &has_unmasked_read);
    if (!has_unmasked_read) {
      if (mask_255_count > 0 && mask_65535_count == 0) {
        narrow_cond_inc_var_types.emplace(var_name, CType::U8());
      } else if (mask_65535_count > 0 && mask_255_count == 0) {
        narrow_cond_inc_var_types.emplace(var_name, CType::U16());
      }
    }
  }
  if (!narrow_cond_inc_var_types.empty()) {
    auto rewritten_body =
        RewriteNarrowCondIncStmt(**body, narrow_cond_inc_var_types, do_while_post_stepped_vars);
    body->reset(static_cast<CompoundStatement*>(rewritten_body.release()));
  }

  auto new_body = std::make_unique<CompoundStatement>();
  for (const auto& var_name : local_vars_to_declare) {
    if (var_name == "sp") {
      new_body->AddStatement(
          CStatement::VariableDeclaration(CType::U8(), "sp", nullptr, sp_buffer_size));
    } else if (is_fp_reg(var_name)) {
      std::optional<std::string> reg_annot = get_callee_saved_register_annotation(var_name);
      if (absl::EndsWith(var_name, "_d") || double_vars.contains(var_name)) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name, nullptr,
                                                               std::nullopt, std::move(reg_annot)));
      } else {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name, nullptr,
                                                               std::nullopt, std::move(reg_annot)));
      }
    } else if (float_gpr_var_names.contains(var_name)) {
      std::optional<std::string> reg_annot = get_callee_saved_register_annotation(var_name);
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name, nullptr,
                                                             std::nullopt, std::move(reg_annot)));
    } else if (float_local_var_names.contains(var_name)) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name));
    } else if (absl::EndsWith(var_name, "_64") ||
               (s64_local_var_names.contains(var_name) && !double_vars.contains(var_name))) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::Named("s64"), var_name));
    } else if (double_vars.contains(var_name)) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name));
    } else {
      bool is_callee_gpr = IsCalleeSavedGprVarName(var_name);
      auto init_expr = vars_needing_init.contains(var_name) ? CExpression::Integer(0) : nullptr;
      std::optional<std::string> reg_annot = (init_expr == nullptr || is_callee_gpr)
                                                 ? get_callee_saved_register_annotation(var_name)
                                                 : std::nullopt;
      if (reg_annot.has_value() || is_callee_gpr) {
        init_expr = nullptr;
      }
      CType var_type = CType::S32();
      if (auto it = narrow_cond_inc_var_types.find(var_name);
          it != narrow_cond_inc_var_types.end()) {
        var_type = it->second;
      }
      new_body->AddStatement(CStatement::VariableDeclaration(
          std::move(var_type), var_name, std::move(init_expr), std::nullopt, std::move(reg_annot)));
    }
  }
  for (const auto& stmt : (*body)->Statements()) {
    new_body->AddStatement(stmt->Clone());
  }
  *body = std::move(new_body);
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
