#include "tuner/ast_mutator.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "core/c_ast.h"

namespace rom_nom_nom {

namespace {

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

bool IsToggableCompoundAssignment(const AssignmentExpression& assignment) {
  if (assignment.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  const std::string& lhs_name = static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
  if (assignment.Op() == "+=" || assignment.Op() == "-=" || assignment.Op() == "&=" ||
      assignment.Op() == "|=" || assignment.Op() == "^=") {
    return true;
  }
  if (assignment.Op() == "=" && assignment.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
    const auto& binary = static_cast<const BinaryExpression&>(assignment.Rhs());
    if ((binary.Op() == "+" || binary.Op() == "-" || binary.Op() == "&" || binary.Op() == "|" ||
         binary.Op() == "^") &&
        binary.Lhs().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(binary.Lhs()).Name() == lhs_name) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<CExpression> ToggleCompoundAssignment(const AssignmentExpression& assignment) {
  if (assignment.Op() == "+=" || assignment.Op() == "-=" || assignment.Op() == "&=" ||
      assignment.Op() == "|=" || assignment.Op() == "^=") {
    std::string binary_operator(assignment.Op().substr(0, 1));
    return CExpression::Assignment(
        "=", assignment.Lhs().Clone(),
        CExpression::Binary(binary_operator, assignment.Lhs().Clone(), assignment.Rhs().Clone()));
  }
  const auto& binary = static_cast<const BinaryExpression&>(assignment.Rhs());
  std::string compound_operator = binary.Op() + "=";
  return CExpression::Assignment(compound_operator, assignment.Lhs().Clone(), binary.Rhs().Clone());
}

bool IsToggableIntegerCast(const CastExpression& cast) {
  const auto& target_type = cast.TargetType();
  return target_type.pointer_depth == 0 && !target_type.is_const &&
         (target_type.name == "s32" || target_type.name == "u32");
}

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

// Matches `*(T *)&symbol` where T is a scalar type (`u8`, `s8`, `u16`, `s16`, `u32`, `s32`, `f32`,
// `f64`).
bool ExtractGlobalCastTarget(const CExpression& expression, std::string* out_symbol = nullptr,
                             CType* out_type = nullptr) {
  if (expression.Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& dereference = static_cast<const UnaryExpression&>(expression);
  if (!dereference.IsPrefix() || dereference.Op() != "*") return false;
  if (dereference.Operand().Kind() != CExpressionKind::kCastExpression) return false;
  const auto& cast = static_cast<const CastExpression&>(dereference.Operand());
  const CType& pointer_type = cast.TargetType();
  if (pointer_type.pointer_depth != 1) return false;
  if (cast.Operand().Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& address_of = static_cast<const UnaryExpression&>(cast.Operand());
  if (!address_of.IsPrefix() || address_of.Op() != "&") return false;
  if (address_of.Operand().Kind() != CExpressionKind::kIdentifier) return false;
  const auto& identifier = static_cast<const IdentifierExpression&>(address_of.Operand());
  if (out_symbol != nullptr) *out_symbol = identifier.Name();
  if (out_type != nullptr) {
    *out_type =
        CType{.name = pointer_type.name, .is_const = pointer_type.is_const, .pointer_depth = 0};
  }
  return true;
}

std::optional<int64_t> TypeByteSize(std::string_view type_name) {
  if (type_name == "u8" || type_name == "s8") return 1;
  if (type_name == "u16" || type_name == "s16") return 2;
  if (type_name == "u32" || type_name == "s32" || type_name == "f32") return 4;
  if (type_name == "u64" || type_name == "s64" || type_name == "f64") return 8;
  return std::nullopt;
}

// Matches `*(T *)(base + offset)` or `*(T *)(base - offset)` where `offset % sizeof(T) == 0`.
bool MatchPointerOffsetDereference(const CExpression& expression, CType* out_pointer_type = nullptr,
                                   const CExpression** out_base = nullptr,
                                   int64_t* out_element_index = nullptr) {
  if (expression.Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& dereference = static_cast<const UnaryExpression&>(expression);
  if (!dereference.IsPrefix() || dereference.Op() != "*") return false;
  if (dereference.Operand().Kind() != CExpressionKind::kCastExpression) return false;
  const auto& cast = static_cast<const CastExpression&>(dereference.Operand());
  const CType& pointer_type = cast.TargetType();
  if (pointer_type.pointer_depth != 1) return false;
  auto element_size_or = TypeByteSize(pointer_type.name);
  if (!element_size_or.has_value() || *element_size_or <= 0) return false;
  int64_t element_size = *element_size_or;

  if (cast.Operand().Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& binary = static_cast<const BinaryExpression&>(cast.Operand());
  if (binary.Op() != "+" && binary.Op() != "-") return false;
  if (binary.Rhs().Kind() != CExpressionKind::kIntegerLiteral) return false;
  // Exclude `(u8 *)&symbol + offset` so global byte-indexed array patterns stay handled by global
  // array mutations.
  if (binary.Lhs().Kind() == CExpressionKind::kCastExpression) {
    const auto& lhs_cast = static_cast<const CastExpression&>(binary.Lhs());
    if (lhs_cast.Operand().Kind() == CExpressionKind::kUnaryExpression &&
        static_cast<const UnaryExpression&>(lhs_cast.Operand()).Op() == "&") {
      return false;
    }
  }

  int64_t byte_offset = static_cast<const IntegerLiteralExpression&>(binary.Rhs()).Value();
  if (binary.Op() == "-") byte_offset = -byte_offset;
  if (byte_offset % element_size != 0) return false;

  if (out_pointer_type != nullptr) *out_pointer_type = pointer_type;
  if (out_base != nullptr) *out_base = &binary.Lhs();
  if (out_element_index != nullptr) *out_element_index = byte_offset / element_size;
  return true;
}

bool ExpressionReadsIndirectMemory(const CExpression& expression) {
  std::string symbol;
  if (ExtractGlobalCastTarget(expression, &symbol)) {
    return false;
  }
  switch (expression.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      if (unary.IsPrefix() && unary.Op() == "*") return true;
      if (unary.IsPrefix() && unary.Op() == "&") return false;
      return ExpressionReadsIndirectMemory(unary.Operand());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      if (array_index.Array().Kind() == CExpressionKind::kIdentifier) {
        return ExpressionReadsIndirectMemory(array_index.Index());
      }
      return true;
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return ExpressionReadsIndirectMemory(binary.Lhs()) ||
             ExpressionReadsIndirectMemory(binary.Rhs());
    }
    case CExpressionKind::kCastExpression:
      return ExpressionReadsIndirectMemory(
          static_cast<const CastExpression&>(expression).Operand());
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return member.IsArrow() || ExpressionReadsIndirectMemory(member.Object());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return ExpressionReadsIndirectMemory(ternary.Condition()) ||
             ExpressionReadsIndirectMemory(ternary.TrueExpression()) ||
             ExpressionReadsIndirectMemory(ternary.FalseExpression());
    }
    default:
      return false;
  }
}

bool ExtractLhsTargetIdentifier(const CExpression& lhs, std::string* out_name,
                                bool* out_is_indirect_memory) {
  if (out_is_indirect_memory != nullptr) *out_is_indirect_memory = false;
  if (lhs.Kind() == CExpressionKind::kIdentifier) {
    if (out_name != nullptr) *out_name = static_cast<const IdentifierExpression&>(lhs).Name();
    return true;
  }
  std::string symbol;
  if (ExtractGlobalCastTarget(lhs, &symbol)) {
    if (out_name != nullptr) *out_name = symbol;
    return true;
  }
  if (lhs.Kind() == CExpressionKind::kArrayIndexExpression) {
    const auto& array_index = static_cast<const ArrayIndexExpression&>(lhs);
    if (array_index.Array().Kind() == CExpressionKind::kIdentifier &&
        !ExpressionReadsIndirectMemory(array_index.Index())) {
      if (out_name != nullptr) {
        *out_name = static_cast<const IdentifierExpression&>(array_index.Array()).Name();
      }
      return true;
    }
  }
  if (out_is_indirect_memory != nullptr) *out_is_indirect_memory = true;
  return false;
}

bool StatementContainsJumpOrLabel(const CStatement& statement) {
  switch (statement.Kind()) {
    case CStatementKind::kGotoStatement:
    case CStatementKind::kLabelStatement:
    case CStatementKind::kBreakStatement:
    case CStatementKind::kContinueStatement:
    case CStatementKind::kReturnStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      for (const auto& child_statement :
           static_cast<const CompoundStatement&>(statement).Statements()) {
        if (StatementContainsJumpOrLabel(*child_statement)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return StatementContainsJumpOrLabel(if_statement.ThenBranch()) ||
             (if_statement.ElseBranch() != nullptr &&
              StatementContainsJumpOrLabel(*if_statement.ElseBranch()));
    }
    case CStatementKind::kWhileStatement:
      return StatementContainsJumpOrLabel(static_cast<const WhileStatement&>(statement).Body());
    case CStatementKind::kDoWhileStatement:
      return StatementContainsJumpOrLabel(static_cast<const DoWhileStatement&>(statement).Body());
    case CStatementKind::kForStatement:
      return StatementContainsJumpOrLabel(static_cast<const ForStatement&>(statement).Body());
    case CStatementKind::kSwitchStatement:
      return true;
    default:
      return false;
  }
}

bool StatementContainsGotoOrLabel(const CStatement& statement) {
  switch (statement.Kind()) {
    case CStatementKind::kGotoStatement:
    case CStatementKind::kLabelStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      for (const auto& child_statement :
           static_cast<const CompoundStatement&>(statement).Statements()) {
        if (StatementContainsGotoOrLabel(*child_statement)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return StatementContainsGotoOrLabel(if_statement.ThenBranch()) ||
             (if_statement.ElseBranch() != nullptr &&
              StatementContainsGotoOrLabel(*if_statement.ElseBranch()));
    }
    case CStatementKind::kWhileStatement:
      return StatementContainsGotoOrLabel(static_cast<const WhileStatement&>(statement).Body());
    case CStatementKind::kDoWhileStatement:
      return StatementContainsGotoOrLabel(static_cast<const DoWhileStatement&>(statement).Body());
    case CStatementKind::kForStatement:
      return StatementContainsGotoOrLabel(static_cast<const ForStatement&>(statement).Body());
    case CStatementKind::kSwitchStatement: {
      for (const auto& switch_case : static_cast<const SwitchStatement&>(statement).Cases()) {
        if (switch_case.body != nullptr && StatementContainsGotoOrLabel(*switch_case.body)) {
          return true;
        }
      }
      return false;
    }
    default:
      return false;
  }
}

int CountNonDeclarationOccurrencesInStatement(const CStatement& statement, std::string_view name) {
  if (statement.Kind() == CStatementKind::kVariableDeclarationStatement) {
    const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
    return declaration.Initializer() != nullptr
               ? CountIdentifierOccurrencesInExpression(*declaration.Initializer(), name)
               : 0;
  }
  if (statement.Kind() == CStatementKind::kCompoundStatement) {
    int total = 0;
    for (const auto& child_statement :
         static_cast<const CompoundStatement&>(statement).Statements()) {
      total += CountNonDeclarationOccurrencesInStatement(*child_statement, name);
    }
    return total;
  }
  return CountIdentifierOccurrencesInStatement(statement, name);
}

bool IsUnconditionalOverwriteOfVariable(const CStatement& statement,
                                        std::string_view variable_name) {
  if (statement.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expression = static_cast<const ExpressionStatement&>(statement).Expression();
  if (expression.Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assignment = static_cast<const AssignmentExpression&>(expression);
  return assignment.Op() == "=" && assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
         static_cast<const IdentifierExpression&>(assignment.Lhs()).Name() == variable_name &&
         CountIdentifierOccurrencesInExpression(assignment.Rhs(), variable_name) == 0;
}

bool DoesStatementReadVariableBeforeOverwrite(const CStatement& statement,
                                              std::string_view variable_name,
                                              bool* out_always_overwrites = nullptr) {
  if (out_always_overwrites != nullptr) *out_always_overwrites = false;
  switch (statement.Kind()) {
    case CStatementKind::kExpressionStatement: {
      if (IsUnconditionalOverwriteOfVariable(statement, variable_name)) {
        if (out_always_overwrites != nullptr) *out_always_overwrites = true;
        return false;
      }
      return CountIdentifierOccurrencesInStatement(statement, variable_name) > 0;
    }
    case CStatementKind::kCompoundStatement: {
      const auto& compound = static_cast<const CompoundStatement&>(statement);
      for (const auto& child_statement : compound.Statements()) {
        bool sub_overwrites = false;
        if (DoesStatementReadVariableBeforeOverwrite(*child_statement, variable_name,
                                                     &sub_overwrites)) {
          return true;
        }
        if (sub_overwrites) {
          if (out_always_overwrites != nullptr) *out_always_overwrites = true;
          return false;
        }
        if (child_statement->Kind() == CStatementKind::kReturnStatement ||
            child_statement->Kind() == CStatementKind::kBreakStatement ||
            child_statement->Kind() == CStatementKind::kContinueStatement ||
            child_statement->Kind() == CStatementKind::kGotoStatement) {
          break;
        }
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      if (CountIdentifierOccurrencesInExpression(if_statement.Condition(), variable_name) > 0) {
        return true;
      }
      bool then_overwrites = false;
      if (DoesStatementReadVariableBeforeOverwrite(if_statement.ThenBranch(), variable_name,
                                                   &then_overwrites)) {
        return true;
      }
      bool else_overwrites = false;
      if (if_statement.ElseBranch() != nullptr &&
          DoesStatementReadVariableBeforeOverwrite(*if_statement.ElseBranch(), variable_name,
                                                   &else_overwrites)) {
        return true;
      }
      if (if_statement.ElseBranch() != nullptr && then_overwrites && else_overwrites &&
          out_always_overwrites != nullptr) {
        *out_always_overwrites = true;
      }
      return false;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      if (CountIdentifierOccurrencesInExpression(while_statement.Condition(), variable_name) > 0) {
        return true;
      }
      bool body_overwrites = false;
      if (DoesStatementReadVariableBeforeOverwrite(while_statement.Body(), variable_name,
                                                   &body_overwrites)) {
        return true;
      }
      if (!body_overwrites) {
        return CountIdentifierOccurrencesInStatement(while_statement.Body(), variable_name) > 0;
      }
      if (while_statement.Condition().Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(while_statement.Condition()).Value() != 0 &&
          out_always_overwrites != nullptr) {
        *out_always_overwrites = true;
      }
      return false;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      bool body_overwrites = false;
      if (DoesStatementReadVariableBeforeOverwrite(do_while_statement.Body(), variable_name,
                                                   &body_overwrites)) {
        return true;
      }
      if (body_overwrites) {
        if (out_always_overwrites != nullptr) {
          *out_always_overwrites = true;
        }
        return false;
      }
      return CountIdentifierOccurrencesInStatement(do_while_statement, variable_name) > 0;
    }
    default:
      return CountIdentifierOccurrencesInStatement(statement, variable_name) > 0;
  }
}

bool NonLoopStatementContainsBlock(
    const CStatement& statement,
    const std::vector<std::unique_ptr<CStatement>>* target_statements) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& compound = static_cast<const CompoundStatement&>(statement);
      if (&compound.Statements() == target_statements) return true;
      for (const auto& child_statement : compound.Statements()) {
        if (NonLoopStatementContainsBlock(*child_statement, target_statements)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return NonLoopStatementContainsBlock(if_statement.ThenBranch(), target_statements) ||
             (if_statement.ElseBranch() != nullptr &&
              NonLoopStatementContainsBlock(*if_statement.ElseBranch(), target_statements));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_statement = static_cast<const SwitchStatement&>(statement);
      for (const auto& switch_case : switch_statement.Cases()) {
        if (switch_case.body != nullptr &&
            NonLoopStatementContainsBlock(*switch_case.body, target_statements)) {
          return true;
        }
      }
      return false;
    }
    default:
      return false;
  }
}

bool IsVariableDeadAfter(const std::vector<std::unique_ptr<CStatement>>& statements,
                         size_t after_index, std::string_view variable_name,
                         bool is_top_level_block, const CompoundStatement* root_body = nullptr) {
  for (size_t k = after_index + 1; k < statements.size(); ++k) {
    bool always_overwrites = false;
    if (DoesStatementReadVariableBeforeOverwrite(*statements[k], variable_name,
                                                 &always_overwrites)) {
      return false;
    }
    if (always_overwrites) {
      return true;
    }
    if (statements[k]->Kind() == CStatementKind::kReturnStatement) {
      return true;
    }
    if (StatementContainsJumpOrLabel(*statements[k])) {
      return false;
    }
  }
  if (is_top_level_block) return true;
  if (root_body != nullptr && !StatementContainsGotoOrLabel(*root_body)) {
    if (CountNonDeclarationOccurrencesInStatement(*root_body, variable_name) <= 2) {
      return true;
    }
    const auto& root_statements = root_body->Statements();
    for (size_t top_index = 0; top_index < root_statements.size(); ++top_index) {
      if (NonLoopStatementContainsBlock(*root_statements[top_index], &statements)) {
        return IsVariableDeadAfter(root_statements, top_index, variable_name,
                                   /*is_top_level_block=*/true, nullptr);
      }
      if (root_statements[top_index]->Kind() == CStatementKind::kWhileStatement) {
        const auto& while_statement =
            static_cast<const WhileStatement&>(*root_statements[top_index]);
        if (while_statement.Body().Kind() == CStatementKind::kCompoundStatement &&
            &static_cast<const CompoundStatement&>(while_statement.Body()).Statements() ==
                &statements &&
            CountIdentifierOccurrencesInExpression(while_statement.Condition(), variable_name) ==
                0 &&
            !statements.empty() &&
            IsUnconditionalOverwriteOfVariable(*statements.front(), variable_name)) {
          if (while_statement.Condition().Kind() == CExpressionKind::kIntegerLiteral &&
              static_cast<const IntegerLiteralExpression&>(while_statement.Condition()).Value() !=
                  0) {
            return true;
          }
          return IsVariableDeadAfter(root_statements, top_index, variable_name,
                                     /*is_top_level_block=*/true, nullptr);
        }
      }
      if (root_statements[top_index]->Kind() == CStatementKind::kDoWhileStatement) {
        const auto& do_while_statement =
            static_cast<const DoWhileStatement&>(*root_statements[top_index]);
        if (do_while_statement.Body().Kind() == CStatementKind::kCompoundStatement &&
            &static_cast<const CompoundStatement&>(do_while_statement.Body()).Statements() ==
                &statements &&
            CountIdentifierOccurrencesInExpression(do_while_statement.Condition(), variable_name) ==
                0 &&
            !statements.empty() &&
            IsUnconditionalOverwriteOfVariable(*statements.front(), variable_name)) {
          return IsVariableDeadAfter(root_statements, top_index, variable_name,
                                     /*is_top_level_block=*/true, nullptr);
        }
      }
    }
  }
  return false;
}

bool IsReadModifyWriteThroughVariable(const AssignmentExpression& assignment,
                                      std::string_view variable_name,
                                      std::string* out_compound_operator = nullptr,
                                      const CExpression** out_rhs = nullptr) {
  if (assignment.Op() != "=" || assignment.Lhs().Kind() != CExpressionKind::kUnaryExpression) {
    return false;
  }
  const auto& lhs_unary = static_cast<const UnaryExpression&>(assignment.Lhs());
  if (!lhs_unary.IsPrefix() || lhs_unary.Op() != "*") return false;
  if (CountIdentifierOccurrencesInExpression(assignment.Lhs(), variable_name) != 1) return false;
  if (assignment.Rhs().Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& binary = static_cast<const BinaryExpression&>(assignment.Rhs());
  if (binary.Op() != "+" && binary.Op() != "-" && binary.Op() != "|" && binary.Op() != "&" &&
      binary.Op() != "^") {
    return false;
  }
  if (CountIdentifierOccurrencesInExpression(binary.Rhs(), variable_name) != 0) return false;
  if (assignment.Lhs().ToString() != binary.Lhs().ToString()) return false;
  if (out_compound_operator != nullptr) *out_compound_operator = binary.Op() + "=";
  if (out_rhs != nullptr) *out_rhs = &binary.Rhs();
  return true;
}

std::unique_ptr<CExpression> ReplaceIdentifierInExpression(const CExpression& expression,
                                                           std::string_view target_name,
                                                           const CExpression& replacement) {
  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& identifier = static_cast<const IdentifierExpression&>(expression);
      if (identifier.Name() == target_name) {
        return replacement.Clone();
      }
      std::string_view prefix;
      std::string_view inner_identifier;
      if (ExtractFunctionPointerCastCalleeIdentifier(identifier.Name(), &prefix,
                                                     &inner_identifier) &&
          inner_identifier == target_name) {
        return CExpression::Identifier(std::string(prefix) + replacement.ToString() + ")");
      }
      return expression.Clone();
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      return CExpression::Unary(
          unary.Op(), ReplaceIdentifierInExpression(unary.Operand(), target_name, replacement),
          unary.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return CExpression::Binary(
          binary.Op(), ReplaceIdentifierInExpression(binary.Lhs(), target_name, replacement),
          ReplaceIdentifierInExpression(binary.Rhs(), target_name, replacement));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      std::string compound_operator;
      const CExpression* compound_rhs = nullptr;
      if (IsReadModifyWriteThroughVariable(assignment, target_name, &compound_operator,
                                           &compound_rhs)) {
        return CExpression::Assignment(
            compound_operator,
            ReplaceIdentifierInExpression(assignment.Lhs(), target_name, replacement),
            ReplaceIdentifierInExpression(*compound_rhs, target_name, replacement));
      }
      if (assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assignment.Lhs()).Name() == target_name &&
          (assignment.Op() == "+=" || assignment.Op() == "-=" || assignment.Op() == "|=" ||
           assignment.Op() == "&=" || assignment.Op() == "^=") &&
          CountIdentifierOccurrencesInExpression(assignment.Rhs(), target_name) == 0) {
        std::string binary_operator(assignment.Op().substr(0, 1));
        return CExpression::Assignment(
            "=", assignment.Lhs().Clone(),
            CExpression::Binary(binary_operator, replacement.Clone(), assignment.Rhs().Clone()));
      }
      std::unique_ptr<CExpression> new_lhs =
          (assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
           static_cast<const IdentifierExpression&>(assignment.Lhs()).Name() == target_name &&
           assignment.Op() == "=")
              ? assignment.Lhs().Clone()
              : ReplaceIdentifierInExpression(assignment.Lhs(), target_name, replacement);
      return CExpression::Assignment(
          assignment.Op(), std::move(new_lhs),
          ReplaceIdentifierInExpression(assignment.Rhs(), target_name, replacement));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      std::vector<std::unique_ptr<CExpression>> arguments;
      arguments.reserve(call.Arguments().size());
      for (const auto& argument : call.Arguments()) {
        arguments.push_back(ReplaceIdentifierInExpression(*argument, target_name, replacement));
      }
      return CExpression::Call(
          ReplaceIdentifierInExpression(call.Callee(), target_name, replacement),
          std::move(arguments));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      return CExpression::Cast(cast.TargetType(), ReplaceIdentifierInExpression(
                                                      cast.Operand(), target_name, replacement));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return CExpression::MemberAccess(
          ReplaceIdentifierInExpression(member.Object(), target_name, replacement),
          member.MemberName(), member.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return CExpression::ArrayIndex(
          ReplaceIdentifierInExpression(array_index.Array(), target_name, replacement),
          ReplaceIdentifierInExpression(array_index.Index(), target_name, replacement));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CExpression::Ternary(
          ReplaceIdentifierInExpression(ternary.Condition(), target_name, replacement),
          ReplaceIdentifierInExpression(ternary.TrueExpression(), target_name, replacement),
          ReplaceIdentifierInExpression(ternary.FalseExpression(), target_name, replacement));
    }
    default:
      return expression.Clone();
  }
}

std::unique_ptr<CStatement> ReplaceIdentifierInSingleStatement(const CStatement& statement,
                                                               std::string_view target_name,
                                                               const CExpression& replacement) {
  switch (statement.Kind()) {
    case CStatementKind::kExpressionStatement: {
      const auto& expression_statement = static_cast<const ExpressionStatement&>(statement);
      return CStatement::Expression(ReplaceIdentifierInExpression(expression_statement.Expression(),
                                                                  target_name, replacement));
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() == nullptr) return return_statement.Clone();
      return CStatement::Return(
          ReplaceIdentifierInExpression(*return_statement.ReturnValue(), target_name, replacement));
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return CStatement::If(
          ReplaceIdentifierInExpression(if_statement.Condition(), target_name, replacement),
          if_statement.ThenBranch().Clone(),
          if_statement.ElseBranch() ? if_statement.ElseBranch()->Clone() : nullptr);
    }
    default:
      return statement.Clone();
  }
}

struct InlineCandidateInfo {
  size_t use_index = 0;
  std::string variable_name;
  const CExpression* rhs = nullptr;
  bool can_sink_to_use = false;
  bool can_hoist_to_def = false;
};

bool AnalyzeInlineSingleUseTemporaryAt(const std::vector<std::unique_ptr<CStatement>>& statements,
                                       size_t index, bool is_top_level_block,
                                       InlineCandidateInfo* out_info,
                                       const CompoundStatement* root_body = nullptr) {
  if (index + 1 >= statements.size()) return false;
  if (statements[index]->Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expression_statement = static_cast<const ExpressionStatement&>(*statements[index]);
  if (expression_statement.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
    return false;
  }
  const auto& assignment =
      static_cast<const AssignmentExpression&>(expression_statement.Expression());
  if (assignment.Op() != "=" || assignment.Lhs().Kind() != CExpressionKind::kIdentifier) {
    return false;
  }
  if (!IsPureExpression(assignment.Rhs())) return false;

  const std::string& variable_name =
      static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
  const CExpression& rhs = assignment.Rhs();

  std::set<std::string> rhs_identifiers;
  CollectIdentifiersInExpression(rhs, &rhs_identifiers);
  const bool rhs_reads_indirect_memory = ExpressionReadsIndirectMemory(rhs);

  // Find the first subsequent statement `use_index` that references `variable_name`.
  std::optional<size_t> found_use_index;
  bool can_sink = true;
  for (size_t m = index + 1; m < statements.size(); ++m) {
    if (CountIdentifierOccurrencesInStatement(*statements[m], variable_name) > 0) {
      found_use_index = m;
      break;
    }
    // Check if `statements[m]` is a pure assignment so we can potentially cross it.
    if (statements[m]->Kind() != CStatementKind::kExpressionStatement) {
      return false;
    }
    const auto& middle_expression =
        static_cast<const ExpressionStatement&>(*statements[m]).Expression();
    if (middle_expression.Kind() != CExpressionKind::kAssignmentExpression) {
      return false;
    }
    const auto& middle_assignment = static_cast<const AssignmentExpression&>(middle_expression);
    if (middle_assignment.Op() != "=" || !IsPureExpression(middle_assignment.Lhs()) ||
        !IsPureExpression(middle_assignment.Rhs())) {
      return false;
    }
    std::string middle_written_identifier;
    bool middle_writes_indirect = false;
    if (ExtractLhsTargetIdentifier(middle_assignment.Lhs(), &middle_written_identifier,
                                   &middle_writes_indirect)) {
      if (rhs_identifiers.count(middle_written_identifier) > 0) {
        can_sink = false;
      }
    }
    if (middle_writes_indirect && (rhs_reads_indirect_memory || !rhs_identifiers.empty())) {
      can_sink = false;
    }
  }

  if (!found_use_index.has_value()) return false;
  const size_t use_index = *found_use_index;
  const CStatement& use_statement = *statements[use_index];

  int use_count = 0;
  bool use_kills_variable = false;
  if (use_statement.Kind() == CStatementKind::kExpressionStatement) {
    const auto& use_expression_statement = static_cast<const ExpressionStatement&>(use_statement);
    if (use_expression_statement.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& use_assignment =
          static_cast<const AssignmentExpression&>(use_expression_statement.Expression());
      if (use_assignment.Op() == "=" &&
          use_assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(use_assignment.Lhs()).Name() == variable_name) {
        use_count = CountIdentifierOccurrencesInExpression(use_assignment.Rhs(), variable_name);
        use_kills_variable = true;
      } else if ((use_assignment.Op() == "+=" || use_assignment.Op() == "-=" ||
                  use_assignment.Op() == "|=" || use_assignment.Op() == "&=" ||
                  use_assignment.Op() == "^=") &&
                 use_assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
                 static_cast<const IdentifierExpression&>(use_assignment.Lhs()).Name() ==
                     variable_name &&
                 CountIdentifierOccurrencesInExpression(use_assignment.Rhs(), variable_name) == 0) {
        use_count = 1;
        use_kills_variable = true;
      } else if (IsReadModifyWriteThroughVariable(use_assignment, variable_name)) {
        use_count = 1;
      } else {
        use_count = CountIdentifierOccurrencesInExpression(use_expression_statement.Expression(),
                                                           variable_name);
      }
    } else {
      use_count = CountIdentifierOccurrencesInExpression(use_expression_statement.Expression(),
                                                         variable_name);
    }
  } else if (use_statement.Kind() == CStatementKind::kReturnStatement) {
    const auto& return_statement = static_cast<const ReturnStatement&>(use_statement);
    if (return_statement.ReturnValue() != nullptr) {
      use_count =
          CountIdentifierOccurrencesInExpression(*return_statement.ReturnValue(), variable_name);
    }
    use_kills_variable = true;
  } else if (use_statement.Kind() == CStatementKind::kIfStatement) {
    const auto& if_statement = static_cast<const IfStatement&>(use_statement);
    if (CountIdentifierOccurrencesInStatement(if_statement.ThenBranch(), variable_name) == 0 &&
        (if_statement.ElseBranch() == nullptr ||
         CountIdentifierOccurrencesInStatement(*if_statement.ElseBranch(), variable_name) == 0)) {
      use_count = CountIdentifierOccurrencesInExpression(if_statement.Condition(), variable_name);
    }
  }

  if (use_count != 1) return false;
  if (!use_kills_variable &&
      !IsVariableDeadAfter(statements, use_index, variable_name, is_top_level_block, root_body)) {
    return false;
  }

  // Check if `use_statement` can be hoisted up to `index` when `use_index > index + 1`.
  bool can_hoist = (use_index == index + 1);
  if (use_index > index + 1 && use_statement.Kind() == CStatementKind::kExpressionStatement) {
    const auto& use_expression_statement = static_cast<const ExpressionStatement&>(use_statement);
    if (use_expression_statement.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& use_assignment =
          static_cast<const AssignmentExpression&>(use_expression_statement.Expression());
      if (use_assignment.Op() == "=" && IsPureExpression(use_assignment.Lhs()) &&
          IsPureExpression(use_assignment.Rhs())) {
        std::string use_written_identifier;
        bool use_writes_indirect = false;
        if (ExtractLhsTargetIdentifier(use_assignment.Lhs(), &use_written_identifier,
                                       &use_writes_indirect) &&
            !use_writes_indirect) {
          auto placeholder_statement = ReplaceIdentifierInSingleStatement(
              use_statement, variable_name, *CExpression::Identifier("__inline_placeholder__"));
          const auto& placeholder_expression =
              static_cast<const ExpressionStatement&>(*placeholder_statement).Expression();
          std::set<std::string> use_identifiers;
          CollectIdentifiersInExpression(placeholder_expression, &use_identifiers);
          use_identifiers.erase("__inline_placeholder__");
          bool use_reads_indirect = ExpressionReadsIndirectMemory(placeholder_expression);

          can_hoist = true;
          for (size_t m = index + 1; m < use_index; ++m) {
            const auto& middle_assignment = static_cast<const AssignmentExpression&>(
                static_cast<const ExpressionStatement&>(*statements[m]).Expression());
            std::string middle_written_identifier;
            bool middle_writes_indirect = false;
            ExtractLhsTargetIdentifier(middle_assignment.Lhs(), &middle_written_identifier,
                                       &middle_writes_indirect);
            if (!middle_written_identifier.empty() &&
                use_identifiers.count(middle_written_identifier) > 0) {
              can_hoist = false;
              break;
            }
            if (CountIdentifierOccurrencesInStatement(*statements[m], use_written_identifier) > 0) {
              can_hoist = false;
              break;
            }
            if (middle_writes_indirect && (use_reads_indirect || !use_identifiers.empty())) {
              can_hoist = false;
              break;
            }
          }
        }
      }
    }
  }

  if (!can_sink && !can_hoist) return false;

  if (out_info != nullptr) {
    out_info->use_index = use_index;
    out_info->variable_name = variable_name;
    out_info->rhs = &rhs;
    out_info->can_sink_to_use = can_sink;
    out_info->can_hoist_to_def = can_hoist;
  }
  return true;
}

bool CanSwapAdjacentStatements(const std::vector<std::unique_ptr<CStatement>>& statements,
                               size_t index) {
  if (index + 1 >= statements.size()) return false;
  if (statements[index]->Kind() != CStatementKind::kExpressionStatement ||
      statements[index + 1]->Kind() != CStatementKind::kExpressionStatement) {
    return false;
  }
  const auto& first_expression =
      static_cast<const ExpressionStatement&>(*statements[index]).Expression();
  const auto& second_expression =
      static_cast<const ExpressionStatement&>(*statements[index + 1]).Expression();
  if (first_expression.Kind() != CExpressionKind::kAssignmentExpression ||
      second_expression.Kind() != CExpressionKind::kAssignmentExpression) {
    return false;
  }
  const auto& first_assignment = static_cast<const AssignmentExpression&>(first_expression);
  const auto& second_assignment = static_cast<const AssignmentExpression&>(second_expression);
  if (first_assignment.Op() != "=" || second_assignment.Op() != "=" ||
      !IsPureExpression(first_assignment.Lhs()) || !IsPureExpression(first_assignment.Rhs()) ||
      !IsPureExpression(second_assignment.Lhs()) || !IsPureExpression(second_assignment.Rhs())) {
    return false;
  }

  std::string first_written;
  bool first_indirect = false;
  ExtractLhsTargetIdentifier(first_assignment.Lhs(), &first_written, &first_indirect);

  std::string second_written;
  bool second_indirect = false;
  ExtractLhsTargetIdentifier(second_assignment.Lhs(), &second_written, &second_indirect);

  if (first_indirect && second_indirect) return false;
  if (!first_written.empty() && !second_written.empty() && first_written == second_written) {
    return false;
  }
  if (!first_written.empty() &&
      CountIdentifierOccurrencesInExpression(second_expression, first_written) > 0) {
    return false;
  }
  if (!second_written.empty() &&
      CountIdentifierOccurrencesInExpression(first_expression, second_written) > 0) {
    return false;
  }
  if (first_indirect && ExpressionReadsIndirectMemory(second_expression)) return false;
  if (second_indirect && ExpressionReadsIndirectMemory(first_expression)) return false;

  return true;
}

bool IsDeadAssignmentAt(const std::vector<std::unique_ptr<CStatement>>& statements, size_t index,
                        bool is_top_level_block, const std::set<std::string>& local_variables,
                        bool* out_is_call = nullptr, const CompoundStatement* root_body = nullptr) {
  if (statements[index]->Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expression = static_cast<const ExpressionStatement&>(*statements[index]).Expression();
  if (expression.Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assignment = static_cast<const AssignmentExpression&>(expression);
  if (assignment.Op() != "=" || assignment.Lhs().Kind() != CExpressionKind::kIdentifier) {
    return false;
  }
  const std::string& variable_name =
      static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
  if (local_variables.count(variable_name) == 0) return false;

  const CExpression* rhs = &assignment.Rhs();
  bool is_call = false;
  if (rhs->Kind() == CExpressionKind::kCallExpression) {
    is_call = true;
  } else if (rhs->Kind() == CExpressionKind::kCastExpression &&
             static_cast<const CastExpression&>(*rhs).Operand().Kind() ==
                 CExpressionKind::kCallExpression) {
    is_call = true;
  } else if (!IsPureExpression(*rhs)) {
    return false;
  }

  if (!IsVariableDeadAfter(statements, index, variable_name, is_top_level_block, root_body)) {
    return false;
  }
  if (out_is_call != nullptr) *out_is_call = is_call;
  return true;
}

std::unique_ptr<CompoundStatement> PruneUnusedLocalDeclarations(const CompoundStatement& body) {
  const auto& statements = body.Statements();
  std::vector<std::unique_ptr<CStatement>> new_statements;
  new_statements.reserve(statements.size());

  for (size_t i = 0; i < statements.size(); ++i) {
    if (statements[i]->Kind() == CStatementKind::kVariableDeclarationStatement) {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(*statements[i]);
      if ((declaration.Initializer() == nullptr || IsPureExpression(*declaration.Initializer())) &&
          !declaration.ArraySize().has_value()) {
        int uses = 0;
        for (size_t j = 0; j < statements.size(); ++j) {
          if (j == i) continue;
          uses += CountIdentifierOccurrencesInStatement(*statements[j], declaration.Name());
        }
        if (uses == 0) {
          continue;
        }
        if (declaration.Initializer() != nullptr &&
            IsVariableDeadAfter(statements, i, declaration.Name(), /*is_top_level_block=*/true,
                                &body)) {
          new_statements.push_back(CStatement::VariableDeclaration(
              declaration.Type(), declaration.Name(), nullptr, declaration.ArraySize()));
          continue;
        }
      }
    }
    new_statements.push_back(statements[i]->Clone());
  }
  if (!new_statements.empty() &&
      new_statements.back()->Kind() == CStatementKind::kReturnStatement &&
      static_cast<const ReturnStatement&>(*new_statements.back()).ReturnValue() == nullptr) {
    new_statements.pop_back();
  }
  return std::make_unique<CompoundStatement>(std::move(new_statements));
}

// Collects global symbols that are only ever accessed via `*(T *)&symbol` with a consistent width.
struct DirectTypedGlobalCandidate {
  std::string symbol_name;
  CType resolved_type;
};

void ScanDirectTypedGlobalsInExpression(const CExpression& expression,
                                        const std::set<std::string>& local_variables,
                                        std::map<std::string, std::set<std::string>>* cast_types,
                                        std::set<std::string>* non_cast_uses) {
  std::string symbol_name;
  CType resolved_type;
  if (ExtractGlobalCastTarget(expression, &symbol_name, &resolved_type)) {
    if (local_variables.count(symbol_name) == 0) {
      (*cast_types)[symbol_name].insert(resolved_type.name);
    }
    return;
  }
  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& identifier = static_cast<const IdentifierExpression&>(expression);
      if (local_variables.count(identifier.Name()) == 0) {
        non_cast_uses->insert(identifier.Name());
      }
      break;
    }
    case CExpressionKind::kUnaryExpression:
      ScanDirectTypedGlobalsInExpression(static_cast<const UnaryExpression&>(expression).Operand(),
                                         local_variables, cast_types, non_cast_uses);
      break;
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      ScanDirectTypedGlobalsInExpression(binary.Lhs(), local_variables, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInExpression(binary.Rhs(), local_variables, cast_types, non_cast_uses);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      ScanDirectTypedGlobalsInExpression(assignment.Lhs(), local_variables, cast_types,
                                         non_cast_uses);
      ScanDirectTypedGlobalsInExpression(assignment.Rhs(), local_variables, cast_types,
                                         non_cast_uses);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      for (const auto& argument : call.Arguments()) {
        ScanDirectTypedGlobalsInExpression(*argument, local_variables, cast_types, non_cast_uses);
      }
      break;
    }
    case CExpressionKind::kCastExpression:
      ScanDirectTypedGlobalsInExpression(static_cast<const CastExpression&>(expression).Operand(),
                                         local_variables, cast_types, non_cast_uses);
      break;
    case CExpressionKind::kMemberAccessExpression:
      ScanDirectTypedGlobalsInExpression(
          static_cast<const MemberAccessExpression&>(expression).Object(), local_variables,
          cast_types, non_cast_uses);
      break;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      ScanDirectTypedGlobalsInExpression(array_index.Array(), local_variables, cast_types,
                                         non_cast_uses);
      ScanDirectTypedGlobalsInExpression(array_index.Index(), local_variables, cast_types,
                                         non_cast_uses);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      ScanDirectTypedGlobalsInExpression(ternary.Condition(), local_variables, cast_types,
                                         non_cast_uses);
      ScanDirectTypedGlobalsInExpression(ternary.TrueExpression(), local_variables, cast_types,
                                         non_cast_uses);
      ScanDirectTypedGlobalsInExpression(ternary.FalseExpression(), local_variables, cast_types,
                                         non_cast_uses);
      break;
    }
    default:
      break;
  }
}

void ScanDirectTypedGlobalsInStatement(const CStatement& statement,
                                       const std::set<std::string>& local_variables,
                                       std::map<std::string, std::set<std::string>>* cast_types,
                                       std::set<std::string>* non_cast_uses) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(statement).Statements()) {
        ScanDirectTypedGlobalsInStatement(*child, local_variables, cast_types, non_cast_uses);
      }
      break;
    case CStatementKind::kExpressionStatement:
      ScanDirectTypedGlobalsInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), local_variables,
          cast_types, non_cast_uses);
      break;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      if (declaration.Initializer() != nullptr) {
        ScanDirectTypedGlobalsInExpression(*declaration.Initializer(), local_variables, cast_types,
                                           non_cast_uses);
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      ScanDirectTypedGlobalsInExpression(if_statement.Condition(), local_variables, cast_types,
                                         non_cast_uses);
      ScanDirectTypedGlobalsInStatement(if_statement.ThenBranch(), local_variables, cast_types,
                                        non_cast_uses);
      if (if_statement.ElseBranch() != nullptr) {
        ScanDirectTypedGlobalsInStatement(*if_statement.ElseBranch(), local_variables, cast_types,
                                          non_cast_uses);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      ScanDirectTypedGlobalsInExpression(while_statement.Condition(), local_variables, cast_types,
                                         non_cast_uses);
      ScanDirectTypedGlobalsInStatement(while_statement.Body(), local_variables, cast_types,
                                        non_cast_uses);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      ScanDirectTypedGlobalsInStatement(do_while_statement.Body(), local_variables, cast_types,
                                        non_cast_uses);
      ScanDirectTypedGlobalsInExpression(do_while_statement.Condition(), local_variables,
                                         cast_types, non_cast_uses);
      break;
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      if (for_statement.Init() != nullptr) {
        ScanDirectTypedGlobalsInStatement(*for_statement.Init(), local_variables, cast_types,
                                          non_cast_uses);
      }
      if (for_statement.Condition() != nullptr) {
        ScanDirectTypedGlobalsInExpression(*for_statement.Condition(), local_variables, cast_types,
                                           non_cast_uses);
      }
      if (for_statement.Step() != nullptr) {
        ScanDirectTypedGlobalsInExpression(*for_statement.Step(), local_variables, cast_types,
                                           non_cast_uses);
      }
      ScanDirectTypedGlobalsInStatement(for_statement.Body(), local_variables, cast_types,
                                        non_cast_uses);
      break;
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() != nullptr) {
        ScanDirectTypedGlobalsInExpression(*return_statement.ReturnValue(), local_variables,
                                           cast_types, non_cast_uses);
      }
      break;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_statement = static_cast<const SwitchStatement&>(statement);
      ScanDirectTypedGlobalsInExpression(switch_statement.Condition(), local_variables, cast_types,
                                         non_cast_uses);
      for (const auto& switch_case : switch_statement.Cases()) {
        if (switch_case.body != nullptr) {
          ScanDirectTypedGlobalsInStatement(*switch_case.body, local_variables, cast_types,
                                            non_cast_uses);
        }
      }
      break;
    }
    default:
      break;
  }
}

std::vector<DirectTypedGlobalCandidate> CollectDirectTypedGlobalCandidates(
    const FunctionDeclaration& function, const ReferencedSymbols& referenced_symbols) {
  std::map<std::string, std::set<std::string>> cast_types;
  std::set<std::string> non_cast_uses;
  ScanDirectTypedGlobalsInStatement(function.Body(), referenced_symbols.local_variables,
                                    &cast_types, &non_cast_uses);

  std::vector<DirectTypedGlobalCandidate> candidates;
  for (const auto& [symbol_name, types] : cast_types) {
    if (non_cast_uses.count(symbol_name) > 0) continue;
    if (types.size() == 1) {
      candidates.push_back({symbol_name, CType::Named(*types.begin())});
    } else if (types.size() == 2 && types.count("s16") > 0 && types.count("u16") > 0) {
      candidates.push_back({symbol_name, CType::S16()});
    } else if (types.size() == 2 && types.count("s8") > 0 && types.count("u8") > 0) {
      candidates.push_back({symbol_name, CType::S8()});
    }
  }
  return candidates;
}

std::unique_ptr<CExpression> RewriteDirectTypedGlobalInExpression(
    const CExpression& expression, const std::map<std::string, CType>& target_globals) {
  std::string symbol_name;
  CType resolved_type;
  if (ExtractGlobalCastTarget(expression, &symbol_name, &resolved_type)) {
    auto it = target_globals.find(symbol_name);
    if (it != target_globals.end()) {
      return CExpression::Identifier(symbol_name, it->second);
    }
  }
  switch (expression.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      return CExpression::Unary(
          unary.Op(), RewriteDirectTypedGlobalInExpression(unary.Operand(), target_globals),
          unary.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return CExpression::Binary(
          binary.Op(), RewriteDirectTypedGlobalInExpression(binary.Lhs(), target_globals),
          RewriteDirectTypedGlobalInExpression(binary.Rhs(), target_globals));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      return CExpression::Assignment(
          assignment.Op(), RewriteDirectTypedGlobalInExpression(assignment.Lhs(), target_globals),
          RewriteDirectTypedGlobalInExpression(assignment.Rhs(), target_globals));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      std::vector<std::unique_ptr<CExpression>> arguments;
      arguments.reserve(call.Arguments().size());
      for (const auto& argument : call.Arguments()) {
        arguments.push_back(RewriteDirectTypedGlobalInExpression(*argument, target_globals));
      }
      return CExpression::Call(RewriteDirectTypedGlobalInExpression(call.Callee(), target_globals),
                               std::move(arguments));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      return CExpression::Cast(
          cast.TargetType(), RewriteDirectTypedGlobalInExpression(cast.Operand(), target_globals));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return CExpression::MemberAccess(
          RewriteDirectTypedGlobalInExpression(member.Object(), target_globals),
          member.MemberName(), member.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return CExpression::ArrayIndex(
          RewriteDirectTypedGlobalInExpression(array_index.Array(), target_globals),
          RewriteDirectTypedGlobalInExpression(array_index.Index(), target_globals));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CExpression::Ternary(
          RewriteDirectTypedGlobalInExpression(ternary.Condition(), target_globals),
          RewriteDirectTypedGlobalInExpression(ternary.TrueExpression(), target_globals),
          RewriteDirectTypedGlobalInExpression(ternary.FalseExpression(), target_globals));
    }
    default:
      return expression.Clone();
  }
}

std::unique_ptr<CStatement> RewriteDirectTypedGlobalInStatement(
    const CStatement& statement, const std::map<std::string, CType>& target_globals) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> statements;
      for (const auto& child : static_cast<const CompoundStatement&>(statement).Statements()) {
        statements.push_back(RewriteDirectTypedGlobalInStatement(*child, target_globals));
      }
      return std::make_unique<CompoundStatement>(std::move(statements));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(RewriteDirectTypedGlobalInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), target_globals));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      return CStatement::VariableDeclaration(
          declaration.Type(), declaration.Name(),
          declaration.Initializer()
              ? RewriteDirectTypedGlobalInExpression(*declaration.Initializer(), target_globals)
              : nullptr,
          declaration.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return CStatement::If(
          RewriteDirectTypedGlobalInExpression(if_statement.Condition(), target_globals),
          RewriteDirectTypedGlobalInStatement(if_statement.ThenBranch(), target_globals),
          if_statement.ElseBranch()
              ? RewriteDirectTypedGlobalInStatement(*if_statement.ElseBranch(), target_globals)
              : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      return CStatement::While(
          RewriteDirectTypedGlobalInExpression(while_statement.Condition(), target_globals),
          RewriteDirectTypedGlobalInStatement(while_statement.Body(), target_globals));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      return CStatement::DoWhile(
          RewriteDirectTypedGlobalInStatement(do_while_statement.Body(), target_globals),
          RewriteDirectTypedGlobalInExpression(do_while_statement.Condition(), target_globals));
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      return CStatement::For(
          for_statement.Init()
              ? RewriteDirectTypedGlobalInStatement(*for_statement.Init(), target_globals)
              : nullptr,
          for_statement.Condition()
              ? RewriteDirectTypedGlobalInExpression(*for_statement.Condition(), target_globals)
              : nullptr,
          for_statement.Step()
              ? RewriteDirectTypedGlobalInExpression(*for_statement.Step(), target_globals)
              : nullptr,
          RewriteDirectTypedGlobalInStatement(for_statement.Body(), target_globals));
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(
          RewriteDirectTypedGlobalInExpression(*return_statement.ReturnValue(), target_globals));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_statement = static_cast<const SwitchStatement&>(statement);
      std::vector<SwitchCase> cases;
      for (const auto& switch_case : switch_statement.Cases()) {
        SwitchCase new_case;
        new_case.case_values = switch_case.case_values;
        new_case.is_default = switch_case.is_default;
        if (switch_case.body != nullptr) {
          new_case.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              RewriteDirectTypedGlobalInStatement(*switch_case.body, target_globals).release()));
        }
        cases.push_back(std::move(new_case));
      }
      return CStatement::Switch(
          RewriteDirectTypedGlobalInExpression(switch_statement.Condition(), target_globals),
          std::move(cases));
    }
    default:
      return statement.Clone();
  }
}

// Rewrites uses of `pointer_variable` (where `pointer_variable = &symbol;`) and 32-bit global
// `symbol` into `symbol[index]` / `&symbol[index]`.
std::unique_ptr<CExpression> RewriteGlobalArrayInExpression(
    const CExpression& expression, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& address_temporary_to_symbol);

bool ExtractGlobalArrayBaseSymbol(
    const CExpression& expression, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& address_temporary_to_symbol,
    std::string* out_symbol) {
  if (expression.Kind() == CExpressionKind::kIdentifier) {
    const auto& identifier = static_cast<const IdentifierExpression&>(expression);
    auto it = address_temporary_to_symbol.find(identifier.Name());
    if (it != address_temporary_to_symbol.end()) {
      *out_symbol = it->second;
      return true;
    }
    return false;
  }
  if (expression.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& unary = static_cast<const UnaryExpression&>(expression);
    if (unary.IsPrefix() && unary.Op() == "&" &&
        unary.Operand().Kind() == CExpressionKind::kIdentifier) {
      const auto& identifier = static_cast<const IdentifierExpression&>(unary.Operand());
      if (array_globals.count(identifier.Name()) > 0) {
        *out_symbol = identifier.Name();
        return true;
      }
    }
  }
  return false;
}

bool MatchScaledGlobalArrayAddress(
    const CExpression& expression, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& address_temporary_to_symbol, std::string* out_symbol,
    std::unique_ptr<CExpression>* out_index) {
  if (expression.Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& binary = static_cast<const BinaryExpression&>(expression);
  if (binary.Op() != "+" && binary.Op() != "-") return false;

  auto try_match_pair = [&](const CExpression& base_side, const CExpression& index_side,
                            bool allow_negation) -> bool {
    std::string symbol_name;
    if (!ExtractGlobalArrayBaseSymbol(base_side, array_globals, address_temporary_to_symbol,
                                      &symbol_name)) {
      return false;
    }
    if (index_side.Kind() == CExpressionKind::kIntegerLiteral) {
      int64_t offset = static_cast<const IntegerLiteralExpression&>(index_side).Value();
      if (allow_negation && binary.Op() == "-") offset = -offset;
      if ((offset % 4) == 0) {
        *out_symbol = std::move(symbol_name);
        *out_index = CExpression::Integer(offset / 4);
        return true;
      }
      return false;
    }
    if (binary.Op() == "+" && index_side.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& shift_binary = static_cast<const BinaryExpression&>(index_side);
      if (shift_binary.Op() == "<<" &&
          shift_binary.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(shift_binary.Rhs()).Value() == 2) {
        *out_symbol = std::move(symbol_name);
        *out_index = RewriteGlobalArrayInExpression(shift_binary.Lhs(), array_globals,
                                                    address_temporary_to_symbol);
        return true;
      }
    }
    return false;
  };

  if (try_match_pair(binary.Lhs(), binary.Rhs(), /*allow_negation=*/true)) return true;
  if (binary.Op() == "+" && try_match_pair(binary.Rhs(), binary.Lhs(), /*allow_negation=*/false)) {
    return true;
  }
  return false;
}

std::unique_ptr<CExpression> RewriteGlobalArrayInExpression(
    const CExpression& expression, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& address_temporary_to_symbol) {
  // 1. Check `*(s32 *)pointer_or_scaled_address` -> `symbol[index]`
  if (expression.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& dereference = static_cast<const UnaryExpression&>(expression);
    if (dereference.IsPrefix() && dereference.Op() == "*" &&
        dereference.Operand().Kind() == CExpressionKind::kCastExpression) {
      const auto& cast = static_cast<const CastExpression&>(dereference.Operand());
      if (cast.TargetType().pointer_depth == 1 &&
          (cast.TargetType().name == "s32" || cast.TargetType().name == "u32")) {
        std::string symbol_name;
        if (ExtractGlobalArrayBaseSymbol(cast.Operand(), array_globals, address_temporary_to_symbol,
                                         &symbol_name)) {
          return CExpression::ArrayIndex(
              CExpression::Identifier(symbol_name, CType::S32(), /*is_array_symbol=*/true),
              CExpression::Integer(0));
        }
        std::unique_ptr<CExpression> index_expression;
        if (MatchScaledGlobalArrayAddress(cast.Operand(), array_globals,
                                          address_temporary_to_symbol, &symbol_name,
                                          &index_expression)) {
          return CExpression::ArrayIndex(
              CExpression::Identifier(symbol_name, CType::S32(), /*is_array_symbol=*/true),
              std::move(index_expression));
        }
      }
    }
  }

  // 2. Check `(pointer_variable + offset)` or `((index << 2) + &symbol)` -> `&symbol[index]`
  {
    std::string symbol_name;
    std::unique_ptr<CExpression> index_expression;
    if (MatchScaledGlobalArrayAddress(expression, array_globals, address_temporary_to_symbol,
                                      &symbol_name, &index_expression)) {
      return CExpression::Unary(
          "&",
          CExpression::ArrayIndex(
              CExpression::Identifier(symbol_name, CType::S32(), /*is_array_symbol=*/true),
              std::move(index_expression)),
          /*is_prefix=*/true);
    }
  }

  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& identifier = static_cast<const IdentifierExpression&>(expression);
      auto it = address_temporary_to_symbol.find(identifier.Name());
      if (it != address_temporary_to_symbol.end()) {
        return CExpression::Unary(
            "&",
            CExpression::ArrayIndex(
                CExpression::Identifier(it->second, CType::S32(), /*is_array_symbol=*/true),
                CExpression::Integer(0)),
            /*is_prefix=*/true);
      }
      if (array_globals.count(identifier.Name()) > 0) {
        return CExpression::ArrayIndex(
            CExpression::Identifier(identifier.Name(), CType::S32(), /*is_array_symbol=*/true),
            CExpression::Integer(0));
      }
      return expression.Clone();
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      if (unary.IsPrefix() && unary.Op() == "&" &&
          unary.Operand().Kind() == CExpressionKind::kIdentifier) {
        return expression.Clone();
      }
      return CExpression::Unary(unary.Op(),
                                RewriteGlobalArrayInExpression(unary.Operand(), array_globals,
                                                               address_temporary_to_symbol),
                                unary.IsPrefix());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      if (array_index.Array().Kind() == CExpressionKind::kIdentifier) {
        const auto& identifier = static_cast<const IdentifierExpression&>(array_index.Array());
        // Toggle back to scalar `symbol` if `symbol[0]` is toggled individually.
        if (array_globals.count(identifier.Name()) > 0 &&
            array_index.Index().Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(array_index.Index()).Value() == 0) {
          return CExpression::Identifier(identifier.Name());
        }
        return CExpression::ArrayIndex(
            array_index.Array().Clone(),
            RewriteGlobalArrayInExpression(array_index.Index(), array_globals,
                                           address_temporary_to_symbol));
      }
      return CExpression::ArrayIndex(
          RewriteGlobalArrayInExpression(array_index.Array(), array_globals,
                                         address_temporary_to_symbol),
          RewriteGlobalArrayInExpression(array_index.Index(), array_globals,
                                         address_temporary_to_symbol));
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return CExpression::Binary(
          binary.Op(),
          RewriteGlobalArrayInExpression(binary.Lhs(), array_globals, address_temporary_to_symbol),
          RewriteGlobalArrayInExpression(binary.Rhs(), array_globals, address_temporary_to_symbol));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      return CExpression::Assignment(assignment.Op(),
                                     RewriteGlobalArrayInExpression(assignment.Lhs(), array_globals,
                                                                    address_temporary_to_symbol),
                                     RewriteGlobalArrayInExpression(assignment.Rhs(), array_globals,
                                                                    address_temporary_to_symbol));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      std::vector<std::unique_ptr<CExpression>> arguments;
      arguments.reserve(call.Arguments().size());
      for (const auto& argument : call.Arguments()) {
        arguments.push_back(
            RewriteGlobalArrayInExpression(*argument, array_globals, address_temporary_to_symbol));
      }
      std::unique_ptr<CExpression> new_callee =
          call.Callee().Kind() == CExpressionKind::kIdentifier
              ? call.Callee().Clone()
              : RewriteGlobalArrayInExpression(call.Callee(), array_globals,
                                               address_temporary_to_symbol);
      return CExpression::Call(std::move(new_callee), std::move(arguments));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      return CExpression::Cast(cast.TargetType(),
                               RewriteGlobalArrayInExpression(cast.Operand(), array_globals,
                                                              address_temporary_to_symbol));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return CExpression::MemberAccess(
          RewriteGlobalArrayInExpression(member.Object(), array_globals,
                                         address_temporary_to_symbol),
          member.MemberName(), member.IsArrow());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CExpression::Ternary(
          RewriteGlobalArrayInExpression(ternary.Condition(), array_globals,
                                         address_temporary_to_symbol),
          RewriteGlobalArrayInExpression(ternary.TrueExpression(), array_globals,
                                         address_temporary_to_symbol),
          RewriteGlobalArrayInExpression(ternary.FalseExpression(), array_globals,
                                         address_temporary_to_symbol));
    }
    default:
      return expression.Clone();
  }
}

