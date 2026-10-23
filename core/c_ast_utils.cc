#include "core/c_ast_utils.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "core/c_ast.h"

namespace rom_nom_nom {

bool ExtractFunctionPointerCastCalleeIdentifier(std::string_view text, std::string_view* out_prefix,
                                                std::string_view* out_identifier) {
  if (text.size() < 10 || text.substr(0, 2) != "((" || text.back() != ')') return false;
  size_t position = text.find("(*)())");
  if (position == std::string_view::npos) return false;
  size_t identifier_start = position + 6;
  if (identifier_start + 1 >= text.size()) return false;
  std::string_view identifier = text.substr(identifier_start, text.size() - identifier_start - 1);
  for (char character : identifier) {
    if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') return false;
  }
  if (out_prefix != nullptr) *out_prefix = text.substr(0, identifier_start);
  if (out_identifier != nullptr) *out_identifier = identifier;
  return true;
}

bool IsPureExpression(const CExpression& expression) {
  switch (expression.Kind()) {
    case CExpressionKind::kIntegerLiteral:
    case CExpressionKind::kFloatLiteral:
    case CExpressionKind::kStringLiteral:
    case CExpressionKind::kIdentifier:
      return true;
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      if (unary.Op() == "++" || unary.Op() == "--") return false;
      return IsPureExpression(unary.Operand());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return IsPureExpression(binary.Lhs()) && IsPureExpression(binary.Rhs());
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      return IsPureExpression(cast.Operand());
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return IsPureExpression(member.Object());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return IsPureExpression(array_index.Array()) && IsPureExpression(array_index.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return IsPureExpression(ternary.Condition()) && IsPureExpression(ternary.TrueExpression()) &&
             IsPureExpression(ternary.FalseExpression());
    }
    case CExpressionKind::kAssignmentExpression:
    case CExpressionKind::kCallExpression:
      return false;
  }
  return false;
}

std::optional<std::string> CommutedBinaryOperator(std::string_view binary_operator) {
  if (binary_operator == "+" || binary_operator == "*" || binary_operator == "&" ||
      binary_operator == "|" || binary_operator == "^" || binary_operator == "==" ||
      binary_operator == "!=") {
    return std::string(binary_operator);
  }
  if (binary_operator == "<") return ">";
  if (binary_operator == ">") return "<";
  if (binary_operator == "<=") return ">=";
  if (binary_operator == ">=") return "<=";
  return std::nullopt;
}

bool IsCommutableBinary(const BinaryExpression& binary) {
  return CommutedBinaryOperator(binary.Op()).has_value() && IsPureExpression(binary.Lhs()) &&
         IsPureExpression(binary.Rhs());
}

std::unique_ptr<CExpression> NegateCondition(const CExpression& condition) {
  if (condition.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& unary = static_cast<const UnaryExpression&>(condition);
    if (unary.IsPrefix() && unary.Op() == "!") {
      return unary.Operand().Clone();
    }
  }
  if (condition.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& binary = static_cast<const BinaryExpression&>(condition);
    if (binary.Op() == "==") {
      return CExpression::Binary("!=", binary.Lhs().Clone(), binary.Rhs().Clone());
    }
    if (binary.Op() == "!=") {
      return CExpression::Binary("==", binary.Lhs().Clone(), binary.Rhs().Clone());
    }
    if (binary.Op() == "<") {
      return CExpression::Binary(">=", binary.Lhs().Clone(), binary.Rhs().Clone());
    }
    if (binary.Op() == ">=") {
      return CExpression::Binary("<", binary.Lhs().Clone(), binary.Rhs().Clone());
    }
    if (binary.Op() == ">") {
      return CExpression::Binary("<=", binary.Lhs().Clone(), binary.Rhs().Clone());
    }
    if (binary.Op() == "<=") {
      return CExpression::Binary(">", binary.Lhs().Clone(), binary.Rhs().Clone());
    }
  }
  return CExpression::Unary("!", condition.Clone(), /*is_prefix=*/true);
}

std::optional<int64_t> TypeByteSize(std::string_view type_name) {
  if (type_name == "u8" || type_name == "s8") return 1;
  if (type_name == "u16" || type_name == "s16") return 2;
  if (type_name == "u32" || type_name == "s32" || type_name == "f32") return 4;
  if (type_name == "u64" || type_name == "s64" || type_name == "f64") return 8;
  return std::nullopt;
}

bool IsInlineAsmIntrinsicCall(const CallExpression& call) {
  if (call.Callee().Kind() != CExpressionKind::kIdentifier) return false;
  std::string_view name = static_cast<const IdentifierExpression&>(call.Callee()).Name();
  return name == "N64Sqrtf" || name == "N64Sqrt" || name == "sqrtf" || name == "sqrt" ||
         name == "fabsf" || name == "fabs" || name == "ceilf" || name == "floorf" ||
         name == "roundf" || name == "truncf" || name == "ceil" || name == "floor" ||
         name == "round" || name == "trunc";
}

bool ExpressionContainsCall(const CExpression& expr, bool ignore_math_intrinsics) {
  switch (expr.Kind()) {
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (!ignore_math_intrinsics || !IsInlineAsmIntrinsicCall(call)) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExpressionContainsCall(*arg, ignore_math_intrinsics)) return true;
      }
      return false;
    }
    case CExpressionKind::kUnaryExpression:
      return ExpressionContainsCall(static_cast<const UnaryExpression&>(expr).Operand(),
                                    ignore_math_intrinsics);
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return ExpressionContainsCall(bin.Lhs(), ignore_math_intrinsics) ||
             ExpressionContainsCall(bin.Rhs(), ignore_math_intrinsics);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return ExpressionContainsCall(assign.Lhs(), ignore_math_intrinsics) ||
             ExpressionContainsCall(assign.Rhs(), ignore_math_intrinsics);
    }
    case CExpressionKind::kCastExpression:
      return ExpressionContainsCall(static_cast<const CastExpression&>(expr).Operand(),
                                    ignore_math_intrinsics);
    case CExpressionKind::kMemberAccessExpression:
      return ExpressionContainsCall(static_cast<const MemberAccessExpression&>(expr).Object(),
                                    ignore_math_intrinsics);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return ExpressionContainsCall(arr.Array(), ignore_math_intrinsics) ||
             ExpressionContainsCall(arr.Index(), ignore_math_intrinsics);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return ExpressionContainsCall(tern.Condition(), ignore_math_intrinsics) ||
             ExpressionContainsCall(tern.TrueExpression(), ignore_math_intrinsics) ||
             ExpressionContainsCall(tern.FalseExpression(), ignore_math_intrinsics);
    }
    default:
      return false;
  }
}

