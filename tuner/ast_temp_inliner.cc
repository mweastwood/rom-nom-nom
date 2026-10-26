#include "tuner/ast_temp_inliner.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "tuner/ast_global_mutations.h"

namespace rom_nom_nom {

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
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      return ExpressionReadsIndirectMemory(assignment.Lhs()) ||
             ExpressionReadsIndirectMemory(assignment.Rhs());
    }
    case CExpressionKind::kCallExpression:
      return true;
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
                                              bool* out_always_overwrites) {
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

bool IsVariableDeadAfter(const std::vector<std::unique_ptr<CStatement>>& statements,
                         size_t after_index, std::string_view variable_name,
                         bool is_top_level_block, const CompoundStatement* root_body,
                         int max_dead_occurrences) {
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
    if (CountNonDeclarationOccurrencesInStatement(*root_body, variable_name) <=
        max_dead_occurrences) {
      return true;
    }
    const auto& root_statements = root_body->Statements();
    for (size_t top_index = 0; top_index < root_statements.size(); ++top_index) {
      if (root_statements[top_index]->Kind() == CStatementKind::kIfStatement) {
        const auto& if_statement = static_cast<const IfStatement&>(*root_statements[top_index]);
        bool is_immediate_branch = false;
        if (if_statement.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
            &static_cast<const CompoundStatement&>(if_statement.ThenBranch()).Statements() ==
                &statements) {
          is_immediate_branch = true;
        } else if (if_statement.ElseBranch() != nullptr &&
                   if_statement.ElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
                   &static_cast<const CompoundStatement&>(*if_statement.ElseBranch())
                           .Statements() == &statements) {
          is_immediate_branch = true;
        }
        if (is_immediate_branch) {
          return IsVariableDeadAfter(root_statements, top_index, variable_name,
                                     /*is_top_level_block=*/true, nullptr);
        }
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
                                                           const CExpression& replacement,
                                                           bool replace_assignment_lhs) {
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
      return CExpression::Unary(unary.Op(),
                                ReplaceIdentifierInExpression(unary.Operand(), target_name,
                                                              replacement, replace_assignment_lhs),
                                unary.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return CExpression::Binary(
          binary.Op(),
          ReplaceIdentifierInExpression(binary.Lhs(), target_name, replacement,
                                        replace_assignment_lhs),
          ReplaceIdentifierInExpression(binary.Rhs(), target_name, replacement,
                                        replace_assignment_lhs));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      std::string compound_operator;
      const CExpression* compound_rhs = nullptr;
      if (IsReadModifyWriteThroughVariable(assignment, target_name, &compound_operator,
                                           &compound_rhs)) {
        return CExpression::Assignment(
            compound_operator,
            ReplaceIdentifierInExpression(assignment.Lhs(), target_name, replacement,
                                          replace_assignment_lhs),
            ReplaceIdentifierInExpression(*compound_rhs, target_name, replacement,
                                          replace_assignment_lhs));
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
          (!replace_assignment_lhs && assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
           static_cast<const IdentifierExpression&>(assignment.Lhs()).Name() == target_name &&
           assignment.Op() == "=")
              ? assignment.Lhs().Clone()
              : ReplaceIdentifierInExpression(assignment.Lhs(), target_name, replacement,
                                              replace_assignment_lhs);
      return CExpression::Assignment(
          assignment.Op(), std::move(new_lhs),
          ReplaceIdentifierInExpression(assignment.Rhs(), target_name, replacement,
                                        replace_assignment_lhs));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      std::vector<std::unique_ptr<CExpression>> arguments;
      arguments.reserve(call.Arguments().size());
      for (const auto& argument : call.Arguments()) {
        arguments.push_back(ReplaceIdentifierInExpression(*argument, target_name, replacement,
                                                          replace_assignment_lhs));
      }
      return CExpression::Call(ReplaceIdentifierInExpression(call.Callee(), target_name,
                                                             replacement, replace_assignment_lhs),
                               std::move(arguments));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expression);
      return CExpression::Cast(
          cast.TargetType(), ReplaceIdentifierInExpression(cast.Operand(), target_name, replacement,
                                                           replace_assignment_lhs));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expression);
      return CExpression::MemberAccess(
          ReplaceIdentifierInExpression(member.Object(), target_name, replacement,
                                        replace_assignment_lhs),
          member.MemberName(), member.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return CExpression::ArrayIndex(
          ReplaceIdentifierInExpression(array_index.Array(), target_name, replacement,
                                        replace_assignment_lhs),
          ReplaceIdentifierInExpression(array_index.Index(), target_name, replacement,
                                        replace_assignment_lhs));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return CExpression::Ternary(
          ReplaceIdentifierInExpression(ternary.Condition(), target_name, replacement,
                                        replace_assignment_lhs),
          ReplaceIdentifierInExpression(ternary.TrueExpression(), target_name, replacement,
                                        replace_assignment_lhs),
          ReplaceIdentifierInExpression(ternary.FalseExpression(), target_name, replacement,
                                        replace_assignment_lhs));
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

std::unique_ptr<CStatement> ReplaceIdentifierInStatement(const CStatement& statement,
                                                         std::string_view target_name,
                                                         const CExpression& replacement,
                                                         bool replace_assignment_lhs) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& compound = static_cast<const CompoundStatement&>(statement);
      std::vector<std::unique_ptr<CStatement>> new_statements;
      new_statements.reserve(compound.Statements().size());
      for (const auto& child : compound.Statements()) {
        if (child != nullptr) {
          new_statements.push_back(ReplaceIdentifierInStatement(*child, target_name, replacement,
                                                                replace_assignment_lhs));
        }
      }
      return std::make_unique<CompoundStatement>(std::move(new_statements));
    }
    case CStatementKind::kExpressionStatement: {
      const auto& expression_statement = static_cast<const ExpressionStatement&>(statement);
      return CStatement::Expression(ReplaceIdentifierInExpression(
          expression_statement.Expression(), target_name, replacement, replace_assignment_lhs));
    }
    case CStatementKind::kReturnStatement: {
      const auto& return_statement = static_cast<const ReturnStatement&>(statement);
      if (return_statement.ReturnValue() == nullptr) return return_statement.Clone();
      return CStatement::Return(ReplaceIdentifierInExpression(
          *return_statement.ReturnValue(), target_name, replacement, replace_assignment_lhs));
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      return CStatement::If(ReplaceIdentifierInExpression(if_statement.Condition(), target_name,
                                                          replacement, replace_assignment_lhs),
                            ReplaceIdentifierInStatement(if_statement.ThenBranch(), target_name,
                                                         replacement, replace_assignment_lhs),
                            if_statement.ElseBranch() ? ReplaceIdentifierInStatement(
                                                            *if_statement.ElseBranch(), target_name,
                                                            replacement, replace_assignment_lhs)
                                                      : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_stmt = static_cast<const WhileStatement&>(statement);
      return CStatement::While(ReplaceIdentifierInExpression(while_stmt.Condition(), target_name,
                                                             replacement, replace_assignment_lhs),
                               ReplaceIdentifierInStatement(while_stmt.Body(), target_name,
                                                            replacement, replace_assignment_lhs));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while = static_cast<const DoWhileStatement&>(statement);
      return CStatement::DoWhile(
          ReplaceIdentifierInStatement(do_while.Body(), target_name, replacement,
                                       replace_assignment_lhs),
          ReplaceIdentifierInExpression(do_while.Condition(), target_name, replacement,
                                        replace_assignment_lhs));
    }
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(statement);
      auto new_init = for_stmt.Init() != nullptr
                          ? ReplaceIdentifierInStatement(*for_stmt.Init(), target_name, replacement,
                                                         replace_assignment_lhs)
                          : nullptr;
      auto new_cond = for_stmt.Condition() != nullptr
                          ? ReplaceIdentifierInExpression(*for_stmt.Condition(), target_name,
                                                          replacement, replace_assignment_lhs)
                          : nullptr;
      auto new_step = for_stmt.Step() != nullptr
                          ? ReplaceIdentifierInExpression(*for_stmt.Step(), target_name,
                                                          replacement, replace_assignment_lhs)
                          : nullptr;
      return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                             ReplaceIdentifierInStatement(for_stmt.Body(), target_name, replacement,
                                                          replace_assignment_lhs));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_stmt = static_cast<const SwitchStatement&>(statement);
      std::vector<SwitchCase> new_cases;
      new_cases.reserve(switch_stmt.Cases().size());
      for (const auto& sc : switch_stmt.Cases()) {
        SwitchCase new_case;
        new_case.case_values = sc.case_values;
        new_case.is_default = sc.is_default;
        if (sc.body != nullptr) {
          auto replaced_body = ReplaceIdentifierInStatement(*sc.body, target_name, replacement,
                                                            replace_assignment_lhs);
          new_case.body = std::unique_ptr<CompoundStatement>(
              static_cast<CompoundStatement*>(replaced_body.release()));
        }
        new_cases.push_back(std::move(new_case));
      }
      return CStatement::Switch(ReplaceIdentifierInExpression(switch_stmt.Condition(), target_name,
                                                              replacement, replace_assignment_lhs),
                                std::move(new_cases));
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(statement);
      if (decl.Name() == target_name) {
        return decl.Clone();
      }
      auto new_init = decl.Initializer() != nullptr
                          ? ReplaceIdentifierInExpression(*decl.Initializer(), target_name,
                                                          replacement, replace_assignment_lhs)
                          : nullptr;
      return CStatement::VariableDeclaration(decl.Type(), decl.Name(), std::move(new_init),
                                             decl.ArraySize(), decl.RegisterAnnotation());
    }
    default:
      return statement.Clone();
  }
}

bool IsAddressOfExpression(const CExpression& expression) {
  if (expression.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& unary = static_cast<const UnaryExpression&>(expression);
    return unary.IsPrefix() && (unary.Op().find('&') != std::string::npos);
  }
  if (expression.Kind() == CExpressionKind::kCastExpression) {
    return IsAddressOfExpression(static_cast<const CastExpression&>(expression).Operand());
  }
  return false;
}

bool IsIdentifierUsedInBinaryArithmetic(const CExpression& expression,
                                        std::string_view variable_name) {
  switch (expression.Kind()) {
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      if (binary.Op() == "+" || binary.Op() == "-") {
        if (CountIdentifierOccurrencesInExpression(binary.Lhs(), variable_name) > 0 ||
            CountIdentifierOccurrencesInExpression(binary.Rhs(), variable_name) > 0) {
          return true;
        }
      }
      return IsIdentifierUsedInBinaryArithmetic(binary.Lhs(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(binary.Rhs(), variable_name);
    }
    case CExpressionKind::kUnaryExpression:
      return IsIdentifierUsedInBinaryArithmetic(
          static_cast<const UnaryExpression&>(expression).Operand(), variable_name);
    case CExpressionKind::kCastExpression:
      return IsIdentifierUsedInBinaryArithmetic(
          static_cast<const CastExpression&>(expression).Operand(), variable_name);
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expression);
      return IsIdentifierUsedInBinaryArithmetic(assign.Lhs(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(assign.Rhs(), variable_name);
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      if (IsIdentifierUsedInBinaryArithmetic(call.Callee(), variable_name)) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && IsIdentifierUsedInBinaryArithmetic(*arg, variable_name)) {
          return true;
        }
      }
      return false;
    }
    case CExpressionKind::kMemberAccessExpression:
      return IsIdentifierUsedInBinaryArithmetic(
          static_cast<const MemberAccessExpression&>(expression).Object(), variable_name);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expression);
      return IsIdentifierUsedInBinaryArithmetic(arr.Array(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(arr.Index(), variable_name);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return IsIdentifierUsedInBinaryArithmetic(ternary.Condition(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(ternary.TrueExpression(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(ternary.FalseExpression(), variable_name);
    }
    default:
      return false;
  }
}

bool IsIdentifierUsedInBinaryArithmetic(const CStatement& statement,
                                        std::string_view variable_name) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& compound = static_cast<const CompoundStatement&>(statement);
      for (const auto& child : compound.Statements()) {
        if (child != nullptr && IsIdentifierUsedInBinaryArithmetic(*child, variable_name)) {
          return true;
        }
      }
      return false;
    }
    case CStatementKind::kExpressionStatement:
      return IsIdentifierUsedInBinaryArithmetic(
          static_cast<const ExpressionStatement&>(statement).Expression(), variable_name);
    case CStatementKind::kReturnStatement: {
      const auto& ret = static_cast<const ReturnStatement&>(statement);
      return ret.ReturnValue() != nullptr &&
             IsIdentifierUsedInBinaryArithmetic(*ret.ReturnValue(), variable_name);
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(statement);
      return IsIdentifierUsedInBinaryArithmetic(if_stmt.Condition(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(if_stmt.ThenBranch(), variable_name) ||
             (if_stmt.ElseBranch() != nullptr &&
              IsIdentifierUsedInBinaryArithmetic(*if_stmt.ElseBranch(), variable_name));
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_stmt = static_cast<const WhileStatement&>(statement);
      return IsIdentifierUsedInBinaryArithmetic(while_stmt.Condition(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(while_stmt.Body(), variable_name);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while = static_cast<const DoWhileStatement&>(statement);
      return IsIdentifierUsedInBinaryArithmetic(do_while.Body(), variable_name) ||
             IsIdentifierUsedInBinaryArithmetic(do_while.Condition(), variable_name);
    }
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(statement);
      return (for_stmt.Init() != nullptr &&
              IsIdentifierUsedInBinaryArithmetic(*for_stmt.Init(), variable_name)) ||
             (for_stmt.Condition() != nullptr &&
              IsIdentifierUsedInBinaryArithmetic(*for_stmt.Condition(), variable_name)) ||
             (for_stmt.Step() != nullptr &&
              IsIdentifierUsedInBinaryArithmetic(*for_stmt.Step(), variable_name)) ||
             IsIdentifierUsedInBinaryArithmetic(for_stmt.Body(), variable_name);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_stmt = static_cast<const SwitchStatement&>(statement);
      if (IsIdentifierUsedInBinaryArithmetic(switch_stmt.Condition(), variable_name)) return true;
      for (const auto& sc : switch_stmt.Cases()) {
        if (sc.body != nullptr && IsIdentifierUsedInBinaryArithmetic(*sc.body, variable_name)) {
          return true;
        }
      }
      return false;
    }
    default:
      return false;
  }
}

bool IsIdentifierAssignedInExpression(const CExpression& expression, std::string_view name) {
  switch (expression.Kind()) {
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      if (assignment.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assignment.Lhs()).Name() == name) {
        return true;
      }
      return IsIdentifierAssignedInExpression(assignment.Lhs(), name) ||
             IsIdentifierAssignedInExpression(assignment.Rhs(), name);
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      if ((unary.Op() == "++" || unary.Op() == "--") &&
          unary.Operand().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(unary.Operand()).Name() == name) {
        return true;
      }
      return IsIdentifierAssignedInExpression(unary.Operand(), name);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return IsIdentifierAssignedInExpression(binary.Lhs(), name) ||
             IsIdentifierAssignedInExpression(binary.Rhs(), name);
    }
    case CExpressionKind::kCastExpression:
      return IsIdentifierAssignedInExpression(
          static_cast<const CastExpression&>(expression).Operand(), name);
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      if (IsIdentifierAssignedInExpression(call.Callee(), name)) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && IsIdentifierAssignedInExpression(*arg, name)) return true;
      }
      return false;
    }
    case CExpressionKind::kMemberAccessExpression:
      return IsIdentifierAssignedInExpression(
          static_cast<const MemberAccessExpression&>(expression).Object(), name);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return IsIdentifierAssignedInExpression(array_index.Array(), name) ||
             IsIdentifierAssignedInExpression(array_index.Index(), name);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return IsIdentifierAssignedInExpression(ternary.Condition(), name) ||
             IsIdentifierAssignedInExpression(ternary.TrueExpression(), name) ||
             IsIdentifierAssignedInExpression(ternary.FalseExpression(), name);
    }
    default:
      return false;
  }
}