std::unique_ptr<CStatement> RewriteGlobalArrayInStatement(
    const CStatement& statement, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& address_temporary_to_symbol) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> statements;
      for (const auto& child : static_cast<const CompoundStatement&>(statement).Statements()) {
        if (child->Kind() == CStatementKind::kExpressionStatement) {
          const auto& expression = static_cast<const ExpressionStatement&>(*child).Expression();
          if (expression.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& assignment = static_cast<const AssignmentExpression&>(expression);
            if (assignment.Op() == "=" && assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
                address_temporary_to_symbol.count(
                    static_cast<const IdentifierExpression&>(assignment.Lhs()).Name()) > 0) {
              continue;
            }
          }
        }
        statements.push_back(
            RewriteGlobalArrayInStatement(*child, array_globals, address_temporary_to_symbol));
      }
      return std::make_unique<CompoundStatement>(std::move(statements));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(RewriteGlobalArrayInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), array_globals,
          address_temporary_to_symbol));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      return CStatement::VariableDeclaration(
          declaration.Type(), declaration.Name(),
          declaration.Initializer()
              ? RewriteGlobalArrayInExpression(*declaration.Initializer(), array_globals,
                                               address_temporary_to_symbol)
              : nullptr,
          declaration.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return CStatement::If(
          RewriteGlobalArrayInExpression(if_statement.Condition(), array_globals,
                                         address_temporary_to_symbol),
          RewriteGlobalArrayInStatement(if_statement.ThenBranch(), array_globals,
                                        address_temporary_to_symbol),
          if_statement.ElseBranch()
              ? RewriteGlobalArrayInStatement(*if_statement.ElseBranch(), array_globals,
                                              address_temporary_to_symbol)
              : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      return CStatement::While(
          RewriteGlobalArrayInExpression(while_statement.Condition(), array_globals,
                                         address_temporary_to_symbol),
          RewriteGlobalArrayInStatement(while_statement.Body(), array_globals,
                                        address_temporary_to_symbol));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      return CStatement::DoWhile(
          RewriteGlobalArrayInStatement(do_while_statement.Body(), array_globals,
                                        address_temporary_to_symbol),
          RewriteGlobalArrayInExpression(do_while_statement.Condition(), array_globals,
                                         address_temporary_to_symbol));
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      return CStatement::For(
          for_statement.Init() ? RewriteGlobalArrayInStatement(*for_statement.Init(), array_globals,
                                                               address_temporary_to_symbol)
                               : nullptr,
          for_statement.Condition()
              ? RewriteGlobalArrayInExpression(*for_statement.Condition(), array_globals,
                                               address_temporary_to_symbol)
              : nullptr,
          for_statement.Step()
              ? RewriteGlobalArrayInExpression(*for_statement.Step(), array_globals,
                                               address_temporary_to_symbol)
              : nullptr,
          RewriteGlobalArrayInStatement(for_statement.Body(), array_globals,
                                        address_temporary_to_symbol));
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(RewriteGlobalArrayInExpression(
          *return_statement.ReturnValue(), array_globals, address_temporary_to_symbol));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_statement = static_cast<const SwitchStatement&>(statement);
      std::vector<SwitchCase> cases;
      for (const auto& switch_case : switch_statement.Cases()) {
        SwitchCase new_case;
        new_case.case_values = switch_case.case_values;
        new_case.is_default = switch_case.is_default;
        if (switch_case.body != nullptr) {
          new_case.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              RewriteGlobalArrayInStatement(*switch_case.body, array_globals,
                                            address_temporary_to_symbol)
                  .release()));
        }
        cases.push_back(std::move(new_case));
      }
      return CStatement::Switch(
          RewriteGlobalArrayInExpression(switch_statement.Condition(), array_globals,
                                         address_temporary_to_symbol),
          std::move(cases));
    }
    default:
      return statement.Clone();
  }
}