bool StatementContainsCall(const CStatement& stmt, bool ignore_math_intrinsics) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsCall(*child, ignore_math_intrinsics)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement:
      return ExpressionContainsCall(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                    ignore_math_intrinsics);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return vd.Initializer() != nullptr &&
             ExpressionContainsCall(*vd.Initializer(), ignore_math_intrinsics);
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExpressionContainsCall(ifs.Condition(), ignore_math_intrinsics) ||
             StatementContainsCall(ifs.ThenBranch(), ignore_math_intrinsics) ||
             (ifs.ElseBranch() != nullptr &&
              StatementContainsCall(*ifs.ElseBranch(), ignore_math_intrinsics));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExpressionContainsCall(ws.Condition(), ignore_math_intrinsics) ||
             StatementContainsCall(ws.Body(), ignore_math_intrinsics);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StatementContainsCall(dws.Body(), ignore_math_intrinsics) ||
             ExpressionContainsCall(dws.Condition(), ignore_math_intrinsics);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StatementContainsCall(*fs.Init(), ignore_math_intrinsics)) ||
             (fs.Condition() != nullptr &&
              ExpressionContainsCall(*fs.Condition(), ignore_math_intrinsics)) ||
             (fs.Step() != nullptr && ExpressionContainsCall(*fs.Step(), ignore_math_intrinsics)) ||
             StatementContainsCall(fs.Body(), ignore_math_intrinsics);
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr &&
             ExpressionContainsCall(*rs.ReturnValue(), ignore_math_intrinsics);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExpressionContainsCall(ss.Condition(), ignore_math_intrinsics)) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StatementContainsCall(*c.body, ignore_math_intrinsics)) {
          return true;
        }
      }
      return false;
    }
    default:
      return false;
  }
}

int CountIdentifierOccurrencesInExpression(const CExpression& expression, std::string_view name) {
  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier: {
      const std::string& identifier_name =
          static_cast<const IdentifierExpression&>(expression).Name();
      if (identifier_name == name) return 1;
      std::string_view prefix;
      std::string_view inner_identifier;
      if (ExtractFunctionPointerCastCalleeIdentifier(identifier_name, &prefix, &inner_identifier) &&
          inner_identifier == name) {
        return 1;
      }
      return 0;
    }
    case CExpressionKind::kUnaryExpression:
      return CountIdentifierOccurrencesInExpression(
          static_cast<const UnaryExpression&>(expression).Operand(), name);
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return CountIdentifierOccurrencesInExpression(binary.Lhs(), name) +
             CountIdentifierOccurrencesInExpression(binary.Rhs(), name);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      return CountIdentifierOccurrencesInExpression(assignment.Lhs(), name) +
             CountIdentifierOccurrencesInExpression(assignment.Rhs(), name);
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      int total = CountIdentifierOccurrencesInExpression(call.Callee(), name);
      for (const auto& argument : call.Arguments()) {
        total += CountIdentifierOccurrencesInExpression(*argument, name);
      }
      return total;
    }
    case CExpressionKind::kCastExpression:
      return CountIdentifierOccurrencesInExpression(
          static_cast<const CastExpression&>(expression).Operand(), name);
    case CExpressionKind::kMemberAccessExpression:
      return CountIdentifierOccurrencesInExpression(
          static_cast<const MemberAccessExpression&>(expression).Object(), name);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return CountIdentifierOccurrencesInExpression(array_index.Array(), name) +
             CountIdentifierOccurrencesInExpression(array_index.Index(), name);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CountIdentifierOccurrencesInExpression(ternary.Condition(), name) +
             CountIdentifierOccurrencesInExpression(ternary.TrueExpression(), name) +
             CountIdentifierOccurrencesInExpression(ternary.FalseExpression(), name);
    }
    default:
      return 0;
  }
}

int CountIdentifierOccurrencesInStatement(const CStatement& statement, std::string_view name) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      int total = 0;
      for (const auto& child_statement :
           static_cast<const CompoundStatement&>(statement).Statements()) {
        total += CountIdentifierOccurrencesInStatement(*child_statement, name);
      }
      return total;
    }
    case CStatementKind::kExpressionStatement:
      return CountIdentifierOccurrencesInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), name);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      int total = (declaration.Name() == name) ? 1 : 0;
      if (declaration.Initializer() != nullptr) {
        total += CountIdentifierOccurrencesInExpression(*declaration.Initializer(), name);
      }
      return total;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      int total = CountIdentifierOccurrencesInExpression(if_statement.Condition(), name) +
                  CountIdentifierOccurrencesInStatement(if_statement.ThenBranch(), name);
      if (if_statement.ElseBranch() != nullptr) {
        total += CountIdentifierOccurrencesInStatement(*if_statement.ElseBranch(), name);
      }
      return total;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      return CountIdentifierOccurrencesInExpression(while_statement.Condition(), name) +
             CountIdentifierOccurrencesInStatement(while_statement.Body(), name);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      return CountIdentifierOccurrencesInStatement(do_while_statement.Body(), name) +
             CountIdentifierOccurrencesInExpression(do_while_statement.Condition(), name);
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      int total = CountIdentifierOccurrencesInStatement(for_statement.Body(), name);
      if (for_statement.Init() != nullptr) {
        total += CountIdentifierOccurrencesInStatement(*for_statement.Init(), name);
      }
      if (for_statement.Condition() != nullptr) {
        total += CountIdentifierOccurrencesInExpression(*for_statement.Condition(), name);
      }
      if (for_statement.Step() != nullptr) {
        total += CountIdentifierOccurrencesInExpression(*for_statement.Step(), name);
      }
      return total;
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      return return_statement.ReturnValue() != nullptr
                 ? CountIdentifierOccurrencesInExpression(*return_statement.ReturnValue(), name)
                 : 0;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_statement = static_cast<const SwitchStatement&>(statement);
      int total = CountIdentifierOccurrencesInExpression(switch_statement.Condition(), name);
      for (const auto& switch_case : switch_statement.Cases()) {
        if (switch_case.body != nullptr) {
          total += CountIdentifierOccurrencesInStatement(*switch_case.body, name);
        }
      }
      return total;
    }
    default:
      return 0;
  }
}

