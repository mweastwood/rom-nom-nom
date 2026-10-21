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

#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "tuner/ast_global_mutations.h"
#include "tuner/ast_temp_inliner.h"

namespace rom_nom_nom {

namespace {

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

bool MatchDirectPointerStore(const CStatement& statement, const CExpression** out_ptr_expr,
                             CType* out_cast_type, bool* out_has_cast,
                             const CExpression** out_rhs) {
  if (statement.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expr_stmt = static_cast<const ExpressionStatement&>(statement);
  if (expr_stmt.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assignment = static_cast<const AssignmentExpression&>(expr_stmt.Expression());
  if (assignment.Op() != "=") return false;
  if (assignment.Lhs().Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& unary = static_cast<const UnaryExpression&>(assignment.Lhs());
  if (unary.Op() != "*" || !unary.IsPrefix()) return false;

  const CExpression* inner = &unary.Operand();
  if (inner->Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(*inner);
    *out_has_cast = true;
    *out_cast_type = cast.TargetType();
    *out_ptr_expr = &cast.Operand();
  } else {
    *out_has_cast = false;
    *out_ptr_expr = inner;
  }
  *out_rhs = &assignment.Rhs();
  return true;
}

bool IsVariableArrayInStatement(const CStatement& stmt, std::string_view name) {
  if (stmt.Kind() == CStatementKind::kVariableDeclarationStatement) {
    const auto& decl = static_cast<const VariableDeclarationStatement&>(stmt);
    if (decl.Name() == name && decl.ArraySize().has_value()) {
      return true;
    }
  } else if (stmt.Kind() == CStatementKind::kCompoundStatement) {
    for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
      if (child != nullptr && IsVariableArrayInStatement(*child, name)) return true;
    }
  } else if (stmt.Kind() == CStatementKind::kIfStatement) {
    const auto& if_stmt = static_cast<const IfStatement&>(stmt);
    if (IsVariableArrayInStatement(if_stmt.ThenBranch(), name)) return true;
    if (if_stmt.ElseBranch() != nullptr &&
        IsVariableArrayInStatement(*if_stmt.ElseBranch(), name)) {
      return true;
    }
  }
  return false;
}

bool IsGlobalIdentifierName(std::string_view name) {
  return name.rfind("D_", 0) == 0 || name.rfind("g_", 0) == 0;
}

bool ExpressionReadsOrWritesMemoryOrGlobals(const CExpression& expression) {
  switch (expression.Kind()) {
    case CExpressionKind::kIdentifier: {
      const std::string& name = static_cast<const IdentifierExpression&>(expression).Name();
      return IsGlobalIdentifierName(name);
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& unary = static_cast<const UnaryExpression&>(expression);
      if (unary.IsPrefix() && unary.Op() == "*") return true;
      return ExpressionReadsOrWritesMemoryOrGlobals(unary.Operand());
    }
    case CExpressionKind::kArrayIndexExpression:
      return true;
    case CExpressionKind::kMemberAccessExpression:
      return true;
    case CExpressionKind::kCallExpression:
      return true;
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expression);
      return ExpressionReadsOrWritesMemoryOrGlobals(bin.Lhs()) ||
             ExpressionReadsOrWritesMemoryOrGlobals(bin.Rhs());
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expression);
      return ExpressionReadsOrWritesMemoryOrGlobals(assign.Lhs()) ||
             ExpressionReadsOrWritesMemoryOrGlobals(assign.Rhs());
    }
    case CExpressionKind::kCastExpression:
      return ExpressionReadsOrWritesMemoryOrGlobals(
          static_cast<const CastExpression&>(expression).Operand());
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expression);
      return ExpressionReadsOrWritesMemoryOrGlobals(tern.Condition()) ||
             ExpressionReadsOrWritesMemoryOrGlobals(tern.TrueExpression()) ||
             ExpressionReadsOrWritesMemoryOrGlobals(tern.FalseExpression());
    }
    default:
      return false;
  }
}

bool StatementReadsOrWritesMemoryOrGlobals(const CStatement& statement) {
  switch (statement.Kind()) {
    case CStatementKind::kExpressionStatement:
      return ExpressionReadsOrWritesMemoryOrGlobals(
          static_cast<const ExpressionStatement&>(statement).Expression());
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(statement);
      return decl.Initializer() != nullptr &&
             ExpressionReadsOrWritesMemoryOrGlobals(*decl.Initializer());
    }
    default:
      return true;
  }
}