std::vector<std::string> Collect32BitGlobalCandidates(const FunctionDeclaration& function,
                                                      const ReferencedSymbols& referenced_symbols,
                                                      bool non_zero_or_pointer_only) {
  std::map<std::string, std::set<std::string>> cast_types;
  std::set<std::string> non_cast_uses;
  ScanDirectTypedGlobalsInStatement(function.Body(), referenced_symbols.local_variables,
                                    &cast_types, &non_cast_uses);

  std::set<std::string> zero_only_assigned;
  if (non_zero_or_pointer_only) {
    for (const auto& statement : function.Body().Statements()) {
      if (statement->Kind() != CStatementKind::kExpressionStatement) continue;
      const auto& expression = static_cast<const ExpressionStatement&>(*statement).Expression();
      if (expression.Kind() != CExpressionKind::kAssignmentExpression) continue;
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      if (assignment.Op() == "=" && assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
          assignment.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(assignment.Rhs()).Value() == 0) {
        const std::string& name = static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
        if (CountIdentifierOccurrencesInStatement(function.Body(), name) == 1) {
          zero_only_assigned.insert(name);
        }
      }
    }
  }

  std::vector<std::string> candidates;
  for (const auto& symbol_name : referenced_symbols.external_data) {
    if (cast_types.count(symbol_name) > 0 && non_cast_uses.count(symbol_name) == 0) continue;
    auto type_it = referenced_symbols.inferred_data_types.find(symbol_name);
    if (type_it != referenced_symbols.inferred_data_types.end() && type_it->second.name != "s32" &&
        type_it->second.name != "u32") {
      continue;
    }
    if (non_zero_or_pointer_only && zero_only_assigned.count(symbol_name) > 0) continue;
    candidates.push_back(symbol_name);
  }
  return candidates;
}