bool IsIdentifierAssignedInStatement(const CStatement& statement, std::string_view name) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& compound = static_cast<const CompoundStatement&>(statement);
      for (const auto& child : compound.Statements()) {
        if (child != nullptr && IsIdentifierAssignedInStatement(*child, name)) return true;
      }
      return false;
    }
    case CStatementKind::kExpressionStatement:
      return IsIdentifierAssignedInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), name);
    case CStatementKind::kReturnStatement: {
      const auto& return_stmt = static_cast<const ReturnStatement&>(statement);
      return return_stmt.ReturnValue() != nullptr &&
             IsIdentifierAssignedInExpression(*return_stmt.ReturnValue(), name);
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(statement);
      if (IsIdentifierAssignedInExpression(if_stmt.Condition(), name)) return true;
      if (IsIdentifierAssignedInStatement(if_stmt.ThenBranch(), name)) return true;
      if (if_stmt.ElseBranch() != nullptr &&
          IsIdentifierAssignedInStatement(*if_stmt.ElseBranch(), name)) {
        return true;
      }
      return false;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_stmt = static_cast<const WhileStatement&>(statement);
      return IsIdentifierAssignedInExpression(while_stmt.Condition(), name) ||
             IsIdentifierAssignedInStatement(while_stmt.Body(), name);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while = static_cast<const DoWhileStatement&>(statement);
      return IsIdentifierAssignedInStatement(do_while.Body(), name) ||
             IsIdentifierAssignedInExpression(do_while.Condition(), name);
    }
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(statement);
      if (for_stmt.Init() != nullptr && IsIdentifierAssignedInStatement(*for_stmt.Init(), name)) {
        return true;
      }
      if (for_stmt.Condition() != nullptr &&
          IsIdentifierAssignedInExpression(*for_stmt.Condition(), name)) {
        return true;
      }
      if (for_stmt.Step() != nullptr && IsIdentifierAssignedInExpression(*for_stmt.Step(), name)) {
        return true;
      }
      return IsIdentifierAssignedInStatement(for_stmt.Body(), name);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_stmt = static_cast<const SwitchStatement&>(statement);
      if (IsIdentifierAssignedInExpression(switch_stmt.Condition(), name)) return true;
      for (const auto& switch_case : switch_stmt.Cases()) {
        if (switch_case.body != nullptr &&
            IsIdentifierAssignedInStatement(*switch_case.body, name)) {
          return true;
        }
      }
      return false;
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(statement);
      return decl.Initializer() != nullptr &&
             IsIdentifierAssignedInExpression(*decl.Initializer(), name);
    }
    default:
      return false;
  }
}