bool ExpressionUsesVariable(const CExpression& expr, std::string_view target_var) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier:
      return static_cast<const IdentifierExpression&>(expr).Name() == target_var;
    case CExpressionKind::kUnaryExpression:
      return ExpressionUsesVariable(static_cast<const UnaryExpression&>(expr).Operand(),
                                    target_var);
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return ExpressionUsesVariable(b.Lhs(), target_var) ||
             ExpressionUsesVariable(b.Rhs(), target_var);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return ExpressionUsesVariable(a.Lhs(), target_var) ||
             ExpressionUsesVariable(a.Rhs(), target_var);
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      if (ExpressionUsesVariable(c.Callee(), target_var)) return true;
      for (const auto& arg : c.Arguments()) {
        if (arg != nullptr && ExpressionUsesVariable(*arg, target_var)) return true;
      }
      return false;
    }
    case CExpressionKind::kCastExpression:
      return ExpressionUsesVariable(static_cast<const CastExpression&>(expr).Operand(), target_var);
    case CExpressionKind::kMemberAccessExpression:
      return ExpressionUsesVariable(static_cast<const MemberAccessExpression&>(expr).Object(),
                                    target_var);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      return ExpressionUsesVariable(a.Array(), target_var) ||
             ExpressionUsesVariable(a.Index(), target_var);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExpressionUsesVariable(t.Condition(), target_var) ||
             ExpressionUsesVariable(t.TrueExpression(), target_var) ||
             ExpressionUsesVariable(t.FalseExpression(), target_var);
    }
    default:
      return false;
  }
}

bool StatementReferencesVariable(const CStatement& stmt, std::string_view target_var) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementReferencesVariable(*child, target_var)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement:
      return ExpressionUsesVariable(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                    target_var);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return vd.Initializer() != nullptr && ExpressionUsesVariable(*vd.Initializer(), target_var);
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExpressionUsesVariable(ifs.Condition(), target_var) ||
             StatementReferencesVariable(ifs.ThenBranch(), target_var) ||
             (ifs.ElseBranch() != nullptr &&
              StatementReferencesVariable(*ifs.ElseBranch(), target_var));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExpressionUsesVariable(ws.Condition(), target_var) ||
             StatementReferencesVariable(ws.Body(), target_var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StatementReferencesVariable(dws.Body(), target_var) ||
             ExpressionUsesVariable(dws.Condition(), target_var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StatementReferencesVariable(*fs.Init(), target_var)) ||
             (fs.Condition() != nullptr && ExpressionUsesVariable(*fs.Condition(), target_var)) ||
             (fs.Step() != nullptr && ExpressionUsesVariable(*fs.Step(), target_var)) ||
             StatementReferencesVariable(fs.Body(), target_var);
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr && ExpressionUsesVariable(*rs.ReturnValue(), target_var);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExpressionUsesVariable(ss.Condition(), target_var)) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StatementReferencesVariable(*c.body, target_var)) return true;
      }
      return false;
    }
    default:
      return false;
  }
}

bool ExpressionTakesAddressOf(const CExpression& expr, std::string_view target_var,
                              bool ignore_assigned_float_bitcast_lhs) {
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      if (un.IsPrefix() &&
          (un.Op() == "&" || absl::EndsWith(un.Op(), "*&") ||
           un.Op().find('&') != std::string::npos) &&
          un.Operand().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(un.Operand()).Name() == target_var) {
        return true;
      }
      return ExpressionTakesAddressOf(un.Operand(), target_var, ignore_assigned_float_bitcast_lhs);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return ExpressionTakesAddressOf(bin.Lhs(), target_var, ignore_assigned_float_bitcast_lhs) ||
             ExpressionTakesAddressOf(bin.Rhs(), target_var, ignore_assigned_float_bitcast_lhs);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (!ignore_assigned_float_bitcast_lhs || assign.Op() != "=" ||
          ExtractAssignedLhsVar(assign.Lhs()).empty()) {
        if (ExpressionTakesAddressOf(assign.Lhs(), target_var, ignore_assigned_float_bitcast_lhs)) {
          return true;
        }
      }
      return ExpressionTakesAddressOf(assign.Rhs(), target_var, ignore_assigned_float_bitcast_lhs);
    }
    case CExpressionKind::kCastExpression:
      return ExpressionTakesAddressOf(static_cast<const CastExpression&>(expr).Operand(),
                                      target_var, ignore_assigned_float_bitcast_lhs);
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (ExpressionTakesAddressOf(call.Callee(), target_var, ignore_assigned_float_bitcast_lhs)) {
        return true;
      }
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr &&
            ExpressionTakesAddressOf(*arg, target_var, ignore_assigned_float_bitcast_lhs)) {
          return true;
        }
      }
      return false;
    }
    case CExpressionKind::kMemberAccessExpression:
      return ExpressionTakesAddressOf(static_cast<const MemberAccessExpression&>(expr).Object(),
                                      target_var, ignore_assigned_float_bitcast_lhs);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return ExpressionTakesAddressOf(arr.Array(), target_var, ignore_assigned_float_bitcast_lhs) ||
             ExpressionTakesAddressOf(arr.Index(), target_var, ignore_assigned_float_bitcast_lhs);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return ExpressionTakesAddressOf(tern.Condition(), target_var,
                                      ignore_assigned_float_bitcast_lhs) ||
             ExpressionTakesAddressOf(tern.TrueExpression(), target_var,
                                      ignore_assigned_float_bitcast_lhs) ||
             ExpressionTakesAddressOf(tern.FalseExpression(), target_var,
                                      ignore_assigned_float_bitcast_lhs);
    }
    default:
      return false;
  }
}