std::map<std::string, std::string> CollectSingleAssignmentGlobalAddressTemporaries(
    const FunctionDeclaration& function, const std::set<std::string>& target_globals) {
  std::map<std::string, std::string> address_temporaries;
  const auto& statements = function.Body().Statements();
  for (size_t i = 0; i < statements.size(); ++i) {
    if (statements[i]->Kind() != CStatementKind::kExpressionStatement) continue;
    const auto& expression = static_cast<const ExpressionStatement&>(*statements[i]).Expression();
    if (expression.Kind() != CExpressionKind::kAssignmentExpression) continue;
    const auto& assignment = static_cast<const AssignmentExpression&>(expression);
    if (assignment.Op() != "=" || assignment.Lhs().Kind() != CExpressionKind::kIdentifier) {
      continue;
    }
    if (assignment.Rhs().Kind() != CExpressionKind::kUnaryExpression) continue;
    const auto& unary = static_cast<const UnaryExpression&>(assignment.Rhs());
    if (!unary.IsPrefix() || unary.Op() != "&" ||
        unary.Operand().Kind() != CExpressionKind::kIdentifier) {
      continue;
    }
    const std::string& variable_name =
        static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
    const std::string& symbol_name =
        static_cast<const IdentifierExpression&>(unary.Operand()).Name();
    if (target_globals.count(symbol_name) == 0) continue;

    // Ensure `variable_name` is never reassigned anywhere else in `function.Body()`.
    bool reassigned = false;
    for (size_t j = 0; j < statements.size(); ++j) {
      if (j == i) continue;
      if (statements[j]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& other_expression =
            static_cast<const ExpressionStatement&>(*statements[j]).Expression();
        if (other_expression.Kind() == CExpressionKind::kAssignmentExpression &&
            static_cast<const AssignmentExpression&>(other_expression).Lhs().Kind() ==
                CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(
                static_cast<const AssignmentExpression&>(other_expression).Lhs())
                    .Name() == variable_name) {
          reassigned = true;
          break;
        }
      }
    }
    if (!reassigned) {
      address_temporaries[variable_name] = symbol_name;
    }
  }
  return address_temporaries;
}