bool StatementContainsLoop(const CStatement& statement) {
  switch (statement.Kind()) {
    case CStatementKind::kWhileStatement:
    case CStatementKind::kDoWhileStatement:
    case CStatementKind::kForStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      for (const auto& child : static_cast<const CompoundStatement&>(statement).Statements()) {
        if (StatementContainsLoop(*child)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(statement);
      if (StatementContainsLoop(if_stmt.ThenBranch())) return true;
      if (if_stmt.ElseBranch() != nullptr && StatementContainsLoop(*if_stmt.ElseBranch())) {
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

bool MatchRegisterCopy(const CStatement& statement, std::string* out_dst, std::string* out_src) {
  if (statement.Kind() == CStatementKind::kExpressionStatement) {
    const auto& expr_stmt = static_cast<const ExpressionStatement&>(statement);
    if (expr_stmt.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& assign = static_cast<const AssignmentExpression&>(expr_stmt.Expression());
      if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          assign.Rhs().Kind() == CExpressionKind::kIdentifier) {
        const std::string& dst = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
        const std::string& src = static_cast<const IdentifierExpression&>(assign.Rhs()).Name();
        if (dst != src && !IsGlobalIdentifierName(dst) && !IsGlobalIdentifierName(src)) {
          if (out_dst != nullptr) *out_dst = dst;
          if (out_src != nullptr) *out_src = src;
          return true;
        }
      }
    }
  } else if (statement.Kind() == CStatementKind::kVariableDeclarationStatement) {
    const auto& decl = static_cast<const VariableDeclarationStatement&>(statement);
    if (decl.Initializer() != nullptr &&
        decl.Initializer()->Kind() == CExpressionKind::kIdentifier) {
      const std::string& src = static_cast<const IdentifierExpression&>(*decl.Initializer()).Name();
      if (decl.Name() != src && !IsGlobalIdentifierName(decl.Name()) &&
          !IsGlobalIdentifierName(src)) {
        if (out_dst != nullptr) *out_dst = decl.Name();
        if (out_src != nullptr) *out_src = src;
        return true;
      }
    }
  }
  return false;
}

bool MatchSelfMask(const CStatement& statement, std::string* out_var, CType* out_cast_type,
                   int64_t* out_mask) {
  if (statement.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expr_stmt = static_cast<const ExpressionStatement&>(statement);
  if (expr_stmt.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assign = static_cast<const AssignmentExpression&>(expr_stmt.Expression());
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  const std::string& var_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();

  const CExpression* rhs = &assign.Rhs();
  if (rhs->Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(*rhs);
    if (cast.Operand().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(cast.Operand()).Name() == var_name) {
      if (out_var != nullptr) *out_var = var_name;
      if (out_cast_type != nullptr) *out_cast_type = cast.TargetType();
      if (out_mask != nullptr) {
        if (cast.TargetType().name == "u8") {
          *out_mask = 0xFF;
        } else if (cast.TargetType().name == "u16") {
          *out_mask = 0xFFFF;
        } else {
          *out_mask = 0;
        }
      }
      return true;
    }
  } else if (rhs->Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(*rhs);
    if (bin.Op() == "&") {
      const CExpression* id_expr = nullptr;
      const CExpression* mask_expr = nullptr;
      if (bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
          bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        id_expr = &bin.Lhs();
        mask_expr = &bin.Rhs();
      } else if (bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
                 bin.Lhs().Kind() == CExpressionKind::kIntegerLiteral) {
        id_expr = &bin.Rhs();
        mask_expr = &bin.Lhs();
      }
      if (id_expr != nullptr &&
          static_cast<const IdentifierExpression&>(*id_expr).Name() == var_name) {
        int64_t mask_val = static_cast<const IntegerLiteralExpression&>(*mask_expr).Value();
        if (mask_val == 0xFF || mask_val == 0xFFFF || mask_val == 255 || mask_val == 65535) {
          if (out_var != nullptr) *out_var = var_name;
          if (out_cast_type != nullptr) {
            *out_cast_type = (mask_val == 0xFF || mask_val == 255) ? CType::U8() : CType::U16();
          }
          if (out_mask != nullptr) *out_mask = mask_val;
          return true;
        }
      }
    }
  }
  return false;
}

std::unique_ptr<CExpression> SimplifyDoubleCasts(std::unique_ptr<CExpression> expr) {
  if (expr == nullptr) return nullptr;
  if (expr->Kind() == CExpressionKind::kCastExpression) {
    auto& cast = static_cast<CastExpression&>(*expr);
    if (cast.Operand().Kind() == CExpressionKind::kCastExpression) {
      const auto& inner_cast = static_cast<const CastExpression&>(cast.Operand());
      if ((cast.TargetType().name == "u32" || cast.TargetType().name == "s32" ||
           cast.TargetType().name == "u16" || cast.TargetType().name == "s16") &&
          (inner_cast.TargetType().name == "u16" || inner_cast.TargetType().name == "u8" ||
           inner_cast.TargetType().name == "s16" || inner_cast.TargetType().name == "s8")) {
        return inner_cast.Clone();
      }
    }
    return CExpression::Cast(cast.TargetType(), SimplifyDoubleCasts(cast.Operand().Clone()));
  } else if (expr->Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(*expr);
    return CExpression::Binary(bin.Op(), SimplifyDoubleCasts(bin.Lhs().Clone()),
                               SimplifyDoubleCasts(bin.Rhs().Clone()));
  }
  return expr;
}

std::vector<std::string> FindZeroValuedVariables(const CompoundStatement* root_body,
                                                 const std::set<std::string>& local_variables,
                                                 const CStatement* before_statement) {
  std::vector<std::string> zero_vars;
  if (root_body == nullptr) return zero_vars;
  for (const auto& s : root_body->Statements()) {
    if (s.get() == before_statement) break;
    if (s->Kind() == CStatementKind::kVariableDeclarationStatement) {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(*s);
      if (decl.Initializer() != nullptr &&
          decl.Initializer()->Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(*decl.Initializer()).Value() == 0) {
        if (std::find(zero_vars.begin(), zero_vars.end(), decl.Name()) == zero_vars.end()) {
          zero_vars.push_back(decl.Name());
        }
      }
    } else if (s->Kind() == CStatementKind::kExpressionStatement) {
      const auto& expr = static_cast<const ExpressionStatement&>(*s).Expression();
      if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr);
        if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
            assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(assign.Rhs()).Value() == 0) {
          const std::string& name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
          if (local_variables.count(name) > 0 &&
              std::find(zero_vars.begin(), zero_vars.end(), name) == zero_vars.end()) {
            zero_vars.push_back(name);
          }
        }
      }
    }
  }
  return zero_vars;
}

bool MatchBreakIf(const CStatement& statement, const CExpression** out_exit_cond) {
  if (statement.Kind() != CStatementKind::kIfStatement) return false;
  const auto& if_stmt = static_cast<const IfStatement&>(statement);
  if (if_stmt.ElseBranch() != nullptr) return false;
  if (if_stmt.ThenBranch().Kind() == CStatementKind::kBreakStatement) {
    if (out_exit_cond != nullptr) *out_exit_cond = &if_stmt.Condition();
    return true;
  }
  if (if_stmt.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
    const auto& then_stmts =
        static_cast<const CompoundStatement&>(if_stmt.ThenBranch()).Statements();
    if (then_stmts.size() == 1 && then_stmts[0]->Kind() == CStatementKind::kBreakStatement) {
      if (out_exit_cond != nullptr) *out_exit_cond = &if_stmt.Condition();
      return true;
    }
  }
  return false;
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
  uint32_t split_temporary_index = 0;
  uint32_t propagate_register_copy_index = 0;
  uint32_t hoist_while_guard_index = 0;
  uint32_t reload_or_early_return_var_index = 0;

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

    // 5. Check splitting direct pointer dereference store into temporary assignment
    const CExpression* ptr_expr = nullptr;
    CType cast_type;
    bool has_cast = false;
    const CExpression* rhs = nullptr;
    if (MatchDirectPointerStore(*statements[i], &ptr_expr, &cast_type, &has_cast, &rhs)) {
      std::vector<std::string> eligible_candidates;
      for (const auto& cand_var : context->local_variables) {
        if (CountIdentifierOccurrencesInExpression(*ptr_expr, cand_var) > 0) continue;
        if (CountIdentifierOccurrencesInExpression(*rhs, cand_var) > 0) continue;
        if (context->root_body != nullptr &&
            IsVariableArrayInStatement(*context->root_body, cand_var)) {
          continue;
        }
        if (IsVariableDeadAfter(statements, i, cand_var, is_top_level_block, context->root_body)) {
          eligible_candidates.push_back(cand_var);
        }
      }
      if (!eligible_candidates.empty()) {
        uint32_t site = context->split_temporary_index++;
        size_t num_candidates = std::min<size_t>(eligible_candidates.size(), 2);
        for (size_t v = 0; v < num_candidates; ++v) {
          context->Record(AST_MUTATION_SPLIT_SINGLE_USE_TEMP, site, static_cast<uint32_t>(v));
        }
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_SPLIT_SINGLE_USE_TEMP, site, &parameter)) {
          size_t chosen_idx = parameter < eligible_candidates.size() ? parameter : 0;
          const std::string& chosen_var = eligible_candidates[chosen_idx];
          new_statements.push_back(CStatement::Expression(CExpression::Assignment(
              "=", CExpression::Identifier(chosen_var), TraverseExpression(*ptr_expr, context))));
          std::unique_ptr<CExpression> deref_target;
          if (has_cast) {
            deref_target = CExpression::Unary(
                "*", CExpression::Cast(cast_type, CExpression::Identifier(chosen_var)),
                /*is_prefix=*/true);
          } else {
            deref_target = CExpression::Unary("*", CExpression::Identifier(chosen_var),
                                              /*is_prefix=*/true);
          }
          new_statements.push_back(CStatement::Expression(CExpression::Assignment(
              "=", std::move(deref_target), TraverseExpression(*rhs, context))));
          continue;
        }
      }
    }

    // 6. Check register copy propagation (`AST_MUTATION_PROPAGATE_REGISTER_COPY`)
    std::string copy_dst, copy_src;
    if (MatchRegisterCopy(*statements[i], &copy_dst, &copy_src) &&
        !IsGlobalIdentifierName(copy_dst) && !IsGlobalIdentifierName(copy_src)) {
      std::vector<size_t> read_indices;
      for (size_t j = i + 1; j < statements.size(); ++j) {
        // Soundness guard: if statements[j] assigns either copy_src or copy_dst,
        // or contains a loop where either is modified, stop immediately.
        // Never add statements[j] if it assigns copy_src or copy_dst!
        if (IsIdentifierAssignedInStatement(*statements[j], copy_src) ||
            IsIdentifierAssignedInStatement(*statements[j], copy_dst) ||
            StatementContainsLoop(*statements[j])) {
          break;
        }
        if (CountIdentifierOccurrencesInStatement(*statements[j], copy_src) > 0) {
          read_indices.push_back(j);
        }
      }
      if (!read_indices.empty()) {
        uint32_t site = context->propagate_register_copy_index++;
        context->Record(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, 0);
        if (read_indices.size() > 1) {
          context->Record(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, 1);
        }
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, &parameter)) {
          new_statements.push_back(TraverseStatement(*statements[i], context));
          for (size_t k = i + 1; k < statements.size(); ++k) {
            bool should_replace = false;
            if (parameter == 0 && k == read_indices[0]) {
              should_replace = true;
            } else if (parameter == 1 && std::find(read_indices.begin(), read_indices.end(), k) !=
                                             read_indices.end()) {
              should_replace = true;
            }
            if (should_replace) {
              auto replaced = ReplaceIdentifierInStatement(*statements[k], copy_src,
                                                           *CExpression::Identifier(copy_dst));
              new_statements.push_back(TraverseStatement(*replaced, context));
            } else {
              new_statements.push_back(TraverseStatement(*statements[k], context));
            }
          }
          i = statements.size();
          continue;
        }
      }
    }

    // 7. Check dead local variable initialization elimination
    // (`AST_MUTATION_PROPAGATE_REGISTER_COPY`)
    if (statements[i]->Kind() == CStatementKind::kVariableDeclarationStatement) {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(*statements[i]);
      if (decl.Initializer() != nullptr && !decl.ArraySize().has_value() &&
          IsPureExpression(*decl.Initializer())) {
        bool is_dead_init = false;
        for (size_t j = i + 1; j < statements.size(); ++j) {
          if (DoesStatementReadVariableBeforeOverwrite(*statements[j], decl.Name())) {
            break;
          }
          bool always_ow = false;
          (void)DoesStatementReadVariableBeforeOverwrite(*statements[j], decl.Name(), &always_ow);
          if (always_ow) {
            is_dead_init = true;
            break;
          }
          if (statements[j]->Kind() == CStatementKind::kIfStatement) {
            const auto& if_stmt = static_cast<const IfStatement&>(*statements[j]);
            bool then_ow = false;
            if (!DoesStatementReadVariableBeforeOverwrite(if_stmt.ThenBranch(), decl.Name(),
                                                          &then_ow) &&
                then_ow) {
              bool read_after_if = false;
              for (size_t rem = j + 1; rem < statements.size(); ++rem) {
                if (CountIdentifierOccurrencesInStatement(*statements[rem], decl.Name()) > 0) {
                  read_after_if = true;
                  break;
                }
              }
              if (!read_after_if) {
                is_dead_init = true;
                break;
              }
            }
          }
        }
        if (is_dead_init) {
          uint32_t site = context->propagate_register_copy_index++;
          context->Record(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, 0);
          uint32_t parameter = 0;
          if (context->ShouldApply(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, &parameter)) {
            new_statements.push_back(CStatement::VariableDeclaration(
                decl.Type(), decl.Name(), nullptr, decl.ArraySize(), decl.RegisterAnnotation()));
            continue;
          }
        }
      }
    }

    // 8. Check self-mask sinking and cast folding (`AST_MUTATION_PROPAGATE_REGISTER_COPY`)
    std::string mask_var;
    CType mask_cast_type;
    int64_t mask_val = 0;
    if (MatchSelfMask(*statements[i], &mask_var, &mask_cast_type, &mask_val) &&
        !IsGlobalIdentifierName(mask_var)) {
      size_t first_read_idx = statements.size();
      bool modified_in_between = false;
      for (size_t j = i + 1; j < statements.size(); ++j) {
        if (CountIdentifierOccurrencesInStatement(*statements[j], mask_var) > 0) {
          first_read_idx = j;
          break;
        }
        if (IsIdentifierAssignedInStatement(*statements[j], mask_var) ||
            StatementReadsOrWritesMemoryOrGlobals(*statements[j]) ||
            StatementContainsCall(*statements[j]) || StatementContainsLoop(*statements[j])) {
          modified_in_between = true;
          break;
        }
      }
      if (!modified_in_between && first_read_idx < statements.size() && first_read_idx > i) {
        uint32_t site = context->propagate_register_copy_index++;
        if (first_read_idx > i + 1) {
          context->Record(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, 0);
        }
        if (statements[first_read_idx]->Kind() == CStatementKind::kIfStatement) {
          const auto& if_stmt = static_cast<const IfStatement&>(*statements[first_read_idx]);
          if (CountIdentifierOccurrencesInExpression(if_stmt.Condition(), mask_var) > 0) {
            context->Record(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, 1);
          }
        }

        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_PROPAGATE_REGISTER_COPY, site, &parameter)) {
          if (parameter == 0 && first_read_idx > i + 1) {
            for (size_t middle = i + 1; middle < first_read_idx; ++middle) {
              new_statements.push_back(TraverseStatement(*statements[middle], context));
            }
            new_statements.push_back(TraverseStatement(*statements[i], context));
            new_statements.push_back(TraverseStatement(*statements[first_read_idx], context));
            i = first_read_idx;
            continue;
          } else if (parameter == 1 &&
                     statements[first_read_idx]->Kind() == CStatementKind::kIfStatement) {
            for (size_t middle = i + 1; middle < first_read_idx; ++middle) {
              new_statements.push_back(TraverseStatement(*statements[middle], context));
            }
            const auto& if_stmt = static_cast<const IfStatement&>(*statements[first_read_idx]);
            auto folded_cond = SimplifyDoubleCasts(ReplaceIdentifierInExpression(
                if_stmt.Condition(), mask_var,
                *CExpression::Cast(mask_cast_type, CExpression::Identifier(mask_var))));
            std::unique_ptr<CStatement> new_then;
            if (CountIdentifierOccurrencesInStatement(if_stmt.ThenBranch(), mask_var) > 0) {
              std::vector<std::unique_ptr<CStatement>> then_stmts;
              then_stmts.push_back(CStatement::Expression(CExpression::Assignment(
                  "=", CExpression::Identifier(mask_var),
                  CExpression::Cast(mask_cast_type, CExpression::Identifier(mask_var)))));
              if (if_stmt.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
                for (const auto& s :
                     static_cast<const CompoundStatement&>(if_stmt.ThenBranch()).Statements()) {
                  then_stmts.push_back(s->Clone());
                }
              } else {
                then_stmts.push_back(if_stmt.ThenBranch().Clone());
              }
              new_then = std::make_unique<CompoundStatement>(std::move(then_stmts));
            } else {
              new_then = if_stmt.ThenBranch().Clone();
            }
            auto mutated_if =
                CStatement::If(std::move(folded_cond), std::move(new_then),
                               if_stmt.ElseBranch() ? if_stmt.ElseBranch()->Clone() : nullptr);
            new_statements.push_back(TraverseStatement(*mutated_if, context));
            i = first_read_idx;
            continue;
          }
        }
      }
    }

    // 9. Check memory store reload / volatile store & reload
    // (`AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR`)
    const CExpression* store_ptr = nullptr;
    CType store_cast_type;
    bool store_has_cast = false;
    const CExpression* store_rhs = nullptr;
    if (MatchDirectPointerStore(*statements[i], &store_ptr, &store_cast_type, &store_has_cast,
                                &store_rhs) &&
        store_rhs->Kind() == CExpressionKind::kIdentifier) {
      const std::string& stored_var = static_cast<const IdentifierExpression&>(*store_rhs).Name();
      if (i + 1 < statements.size()) {
        bool next_is_reload = false;
        CType next_reload_type = store_cast_type;
        if (statements[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
          const auto& next_expr =
              static_cast<const ExpressionStatement&>(*statements[i + 1]).Expression();
          if (next_expr.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& next_assign = static_cast<const AssignmentExpression&>(next_expr);
            if (next_assign.Op() == "=" &&
                next_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(next_assign.Lhs()).Name() == stored_var) {
              const CExpression* next_ptr = nullptr;
              if (next_assign.Rhs().Kind() == CExpressionKind::kUnaryExpression) {
                const auto& un = static_cast<const UnaryExpression&>(next_assign.Rhs());
                if (un.Op() == "*" && un.IsPrefix()) {
                  const CExpression* in = &un.Operand();
                  if (in->Kind() == CExpressionKind::kCastExpression) {
                    const auto& c = static_cast<const CastExpression&>(*in);
                    next_reload_type = c.TargetType();
                    next_ptr = &c.Operand();
                  } else {
                    next_ptr = in;
                  }
                  if (next_ptr != nullptr && next_ptr->ToString() == store_ptr->ToString()) {
                    next_is_reload = true;
                  }
                }
              }
            }
          }
        }

        bool next_reads_var = false;
        if (statements[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
          const auto& expr =
              static_cast<const ExpressionStatement&>(*statements[i + 1]).Expression();
          if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& assign = static_cast<const AssignmentExpression&>(expr);
            next_reads_var = CountIdentifierOccurrencesInExpression(assign.Rhs(), stored_var) > 0;
          } else {
            next_reads_var = CountIdentifierOccurrencesInExpression(expr, stored_var) > 0;
          }
        } else {
          next_reads_var =
              CountIdentifierOccurrencesInStatement(*statements[i + 1], stored_var) > 0;
        }

        if (next_is_reload || next_reads_var) {
          uint32_t site = context->reload_or_early_return_var_index++;
          context->Record(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site, 0);
          context->Record(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site, 1);
          if (next_is_reload) {
            context->Record(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site, 2);
          }
          uint32_t parameter = 0;
          if (context->ShouldApply(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site, &parameter)) {
            bool make_store_volatile = (parameter == 0 || parameter == 1 || !next_is_reload);
            bool make_reload_volatile = (parameter == 0 || parameter == 2);

            std::string vol_store_name =
                (make_store_volatile && !absl::StartsWith(store_cast_type.name, "volatile "))
                    ? "volatile " + store_cast_type.name
                    : store_cast_type.name;
            CType vol_store_ptr_type = CType{.name = vol_store_name, .pointer_depth = 1};
            auto vol_store_deref =
                CExpression::Unary("*", CExpression::Cast(vol_store_ptr_type, store_ptr->Clone()),
                                   /*is_prefix=*/true);
            new_statements.push_back(CStatement::Expression(
                CExpression::Assignment("=", std::move(vol_store_deref), store_rhs->Clone())));

            if (next_is_reload) {
              std::string vol_reload_name =
                  (make_reload_volatile && !absl::StartsWith(next_reload_type.name, "volatile "))
                      ? "volatile " + next_reload_type.name
                      : next_reload_type.name;
              CType vol_reload_ptr_type = CType{.name = vol_reload_name, .pointer_depth = 1};
              auto vol_reload_deref = CExpression::Unary(
                  "*", CExpression::Cast(vol_reload_ptr_type, store_ptr->Clone()),
                  /*is_prefix=*/true);
              new_statements.push_back(CStatement::Expression(CExpression::Assignment(
                  "=", CExpression::Identifier(stored_var), std::move(vol_reload_deref))));
              ++i;
            } else if (parameter == 0) {
              auto vol_reload_deref =
                  CExpression::Unary("*", CExpression::Cast(vol_store_ptr_type, store_ptr->Clone()),
                                     /*is_prefix=*/true);
              new_statements.push_back(CStatement::Expression(CExpression::Assignment(
                  "=", CExpression::Identifier(stored_var), std::move(vol_reload_deref))));
            }
            continue;
          }
        }
      }
    }

    // 10. Check replacing `v0 = 0` with `v0 = <zero_var>`
    // (`AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR`)
    if (statements[i]->Kind() == CStatementKind::kExpressionStatement) {
      const auto& expr = static_cast<const ExpressionStatement&>(*statements[i]).Expression();
      if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr);
        if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
            assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(assign.Rhs()).Value() == 0) {
          const std::string& assign_dst =
              static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
          auto zero_vars = FindZeroValuedVariables(context->root_body, context->local_variables,
                                                   statements[i].get());
          std::vector<std::string> eligible_zero_vars;
          for (const auto& zv : zero_vars) {
            if (zv != assign_dst) eligible_zero_vars.push_back(zv);
          }
          if (!eligible_zero_vars.empty()) {
            uint32_t site = context->reload_or_early_return_var_index++;
            for (size_t z = 0; z < eligible_zero_vars.size(); ++z) {
              context->Record(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site,
                              static_cast<uint32_t>(z));
            }
            uint32_t parameter = 0;
            if (context->ShouldApply(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site, &parameter)) {
              size_t chosen = parameter < eligible_zero_vars.size() ? parameter : 0;
              new_statements.push_back(CStatement::Expression(
                  CExpression::Assignment("=", CExpression::Identifier(assign_dst),
                                          CExpression::Identifier(eligible_zero_vars[chosen]))));
              continue;
            }
          }
        }
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
                                             std::move(initializer), declaration.ArraySize(),
                                             declaration.RegisterAnnotation());
    }
    case CStatementKind::kIfStatement: {
      const auto& if_statement = static_cast<const IfStatement&>(statement);
      auto new_condition = TraverseExpression(if_statement.Condition(), context);
      auto new_then = TraverseStatement(if_statement.ThenBranch(), context);
      std::unique_ptr<CStatement> new_else =
          if_statement.ElseBranch() != nullptr
              ? TraverseStatement(*if_statement.ElseBranch(), context)
              : nullptr;

      // Check if `if (cond) { do { body; } while (cond2); }` can collapse back to `while (cond) {
      // body; }`
      if (if_statement.ElseBranch() == nullptr) {
        const CStatement* candidate_loop = &if_statement.ThenBranch();
        if (candidate_loop->Kind() == CStatementKind::kCompoundStatement) {
          const auto& comp = static_cast<const CompoundStatement&>(*candidate_loop);
          if (comp.Statements().size() == 1) {
            candidate_loop = comp.Statements()[0].get();
          }
        }
        if (candidate_loop->Kind() == CStatementKind::kDoWhileStatement) {
          const auto& do_while = static_cast<const DoWhileStatement&>(*candidate_loop);
          if (do_while.Condition().ToString() == if_statement.Condition().ToString()) {
            uint32_t site = context->hoist_while_guard_index++;
            context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 0);
            uint32_t parameter = 0;
            if (context->ShouldApply(AST_MUTATION_HOIST_WHILE_GUARD, site, &parameter)) {
              return CStatement::While(if_statement.Condition().Clone(), do_while.Body().Clone());
            }
          }
        }
      }

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

      bool is_while_true = false;
      if (while_statement.Condition().Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(while_statement.Condition()).Value() != 0) {
        is_while_true = true;
      }

      if (is_while_true && while_statement.Body().Kind() == CStatementKind::kCompoundStatement) {
        const auto& body_compound = static_cast<const CompoundStatement&>(while_statement.Body());
        const auto& body_stmts = body_compound.Statements();
        const CExpression* exit_cond = nullptr;
        if (!body_stmts.empty() && MatchBreakIf(*body_stmts[0], &exit_cond) &&
            exit_cond != nullptr) {
          uint32_t site = context->hoist_while_guard_index++;
          context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 0);
          context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 1);
          context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 2);
          uint32_t parameter = 0;
          if (context->ShouldApply(AST_MUTATION_HOIST_WHILE_GUARD, site, &parameter)) {
            std::vector<std::unique_ptr<CStatement>> remaining_stmts;
            for (size_t b = 1; b < body_stmts.size(); ++b) {
              remaining_stmts.push_back(body_stmts[b]->Clone());
            }
            auto remaining_body = std::make_unique<CompoundStatement>(std::move(remaining_stmts));
            auto loop_cond = NegateCondition(*exit_cond);
            if (parameter == 0) {
              return CStatement::While(std::move(loop_cond), std::move(remaining_body));
            } else if (parameter == 1) {
              return CStatement::DoWhile(std::move(remaining_body), std::move(loop_cond));
            } else if (parameter == 2) {
              auto do_while = CStatement::DoWhile(std::move(remaining_body), loop_cond->Clone());
              auto if_body = std::make_unique<CompoundStatement>();
              if_body->AddStatement(std::move(do_while));
              return CStatement::If(std::move(loop_cond), std::move(if_body));
            }
          }
        }
      } else if (!is_while_true) {
        uint32_t site = context->hoist_while_guard_index++;
        context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 0);
        context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 1);
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_HOIST_WHILE_GUARD, site, &parameter)) {
          if (parameter == 0) {
            auto do_while = CStatement::DoWhile(new_body->Clone(), new_condition->Clone());
            auto if_body = std::make_unique<CompoundStatement>();
            if_body->AddStatement(std::move(do_while));
            return CStatement::If(std::move(new_condition), std::move(if_body));
          } else if (parameter == 1) {
            return CStatement::DoWhile(std::move(new_body), std::move(new_condition));
          }
        }
      }

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
      if (return_statement.ReturnValue()->Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(*return_statement.ReturnValue()).Value() ==
              0) {
        auto zero_vars =
            FindZeroValuedVariables(context->root_body, context->local_variables, &statement);
        if (!zero_vars.empty()) {
          uint32_t site = context->reload_or_early_return_var_index++;
          for (size_t z = 0; z < zero_vars.size(); ++z) {
            context->Record(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site,
                            static_cast<uint32_t>(z));
          }
          uint32_t parameter = 0;
          if (context->ShouldApply(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR, site, &parameter)) {
            size_t chosen = parameter < zero_vars.size() ? parameter : 0;
            return CStatement::Return(CExpression::Identifier(zero_vars[chosen]));
          }
        }
      }
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