bool StatementTakesAddressOf(const CStatement& stmt, std::string_view target_var,
                             bool ignore_assigned_float_bitcast_lhs) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement:
      return ExpressionTakesAddressOf(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                      target_var, ignore_assigned_float_bitcast_lhs);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return vd.Initializer() != nullptr &&
             ExpressionTakesAddressOf(*vd.Initializer(), target_var,
                                      ignore_assigned_float_bitcast_lhs);
    }
    case CStatementKind::kCompoundStatement: {
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr &&
            StatementTakesAddressOf(*child, target_var, ignore_assigned_float_bitcast_lhs)) {
          return true;
        }
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ExpressionTakesAddressOf(ifs.Condition(), target_var,
                                   ignore_assigned_float_bitcast_lhs) ||
          StatementTakesAddressOf(ifs.ThenBranch(), target_var,
                                  ignore_assigned_float_bitcast_lhs)) {
        return true;
      }
      return ifs.ElseBranch() != nullptr &&
             StatementTakesAddressOf(*ifs.ElseBranch(), target_var,
                                     ignore_assigned_float_bitcast_lhs);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExpressionTakesAddressOf(ws.Condition(), target_var,
                                      ignore_assigned_float_bitcast_lhs) ||
             StatementTakesAddressOf(ws.Body(), target_var, ignore_assigned_float_bitcast_lhs);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StatementTakesAddressOf(dws.Body(), target_var, ignore_assigned_float_bitcast_lhs) ||
             ExpressionTakesAddressOf(dws.Condition(), target_var,
                                      ignore_assigned_float_bitcast_lhs);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr &&
          StatementTakesAddressOf(*fs.Init(), target_var, ignore_assigned_float_bitcast_lhs)) {
        return true;
      }
      if (fs.Condition() != nullptr &&
          ExpressionTakesAddressOf(*fs.Condition(), target_var,
                                   ignore_assigned_float_bitcast_lhs)) {
        return true;
      }
      if (fs.Step() != nullptr &&
          ExpressionTakesAddressOf(*fs.Step(), target_var, ignore_assigned_float_bitcast_lhs)) {
        return true;
      }
      return StatementTakesAddressOf(fs.Body(), target_var, ignore_assigned_float_bitcast_lhs);
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr &&
             ExpressionTakesAddressOf(*rs.ReturnValue(), target_var,
                                      ignore_assigned_float_bitcast_lhs);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExpressionTakesAddressOf(ss.Condition(), target_var, ignore_assigned_float_bitcast_lhs)) {
        return true;
      }
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr &&
            StatementTakesAddressOf(*c.body, target_var, ignore_assigned_float_bitcast_lhs)) {
          return true;
        }
      }
      return false;
    }
    default:
      return false;
  }
}

std::string_view ExtractFloatBitcastLhsVar(const CExpression& lhs) {
  if (lhs.Kind() != CExpressionKind::kUnaryExpression) return {};
  const auto& deref = static_cast<const UnaryExpression&>(lhs);
  if (deref.Op() != "*" || !deref.IsPrefix()) return {};
  if (deref.Operand().Kind() != CExpressionKind::kCastExpression) return {};
  const auto& cast = static_cast<const CastExpression&>(deref.Operand());
  if (cast.TargetType().name != "f32" || cast.TargetType().pointer_depth != 1) return {};
  if (cast.Operand().Kind() != CExpressionKind::kUnaryExpression) return {};
  const auto& addr = static_cast<const UnaryExpression&>(cast.Operand());
  if (addr.Op() != "&" || !addr.IsPrefix()) return {};
  if (addr.Operand().Kind() != CExpressionKind::kIdentifier) return {};
  return static_cast<const IdentifierExpression&>(addr.Operand()).Name();
}

std::string_view ExtractAssignedLhsVar(const CExpression& lhs) {
  if (lhs.Kind() == CExpressionKind::kIdentifier) {
    return static_cast<const IdentifierExpression&>(lhs).Name();
  }
  return ExtractFloatBitcastLhsVar(lhs);
}

int CountVariableReadUsesInExpression(const CExpression& expression, std::string_view var) {
  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier: {
      const std::string& id_name = static_cast<const IdentifierExpression&>(expression).Name();
      if (id_name == var) return 1;
      if (absl::StartsWith(id_name, "((") && absl::EndsWith(id_name, ")")) {
        std::string suffix = absl::StrCat(")", var, ")");
        if (absl::EndsWith(id_name, suffix)) return 1;
      }
      return 0;
    }
    case CExpressionKind::kUnaryExpression:
      return CountVariableReadUsesInExpression(
          static_cast<const UnaryExpression&>(expression).Operand(), var);
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expression);
      return CountVariableReadUsesInExpression(bin.Lhs(), var) +
             CountVariableReadUsesInExpression(bin.Rhs(), var);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expression);
      int lhs_uses = 0;
      if (assign.Op() != "=" || ExtractAssignedLhsVar(assign.Lhs()).empty()) {
        lhs_uses = CountVariableReadUsesInExpression(assign.Lhs(), var);
      }
      return lhs_uses + CountVariableReadUsesInExpression(assign.Rhs(), var);
    }
    case CExpressionKind::kCastExpression:
      return CountVariableReadUsesInExpression(
          static_cast<const CastExpression&>(expression).Operand(), var);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expression);
      return CountVariableReadUsesInExpression(arr.Array(), var) +
             CountVariableReadUsesInExpression(arr.Index(), var);
    }
    case CExpressionKind::kMemberAccessExpression:
      return CountVariableReadUsesInExpression(
          static_cast<const MemberAccessExpression&>(expression).Object(), var);
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expression);
      return CountVariableReadUsesInExpression(tern.Condition(), var) +
             CountVariableReadUsesInExpression(tern.TrueExpression(), var) +
             CountVariableReadUsesInExpression(tern.FalseExpression(), var);
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      int total = CountVariableReadUsesInExpression(call.Callee(), var);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr) total += CountVariableReadUsesInExpression(*arg, var);
      }
      return total;
    }
    default:
      return 0;
  }
}