struct ParameterCopyCandidate {
  size_t statement_index = 0;
  std::string temporary_variable;
  std::string parameter_name;
};

std::vector<ParameterCopyCandidate> CollectParameterCopyCandidates(
    const FunctionDeclaration& function) {
  std::set<std::string> parameter_names;
  for (const auto& parameter : function.Parameters()) {
    parameter_names.insert(parameter.name);
  }
  std::vector<ParameterCopyCandidate> candidates;
  const auto& statements = function.Body().Statements();
  for (size_t i = 0; i < statements.size(); ++i) {
    if (statements[i]->Kind() != CStatementKind::kExpressionStatement) continue;
    const auto& expression = static_cast<const ExpressionStatement&>(*statements[i]).Expression();
    if (expression.Kind() != CExpressionKind::kAssignmentExpression) continue;
    const auto& assignment = static_cast<const AssignmentExpression&>(expression);
    if (assignment.Op() != "=" || assignment.Lhs().Kind() != CExpressionKind::kIdentifier ||
        assignment.Rhs().Kind() != CExpressionKind::kIdentifier) {
      continue;
    }
    const std::string& lhs_name = static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
    const std::string& rhs_name = static_cast<const IdentifierExpression&>(assignment.Rhs()).Name();
    if (lhs_name == rhs_name || parameter_names.count(rhs_name) == 0 ||
        parameter_names.count(lhs_name) > 0) {
      continue;
    }
    // Check if `rhs_name` is dead immediately after `i` (either not used again or overwritten
    // before any read).
    if (!IsVariableDeadAfter(statements, i, rhs_name, /*is_top_level_block=*/true)) {
      continue;
    }
    candidates.push_back({i, lhs_name, rhs_name});
  }
  return candidates;
}

std::unique_ptr<CompoundStatement> ApplyCoalesceParameterCopy(
    const FunctionDeclaration& function, const ParameterCopyCandidate& candidate) {
  const auto& statements = function.Body().Statements();
  std::vector<std::unique_ptr<CStatement>> work;
  work.reserve(statements.size());
  size_t coalesce_start_index = 0;
  for (size_t i = 0; i < statements.size(); ++i) {
    if (i == candidate.statement_index) {
      coalesce_start_index = work.size();
      continue;
    }
    work.push_back(statements[i]->Clone());
  }

  // First inline any single-use assignments to `candidate.parameter_name` after
  // `candidate.statement_index` (e.g. `arg0 = temp_s2; call(arg0)`) so they don't collide with
  // `candidate.temporary_variable` being renamed to `candidate.parameter_name`.
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = coalesce_start_index; i < work.size(); ++i) {
      InlineCandidateInfo info;
      if (AnalyzeInlineSingleUseTemporaryAt(work, i, /*is_top_level_block=*/true, &info) &&
          info.variable_name == candidate.parameter_name && info.can_sink_to_use) {
        work[info.use_index] = ReplaceIdentifierInSingleStatement(*work[info.use_index],
                                                                  info.variable_name, *info.rhs);
        work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        break;
      }
    }
  }

  // Replace occurrences of `candidate.temporary_variable` with `candidate.parameter_name` starting
  // from `coalesce_start_index` so any earlier unrelated uses of `candidate.temporary_variable`
  // before `candidate.statement_index` are never clobbered.
  auto parameter_identifier = CExpression::Identifier(candidate.parameter_name);
  for (size_t i = coalesce_start_index; i < work.size(); ++i) {
    if (work[i]->Kind() == CStatementKind::kVariableDeclarationStatement &&
        static_cast<const VariableDeclarationStatement&>(*work[i]).Name() ==
            candidate.temporary_variable) {
      continue;
    }
    if (work[i]->Kind() == CStatementKind::kExpressionStatement) {
      const auto& expression_statement = static_cast<const ExpressionStatement&>(*work[i]);
      if (expression_statement.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assignment =
            static_cast<const AssignmentExpression&>(expression_statement.Expression());
        std::unique_ptr<CExpression> new_lhs =
            (assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
             static_cast<const IdentifierExpression&>(assignment.Lhs()).Name() ==
                 candidate.temporary_variable)
                ? parameter_identifier->Clone()
                : ReplaceIdentifierInExpression(assignment.Lhs(), candidate.temporary_variable,
                                                *parameter_identifier);
        work[i] = CStatement::Expression(CExpression::Assignment(
            assignment.Op(), std::move(new_lhs),
            ReplaceIdentifierInExpression(assignment.Rhs(), candidate.temporary_variable,
                                          *parameter_identifier)));
        continue;
      }
    }
    work[i] = ReplaceIdentifierInSingleStatement(*work[i], candidate.temporary_variable,
                                                 *parameter_identifier);
  }

  return PruneUnusedLocalDeclarations(CompoundStatement(std::move(work)));
}

struct MutationContext {
  const AstMutationStep* step_to_apply = nullptr;
  std::vector<AstMutationStep>* discovered = nullptr;
  size_t max_mutations = 64;
  std::set<std::string> local_variables;
  const CompoundStatement* root_body = nullptr;

  uint32_t commute_index = 0;
  uint32_t invert_if_index = 0;
  uint32_t reorder_declaration_index = 0;
  uint32_t compound_assignment_index = 0;
  uint32_t while_for_index = 0;
  uint32_t cast_index = 0;
  uint32_t inline_temporary_index = 0;
  uint32_t pointer_offset_index = 0;
  uint32_t swap_statement_index = 0;
  uint32_t dead_assignment_index = 0;

  void Record(AstMutationKind kind, uint32_t site_index, uint32_t parameter = 0) {
    if (discovered == nullptr || discovered->size() >= max_mutations) return;
    AstMutationStep step;
    step.set_kind(kind);
    step.set_site_index(site_index);
    step.set_param(parameter);
    discovered->push_back(std::move(step));
  }

  bool ShouldApply(AstMutationKind kind, uint32_t site_index,
                   uint32_t* out_parameter = nullptr) const {
    if (step_to_apply == nullptr) return false;
    if (step_to_apply->kind() == kind && step_to_apply->site_index() == site_index) {
      if (out_parameter != nullptr) *out_parameter = step_to_apply->param();
      return true;
    }
    return false;
  }

  bool ShouldApplyAllSites(AstMutationKind kind, uint32_t expected_parameter = 1) const {
    return step_to_apply != nullptr && step_to_apply->kind() == kind &&
           step_to_apply->param() == expected_parameter;
  }
};

std::unique_ptr<CExpression> TraverseExpression(const CExpression& expression,
                                                MutationContext* context) {
  CType pointer_type;
  const CExpression* base_expression = nullptr;
  int64_t element_index = 0;
  if (MatchPointerOffsetDereference(expression, &pointer_type, &base_expression, &element_index)) {
    auto new_base = TraverseExpression(*base_expression, context);
    uint32_t site = context->pointer_offset_index++;
    context->Record(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, site, 0);
    uint32_t parameter = 0;
    if ((context->ShouldApply(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, site, &parameter) &&
         parameter == 0) ||
        context->ShouldApplyAllSites(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 1)) {
      return CExpression::ArrayIndex(CExpression::Cast(pointer_type, std::move(new_base)),
                                     CExpression::Integer(element_index));
    }
    const auto& dereference = static_cast<const UnaryExpression&>(expression);
    const auto& cast = static_cast<const CastExpression&>(dereference.Operand());
    const auto& binary = static_cast<const BinaryExpression&>(cast.Operand());
    return CExpression::Unary(
        "*",
        CExpression::Cast(pointer_type, CExpression::Binary(binary.Op(), std::move(new_base),
                                                            binary.Rhs().Clone())),
        /*is_prefix=*/true);
  }

  switch (expression.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      return CExpression::Unary(unary.Op(), TraverseExpression(unary.Operand(), context),
                                unary.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      auto new_lhs = TraverseExpression(binary.Lhs(), context);
      auto new_rhs = TraverseExpression(binary.Rhs(), context);
      if (IsCommutableBinary(binary)) {
        uint32_t site = context->commute_index++;
        context->Record(AST_MUTATION_COMMUTE_BINARY_OP, site);
        if (context->ShouldApply(AST_MUTATION_COMMUTE_BINARY_OP, site)) {
          return CExpression::Binary(*CommutedBinaryOperator(binary.Op()), std::move(new_rhs),
                                     std::move(new_lhs));
        }
      }
      return CExpression::Binary(binary.Op(), std::move(new_lhs), std::move(new_rhs));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      auto new_lhs = TraverseExpression(assignment.Lhs(), context);
      auto new_rhs = TraverseExpression(assignment.Rhs(), context);
      AssignmentExpression current(assignment.Op(), std::move(new_lhs), std::move(new_rhs));
      if (IsToggableCompoundAssignment(current)) {
        uint32_t site = context->compound_assignment_index++;
        context->Record(AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT, site);
        if (context->ShouldApply(AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT, site)) {
          return ToggleCompoundAssignment(current);
        }
      }
      return current.Clone();
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      auto new_callee = TraverseExpression(call.Callee(), context);
      std::vector<std::unique_ptr<CExpression>> new_arguments;
      new_arguments.reserve(call.Arguments().size());
      for (const auto& argument : call.Arguments()) {
        new_arguments.push_back(TraverseExpression(*argument, context));
      }
      return CExpression::Call(std::move(new_callee), std::move(new_arguments));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      auto new_operand = TraverseExpression(cast.Operand(), context);
      if (IsToggableIntegerCast(cast)) {
        uint32_t site = context->cast_index++;
        context->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 0);
        context->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 1);
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, &parameter)) {
          if (parameter == 0) {
            return new_operand;
          }
          CType flipped = (cast.TargetType().name == "u32") ? CType::S32() : CType::U32();
          return CExpression::Cast(flipped, std::move(new_operand));
        }
      }
      return CExpression::Cast(cast.TargetType(), std::move(new_operand));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return CExpression::MemberAccess(TraverseExpression(member.Object(), context),
                                       member.MemberName(), member.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return CExpression::ArrayIndex(TraverseExpression(array_index.Array(), context),
                                     TraverseExpression(array_index.Index(), context));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CExpression::Ternary(TraverseExpression(ternary.Condition(), context),
                                  TraverseExpression(ternary.TrueExpression(), context),
                                  TraverseExpression(ternary.FalseExpression(), context));
    }
    default:
      return expression.Clone();
  }
}

std::unique_ptr<CStatement> TraverseStatement(const CStatement& statement, MutationContext* context,
                                              bool is_top_level_block = false);

