#include "tuner/ast_global_mutations.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"

namespace rom_nom_nom {

bool ExtractGlobalCastTarget(const CExpression& expression, std::string* out_symbol,
                             CType* out_type) {
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

namespace {

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

bool ExtractGlobalAssignmentTarget(const CStatement& stmt,
                                   const ReferencedSymbols& referenced_symbols,
                                   std::string* out_name) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expr_stmt = static_cast<const ExpressionStatement&>(stmt);
  if (expr_stmt.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assignment = static_cast<const AssignmentExpression&>(expr_stmt.Expression());
  if (assignment.Op() != "=") return false;
  if (assignment.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  const std::string& name = static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
  if (referenced_symbols.local_variables.count(name) > 0) return false;
  if (referenced_symbols.external_data.count(name) > 0 || absl::StartsWith(name, "D_")) {
    *out_name = name;
    return true;
  }
  return false;
}

void CollectVolatileGlobalsFromCompound(const CompoundStatement& compound,
                                        const ReferencedSymbols& referenced_symbols,
                                        std::set<std::string>* candidate_set) {
  const auto& statements = compound.Statements();
  for (size_t i = 0; i < statements.size(); ++i) {
    std::string global_name;
    if (!ExtractGlobalAssignmentTarget(*statements[i], referenced_symbols, &global_name)) {
      continue;
    }
    for (size_t j = i + 1; j < statements.size(); ++j) {
      if (StatementContainsCall(*statements[j])) {
        break;
      }
      std::string next_target;
      if (ExtractGlobalAssignmentTarget(*statements[j], referenced_symbols, &next_target) &&
          next_target == global_name) {
        const auto& expr_stmt = static_cast<const ExpressionStatement&>(*statements[j]);
        const auto& assign = static_cast<const AssignmentExpression&>(expr_stmt.Expression());
        if (CountIdentifierOccurrencesInExpression(assign.Rhs(), global_name) == 0) {
          candidate_set->insert(global_name);
        }
        break;
      }
      if (CountIdentifierOccurrencesInStatement(*statements[j], global_name) > 0) {
        break;
      }
    }
  }

  for (const auto& stmt : statements) {
    if (stmt == nullptr) continue;
    switch (stmt->Kind()) {
      case CStatementKind::kCompoundStatement:
        CollectVolatileGlobalsFromCompound(static_cast<const CompoundStatement&>(*stmt),
                                           referenced_symbols, candidate_set);
        break;
      case CStatementKind::kIfStatement: {
        const auto& if_stmt = static_cast<const IfStatement&>(*stmt);
        if (if_stmt.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
          CollectVolatileGlobalsFromCompound(
              static_cast<const CompoundStatement&>(if_stmt.ThenBranch()), referenced_symbols,
              candidate_set);
        }
        if (if_stmt.ElseBranch() != nullptr &&
            if_stmt.ElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          CollectVolatileGlobalsFromCompound(
              static_cast<const CompoundStatement&>(*if_stmt.ElseBranch()), referenced_symbols,
              candidate_set);
        }
        break;
      }
      case CStatementKind::kWhileStatement: {
        const auto& while_stmt = static_cast<const WhileStatement&>(*stmt);
        if (while_stmt.Body().Kind() == CStatementKind::kCompoundStatement) {
          CollectVolatileGlobalsFromCompound(
              static_cast<const CompoundStatement&>(while_stmt.Body()), referenced_symbols,
              candidate_set);
        }
        break;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& do_while = static_cast<const DoWhileStatement&>(*stmt);
        if (do_while.Body().Kind() == CStatementKind::kCompoundStatement) {
          CollectVolatileGlobalsFromCompound(static_cast<const CompoundStatement&>(do_while.Body()),
                                             referenced_symbols, candidate_set);
        }
        break;
      }
      case CStatementKind::kForStatement: {
        const auto& for_stmt = static_cast<const ForStatement&>(*stmt);
        if (for_stmt.Body().Kind() == CStatementKind::kCompoundStatement) {
          CollectVolatileGlobalsFromCompound(static_cast<const CompoundStatement&>(for_stmt.Body()),
                                             referenced_symbols, candidate_set);
        }
        break;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& sw = static_cast<const SwitchStatement&>(*stmt);
        for (const auto& c : sw.Cases()) {
          if (c.body != nullptr) {
            CollectVolatileGlobalsFromCompound(*c.body, referenced_symbols, candidate_set);
          }
        }
        break;
      }
      default:
        break;
    }
  }
}

std::unique_ptr<CExpression> RewriteVolatileGlobalAccessInExpression(
    const CExpression& expression, const std::set<std::string>& target_globals,
    const ReferencedSymbols& referenced_symbols, bool use_inline_cast) {
  switch (expression.Kind()) {
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      std::unique_ptr<CExpression> new_lhs;
      if (assignment.Lhs().Kind() == CExpressionKind::kIdentifier) {
        const std::string& name = static_cast<const IdentifierExpression&>(assignment.Lhs()).Name();
        if (target_globals.count(name) > 0) {
          std::string base_type = "s32";
          auto it = referenced_symbols.inferred_data_types.find(name);
          if (it != referenced_symbols.inferred_data_types.end() && !it->second.name.empty() &&
              it->second.name != "void") {
            base_type = it->second.ToString();
          }
          if (use_inline_cast) {
            CType volatile_ptr_type = CType::Named("volatile " + base_type).MakePointer();
            new_lhs = CExpression::Unary(
                "*",
                CExpression::Cast(
                    volatile_ptr_type,
                    CExpression::Unary("&", CExpression::Identifier(name), /*is_prefix=*/true)),
                /*is_prefix=*/true);
          } else {
            new_lhs = CExpression::Identifier(name, CType::Named("volatile " + base_type));
          }
        }
      }
      if (new_lhs == nullptr) {
        new_lhs = RewriteVolatileGlobalAccessInExpression(assignment.Lhs(), target_globals,
                                                          referenced_symbols, use_inline_cast);
      }
      auto new_rhs = RewriteVolatileGlobalAccessInExpression(assignment.Rhs(), target_globals,
                                                             referenced_symbols, use_inline_cast);
      return CExpression::Assignment(assignment.Op(), std::move(new_lhs), std::move(new_rhs));
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      return CExpression::Unary(
          unary.Op(),
          RewriteVolatileGlobalAccessInExpression(unary.Operand(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          unary.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return CExpression::Binary(
          binary.Op(),
          RewriteVolatileGlobalAccessInExpression(binary.Lhs(), target_globals, referenced_symbols,
                                                  use_inline_cast),
          RewriteVolatileGlobalAccessInExpression(binary.Rhs(), target_globals, referenced_symbols,
                                                  use_inline_cast));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      return CExpression::Cast(cast.TargetType(), RewriteVolatileGlobalAccessInExpression(
                                                      cast.Operand(), target_globals,
                                                      referenced_symbols, use_inline_cast));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(call.Arguments().size());
      for (const auto& arg : call.Arguments()) {
        args.push_back(RewriteVolatileGlobalAccessInExpression(
            *arg, target_globals, referenced_symbols, use_inline_cast));
      }
      return CExpression::Call(
          RewriteVolatileGlobalAccessInExpression(call.Callee(), target_globals, referenced_symbols,
                                                  use_inline_cast),
          std::move(args));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return CExpression::MemberAccess(
          RewriteVolatileGlobalAccessInExpression(member.Object(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          member.MemberName(), member.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return CExpression::ArrayIndex(
          RewriteVolatileGlobalAccessInExpression(array_index.Array(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          RewriteVolatileGlobalAccessInExpression(array_index.Index(), target_globals,
                                                  referenced_symbols, use_inline_cast));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CExpression::Ternary(
          RewriteVolatileGlobalAccessInExpression(ternary.Condition(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          RewriteVolatileGlobalAccessInExpression(ternary.TrueExpression(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          RewriteVolatileGlobalAccessInExpression(ternary.FalseExpression(), target_globals,
                                                  referenced_symbols, use_inline_cast));
    }
    default:
      return expression.Clone();
  }
}

}  // namespace

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

std::vector<std::string> CollectVolatileGlobalCandidates(
    const FunctionDeclaration& function, const ReferencedSymbols& referenced_symbols) {
  std::set<std::string> candidates;
  CollectVolatileGlobalsFromCompound(function.Body(), referenced_symbols, &candidates);
  return std::vector<std::string>(candidates.begin(), candidates.end());
}

std::unique_ptr<CStatement> RewriteVolatileGlobalAccessInStatement(
    const CStatement& statement, const std::set<std::string>& target_globals,
    const ReferencedSymbols& referenced_symbols, bool use_inline_cast) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> statements;
      for (const auto& child : static_cast<const CompoundStatement&>(statement).Statements()) {
        if (child != nullptr) {
          statements.push_back(RewriteVolatileGlobalAccessInStatement(
              *child, target_globals, referenced_symbols, use_inline_cast));
        }
      }
      return std::make_unique<CompoundStatement>(std::move(statements));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(RewriteVolatileGlobalAccessInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), target_globals,
          referenced_symbols, use_inline_cast));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      return CStatement::VariableDeclaration(
          declaration.Type(), declaration.Name(),
          declaration.Initializer()
              ? RewriteVolatileGlobalAccessInExpression(*declaration.Initializer(), target_globals,
                                                        referenced_symbols, use_inline_cast)
              : nullptr,
          declaration.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return CStatement::If(
          RewriteVolatileGlobalAccessInExpression(if_statement.Condition(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          RewriteVolatileGlobalAccessInStatement(if_statement.ThenBranch(), target_globals,
                                                 referenced_symbols, use_inline_cast),
          if_statement.ElseBranch()
              ? RewriteVolatileGlobalAccessInStatement(*if_statement.ElseBranch(), target_globals,
                                                       referenced_symbols, use_inline_cast)
              : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_statement = static_cast<const WhileStatement&>(statement);
      return CStatement::While(
          RewriteVolatileGlobalAccessInExpression(while_statement.Condition(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          RewriteVolatileGlobalAccessInStatement(while_statement.Body(), target_globals,
                                                 referenced_symbols, use_inline_cast));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while_statement = static_cast<const DoWhileStatement&>(statement);
      return CStatement::DoWhile(
          RewriteVolatileGlobalAccessInStatement(do_while_statement.Body(), target_globals,
                                                 referenced_symbols, use_inline_cast),
          RewriteVolatileGlobalAccessInExpression(do_while_statement.Condition(), target_globals,
                                                  referenced_symbols, use_inline_cast));
    }
    case CStatementKind::kForStatement: {
      const auto& for_statement = static_cast<const ForStatement&>(statement);
      return CStatement::For(
          for_statement.Init()
              ? RewriteVolatileGlobalAccessInStatement(*for_statement.Init(), target_globals,
                                                       referenced_symbols, use_inline_cast)
              : nullptr,
          for_statement.Condition()
              ? RewriteVolatileGlobalAccessInExpression(*for_statement.Condition(), target_globals,
                                                        referenced_symbols, use_inline_cast)
              : nullptr,
          for_statement.Step()
              ? RewriteVolatileGlobalAccessInExpression(*for_statement.Step(), target_globals,
                                                        referenced_symbols, use_inline_cast)
              : nullptr,
          RewriteVolatileGlobalAccessInStatement(for_statement.Body(), target_globals,
                                                 referenced_symbols, use_inline_cast));
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(RewriteVolatileGlobalAccessInExpression(
          *return_statement.ReturnValue(), target_globals, referenced_symbols, use_inline_cast));
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
              RewriteVolatileGlobalAccessInStatement(*switch_case.body, target_globals,
                                                     referenced_symbols, use_inline_cast)
                  .release()));
        }
        cases.push_back(std::move(new_case));
      }
      return CStatement::Switch(
          RewriteVolatileGlobalAccessInExpression(switch_statement.Condition(), target_globals,
                                                  referenced_symbols, use_inline_cast),
          std::move(cases));
    }
    default:
      return statement.Clone();
  }
}

}  // namespace rom_nom_nom