int CountVariableReadUsesInStatement(const CStatement& statement, std::string_view var) {
  switch (statement.Kind()) {
    case CStatementKind::kExpressionStatement:
      return CountVariableReadUsesInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), var);
    case CStatementKind::kReturnStatement: {
      const auto& ret = static_cast<const ReturnStatement&>(statement);
      return ret.ReturnValue() != nullptr
                 ? CountVariableReadUsesInExpression(*ret.ReturnValue(), var)
                 : 0;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(statement);
      int total = CountVariableReadUsesInExpression(ifs.Condition(), var) +
                  CountVariableReadUsesInStatement(ifs.ThenBranch(), var);
      if (ifs.ElseBranch() != nullptr) {
        total += CountVariableReadUsesInStatement(*ifs.ElseBranch(), var);
      }
      return total;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(statement);
      return CountVariableReadUsesInExpression(ws.Condition(), var) +
             CountVariableReadUsesInStatement(ws.Body(), var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(statement);
      return CountVariableReadUsesInExpression(dws.Condition(), var) +
             CountVariableReadUsesInStatement(dws.Body(), var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(statement);
      int total = CountVariableReadUsesInStatement(fs.Body(), var);
      if (fs.Init() != nullptr) {
        total += CountVariableReadUsesInStatement(*fs.Init(), var);
      }
      if (fs.Condition() != nullptr) {
        total += CountVariableReadUsesInExpression(*fs.Condition(), var);
      }
      if (fs.Step() != nullptr) {
        total += CountVariableReadUsesInExpression(*fs.Step(), var);
      }
      return total;
    }
    case CStatementKind::kCompoundStatement: {
      int total = 0;
      for (const auto& child : static_cast<const CompoundStatement&>(statement).Statements()) {
        if (child != nullptr) total += CountVariableReadUsesInStatement(*child, var);
      }
      return total;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(statement);
      int total = CountVariableReadUsesInExpression(ss.Condition(), var);
      for (const auto& sc : ss.Cases()) {
        if (sc.body != nullptr) total += CountVariableReadUsesInStatement(*sc.body, var);
      }
      return total;
    }
    default:
      return 0;
  }
}

bool StatementContainsLabel(const CStatement& stmt) {
  if (stmt.Kind() == CStatementKind::kLabelStatement) return true;
  switch (stmt.Kind()) {
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StatementContainsLabel(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StatementContainsLabel(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement:
      return StatementContainsLabel(static_cast<const WhileStatement&>(stmt).Body());
    case CStatementKind::kDoWhileStatement:
      return StatementContainsLabel(static_cast<const DoWhileStatement&>(stmt).Body());
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StatementContainsLabel(*fs.Init())) ||
             StatementContainsLabel(fs.Body());
    }
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsLabel(*child)) return true;
      }
      return false;
    case CStatementKind::kSwitchStatement:
      for (const auto& sc : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (sc.body != nullptr && StatementContainsLabel(*sc.body)) return true;
      }
      return false;
    default:
      return false;
  }
}

bool StatementContainsControlFlowJump(const CStatement& stmt) {
  if (stmt.Kind() == CStatementKind::kBreakStatement ||
      stmt.Kind() == CStatementKind::kContinueStatement ||
      stmt.Kind() == CStatementKind::kGotoStatement ||
      stmt.Kind() == CStatementKind::kLabelStatement) {
    return true;
  }
  switch (stmt.Kind()) {
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StatementContainsControlFlowJump(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StatementContainsControlFlowJump(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement:
      return StatementContainsControlFlowJump(static_cast<const WhileStatement&>(stmt).Body());
    case CStatementKind::kDoWhileStatement:
      return StatementContainsControlFlowJump(static_cast<const DoWhileStatement&>(stmt).Body());
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StatementContainsControlFlowJump(*fs.Init())) ||
             StatementContainsControlFlowJump(fs.Body());
    }
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsControlFlowJump(*child)) return true;
      }
      return false;
    case CStatementKind::kSwitchStatement:
      for (const auto& sc : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (sc.body != nullptr && StatementContainsControlFlowJump(*sc.body)) return true;
      }
      return false;
    default:
      return false;
  }
}

bool StatementContainsLoop(const CStatement& stmt) {
  if (stmt.Kind() == CStatementKind::kWhileStatement ||
      stmt.Kind() == CStatementKind::kDoWhileStatement ||
      stmt.Kind() == CStatementKind::kForStatement) {
    return true;
  }
  switch (stmt.Kind()) {
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StatementContainsLoop(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StatementContainsLoop(*ifs.ElseBranch()));
    }
    case CStatementKind::kSwitchStatement:
      for (const auto& sc : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (sc.body != nullptr && StatementContainsLoop(*sc.body)) return true;
      }
      return false;
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsLoop(*child)) return true;
      }
      return false;
    default:
      return false;
  }
}

bool ExpressionModifiesVariable(const CExpression& expr, std::string_view target_var) {
  switch (expr.Kind()) {
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == target_var) {
        return true;
      }
      return ExpressionModifiesVariable(assign.Lhs(), target_var) ||
             ExpressionModifiesVariable(assign.Rhs(), target_var);
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      if ((un.Op() == "++" || un.Op() == "--") &&
          un.Operand().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(un.Operand()).Name() == target_var) {
        return true;
      }
      return ExpressionModifiesVariable(un.Operand(), target_var);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return ExpressionModifiesVariable(bin.Lhs(), target_var) ||
             ExpressionModifiesVariable(bin.Rhs(), target_var);
    }
    case CExpressionKind::kCastExpression:
      return ExpressionModifiesVariable(static_cast<const CastExpression&>(expr).Operand(),
                                        target_var);
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (ExpressionModifiesVariable(call.Callee(), target_var)) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExpressionModifiesVariable(*arg, target_var)) return true;
      }
      return false;
    }
    case CExpressionKind::kMemberAccessExpression:
      return ExpressionModifiesVariable(static_cast<const MemberAccessExpression&>(expr).Object(),
                                        target_var);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return ExpressionModifiesVariable(arr.Array(), target_var) ||
             ExpressionModifiesVariable(arr.Index(), target_var);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return ExpressionModifiesVariable(tern.Condition(), target_var) ||
             ExpressionModifiesVariable(tern.TrueExpression(), target_var) ||
             ExpressionModifiesVariable(tern.FalseExpression(), target_var);
    }
    default:
      return false;
  }
}

bool StatementModifiesVariable(const CStatement& stmt, std::string_view target_var) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement:
      return ExpressionModifiesVariable(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                        target_var);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return vd.Initializer() != nullptr &&
             ExpressionModifiesVariable(*vd.Initializer(), target_var);
    }
    case CStatementKind::kCompoundStatement: {
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementModifiesVariable(*child, target_var)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ExpressionModifiesVariable(ifs.Condition(), target_var) ||
          StatementModifiesVariable(ifs.ThenBranch(), target_var)) {
        return true;
      }
      return ifs.ElseBranch() != nullptr &&
             StatementModifiesVariable(*ifs.ElseBranch(), target_var);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExpressionModifiesVariable(ws.Condition(), target_var) ||
             StatementModifiesVariable(ws.Body(), target_var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StatementModifiesVariable(dws.Body(), target_var) ||
             ExpressionModifiesVariable(dws.Condition(), target_var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr && StatementModifiesVariable(*fs.Init(), target_var)) return true;
      if (fs.Condition() != nullptr && ExpressionModifiesVariable(*fs.Condition(), target_var)) {
        return true;
      }
      if (fs.Step() != nullptr && ExpressionModifiesVariable(*fs.Step(), target_var)) return true;
      return StatementModifiesVariable(fs.Body(), target_var);
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr &&
             ExpressionModifiesVariable(*rs.ReturnValue(), target_var);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExpressionModifiesVariable(ss.Condition(), target_var)) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StatementModifiesVariable(*c.body, target_var)) return true;
      }
      return false;
    }
    default:
      return false;
  }
}