std::unique_ptr<CompoundStatement> TraverseCompound(const CompoundStatement& compound,
                                                    MutationContext* context,
                                                    bool is_top_level_block = false) {
  const auto& statements = compound.Statements();
  std::vector<std::unique_ptr<CStatement>> new_statements;
  new_statements.reserve(statements.size());

  for (size_t i = 0; i < statements.size(); ++i) {
    // 1. Check adjacent VariableDeclarationStatements for reordering
    if (i + 1 < statements.size() &&
        statements[i]->Kind() == CStatementKind::kVariableDeclarationStatement &&
        statements[i + 1]->Kind() == CStatementKind::kVariableDeclarationStatement) {
      const auto& declaration1 = static_cast<const VariableDeclarationStatement&>(*statements[i]);
      const auto& declaration2 =
          static_cast<const VariableDeclarationStatement&>(*statements[i + 1]);
      bool independent = (declaration2.Initializer() == nullptr ||
                          CountIdentifierOccurrencesInExpression(*declaration2.Initializer(),
                                                                 declaration1.Name()) == 0) &&
                         (declaration1.Initializer() == nullptr ||
                          CountIdentifierOccurrencesInExpression(*declaration1.Initializer(),
                                                                 declaration2.Name()) == 0);
      if (independent) {
        uint32_t site = context->reorder_declaration_index++;
        context->Record(AST_MUTATION_REORDER_LOCAL_DECLS, site);
        if (context->ShouldApply(AST_MUTATION_REORDER_LOCAL_DECLS, site)) {
          new_statements.push_back(TraverseStatement(*statements[i + 1], context));
          new_statements.push_back(TraverseStatement(*statements[i], context));
          ++i;
          continue;
        }
      }
    }

    // 2. Check dead assignment / dead call return elimination
    bool is_dead_call = false;
    if (IsDeadAssignmentAt(statements, i, is_top_level_block, context->local_variables,
                           &is_dead_call, context->root_body)) {
      uint32_t site = context->dead_assignment_index++;
      context->Record(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, site, 0);
      uint32_t parameter = 0;
      if ((context->ShouldApply(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, site, &parameter) &&
           parameter == 0) ||
          context->ShouldApplyAllSites(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 1)) {
        if (is_dead_call) {
          const auto& assignment = static_cast<const AssignmentExpression&>(
              static_cast<const ExpressionStatement&>(*statements[i]).Expression());
          const CExpression* rhs = &assignment.Rhs();
          if (rhs->Kind() == CExpressionKind::kCastExpression) {
            rhs = &static_cast<const CastExpression&>(*rhs).Operand();
          }
          new_statements.push_back(CStatement::Expression(TraverseExpression(*rhs, context)));
        }
        continue;
      }
    }

    // 3. Check single-use temporary inlining across `statements[i]` and
    // `statements[inline_info.use_index]`
    InlineCandidateInfo inline_info;
    if (AnalyzeInlineSingleUseTemporaryAt(statements, i, is_top_level_block, &inline_info,
                                          context->root_body)) {
      uint32_t site = context->inline_temporary_index++;
      if (inline_info.can_sink_to_use) {
        context->Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site, 0);
      }
      if (inline_info.can_hoist_to_def && inline_info.use_index > i + 1) {
        context->Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site, 2);
      }
      uint32_t parameter = 0;
      if (context->ShouldApply(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site, &parameter)) {
        auto replaced = ReplaceIdentifierInSingleStatement(
            *statements[inline_info.use_index], inline_info.variable_name, *inline_info.rhs);
        if (parameter == 2 && inline_info.can_hoist_to_def) {
          new_statements.push_back(TraverseStatement(*replaced, context));
          for (size_t middle = i + 1; middle < inline_info.use_index; ++middle) {
            new_statements.push_back(TraverseStatement(*statements[middle], context));
          }
          i = inline_info.use_index;
          continue;
        } else if (inline_info.can_sink_to_use) {
          for (size_t middle = i + 1; middle < inline_info.use_index; ++middle) {
            new_statements.push_back(TraverseStatement(*statements[middle], context));
          }
          new_statements.push_back(TraverseStatement(*replaced, context));
          i = inline_info.use_index;
          continue;
        }
      }
    }

    // 4. Check swapping adjacent independent statements
    if (CanSwapAdjacentStatements(statements, i)) {
      uint32_t site = context->swap_statement_index++;
      context->Record(AST_MUTATION_SWAP_ADJACENT_STATEMENTS, site);
      if (context->ShouldApply(AST_MUTATION_SWAP_ADJACENT_STATEMENTS, site)) {
        new_statements.push_back(TraverseStatement(*statements[i + 1], context));
        new_statements.push_back(TraverseStatement(*statements[i], context));
        ++i;
        continue;
      }
    }

    new_statements.push_back(TraverseStatement(*statements[i], context));
  }

  return std::make_unique<CompoundStatement>(std::move(new_statements));
}

std::unique_ptr<CStatement> TraverseStatement(const CStatement& statement, MutationContext* context,
                                              bool is_top_level_block) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement:
      return TraverseCompound(static_cast<const CompoundStatement&>(statement), context,
                              is_top_level_block);
    case CStatementKind::kExpressionStatement: {
      const auto& expression_statement = static_cast<const ExpressionStatement&>(statement);
      return CStatement::Expression(TraverseExpression(expression_statement.Expression(), context));
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      std::unique_ptr<CExpression> initializer =
          declaration.Initializer() != nullptr
              ? TraverseExpression(*declaration.Initializer(), context)
              : nullptr;
      return CStatement::VariableDeclaration(declaration.Type(), declaration.Name(),
                                             std::move(initializer), declaration.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      auto new_condition = TraverseExpression(if_statement.Condition(), context);
      auto new_then = TraverseStatement(if_statement.ThenBranch(), context);
      std::unique_ptr<CStatement> new_else =
          if_statement.ElseBranch() != nullptr
              ? TraverseStatement(*if_statement.ElseBranch(), context)
              : nullptr;
      if (new_else != nullptr) {
        uint32_t site = context->invert_if_index++;
        context->Record(AST_MUTATION_INVERT_IF_ELSE, site);
        if (context->ShouldApply(AST_MUTATION_INVERT_IF_ELSE, site)) {
          return CStatement::If(NegateCondition(*new_condition), std::move(new_else),
                                std::move(new_then));
        }
      }
      return CStatement::If(std::move(new_condition), std::move(new_then), std::move(new_else));
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      auto new_condition = TraverseExpression(while_statement.Condition(), context);
      auto new_body = TraverseStatement(while_statement.Body(), context);
      uint32_t site = context->while_for_index++;
      context->Record(AST_MUTATION_WHILE_TO_FOR_LOOP, site);
      if (context->ShouldApply(AST_MUTATION_WHILE_TO_FOR_LOOP, site)) {
        return CStatement::For(nullptr, std::move(new_condition), nullptr, std::move(new_body));
      }
      return CStatement::While(std::move(new_condition), std::move(new_body));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      auto new_body = TraverseStatement(do_while_statement.Body(), context);
      auto new_condition = TraverseExpression(do_while_statement.Condition(), context);
      return CStatement::DoWhile(std::move(new_body), std::move(new_condition));
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      std::unique_ptr<CStatement> new_init = for_statement.Init() != nullptr
                                                 ? TraverseStatement(*for_statement.Init(), context)
                                                 : nullptr;
      std::unique_ptr<CExpression> new_condition =
          for_statement.Condition() != nullptr
              ? TraverseExpression(*for_statement.Condition(), context)
              : nullptr;
      std::unique_ptr<CExpression> new_step =
          for_statement.Step() != nullptr ? TraverseExpression(*for_statement.Step(), context)
                                          : nullptr;
      auto new_body = TraverseStatement(for_statement.Body(), context);
      if (new_init == nullptr && new_step == nullptr && new_condition != nullptr) {
        uint32_t site = context->while_for_index++;
        context->Record(AST_MUTATION_WHILE_TO_FOR_LOOP, site);
        if (context->ShouldApply(AST_MUTATION_WHILE_TO_FOR_LOOP, site)) {
          return CStatement::While(std::move(new_condition), std::move(new_body));
        }
      }
      return CStatement::For(std::move(new_init), std::move(new_condition), std::move(new_step),
                             std::move(new_body));
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(TraverseExpression(*return_statement.ReturnValue(), context));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_statement = static_cast<const SwitchStatement&>(statement);
      auto new_condition = TraverseExpression(switch_statement.Condition(), context);
      std::vector<SwitchCase> new_cases;
      new_cases.reserve(switch_statement.Cases().size());
      for (const auto& switch_case : switch_statement.Cases()) {
        SwitchCase new_case;
        new_case.case_values = switch_case.case_values;
        new_case.is_default = switch_case.is_default;
        if (switch_case.body != nullptr) {
          new_case.body = TraverseCompound(*switch_case.body, context);
        }
        new_cases.push_back(std::move(new_case));
      }
      return CStatement::Switch(std::move(new_condition), std::move(new_cases));
    }
    default:
      return statement.Clone();
  }
}

bool ExtractShiftByConstant(const CExpression& expression, const CExpression** out_base,
                            int64_t* out_shift) {
  if (expression.Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& binary = static_cast<const BinaryExpression&>(expression);
  if (binary.Op() != "<<" || binary.Rhs().Kind() != CExpressionKind::kIntegerLiteral) return false;
  int64_t shift_amount = static_cast<const IntegerLiteralExpression&>(binary.Rhs()).Value();
  if (shift_amount < 1 || shift_amount > 15 || !IsPureExpression(binary.Lhs())) return false;
  *out_base = &binary.Lhs();
  *out_shift = shift_amount;
  return true;
}

bool ExtractMultiplyByConstant(const CExpression& expression, const CExpression** out_base,
                               int64_t* out_factor) {
  if (expression.Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& binary = static_cast<const BinaryExpression&>(expression);
  if (binary.Op() != "*" || binary.Rhs().Kind() != CExpressionKind::kIntegerLiteral) return false;
  int64_t factor = static_cast<const IntegerLiteralExpression&>(binary.Rhs()).Value();
  if (factor <= 1 || factor > 65536 || !IsPureExpression(binary.Lhs())) return false;
  *out_base = &binary.Lhs();
  *out_factor = factor;
  return true;
}

std::unique_ptr<CExpression> FoldShiftAddMultiplyInExpression(const CExpression& expression) {
  switch (expression.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      return CExpression::Unary(unary.Op(), FoldShiftAddMultiplyInExpression(unary.Operand()),
                                unary.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      auto lhs = FoldShiftAddMultiplyInExpression(binary.Lhs());
      auto rhs = FoldShiftAddMultiplyInExpression(binary.Rhs());

      if (binary.Op() == "<<" && rhs->Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t shift_amount = static_cast<const IntegerLiteralExpression&>(*rhs).Value();
        const CExpression* multiply_base = nullptr;
        int64_t factor = 0;
        if (shift_amount >= 1 && shift_amount <= 15 &&
            ExtractMultiplyByConstant(*lhs, &multiply_base, &factor)) {
          return CExpression::Binary("*", multiply_base->Clone(),
                                     CExpression::Integer(factor << shift_amount));
        }
      }

      if (binary.Op() == "+" || binary.Op() == "-") {
        const CExpression* shift_base1 = nullptr;
        int64_t shift1 = 0;
        const CExpression* shift_base2 = nullptr;
        int64_t shift2 = 0;
        const CExpression* multiply_base = nullptr;
        int64_t factor = 0;

        if (ExtractShiftByConstant(*lhs, &shift_base1, &shift1)) {
          if (shift_base1->ToString() == rhs->ToString()) {
            int64_t multiplier = (1LL << shift1) + (binary.Op() == "+" ? 1 : -1);
            if (multiplier > 1) {
              return CExpression::Binary("*", shift_base1->Clone(),
                                         CExpression::Integer(multiplier));
            }
          }
          if (ExtractShiftByConstant(*rhs, &shift_base2, &shift2) &&
              shift_base1->ToString() == shift_base2->ToString()) {
            int64_t multiplier =
                (1LL << shift1) + (binary.Op() == "+" ? (1LL << shift2) : -(1LL << shift2));
            if (multiplier > 1) {
              return CExpression::Binary("*", shift_base1->Clone(),
                                         CExpression::Integer(multiplier));
            }
          }
        }
        if (binary.Op() == "+" && ExtractShiftByConstant(*rhs, &shift_base2, &shift2) &&
            shift_base2->ToString() == lhs->ToString()) {
          int64_t multiplier = (1LL << shift2) + 1;
          return CExpression::Binary("*", shift_base2->Clone(), CExpression::Integer(multiplier));
        }
        if (ExtractMultiplyByConstant(*lhs, &multiply_base, &factor)) {
          if (multiply_base->ToString() == rhs->ToString()) {
            int64_t multiplier = factor + (binary.Op() == "+" ? 1 : -1);
            if (multiplier > 1) {
              return CExpression::Binary("*", multiply_base->Clone(),
                                         CExpression::Integer(multiplier));
            }
          }
          if (ExtractShiftByConstant(*rhs, &shift_base2, &shift2) &&
              multiply_base->ToString() == shift_base2->ToString()) {
            int64_t multiplier = factor + (binary.Op() == "+" ? (1LL << shift2) : -(1LL << shift2));
            if (multiplier > 1) {
              return CExpression::Binary("*", multiply_base->Clone(),
                                         CExpression::Integer(multiplier));
            }
          }
        }
      }

      return CExpression::Binary(binary.Op(), std::move(lhs), std::move(rhs));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      return CExpression::Assignment(assignment.Op(),
                                     FoldShiftAddMultiplyInExpression(assignment.Lhs()),
                                     FoldShiftAddMultiplyInExpression(assignment.Rhs()));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      std::vector<std::unique_ptr<CExpression>> arguments;
      arguments.reserve(call.Arguments().size());
      for (const auto& argument : call.Arguments()) {
        arguments.push_back(FoldShiftAddMultiplyInExpression(*argument));
      }
      return CExpression::Call(FoldShiftAddMultiplyInExpression(call.Callee()),
                               std::move(arguments));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      return CExpression::Cast(cast.TargetType(), FoldShiftAddMultiplyInExpression(cast.Operand()));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return CExpression::MemberAccess(FoldShiftAddMultiplyInExpression(member.Object()),
                                       member.MemberName(), member.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return CExpression::ArrayIndex(FoldShiftAddMultiplyInExpression(array_index.Array()),
                                     FoldShiftAddMultiplyInExpression(array_index.Index()));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CExpression::Ternary(FoldShiftAddMultiplyInExpression(ternary.Condition()),
                                  FoldShiftAddMultiplyInExpression(ternary.TrueExpression()),
                                  FoldShiftAddMultiplyInExpression(ternary.FalseExpression()));
    }
    default:
      return expression.Clone();
  }
}

std::unique_ptr<CStatement> FoldShiftAddMultiplyInStatement(const CStatement& statement) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> statements;
      for (const auto& child : static_cast<const CompoundStatement&>(statement).Statements()) {
        statements.push_back(FoldShiftAddMultiplyInStatement(*child));
      }
      return std::make_unique<CompoundStatement>(std::move(statements));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(FoldShiftAddMultiplyInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression()));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      return CStatement::VariableDeclaration(
          declaration.Type(), declaration.Name(),
          declaration.Initializer() ? FoldShiftAddMultiplyInExpression(*declaration.Initializer())
                                    : nullptr,
          declaration.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return CStatement::If(FoldShiftAddMultiplyInExpression(if_statement.Condition()),
                            FoldShiftAddMultiplyInStatement(if_statement.ThenBranch()),
                            if_statement.ElseBranch()
                                ? FoldShiftAddMultiplyInStatement(*if_statement.ElseBranch())
                                : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      return CStatement::While(FoldShiftAddMultiplyInExpression(while_statement.Condition()),
                               FoldShiftAddMultiplyInStatement(while_statement.Body()));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      return CStatement::DoWhile(FoldShiftAddMultiplyInStatement(do_while_statement.Body()),
                                 FoldShiftAddMultiplyInExpression(do_while_statement.Condition()));
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      return CStatement::For(
          for_statement.Init() ? FoldShiftAddMultiplyInStatement(*for_statement.Init()) : nullptr,
          for_statement.Condition() ? FoldShiftAddMultiplyInExpression(*for_statement.Condition())
                                    : nullptr,
          for_statement.Step() ? FoldShiftAddMultiplyInExpression(*for_statement.Step()) : nullptr,
          FoldShiftAddMultiplyInStatement(for_statement.Body()));
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(FoldShiftAddMultiplyInExpression(*return_statement.ReturnValue()));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_statement = static_cast<const SwitchStatement&>(statement);
      std::vector<SwitchCase> cases;
      for (const auto& switch_case : switch_statement.Cases()) {
        SwitchCase new_case;
        new_case.case_values = switch_case.case_values;
        new_case.is_default = switch_case.is_default;
        if (switch_case.body != nullptr) {
          new_case.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              FoldShiftAddMultiplyInStatement(*switch_case.body).release()));
        }
        cases.push_back(std::move(new_case));
      }
      return CStatement::Switch(FoldShiftAddMultiplyInExpression(switch_statement.Condition()),
                                std::move(cases));
    }
    default:
      return statement.Clone();
  }
}