bool IsLvalueFormOfIdentifier(const CExpression& expression, std::string_view name) {
  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier:
      return static_cast<const IdentifierExpression&>(expression).Name() == name;
    case CExpressionKind::kCastExpression:
      return IsLvalueFormOfIdentifier(static_cast<const CastExpression&>(expression).Operand(),
                                      name);
    case CExpressionKind::kMemberAccessExpression:
      return IsLvalueFormOfIdentifier(
          static_cast<const MemberAccessExpression&>(expression).Object(), name);
    case CExpressionKind::kArrayIndexExpression:
      return IsLvalueFormOfIdentifier(static_cast<const ArrayIndexExpression&>(expression).Array(),
                                      name);
    default:
      return false;
  }
}

bool IsIdentifierAddressTakenInExpression(const CExpression& expression, std::string_view name) {
  switch (expression.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      if (unary.IsPrefix() && (unary.Op().find('&') != std::string::npos)) {
        if (IsLvalueFormOfIdentifier(unary.Operand(), name)) {
          return true;
        }
      }
      return IsIdentifierAddressTakenInExpression(unary.Operand(), name);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& binary = static_cast<const BinaryExpression&>(expression);
      return IsIdentifierAddressTakenInExpression(binary.Lhs(), name) ||
             IsIdentifierAddressTakenInExpression(binary.Rhs(), name);
    }
    case CExpressionKind::kCastExpression:
      return IsIdentifierAddressTakenInExpression(
          static_cast<const CastExpression&>(expression).Operand(), name);
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expression);
      if (IsIdentifierAddressTakenInExpression(call.Callee(), name)) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && IsIdentifierAddressTakenInExpression(*arg, name)) return true;
      }
      return false;
    }
    case CExpressionKind::kMemberAccessExpression:
      return IsIdentifierAddressTakenInExpression(
          static_cast<const MemberAccessExpression&>(expression).Object(), name);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& array_index = static_cast<const ArrayIndexExpression&>(expression);
      return IsIdentifierAddressTakenInExpression(array_index.Array(), name) ||
             IsIdentifierAddressTakenInExpression(array_index.Index(), name);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ternary = static_cast<const TernaryExpression&>(expression);
      return IsIdentifierAddressTakenInExpression(ternary.Condition(), name) ||
             IsIdentifierAddressTakenInExpression(ternary.TrueExpression(), name) ||
             IsIdentifierAddressTakenInExpression(ternary.FalseExpression(), name);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assignment = static_cast<const AssignmentExpression&>(expression);
      return IsIdentifierAddressTakenInExpression(assignment.Lhs(), name) ||
             IsIdentifierAddressTakenInExpression(assignment.Rhs(), name);
    }
    default:
      return false;
  }
}