void CollectVariablesInExpression(const CExpression& expr, std::vector<std::string>* vars,
                                  absl::flat_hash_set<std::string>* seen) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      std::string name = id.Name();
      if (absl::StartsWith(name, "((") && absl::EndsWith(name, ")") && name.size() > 4) {
        size_t inner_close = name.rfind(')', name.size() - 2);
        if (inner_close != std::string::npos && inner_close + 1 < name.size() - 1) {
          name = name.substr(inner_close + 1, name.size() - inner_close - 2);
        }
      }
      if (!name.empty() && seen->insert(name).second) {
        vars->push_back(name);
      }
      break;
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      CollectVariablesInExpression(un.Operand(), vars, seen);
      break;
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      CollectVariablesInExpression(bin.Lhs(), vars, seen);
      CollectVariablesInExpression(bin.Rhs(), vars, seen);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      CollectVariablesInExpression(assign.Lhs(), vars, seen);
      CollectVariablesInExpression(assign.Rhs(), vars, seen);
      break;
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expr);
      CollectVariablesInExpression(cast.Operand(), vars, seen);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (call.Callee().Kind() != CExpressionKind::kIdentifier ||
          absl::StartsWith(static_cast<const IdentifierExpression&>(call.Callee()).Name(), "((")) {
        CollectVariablesInExpression(call.Callee(), vars, seen);
      }
      for (const auto& arg : call.Arguments()) {
        CollectVariablesInExpression(*arg, vars, seen);
      }
      break;
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expr);
      CollectVariablesInExpression(member.Object(), vars, seen);
      break;
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      CollectVariablesInExpression(arr.Array(), vars, seen);
      CollectVariablesInExpression(arr.Index(), vars, seen);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      CollectVariablesInExpression(tern.Condition(), vars, seen);
      CollectVariablesInExpression(tern.TrueExpression(), vars, seen);
      CollectVariablesInExpression(tern.FalseExpression(), vars, seen);
      break;
    }
    case CExpressionKind::kIntegerLiteral:
    case CExpressionKind::kFloatLiteral:
    case CExpressionKind::kStringLiteral:
      break;
  }
}

void CollectUsedVariables(const CStatement& stmt, std::vector<std::string>* vars,
                          absl::flat_hash_set<std::string>* seen) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement: {
      const auto& expr_stmt = static_cast<const ExpressionStatement&>(stmt);
      CollectVariablesInExpression(expr_stmt.Expression(), vars, seen);
      break;
    }
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      for (const auto& child : comp.Statements()) {
        CollectUsedVariables(*child, vars, seen);
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      CollectVariablesInExpression(if_stmt.Condition(), vars, seen);
      CollectUsedVariables(if_stmt.ThenBranch(), vars, seen);
      if (if_stmt.ElseBranch() != nullptr) {
        CollectUsedVariables(*if_stmt.ElseBranch(), vars, seen);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_stmt = static_cast<const WhileStatement&>(stmt);
      CollectVariablesInExpression(while_stmt.Condition(), vars, seen);
      CollectUsedVariables(while_stmt.Body(), vars, seen);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dowhile_stmt = static_cast<const DoWhileStatement&>(stmt);
      CollectUsedVariables(dowhile_stmt.Body(), vars, seen);
      CollectVariablesInExpression(dowhile_stmt.Condition(), vars, seen);
      break;
    }
    case CStatementKind::kReturnStatement: {
      const auto& ret = static_cast<const ReturnStatement&>(stmt);
      if (ret.ReturnValue() != nullptr) {
        CollectVariablesInExpression(*ret.ReturnValue(), vars, seen);
      }
      break;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_stmt = static_cast<const SwitchStatement&>(stmt);
      CollectVariablesInExpression(switch_stmt.Condition(), vars, seen);
      for (const auto& sc : switch_stmt.Cases()) {
        if (sc.body != nullptr) {
          CollectUsedVariables(*sc.body, vars, seen);
        }
      }
      break;
    }
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(stmt);
      if (for_stmt.Init() != nullptr) {
        CollectUsedVariables(*for_stmt.Init(), vars, seen);
      }
      if (for_stmt.Condition() != nullptr) {
        CollectVariablesInExpression(*for_stmt.Condition(), vars, seen);
      }
      if (for_stmt.Step() != nullptr) {
        CollectVariablesInExpression(*for_stmt.Step(), vars, seen);
      }
      CollectUsedVariables(for_stmt.Body(), vars, seen);
      break;
    }
    default:
      break;
  }
}

void CollectIdentifiersInExpression(const CExpression& expression, std::set<std::string>* out) {
  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier: {
      const std::string& identifier_name =
          static_cast<const IdentifierExpression&>(expression).Name();
      std::string_view prefix;
      std::string_view inner_identifier;
      if (ExtractFunctionPointerCastCalleeIdentifier(identifier_name, &prefix, &inner_identifier)) {
        out->insert(std::string(inner_identifier));
      } else {
        out->insert(identifier_name);
      }
      break;
    }
    case CExpressionKind::kUnaryExpression:
      CollectIdentifiersInExpression(static_cast<const UnaryExpression&>(expression).Operand(),
                                     out);
      break;
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      CollectIdentifiersInExpression(binary.Lhs(), out);
      CollectIdentifiersInExpression(binary.Rhs(), out);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      CollectIdentifiersInExpression(assignment.Lhs(), out);
      CollectIdentifiersInExpression(assignment.Rhs(), out);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      std::string_view prefix;
      std::string_view inner_identifier;
      if (call.Callee().Kind() == CExpressionKind::kIdentifier &&
          ExtractFunctionPointerCastCalleeIdentifier(
              static_cast<const IdentifierExpression&>(call.Callee()).Name(), &prefix,
              &inner_identifier)) {
        out->insert(std::string(inner_identifier));
      }
      for (const auto& argument : call.Arguments()) {
        CollectIdentifiersInExpression(*argument, out);
      }
      break;
    }
    case CExpressionKind::kCastExpression:
      CollectIdentifiersInExpression(static_cast<const CastExpression&>(expression).Operand(), out);
      break;
    case CExpressionKind::kMemberAccessExpression:
      CollectIdentifiersInExpression(
          static_cast<const MemberAccessExpression&>(expression).Object(), out);
      break;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      CollectIdentifiersInExpression(array_index.Array(), out);
      CollectIdentifiersInExpression(array_index.Index(), out);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      CollectIdentifiersInExpression(ternary.Condition(), out);
      CollectIdentifiersInExpression(ternary.TrueExpression(), out);
      CollectIdentifiersInExpression(ternary.FalseExpression(), out);
      break;
    }
    default:
      break;
  }
}