enum class TrailingReturnKind {
  kNone,
  kReturnVoid,
  kReturnZero,
  kReturnV0,
  kOtherReturn,
};

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

  // Trailing return toggle mutations
  {
    const auto& body_stmts = function.Body().Statements();
    TrailingReturnKind current_trailing = TrailingReturnKind::kNone;
    if (!body_stmts.empty() && body_stmts.back()->Kind() == CStatementKind::kReturnStatement) {
      const auto& ret = static_cast<const ReturnStatement&>(*body_stmts.back());
      if (ret.ReturnValue() == nullptr) {
        current_trailing = TrailingReturnKind::kReturnVoid;
      } else if (ret.ReturnValue()->Kind() == CExpressionKind::kIntegerLiteral &&
                 static_cast<const IntegerLiteralExpression&>(*ret.ReturnValue()).Value() == 0) {
        current_trailing = TrailingReturnKind::kReturnZero;
      } else if (ret.ReturnValue()->Kind() == CExpressionKind::kIdentifier &&
                 static_cast<const IdentifierExpression&>(*ret.ReturnValue()).Name() == "v0") {
        current_trailing = TrailingReturnKind::kReturnV0;
      } else {
        current_trailing = TrailingReturnKind::kOtherReturn;
      }
    }

    bool has_v0 = referenced_symbols.local_variables.count("v0") > 0;
    if (current_trailing == TrailingReturnKind::kNone) {
      context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 0);
      context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 1);
      if (has_v0) {
        context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 2);
      }
    } else if (current_trailing == TrailingReturnKind::kReturnVoid) {
      context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 1);
      if (has_v0) {
        context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 2);
      }
    } else if (current_trailing == TrailingReturnKind::kReturnZero) {
      context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 0);
      if (has_v0) {
        context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 2);
      }
    } else if (current_trailing == TrailingReturnKind::kReturnV0) {
      context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 0);
      context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 1);
    }
  }

  // Discover volatile global access candidates (`D_... = ...` -> direct volatile declaration or
  // inline cast)
  auto volatile_globals = CollectVolatileGlobalCandidates(function, referenced_symbols);
  for (size_t i = 0; i < volatile_globals.size(); ++i) {
    context.Record(AST_MUTATION_VOLATILE_GLOBAL_ACCESS, static_cast<uint32_t>(i), 0);
    context.Record(AST_MUTATION_VOLATILE_GLOBAL_ACCESS, static_cast<uint32_t>(i), 2);
  }
  if (!volatile_globals.empty()) {
    context.Record(AST_MUTATION_VOLATILE_GLOBAL_ACCESS, 0, 1);
    context.Record(AST_MUTATION_VOLATILE_GLOBAL_ACCESS, 0, 3);
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

  if (step.kind() == AST_MUTATION_TOGGLE_TRAILING_RETURN) {
    std::unique_ptr<CStatement> new_ret;
    if (step.param() == 0) {
      new_ret = CStatement::Return();
    } else if (step.param() == 1) {
      new_ret = CStatement::Return(CExpression::Integer(0));
    } else if (step.param() == 2) {
      new_ret = CStatement::Return(CExpression::Identifier("v0"));
    } else {
      return function.Clone();
    }

    const auto& stmts = function.Body().Statements();
    std::vector<std::unique_ptr<CStatement>> new_stmts;
    new_stmts.reserve(stmts.size() + 1);
    if (!stmts.empty() && stmts.back()->Kind() == CStatementKind::kReturnStatement) {
      for (size_t i = 0; i + 1 < stmts.size(); ++i) {
        new_stmts.push_back(stmts[i]->Clone());
      }
      new_stmts.push_back(std::move(new_ret));
    } else {
      for (size_t i = 0; i < stmts.size(); ++i) {
        new_stmts.push_back(stmts[i]->Clone());
      }
      new_stmts.push_back(std::move(new_ret));
    }
    auto mutated_body = std::make_unique<CompoundStatement>(std::move(new_stmts));
    return FunctionDeclaration(function.ReturnType(), function.Name(), function.Parameters(),
                               std::move(mutated_body), function.IsStatic());
  }

  if (step.kind() == AST_MUTATION_VOLATILE_GLOBAL_ACCESS) {
    auto candidates = CollectVolatileGlobalCandidates(function, referenced_symbols);
    std::set<std::string> target_set;
    bool use_inline_cast = (step.param() == 2 || step.param() == 3);
    if (step.param() == 1 || step.param() == 3) {
      target_set.insert(candidates.begin(), candidates.end());
    } else if (step.site_index() < candidates.size()) {
      target_set.insert(candidates[step.site_index()]);
    }
    if (target_set.empty()) return function.Clone();
    auto mutated_body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
        RewriteVolatileGlobalAccessInStatement(function.Body(), target_set, referenced_symbols,
                                               use_inline_cast)
            .release()));
    return FunctionDeclaration(function.ReturnType(), function.Name(), function.Parameters(),
                               std::move(mutated_body), function.IsStatic());
  }

  MutationContext context;
  context.step_to_apply = &step;
  context.local_variables = referenced_symbols.local_variables;
  context.root_body = &function.Body();
  auto mutated_body = TraverseCompound(function.Body(), &context, /*is_top_level_block=*/true);
  if (step.kind() == AST_MUTATION_INLINE_SINGLE_USE_TEMP ||
      step.kind() == AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN ||
      step.kind() == AST_MUTATION_PROPAGATE_REGISTER_COPY ||
      step.kind() == AST_MUTATION_HOIST_WHILE_GUARD) {
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
    case AST_MUTATION_TOGGLE_TRAILING_RETURN:
      return absl::StrFormat("ToggleTrailingReturn(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_SPLIT_SINGLE_USE_TEMP:
      return absl::StrFormat("SplitSingleUseTemp(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_VOLATILE_GLOBAL_ACCESS:
      return absl::StrFormat("VolatileGlobalAccess(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_PROPAGATE_REGISTER_COPY:
      return absl::StrFormat("PropagateRegisterCopy(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_HOIST_WHILE_GUARD:
      return absl::StrFormat("HoistWhileGuard(site=%u, param=%u)", step.site_index(), step.param());
    case AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR:
      return absl::StrFormat("ReloadOrEarlyReturnVar(site=%u, param=%u)", step.site_index(),
                             step.param());
    default:
      return "UnspecifiedMutation";
  }
}

std::unique_ptr<CompoundStatement> AstMutator::PruneUnusedLocalDeclarations(
    const CompoundStatement& body, bool preserve_trailing_return) {
  return rom_nom_nom::PruneUnusedLocalDeclarations(body, preserve_trailing_return);
}

}  // namespace rom_nom_nom