std::unique_ptr<CStatement> InlineOneTemporaryInStatement(const CStatement& statement,
                                                          const CompoundStatement& root_body,
                                                          bool prefer_hoist, bool is_top_level,
                                                          bool* inlined_any);

std::unique_ptr<CompoundStatement> InlineOneTemporaryInCompound(const CompoundStatement& compound,
                                                                const CompoundStatement& root_body,
                                                                bool prefer_hoist,
                                                                bool is_top_level,
                                                                bool* inlined_any) {
  const auto& statements = compound.Statements();
  for (size_t i = 0; i < statements.size(); ++i) {
    // 0. Eliminate dead pure assignments:
    //    - overwritten in the same block before any read, jump, label, or loop, OR
    //    - assigned once and never read anywhere in root_body (e.g. dead argN register setups).
    if (statements[i]->Kind() == CStatementKind::kExpressionStatement) {
      const auto& expression_statement = static_cast<const ExpressionStatement&>(*statements[i]);
      if (expression_statement.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assignment =
            static_cast<const AssignmentExpression&>(expression_statement.Expression());
        if (assignment.Op() == "=" && assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
            IsPureExpression(assignment.Rhs())) {
          const std::string& variable_name =
              static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
          bool dead_in_block = false;
          for (size_t k = i + 1; k < statements.size(); ++k) {
            if (statements[k]->Kind() == CStatementKind::kWhileStatement ||
                statements[k]->Kind() == CStatementKind::kDoWhileStatement ||
                statements[k]->Kind() == CStatementKind::kForStatement) {
              break;
            }
            bool always_overwrites = false;
            if (DoesStatementReadVariableBeforeOverwrite(*statements[k], variable_name,
                                                         &always_overwrites)) {
              break;
            }
            if (always_overwrites || statements[k]->Kind() == CStatementKind::kReturnStatement) {
              dead_in_block = true;
              break;
            }
            if (StatementContainsJumpOrLabel(*statements[k])) {
              break;
            }
          }
          bool is_local_variable = false;
          for (const auto& root_statement : root_body.Statements()) {
            if (root_statement->Kind() == CStatementKind::kVariableDeclarationStatement &&
                static_cast<const VariableDeclarationStatement&>(*root_statement).Name() ==
                    variable_name) {
              is_local_variable = true;
              break;
            }
          }
          const bool never_read_in_function =
              !StatementContainsGotoOrLabel(root_body) &&
              CountNonDeclarationOccurrencesInStatement(root_body, variable_name) == 1 &&
              CountIdentifierOccurrencesInExpression(assignment.Rhs(), variable_name) == 0;
          if (is_local_variable && (dead_in_block || never_read_in_function)) {
            std::vector<std::unique_ptr<CStatement>> next_statements;
            next_statements.reserve(statements.size() - 1);
            for (size_t k = 0; k < statements.size(); ++k) {
              if (k != i) next_statements.push_back(statements[k]->Clone());
            }
            *inlined_any = true;
            return std::make_unique<CompoundStatement>(std::move(next_statements));
          }
        }
      }
    }

    // 1. Check duplicated preheader + do-while/while(1) backedge delay-slot assignment:
    //    `variable = expression; do { ... variable = expression; } while (condition_without_var);`
    //    -> `do { variable = expression; ... } while (condition_without_var);`
    //    Also matches when `statements[i - 1]` is `initial_variable = constant_value;` and
    //    `statements[i]` has `constant_value` substituted for `initial_variable` in
    //    `body_statements.back()`.
    if (i + 1 < statements.size() &&
        statements[i]->Kind() == CStatementKind::kExpressionStatement &&
        (statements[i + 1]->Kind() == CStatementKind::kDoWhileStatement ||
         statements[i + 1]->Kind() == CStatementKind::kWhileStatement)) {
      const auto& preheader_statement = static_cast<const ExpressionStatement&>(*statements[i]);
      const CStatement* loop_body_pointer = nullptr;
      const CExpression* loop_condition_pointer = nullptr;
      bool is_do_while = (statements[i + 1]->Kind() == CStatementKind::kDoWhileStatement);
      bool is_infinite_while = false;
      if (is_do_while) {
        const auto& do_while_statement = static_cast<const DoWhileStatement&>(*statements[i + 1]);
        loop_body_pointer = &do_while_statement.Body();
        loop_condition_pointer = &do_while_statement.Condition();
      } else {
        const auto& while_statement = static_cast<const WhileStatement&>(*statements[i + 1]);
        if (while_statement.Condition().Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(while_statement.Condition()).Value() !=
                0) {
          is_infinite_while = true;
          loop_body_pointer = &while_statement.Body();
        }
      }
      if (preheader_statement.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
          loop_body_pointer != nullptr &&
          loop_body_pointer->Kind() == CStatementKind::kCompoundStatement &&
          (is_do_while ? !StatementContainsJumpOrLabel(*loop_body_pointer)
                       : !StatementContainsGotoOrLabel(*loop_body_pointer))) {
        const auto& preheader_assignment =
            static_cast<const AssignmentExpression&>(preheader_statement.Expression());
        const auto& body_statements =
            static_cast<const CompoundStatement&>(*loop_body_pointer).Statements();
        if (preheader_assignment.Op() == "=" &&
            preheader_assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
            IsPureExpression(preheader_assignment.Rhs()) && !body_statements.empty()) {
          bool backedge_matches_preheader =
              (body_statements.back()->ToString() == statements[i]->ToString());
          if (!backedge_matches_preheader && i > 0 &&
              statements[i - 1]->Kind() == CStatementKind::kExpressionStatement) {
            const auto& initializer_statement =
                static_cast<const ExpressionStatement&>(*statements[i - 1]);
            if (initializer_statement.Expression().Kind() ==
                CExpressionKind::kAssignmentExpression) {
              const auto& initializer_assignment =
                  static_cast<const AssignmentExpression&>(initializer_statement.Expression());
              if (initializer_assignment.Op() == "=" &&
                  initializer_assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
                  IsPureExpression(initializer_assignment.Rhs())) {
                const std::string& initializer_variable =
                    static_cast<const IdentifierExpression&>(initializer_assignment.Lhs()).Name();
                auto substituted_backedge = ReplaceIdentifierInSingleStatement(
                    *body_statements.back(), initializer_variable, initializer_assignment.Rhs());
                if (substituted_backedge->ToString() == statements[i]->ToString()) {
                  backedge_matches_preheader = true;
                }
              }
            }
          }
          if (backedge_matches_preheader) {
            const std::string& variable_name =
                static_cast<const IdentifierExpression&>(preheader_assignment.Lhs()).Name();
            if (is_do_while &&
                CountIdentifierOccurrencesInExpression(*loop_condition_pointer, variable_name) ==
                    0 &&
                IsVariableDeadAfter(statements, i + 1, variable_name, is_top_level, &root_body)) {
              std::vector<std::unique_ptr<CStatement>> new_loop_statements;
              new_loop_statements.reserve(body_statements.size());
              new_loop_statements.push_back(body_statements.back()->Clone());
              for (size_t b = 0; b + 1 < body_statements.size(); ++b) {
                new_loop_statements.push_back(body_statements[b]->Clone());
              }
              std::vector<std::unique_ptr<CStatement>> next_statements;
              next_statements.reserve(statements.size() - 1);
              for (size_t k = 0; k < i; ++k) {
                next_statements.push_back(statements[k]->Clone());
              }
              next_statements.push_back(CStatement::DoWhile(
                  std::make_unique<CompoundStatement>(std::move(new_loop_statements)),
                  loop_condition_pointer->Clone()));
              for (size_t k = i + 2; k < statements.size(); ++k) {
                next_statements.push_back(statements[k]->Clone());
              }
              *inlined_any = true;
              return std::make_unique<CompoundStatement>(std::move(next_statements));
            }
            if (is_infinite_while && body_statements.size() >= 2 &&
                body_statements[body_statements.size() - 2]->Kind() ==
                    CStatementKind::kIfStatement &&
                IsVariableDeadAfter(statements, i + 1, variable_name, is_top_level, &root_body)) {
              const auto& break_if_statement =
                  static_cast<const IfStatement&>(*body_statements[body_statements.size() - 2]);
              bool is_break_if = false;
              if (break_if_statement.ElseBranch() == nullptr &&
                  break_if_statement.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
                const auto& then_statements =
                    static_cast<const CompoundStatement&>(break_if_statement.ThenBranch())
                        .Statements();
                if (then_statements.size() == 1 &&
                    then_statements[0]->Kind() == CStatementKind::kBreakStatement) {
                  is_break_if = true;
                }
              }
              bool other_jumps = false;
              for (size_t b = 0; b + 2 < body_statements.size(); ++b) {
                if (StatementContainsJumpOrLabel(*body_statements[b])) {
                  other_jumps = true;
                  break;
                }
              }
              if (is_break_if && !other_jumps &&
                  CountIdentifierOccurrencesInExpression(break_if_statement.Condition(),
                                                         variable_name) == 0) {
                std::unique_ptr<CExpression> do_condition;
                if (break_if_statement.Condition().Kind() == CExpressionKind::kBinaryExpression) {
                  const auto& binary =
                      static_cast<const BinaryExpression&>(break_if_statement.Condition());
                  if (binary.Op() == "==" &&
                      binary.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
                      static_cast<const IntegerLiteralExpression&>(binary.Rhs()).Value() == 0) {
                    do_condition =
                        CExpression::Binary("!=", binary.Lhs().Clone(), CExpression::Integer(0));
                  }
                }
                if (do_condition == nullptr) {
                  do_condition = CExpression::Unary("!", break_if_statement.Condition().Clone());
                }
                std::vector<std::unique_ptr<CStatement>> new_loop_statements;
                new_loop_statements.reserve(body_statements.size() - 1);
                new_loop_statements.push_back(body_statements.back()->Clone());
                for (size_t b = 0; b + 2 < body_statements.size(); ++b) {
                  new_loop_statements.push_back(body_statements[b]->Clone());
                }
                std::vector<std::unique_ptr<CStatement>> next_statements;
                next_statements.reserve(statements.size() - 1);
                for (size_t k = 0; k < i; ++k) {
                  next_statements.push_back(statements[k]->Clone());
                }
                next_statements.push_back(CStatement::DoWhile(
                    std::make_unique<CompoundStatement>(std::move(new_loop_statements)),
                    std::move(do_condition)));
                for (size_t k = i + 2; k < statements.size(); ++k) {
                  next_statements.push_back(statements[k]->Clone());
                }
                *inlined_any = true;
                return std::make_unique<CompoundStatement>(std::move(next_statements));
              }
            }
          }
        }
      }
    }

    // 2. Check do-while induction copy folding:
    //    `do { ... v1 = v0; } while (condition_using_v0);` -> `do { ... v1 = v0; } while
    //    (condition_using_v1)`
    if (statements[i]->Kind() == CStatementKind::kDoWhileStatement) {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(*statements[i]);
      if (do_while_statement.Body().Kind() == CStatementKind::kCompoundStatement &&
          !StatementContainsJumpOrLabel(do_while_statement.Body())) {
        const auto& body_statements =
            static_cast<const CompoundStatement&>(do_while_statement.Body()).Statements();
        for (size_t m = body_statements.size(); m > 0; --m) {
          const auto& body_statement = *body_statements[m - 1];
          if (body_statement.Kind() != CStatementKind::kExpressionStatement) break;
          const auto& body_expression =
              static_cast<const ExpressionStatement&>(body_statement).Expression();
          if (body_expression.Kind() != CExpressionKind::kAssignmentExpression) break;
          const auto& body_assignment = static_cast<const AssignmentExpression&>(body_expression);
          if (body_assignment.Op() == "=" &&
              body_assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
              body_assignment.Rhs().Kind() == CExpressionKind::kIdentifier) {
            const std::string& destination_variable =
                static_cast<const IdentifierExpression&>(body_assignment.Lhs()).Name();
            const std::string& source_variable =
                static_cast<const IdentifierExpression&>(body_assignment.Rhs()).Name();
            if (destination_variable != source_variable &&
                CountIdentifierOccurrencesInExpression(do_while_statement.Condition(),
                                                       source_variable) > 0 &&
                IsVariableDeadAfter(statements, i, source_variable, is_top_level, &root_body)) {
              bool clobbered_after = false;
              for (size_t after_m = m; after_m < body_statements.size(); ++after_m) {
                if (CountIdentifierOccurrencesInStatement(*body_statements[after_m],
                                                          destination_variable) > 0 ||
                    CountIdentifierOccurrencesInStatement(*body_statements[after_m],
                                                          source_variable) > 0) {
                  clobbered_after = true;
                  break;
                }
              }
              if (!clobbered_after) {
                auto new_condition =
                    ReplaceIdentifierInExpression(do_while_statement.Condition(), source_variable,
                                                  *CExpression::Identifier(destination_variable));
                std::vector<std::unique_ptr<CStatement>> next_statements;
                next_statements.reserve(statements.size());
                for (size_t k = 0; k < statements.size(); ++k) {
                  if (k == i) {
                    next_statements.push_back(CStatement::DoWhile(do_while_statement.Body().Clone(),
                                                                  std::move(new_condition)));
                  } else {
                    next_statements.push_back(statements[k]->Clone());
                  }
                }
                *inlined_any = true;
                return std::make_unique<CompoundStatement>(std::move(next_statements));
              }
            }
          }
        }
      }
    }

    // 3. Check branch delay-slot assignment sinking into or across `if (condition) { ... }`:
    //    3a: `variable = expression; if (condition_without_variable) { first_uses_variable; ... }`
    //        -> `if (condition_without_variable) { variable = expression; first_uses_variable; }`
    //    3b (when !prefer_hoist): `temp_sX = temp_sX + c; if (condition_without_temp_sX) { ... }`
    //        -> `if (condition_without_temp_sX) { ... } temp_sX = temp_sX + c;`
    if (i + 1 < statements.size() &&
        statements[i]->Kind() == CStatementKind::kExpressionStatement &&
        statements[i + 1]->Kind() == CStatementKind::kIfStatement) {
      const auto& pre_expression_statement =
          static_cast<const ExpressionStatement&>(*statements[i]);
      const auto& if_statement = static_cast<const IfStatement&>(*statements[i + 1]);
      if (pre_expression_statement.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
          if_statement.ElseBranch() == nullptr &&
          if_statement.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
        const auto& pre_assignment =
            static_cast<const AssignmentExpression&>(pre_expression_statement.Expression());
        const auto& then_statements =
            static_cast<const CompoundStatement&>(if_statement.ThenBranch()).Statements();
        if (pre_assignment.Op() == "=" &&
            pre_assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
            IsPureExpression(pre_assignment.Rhs()) && !then_statements.empty()) {
          const std::string& variable_name =
              static_cast<const IdentifierExpression&>(pre_assignment.Lhs()).Name();
          if (CountIdentifierOccurrencesInExpression(if_statement.Condition(), variable_name) ==
                  0 &&
              CountIdentifierOccurrencesInStatement(*then_statements.front(), variable_name) > 0 &&
              IsVariableDeadAfter(statements, i + 1, variable_name, is_top_level, &root_body)) {
            std::vector<std::unique_ptr<CStatement>> new_then_statements;
            new_then_statements.reserve(then_statements.size() + 1);
            new_then_statements.push_back(statements[i]->Clone());
            for (const auto& then_statement : then_statements) {
              new_then_statements.push_back(then_statement->Clone());
            }
            std::vector<std::unique_ptr<CStatement>> next_statements;
            next_statements.reserve(statements.size() - 1);
            for (size_t k = 0; k < i; ++k) {
              next_statements.push_back(statements[k]->Clone());
            }
            next_statements.push_back(CStatement::If(
                if_statement.Condition().Clone(),
                std::make_unique<CompoundStatement>(std::move(new_then_statements)), nullptr));
            for (size_t k = i + 2; k < statements.size(); ++k) {
              next_statements.push_back(statements[k]->Clone());
            }
            *inlined_any = true;
            return std::make_unique<CompoundStatement>(std::move(next_statements));
          }
          if (!prefer_hoist && !StatementContainsJumpOrLabel(if_statement) &&
              CountIdentifierOccurrencesInStatement(if_statement, variable_name) == 0 &&
              !ExpressionReadsIndirectMemory(pre_assignment.Rhs())) {
            std::set<std::string> rhs_identifiers;
            CollectIdentifiersInExpression(pre_assignment.Rhs(), &rhs_identifiers);
            bool safe_to_sink_past_if = true;
            for (const auto& identifier : rhs_identifiers) {
              if (identifier != variable_name &&
                  CountIdentifierOccurrencesInStatement(if_statement, identifier) > 0) {
                safe_to_sink_past_if = false;
                break;
              }
            }
            if (safe_to_sink_past_if && i + 2 < statements.size() &&
                CountIdentifierOccurrencesInStatement(*statements[i + 2], variable_name) > 0) {
              std::vector<std::unique_ptr<CStatement>> next_statements;
              next_statements.reserve(statements.size());
              for (size_t k = 0; k < i; ++k) {
                next_statements.push_back(statements[k]->Clone());
              }
              next_statements.push_back(statements[i + 1]->Clone());
              next_statements.push_back(statements[i]->Clone());
              for (size_t k = i + 2; k < statements.size(); ++k) {
                next_statements.push_back(statements[k]->Clone());
              }
              *inlined_any = true;
              return std::make_unique<CompoundStatement>(std::move(next_statements));
            }
          }
        }
      }
    }

    InlineCandidateInfo info;
    if (AnalyzeInlineSingleUseTemporaryAt(statements, i, is_top_level, &info, &root_body)) {
      std::vector<std::unique_ptr<CStatement>> next_statements;
      next_statements.reserve(statements.size() - 1);
      for (size_t k = 0; k < i; ++k) {
        next_statements.push_back(statements[k]->Clone());
      }
      auto replaced = ReplaceIdentifierInSingleStatement(*statements[info.use_index],
                                                         info.variable_name, *info.rhs);
      if (prefer_hoist && info.can_hoist_to_def) {
        next_statements.push_back(std::move(replaced));
        for (size_t m = i + 1; m < info.use_index; ++m) {
          next_statements.push_back(statements[m]->Clone());
        }
      } else if (info.can_sink_to_use) {
        for (size_t m = i + 1; m < info.use_index; ++m) {
          next_statements.push_back(statements[m]->Clone());
        }
        next_statements.push_back(std::move(replaced));
      } else {
        next_statements.push_back(std::move(replaced));
        for (size_t m = i + 1; m < info.use_index; ++m) {
          next_statements.push_back(statements[m]->Clone());
        }
      }
      for (size_t k = info.use_index + 1; k < statements.size(); ++k) {
        next_statements.push_back(statements[k]->Clone());
      }
      *inlined_any = true;
      return std::make_unique<CompoundStatement>(std::move(next_statements));
    }
  }

  std::vector<std::unique_ptr<CStatement>> next_statements;
  next_statements.reserve(statements.size());
  for (const auto& statement : statements) {
    if (!*inlined_any) {
      next_statements.push_back(InlineOneTemporaryInStatement(*statement, root_body, prefer_hoist,
                                                              /*is_top_level=*/false, inlined_any));
    } else {
      next_statements.push_back(statement->Clone());
    }
  }
  return std::make_unique<CompoundStatement>(std::move(next_statements));
}