bool HasReturnValue(const CStatement& stmt) {
  if (stmt.Kind() == CStatementKind::kReturnStatement) {
    const auto& ret = static_cast<const ReturnStatement&>(stmt);
    return ret.ReturnValue() != nullptr;
  }
  if (stmt.Kind() == CStatementKind::kCompoundStatement) {
    const auto& comp = static_cast<const CompoundStatement&>(stmt);
    for (const auto& child : comp.Statements()) {
      if (HasReturnValue(*child)) return true;
    }
  } else if (stmt.Kind() == CStatementKind::kIfStatement) {
    const auto& if_stmt = static_cast<const IfStatement&>(stmt);
    if (HasReturnValue(if_stmt.ThenBranch())) return true;
    if (if_stmt.ElseBranch() != nullptr && HasReturnValue(*if_stmt.ElseBranch())) return true;
  } else if (stmt.Kind() == CStatementKind::kSwitchStatement) {
    const auto& switch_stmt = static_cast<const SwitchStatement&>(stmt);
    for (const auto& sc : switch_stmt.Cases()) {
      if (sc.body != nullptr && HasReturnValue(*sc.body)) return true;
    }
  }
  return false;
}

std::unique_ptr<CExpression> SubstituteIdentifier(const CExpression& expr, std::string_view target,
                                                  const CExpression& replacement) {
  if (expr.Kind() == CExpressionKind::kIdentifier) {
    const auto& id = static_cast<const IdentifierExpression&>(expr);
    if (id.Name() == target) {
      return replacement.Clone();
    }
    if (replacement.Kind() == CExpressionKind::kIdentifier && absl::StartsWith(id.Name(), "((") &&
        absl::EndsWith(id.Name(), ")")) {
      std::string suffix = absl::StrCat(")", target, ")");
      if (absl::EndsWith(id.Name(), suffix)) {
        std::string prefix = id.Name().substr(0, id.Name().size() - target.size() - 1);
        const auto& rep_id = static_cast<const IdentifierExpression&>(replacement);
        return CExpression::Identifier(absl::StrCat(prefix, rep_id.Name(), ")"));
      }
    }
    return expr.Clone();
  }
  if (expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(expr);
    return CExpression::Binary(bin.Op(), SubstituteIdentifier(bin.Lhs(), target, replacement),
                               SubstituteIdentifier(bin.Rhs(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& un = static_cast<const UnaryExpression&>(expr);
    if ((un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&" || un.Op() == "*(u32*)&" ||
         un.Op() == "*(u32 *)&") &&
        un.Operand().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(un.Operand()).Name() == target &&
        replacement.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& rep_un = static_cast<const UnaryExpression&>(replacement);
      if (rep_un.Op() == "*" && rep_un.Operand().Kind() == CExpressionKind::kCastExpression) {
        const auto& rep_cast = static_cast<const CastExpression&>(rep_un.Operand());
        if (rep_cast.TargetType().name == "f32" && rep_cast.TargetType().pointer_depth == 1) {
          CType int_ptr = rep_cast.TargetType();
          int_ptr.name = absl::StartsWith(un.Op(), "*(u32") ? "u32" : "s32";
          return CExpression::Unary("*", CExpression::Cast(int_ptr, rep_cast.Operand().Clone()));
        }
      }
    }
    bool is_fp_bitcast_op = (un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") &&
                            target.size() >= 2 && target[0] == 'f' &&
                            std::isdigit(static_cast<unsigned char>(target[1])) &&
                            replacement.Kind() != CExpressionKind::kFloatLiteral;
    if (un.Op().find('&') != std::string::npos &&
        replacement.Kind() != CExpressionKind::kIdentifier && !is_fp_bitcast_op) {
      if (un.Operand().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(un.Operand()).Name() == target) {
        return expr.Clone();
      }
    }
    return CExpression::Unary(un.Op(), SubstituteIdentifier(un.Operand(), target, replacement),
                              un.IsPrefix());
  }
  if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
    const auto& assign = static_cast<const AssignmentExpression&>(expr);
    std::unique_ptr<CExpression> new_lhs;
    if (assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
      if (replacement.Kind() == CExpressionKind::kIdentifier) {
        new_lhs = SubstituteIdentifier(assign.Lhs(), target, replacement);
      } else {
        new_lhs = assign.Lhs().Clone();
      }
    } else {
      new_lhs = SubstituteIdentifier(assign.Lhs(), target, replacement);
    }
    return CExpression::Assignment(assign.Op(), std::move(new_lhs),
                                   SubstituteIdentifier(assign.Rhs(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(expr);
    return CExpression::Cast(cast.TargetType(),
                             SubstituteIdentifier(cast.Operand(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kArrayIndexExpression) {
    const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
    return CExpression::ArrayIndex(SubstituteIdentifier(arr.Array(), target, replacement),
                                   SubstituteIdentifier(arr.Index(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kMemberAccessExpression) {
    const auto& mem = static_cast<const MemberAccessExpression&>(expr);
    return CExpression::MemberAccess(SubstituteIdentifier(mem.Object(), target, replacement),
                                     mem.MemberName(), mem.IsArrow());
  }
  if (expr.Kind() == CExpressionKind::kTernaryExpression) {
    const auto& tern = static_cast<const TernaryExpression&>(expr);
    return CExpression::Ternary(SubstituteIdentifier(tern.Condition(), target, replacement),
                                SubstituteIdentifier(tern.TrueExpression(), target, replacement),
                                SubstituteIdentifier(tern.FalseExpression(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kCallExpression) {
    const auto& call = static_cast<const CallExpression&>(expr);
    std::vector<std::unique_ptr<CExpression>> new_args;
    new_args.reserve(call.Arguments().size());
    for (size_t ai = 0; ai < call.Arguments().size(); ++ai) {
      const auto& arg = call.Arguments()[ai];
      if (ai >= 1 && arg != nullptr && arg->Kind() == CExpressionKind::kUnaryExpression &&
          target.size() >= 2 && target[0] == 'f' &&
          std::isdigit(static_cast<unsigned char>(target[1])) &&
          replacement.Kind() != CExpressionKind::kFloatLiteral) {
        const auto& un = static_cast<const UnaryExpression&>(*arg);
        if ((un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") &&
            un.Operand().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(un.Operand()).Name() == target) {
          new_args.push_back(CExpression::Unary(un.Op(), replacement.Clone(), un.IsPrefix()));
          continue;
        }
      }
      new_args.push_back(SubstituteIdentifier(*arg, target, replacement));
    }
    return CExpression::Call(SubstituteIdentifier(call.Callee(), target, replacement),
                             std::move(new_args));
  }
  return expr.Clone();
}

std::unique_ptr<CStatement> SubstituteIdentifierInStatement(std::unique_ptr<CStatement> stmt,
                                                            std::string_view from,
                                                            std::string_view to) {
  if (stmt == nullptr) return nullptr;
  auto to_id = CExpression::Identifier(std::string(to));
  switch (stmt->Kind()) {
    case CStatementKind::kCompoundStatement: {
      auto* comp = static_cast<CompoundStatement*>(stmt.get());
      for (auto& child : comp->MutableStatements()) {
        child = SubstituteIdentifierInStatement(std::move(child), from, to);
      }
      return stmt;
    }
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(*stmt);
      return CStatement::Expression(SubstituteIdentifier(es.Expression(), from, *to_id));
    }
    case CStatementKind::kReturnStatement: {
      const auto& ret = static_cast<const ReturnStatement&>(*stmt);
      if (ret.ReturnValue() != nullptr) {
        return CStatement::Return(SubstituteIdentifier(*ret.ReturnValue(), from, *to_id));
      }
      return stmt;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(*stmt);
      auto new_cond = SubstituteIdentifier(ifs.Condition(), from, *to_id);
      auto new_then = SubstituteIdentifierInStatement(ifs.ThenBranch().Clone(), from, to);
      std::unique_ptr<CStatement> new_else =
          ifs.ElseBranch() != nullptr
              ? SubstituteIdentifierInStatement(ifs.ElseBranch()->Clone(), from, to)
              : nullptr;
      return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(*stmt);
      return CStatement::While(SubstituteIdentifier(ws.Condition(), from, *to_id),
                               SubstituteIdentifierInStatement(ws.Body().Clone(), from, to));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(*stmt);
      return CStatement::DoWhile(SubstituteIdentifierInStatement(dws.Body().Clone(), from, to),
                                 SubstituteIdentifier(dws.Condition(), from, *to_id));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(*stmt);
      std::unique_ptr<CStatement> new_init =
          fs.Init() != nullptr ? SubstituteIdentifierInStatement(fs.Init()->Clone(), from, to)
                               : nullptr;
      std::unique_ptr<CExpression> new_cond =
          fs.Condition() != nullptr ? SubstituteIdentifier(*fs.Condition(), from, *to_id) : nullptr;
      std::unique_ptr<CExpression> new_step =
          fs.Step() != nullptr ? SubstituteIdentifier(*fs.Step(), from, *to_id) : nullptr;
      return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                             SubstituteIdentifierInStatement(fs.Body().Clone(), from, to));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(*stmt);
      auto new_cond = SubstituteIdentifier(ss.Condition(), from, *to_id);
      std::vector<SwitchCase> new_cases;
      for (const auto& c : ss.Cases()) {
        SwitchCase sc = c.Clone();
        if (c.body != nullptr) {
          auto rewritten = SubstituteIdentifierInStatement(c.body->Clone(), from, to);
          sc.body = std::unique_ptr<CompoundStatement>(
              static_cast<CompoundStatement*>(rewritten.release()));
        }
        new_cases.push_back(std::move(sc));
      }
      return CStatement::Switch(std::move(new_cond), std::move(new_cases));
    }
    default:
      return stmt;
  }
}

void ForEachSubCompoundStatement(CStatement* stmt,
                                 const std::function<void(CompoundStatement*)>& visitor) {
  if (stmt == nullptr) return;
  if (stmt->Kind() == CStatementKind::kCompoundStatement) {
    visitor(static_cast<CompoundStatement*>(stmt));
  } else if (stmt->Kind() == CStatementKind::kIfStatement) {
    auto* if_stmt = static_cast<IfStatement*>(stmt);
    if (if_stmt->MutableThenBranch() != nullptr &&
        if_stmt->MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(if_stmt->MutableThenBranch()));
    }
    if (if_stmt->MutableElseBranch() != nullptr &&
        if_stmt->MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(if_stmt->MutableElseBranch()));
    }
  } else if (stmt->Kind() == CStatementKind::kWhileStatement) {
    auto* while_stmt = static_cast<WhileStatement*>(stmt);
    if (while_stmt->MutableBody() != nullptr &&
        while_stmt->MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(while_stmt->MutableBody()));
    }
  } else if (stmt->Kind() == CStatementKind::kDoWhileStatement) {
    auto* do_while_stmt = static_cast<DoWhileStatement*>(stmt);
    if (do_while_stmt->MutableBody() != nullptr &&
        do_while_stmt->MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(do_while_stmt->MutableBody()));
    }
  } else if (stmt->Kind() == CStatementKind::kForStatement) {
    auto* for_stmt = static_cast<ForStatement*>(stmt);
    if (for_stmt->MutableBody() != nullptr &&
        for_stmt->MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(for_stmt->MutableBody()));
    }
  } else if (stmt->Kind() == CStatementKind::kSwitchStatement) {
    auto* sw_stmt = static_cast<SwitchStatement*>(stmt);
    for (auto& sc : sw_stmt->MutableCases()) {
      if (sc.body != nullptr) {
        visitor(sc.body.get());
      }
    }
  }
}

}  // namespace rom_nom_nom