bool IsIdentifierAddressTakenInStatement(const CStatement& statement, std::string_view name) {
  switch (statement.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& compound = static_cast<const CompoundStatement&>(statement);
      for (const auto& child : compound.Statements()) {
        if (child != nullptr && IsIdentifierAddressTakenInStatement(*child, name)) return true;
      }
      return false;
    }
    case CStatementKind::kExpressionStatement:
      return IsIdentifierAddressTakenInExpression(
          static_cast<const ExpressionStatement&>(statement).Expression(), name);
    case CStatementKind::kReturnStatement: {
      const auto& return_stmt = static_cast<const ReturnStatement&>(statement);
      return return_stmt.ReturnValue() != nullptr &&
             IsIdentifierAddressTakenInExpression(*return_stmt.ReturnValue(), name);
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(statement);
      if (IsIdentifierAddressTakenInExpression(if_stmt.Condition(), name)) return true;
      if (IsIdentifierAddressTakenInStatement(if_stmt.ThenBranch(), name)) return true;
      if (if_stmt.ElseBranch() != nullptr &&
          IsIdentifierAddressTakenInStatement(*if_stmt.ElseBranch(), name)) {
        return true;
      }
      return false;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_stmt = static_cast<const WhileStatement&>(statement);
      return IsIdentifierAddressTakenInExpression(while_stmt.Condition(), name) ||
             IsIdentifierAddressTakenInStatement(while_stmt.Body(), name);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_while = static_cast<const DoWhileStatement&>(statement);
      return IsIdentifierAddressTakenInStatement(do_while.Body(), name) ||
             IsIdentifierAddressTakenInExpression(do_while.Condition(), name);
    }
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(statement);
      if (for_stmt.Init() != nullptr &&
          IsIdentifierAddressTakenInStatement(*for_stmt.Init(), name)) {
        return true;
      }
      if (for_stmt.Condition() != nullptr &&
          IsIdentifierAddressTakenInExpression(*for_stmt.Condition(), name)) {
        return true;
      }
      if (for_stmt.Step() != nullptr &&
          IsIdentifierAddressTakenInExpression(*for_stmt.Step(), name)) {
        return true;
      }
      return IsIdentifierAddressTakenInStatement(for_stmt.Body(), name);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_stmt = static_cast<const SwitchStatement&>(statement);
      if (IsIdentifierAddressTakenInExpression(switch_stmt.Condition(), name)) return true;
      for (const auto& switch_case : switch_stmt.Cases()) {
        if (switch_case.body != nullptr &&
            IsIdentifierAddressTakenInStatement(*switch_case.body, name)) {
          return true;
        }
      }
      return false;
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(statement);
      return decl.Initializer() != nullptr &&
             IsIdentifierAddressTakenInExpression(*decl.Initializer(), name);
    }
    default:
      return false;
  }
}