std::unique_ptr<CStatement> InlineOneTemporaryInStatement(const CStatement& statement,
                                                          const CompoundStatement& root_body,
                                                          bool prefer_hoist, bool is_top_level,
                                                          bool* inlined_any) {
  if (*inlined_any) return statement.Clone();
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement:
      return InlineOneTemporaryInCompound(static_cast<const CompoundStatement&>(statement),
                                          root_body, prefer_hoist, is_top_level, inlined_any);
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      auto new_then = InlineOneTemporaryInStatement(
          if_statement.ThenBranch(), root_body, prefer_hoist, /*is_top_level=*/false, inlined_any);
      std::unique_ptr<CStatement> new_else =
          if_statement.ElseBranch() != nullptr
              ? InlineOneTemporaryInStatement(*if_statement.ElseBranch(), root_body, prefer_hoist,
                                              /*is_top_level=*/false, inlined_any)
              : nullptr;
      return CStatement::If(if_statement.Condition().Clone(), std::move(new_then),
                            std::move(new_else));
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      auto new_body = InlineOneTemporaryInStatement(while_statement.Body(), root_body, prefer_hoist,
                                                    /*is_top_level=*/false, inlined_any);
      return CStatement::While(while_statement.Condition().Clone(), std::move(new_body));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      auto new_body = InlineOneTemporaryInStatement(
          do_while_statement.Body(), root_body, prefer_hoist, /*is_top_level=*/false, inlined_any);
      return CStatement::DoWhile(std::move(new_body), do_while_statement.Condition().Clone());
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      auto new_body = InlineOneTemporaryInStatement(for_statement.Body(), root_body, prefer_hoist,
                                                    /*is_top_level=*/false, inlined_any);
      return CStatement::For(
          for_statement.Init() ? for_statement.Init()->Clone() : nullptr,
          for_statement.Condition() ? for_statement.Condition()->Clone() : nullptr,
          for_statement.Step() ? for_statement.Step()->Clone() : nullptr, std::move(new_body));
    }
    default:
      return statement.Clone();
  }
}

std::unique_ptr<CompoundStatement> ApplyBulkInlineTemporaries(const CompoundStatement& body,
                                                              bool prefer_hoist) {
  auto current =
      std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(body.Clone().release()));
  for (size_t iteration = 0; iteration < 32; ++iteration) {
    bool inlined_any = false;
    current = InlineOneTemporaryInCompound(*current, *current, prefer_hoist, /*is_top_level=*/true,
                                           &inlined_any);
    current = std::unique_ptr<CompoundStatement>(
        static_cast<CompoundStatement*>(FoldShiftAddMultiplyInStatement(*current).release()));
    if (!inlined_any) break;
  }
  return PruneUnusedLocalDeclarations(*current);
}

}  // namespace

std::vector<AstMutationStep> AstMutator::DiscoverMutations(const FunctionDeclaration& function,
                                                           size_t max_mutations) {
  std::vector<AstMutationStep> mutations;
  ReferencedSymbols referenced_symbols = CollectReferencedSymbols(function);

  MutationContext context;
  context.discovered = &mutations;
  context.max_mutations = max_mutations;
  context.local_variables = referenced_symbols.local_variables;
  context.root_body = &function.Body();
  (void)TraverseCompound(function.Body(), &context, /*is_top_level_block=*/true);

  // Add bulk single-use temporary inlining if multiple inline sites exist
  if (context.inline_temporary_index > 1) {
    context.Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 1);
    context.Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3);
  }
  if (context.dead_assignment_index > 1 ||
      PruneUnusedLocalDeclarations(function.Body())->ToString() != function.Body().ToString()) {
    context.Record(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1);
  }
  if (context.pointer_offset_index > 1) {
    context.Record(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 0, 1);
  }

  // Discover direct typed global candidates (`*(T *)&D_...` -> `extern T D_...;`)
  auto typed_globals = CollectDirectTypedGlobalCandidates(function, referenced_symbols);
  for (size_t i = 0; i < typed_globals.size(); ++i) {
    context.Record(AST_MUTATION_DIRECT_TYPED_GLOBAL, static_cast<uint32_t>(i), 0);
  }
  if (typed_globals.size() > 1) {
    context.Record(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1);
  }

  // Discover 32-bit global array promotion candidates (`D_...` -> `D_...[0]`)
  auto array_globals = Collect32BitGlobalCandidates(function, referenced_symbols,
                                                    /*non_zero_or_pointer_only=*/false);
  for (size_t i = 0; i < array_globals.size(); ++i) {
    context.Record(AST_MUTATION_GLOBAL_ARRAY_ACCESS, static_cast<uint32_t>(i), 0);
  }
  if (!array_globals.empty()) {
    context.Record(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 1);
    context.Record(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 2);
  }

  // Discover parameter copy coalescing candidates (`temp_sX = argY` -> `argY`)
  auto parameter_copies = CollectParameterCopyCandidates(function);
  for (size_t i = 0; i < parameter_copies.size(); ++i) {
    context.Record(AST_MUTATION_COALESCE_PARAM_COPY, static_cast<uint32_t>(i), 0);
  }
  if (parameter_copies.size() > 1) {
    context.Record(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1);
  }

  return mutations;
}

std::vector<std::vector<AstMutationStep>> AstMutator::GenerateCanonicalPresets(
    const FunctionDeclaration& function) {
  auto make_step = [](AstMutationKind kind, uint32_t site, uint32_t parameter) {
    AstMutationStep step;
    step.set_kind(kind);
    step.set_site_index(site);
    step.set_param(parameter);
    return step;
  };

  std::vector<std::vector<AstMutationStep>> presets;
  // Preset 1: Dead assignment removal + direct typed globals + bulk inline (sink) + coalesce
  // parameters
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 1),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  // Preset 2: Dead assignment removal + direct typed globals + bulk inline (hoist) + coalesce
  // parameters
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  // Preset 3: Struct/Array promotion (smart non-zero globals + pointer offsets) after bulk inline
  // (hoist)
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3),
      make_step(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 2),
      make_step(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  // Preset 4: Struct/Array promotion (all globals + pointer offsets) after bulk inline (sink)
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 1),
      make_step(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 1),
      make_step(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 1),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  (void)function;
  return presets;
}

FunctionDeclaration AstMutator::ApplyMutation(const FunctionDeclaration& function,
                                              const AstMutationStep& step) {
  ReferencedSymbols referenced_symbols = CollectReferencedSymbols(function);

  if (step.kind() == AST_MUTATION_INLINE_SINGLE_USE_TEMP &&
      (step.param() == 1 || step.param() == 3)) {
    auto body = ApplyBulkInlineTemporaries(function.Body(), /*prefer_hoist=*/step.param() == 3);
    return FunctionDeclaration(function.ReturnType(), function.Name(), function.Parameters(),
                               std::move(body), function.IsStatic());
  }

  if (step.kind() == AST_MUTATION_DIRECT_TYPED_GLOBAL) {
    auto candidates = CollectDirectTypedGlobalCandidates(function, referenced_symbols);
    std::map<std::string, CType> target_map;
    if (step.param() == 1) {
      for (const auto& candidate : candidates) {
        target_map[candidate.symbol_name] = candidate.resolved_type;
      }
    } else if (step.site_index() < candidates.size()) {
      const auto& candidate = candidates[step.site_index()];
      target_map[candidate.symbol_name] = candidate.resolved_type;
    }
    if (target_map.empty()) return function.Clone();
    auto mutated_body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
        RewriteDirectTypedGlobalInStatement(function.Body(), target_map).release()));
    return FunctionDeclaration(function.ReturnType(), function.Name(), function.Parameters(),
                               std::move(mutated_body), function.IsStatic());
  }

  if (step.kind() == AST_MUTATION_GLOBAL_ARRAY_ACCESS) {
    auto candidates = Collect32BitGlobalCandidates(function, referenced_symbols,
                                                   /*non_zero_or_pointer_only=*/step.param() == 2);
    std::set<std::string> target_set;
    if (step.param() == 1 || step.param() == 2) {
      target_set.insert(candidates.begin(), candidates.end());
    } else if (step.site_index() < candidates.size()) {
      target_set.insert(candidates[step.site_index()]);
    }
    if (target_set.empty()) return function.Clone();
    auto address_temporaries =
        CollectSingleAssignmentGlobalAddressTemporaries(function, target_set);
    auto mutated_body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
        RewriteGlobalArrayInStatement(function.Body(), target_set, address_temporaries).release()));
    mutated_body = PruneUnusedLocalDeclarations(*mutated_body);
    return FunctionDeclaration(function.ReturnType(), function.Name(), function.Parameters(),
                               std::move(mutated_body), function.IsStatic());
  }

  if (step.kind() == AST_MUTATION_COALESCE_PARAM_COPY) {
    FunctionDeclaration current = function.Clone();
    if (step.param() == 1) {
      for (size_t iteration = 0; iteration < 8; ++iteration) {
        auto candidates = CollectParameterCopyCandidates(current);
        if (candidates.empty()) break;
        auto body = ApplyCoalesceParameterCopy(current, candidates[0]);
        current = FunctionDeclaration(current.ReturnType(), current.Name(), current.Parameters(),
                                      std::move(body), current.IsStatic());
      }
      return current;
    }
    auto candidates = CollectParameterCopyCandidates(current);
    if (step.site_index() >= candidates.size()) return current;
    auto body = ApplyCoalesceParameterCopy(current, candidates[step.site_index()]);
    return FunctionDeclaration(current.ReturnType(), current.Name(), current.Parameters(),
                               std::move(body), current.IsStatic());
  }

  MutationContext context;
  context.step_to_apply = &step;
  context.local_variables = referenced_symbols.local_variables;
  context.root_body = &function.Body();
  auto mutated_body = TraverseCompound(function.Body(), &context, /*is_top_level_block=*/true);
  if (step.kind() == AST_MUTATION_INLINE_SINGLE_USE_TEMP ||
      step.kind() == AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN) {
    mutated_body = PruneUnusedLocalDeclarations(*mutated_body);
  }
  return FunctionDeclaration(function.ReturnType(), function.Name(), function.Parameters(),
                             std::move(mutated_body), function.IsStatic());
}

FunctionDeclaration AstMutator::ApplyMutations(const FunctionDeclaration& function,
                                               absl::Span<const AstMutationStep> steps) {
  FunctionDeclaration current = function.Clone();
  for (const auto& step : steps) {
    current = ApplyMutation(current, step);
  }
  return current;
}

std::string AstMutator::DescribeStep(const AstMutationStep& step) {
  switch (step.kind()) {
    case AST_MUTATION_COMMUTE_BINARY_OP:
      return absl::StrFormat("CommuteBinaryOp(site=%u)", step.site_index());
    case AST_MUTATION_INVERT_IF_ELSE:
      return absl::StrFormat("InvertIfElse(site=%u)", step.site_index());
    case AST_MUTATION_REORDER_LOCAL_DECLS:
      return absl::StrFormat("ReorderLocalDecls(site=%u)", step.site_index());
    case AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT:
      return absl::StrFormat("ToggleCompoundAssign(site=%u)", step.site_index());
    case AST_MUTATION_WHILE_TO_FOR_LOOP:
      return absl::StrFormat("WhileToForLoop(site=%u)", step.site_index());
    case AST_MUTATION_TOGGLE_UNSIGNED_CAST:
      return absl::StrFormat("ToggleUnsignedCast(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_INLINE_SINGLE_USE_TEMP:
      return absl::StrFormat("InlineSingleUseTemp(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_DIRECT_TYPED_GLOBAL:
      return absl::StrFormat("DirectTypedGlobal(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_GLOBAL_ARRAY_ACCESS:
      return absl::StrFormat("GlobalArrayAccess(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX:
      return absl::StrFormat("PointerOffsetToArrayIndex(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_SWAP_ADJACENT_STATEMENTS:
      return absl::StrFormat("SwapAdjacentStatements(site=%u)", step.site_index());
    case AST_MUTATION_COALESCE_PARAM_COPY:
      return absl::StrFormat("CoalesceParamCopy(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN:
      return absl::StrFormat("EliminateDeadCallReturn(site=%u, param=%u)", step.site_index(),
                             step.param());
    default:
      return "UnspecifiedMutation";
  }
}

}  // namespace rom_nom_nom