bool AnalyzeInlineSingleUseTemporaryAt(const std::vector<std::unique_ptr<CStatement>>& statements,
                                       size_t index, bool is_top_level_block,
                                       InlineCandidateInfo* out_info,
                                       const CompoundStatement* root_body) {
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
      !IsVariableDeadAfter(statements, use_index, variable_name, is_top_level_block, root_body,
                           /*max_dead_occurrences=*/2)) {
    return false;
  }
  if (IsAddressOfExpression(rhs) &&
      IsIdentifierUsedInBinaryArithmetic(use_statement, variable_name)) {
    return false;
  }
  if (IsIdentifierAddressTakenInStatement(use_statement, variable_name)) {
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
                        bool* out_is_call, const CompoundStatement* root_body) {
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

std::unique_ptr<CompoundStatement> PruneUnusedLocalDeclarations(const CompoundStatement& body,
                                                                bool preserve_trailing_return) {
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
              declaration.Type(), declaration.Name(), nullptr, declaration.ArraySize(),
              declaration.RegisterAnnotation()));
          continue;
        }
      }
    }
    new_statements.push_back(statements[i]->Clone());
  }
  if (!preserve_trailing_return && !new_statements.empty() &&
      new_statements.back()->Kind() == CStatementKind::kReturnStatement &&
      static_cast<const ReturnStatement&>(*new_statements.back()).ReturnValue() == nullptr) {
    new_statements.pop_back();
  }
  return std::make_unique<CompoundStatement>(std::move(new_statements));
}

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
    // Candidate parameter must not be assigned or modified anywhere in the function.
    if (IsIdentifierAssignedInStatement(function.Body(), rhs_name)) {
      continue;
    }
    if (IsIdentifierAddressTakenInStatement(function.Body(), rhs_name) ||
        IsIdentifierAddressTakenInStatement(function.Body(), lhs_name)) {
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
    work[i] =
        ReplaceIdentifierInStatement(*work[i], candidate.temporary_variable, *parameter_identifier,
                                     /*replace_assignment_lhs=*/true);
  }

  return PruneUnusedLocalDeclarations(CompoundStatement(std::move(work)));
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
          declaration.ArraySize(), declaration.RegisterAnnotation());
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

}  // namespace rom_nom_nom
