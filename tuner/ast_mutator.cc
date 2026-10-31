#include "tuner/ast_mutator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    if (s->Kind() == CStatementKind::kLabelStatement ||
        s->Kind() == CStatementKind::kWhileStatement ||
        s->Kind() == CStatementKind::kDoWhileStatement ||
        s->Kind() == CStatementKind::kForStatement) {
      zero_vars.clear();
    } else {
      zero_vars.erase(std::remove_if(zero_vars.begin(), zero_vars.end(),
                                     [&](const std::string& v) {
                                       return StatementModifiesVariable(*s, v) ||
                                              StatementTakesAddressOf(*s, v);
                                     }),
                      zero_vars.end());
    }
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

std::optional<std::string> GetSplitScratchWebBaseName(std::string_view name) {
  size_t underscore = name.rfind('_');
  if (underscore == std::string_view::npos || underscore == 0 || underscore + 1 >= name.size()) {
    return std::nullopt;
  }
  for (size_t i = underscore + 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return std::nullopt;
  }
  std::string_view base = name.substr(0, underscore);
  if (base == "v0" || base == "v1" || base == "arg0" || base == "arg1" || base == "arg2" ||
      base == "arg3" || base == "temp_t0" || base == "temp_t1" || base == "temp_t2" ||
      base == "temp_t3" || base == "temp_t4" || base == "temp_t5" || base == "temp_t6" ||
      base == "temp_t7" || base == "temp_t8" || base == "temp_t9" || base == "temp_at" ||
      (base.size() >= 2 && base[0] == 'f' && base[1] >= '0' && base[1] <= '9')) {
    return std::string(base);
  }
  return std::nullopt;
}

std::optional<std::string> InferDefaultRegisterAnnotation(std::string_view name) {
  if (GetSplitScratchWebBaseName(name).has_value()) {
    return std::nullopt;
  }
  if (name == "v0") return "$2";
  if (name == "v1") return "$3";
  if (name == "arg0") return "$4";
  if (name == "arg1") return "$5";
  if (name == "arg2") return "$6";
  if (name == "arg3") return "$7";
  if (name == "temp_t0") return "$8";
  if (name == "temp_t1") return "$9";
  if (name == "temp_t2") return "$10";
  if (name == "temp_t3") return "$11";
  if (name == "temp_t4") return "$12";
  if (name == "temp_t5") return "$13";
  if (name == "temp_t6") return "$14";
  if (name == "temp_t7") return "$15";
  if (name == "temp_s0") return "$16";
  if (name == "temp_s1") return "$17";
  if (name == "temp_s2") return "$18";
  if (name == "temp_s3") return "$19";
  if (name == "temp_s4") return "$20";
  if (name == "temp_s5") return "$21";
  if (name == "temp_s6") return "$22";
  if (name == "temp_s7") return "$23";
  if (name == "temp_t8") return "$24";
  if (name == "temp_t9") return "$25";
  if (name == "fp") return "$30";
  if (name == "at" || name == "temp_at") return "$1";
  if (name.size() >= 2 && name[0] == 'f' && name[1] >= '0' && name[1] <= '9') {
    for (size_t i = 1; i < name.size(); ++i) {
      if (name[i] < '0' || name[i] > '9') return std::nullopt;
    }
    return "$" + std::string(name);
  }
  return std::nullopt;
}

bool IsComparisonBinaryExpression(const CExpression& expr) {
  if (expr.Kind() != CExpressionKind::kBinaryExpression) return false;
  std::string_view op = static_cast<const BinaryExpression&>(expr).Op();
  return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
}

bool MatchIfStoreFlagStep(const CStatement& stmt, std::unique_ptr<CExpression>* out_lhs,
                          std::string* out_op, std::unique_ptr<CExpression>* out_cond) {
  if (stmt.Kind() != CStatementKind::kIfStatement) return false;
  const auto& if_stmt = static_cast<const IfStatement&>(stmt);
  if (if_stmt.ElseBranch() != nullptr) return false;
  if (!IsComparisonBinaryExpression(if_stmt.Condition())) return false;
  const CStatement* inner = &if_stmt.ThenBranch();
  if (inner->Kind() == CStatementKind::kCompoundStatement) {
    const auto& comp = static_cast<const CompoundStatement&>(*inner);
    if (comp.Statements().size() != 1) return false;
    inner = comp.Statements()[0].get();
  }
  if (inner->Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expr = static_cast<const ExpressionStatement&>(*inner).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (!IsPureExpression(assign.Lhs())) return false;
  if ((assign.Op() == "+=" || assign.Op() == "-=") &&
      assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
      static_cast<const IntegerLiteralExpression&>(assign.Rhs()).Value() == 1) {
    if (out_lhs != nullptr) *out_lhs = assign.Lhs().Clone();
    if (out_op != nullptr) *out_op = assign.Op() == "+=" ? "+" : "-";
    if (out_cond != nullptr) *out_cond = if_stmt.Condition().Clone();
    return true;
  }
  if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
    if ((bin.Op() == "+" || bin.Op() == "-") && bin.Lhs().ToString() == assign.Lhs().ToString() &&
        bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
        static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value() == 1) {
      if (out_lhs != nullptr) *out_lhs = assign.Lhs().Clone();
      if (out_op != nullptr) *out_op = bin.Op();
      if (out_cond != nullptr) *out_cond = if_stmt.Condition().Clone();
      return true;
    }
  }
  return false;
}

bool MatchInlineStoreFlagStep(const CStatement& stmt, std::unique_ptr<CExpression>* out_lhs,
                              std::string* out_op, std::unique_ptr<CExpression>* out_cond) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (!IsPureExpression(assign.Lhs())) return false;
  if ((assign.Op() == "+=" || assign.Op() == "-=") && IsComparisonBinaryExpression(assign.Rhs())) {
    if (out_lhs != nullptr) *out_lhs = assign.Lhs().Clone();
    if (out_op != nullptr) *out_op = assign.Op() == "+=" ? "+" : "-";
    if (out_cond != nullptr) *out_cond = assign.Rhs().Clone();
    return true;
  }
  if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
    if ((bin.Op() == "+" || bin.Op() == "-") && bin.Lhs().ToString() == assign.Lhs().ToString() &&
        IsComparisonBinaryExpression(bin.Rhs())) {
      if (out_lhs != nullptr) *out_lhs = assign.Lhs().Clone();
      if (out_op != nullptr) *out_op = bin.Op();
      if (out_cond != nullptr) *out_cond = bin.Rhs().Clone();
      return true;
    }
  }
  return false;
}

bool IsNonTrivialSubexpression(const CExpression& expr) {
  return expr.Kind() == CExpressionKind::kBinaryExpression ||
         expr.Kind() == CExpressionKind::kUnaryExpression ||
         expr.Kind() == CExpressionKind::kCastExpression ||
         expr.Kind() == CExpressionKind::kCallExpression;
}

bool MatchGeneralSplittableStatement(const CStatement& stmt,
                                     std::unique_ptr<CExpression>* out_subexpr,
                                     const std::string& replacement_var,
                                     std::unique_ptr<CStatement>* out_replaced_stmt) {
  if (stmt.Kind() == CStatementKind::kExpressionStatement) {
    const auto& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
    if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
        if (IsNonTrivialSubexpression(bin.Lhs())) {
          if (out_subexpr != nullptr) *out_subexpr = bin.Lhs().Clone();
          if (out_replaced_stmt != nullptr) {
            *out_replaced_stmt = CStatement::Expression(CExpression::Assignment(
                assign.Op(), assign.Lhs().Clone(),
                CExpression::Binary(bin.Op(), CExpression::Identifier(replacement_var),
                                    bin.Rhs().Clone())));
          }
          return true;
        }
        if (IsNonTrivialSubexpression(bin.Rhs())) {
          if (out_subexpr != nullptr) *out_subexpr = bin.Rhs().Clone();
          if (out_replaced_stmt != nullptr) {
            *out_replaced_stmt = CStatement::Expression(CExpression::Assignment(
                assign.Op(), assign.Lhs().Clone(),
                CExpression::Binary(bin.Op(), bin.Lhs().Clone(),
                                    CExpression::Identifier(replacement_var))));
          }
          return true;
        }
      }
    }
  }
  return false;
}

std::unique_ptr<CExpression> ReplaceExactSubexpressionInExpression(const CExpression& expr,
                                                                   const std::string& target_str,
                                                                   const CExpression& replacement) {
  if (expr.ToString() == target_str) {
    return replacement.Clone();
  }
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(
          un.Op(), ReplaceExactSubexpressionInExpression(un.Operand(), target_str, replacement),
          un.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(
          bin.Op(), ReplaceExactSubexpressionInExpression(bin.Lhs(), target_str, replacement),
          ReplaceExactSubexpressionInExpression(bin.Rhs(), target_str, replacement));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return CExpression::Assignment(
          assign.Op(), ReplaceExactSubexpressionInExpression(assign.Lhs(), target_str, replacement),
          ReplaceExactSubexpressionInExpression(assign.Rhs(), target_str, replacement));
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(cast.TargetType(), ReplaceExactSubexpressionInExpression(
                                                      cast.Operand(), target_str, replacement));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> ReplaceExactSubexpressionInStatement(const CStatement& stmt,
                                                                 const std::string& target_str,
                                                                 const CExpression& replacement) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> new_stmts;
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        new_stmts.push_back(ReplaceExactSubexpressionInStatement(*child, target_str, replacement));
      }
      return std::make_unique<CompoundStatement>(std::move(new_stmts));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(ReplaceExactSubexpressionInExpression(
          static_cast<const ExpressionStatement&>(stmt).Expression(), target_str, replacement));
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      return CStatement::If(
          ReplaceExactSubexpressionInExpression(if_stmt.Condition(), target_str, replacement),
          ReplaceExactSubexpressionInStatement(if_stmt.ThenBranch(), target_str, replacement),
          if_stmt.ElseBranch()
              ? ReplaceExactSubexpressionInStatement(*if_stmt.ElseBranch(), target_str, replacement)
              : nullptr);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dw = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          ReplaceExactSubexpressionInStatement(dw.Body(), target_str, replacement),
          ReplaceExactSubexpressionInExpression(dw.Condition(), target_str, replacement));
    }
    case CStatementKind::kWhileStatement: {
      const auto& wh = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(
          ReplaceExactSubexpressionInExpression(wh.Condition(), target_str, replacement),
          ReplaceExactSubexpressionInStatement(wh.Body(), target_str, replacement));
    }
    case CStatementKind::kForStatement: {
      const auto& f = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          f.Init() ? ReplaceExactSubexpressionInStatement(*f.Init(), target_str, replacement)
                   : nullptr,
          f.Condition()
              ? ReplaceExactSubexpressionInExpression(*f.Condition(), target_str, replacement)
              : nullptr,
          f.Step() ? ReplaceExactSubexpressionInExpression(*f.Step(), target_str, replacement)
                   : nullptr,
          ReplaceExactSubexpressionInStatement(f.Body(), target_str, replacement));
    }
    default:
      return stmt.Clone();
  }
}

bool FindHoistableInvariantInExpression(const CExpression& expr, bool in_condition,
                                        std::unique_ptr<CExpression>* out_invariant) {
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& un = static_cast<const UnaryExpression&>(expr);
    if (un.IsPrefix() && un.Op() == "&" && un.Operand().Kind() == CExpressionKind::kIdentifier &&
        IsGlobalIdentifierName(static_cast<const IdentifierExpression&>(un.Operand()).Name())) {
      if (out_invariant != nullptr) *out_invariant = expr.Clone();
      return true;
    }
    return FindHoistableInvariantInExpression(un.Operand(), in_condition, out_invariant);
  }
  if (expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(expr);
    if (in_condition && IsComparisonBinaryExpression(expr)) {
      if (bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t val = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        if (val != 0) {
          if (out_invariant != nullptr) *out_invariant = bin.Rhs().Clone();
          return true;
        }
      }
    }
    if (!in_condition && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
      int64_t val = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
      if (val < -32768 || val > 65535) {
        if (out_invariant != nullptr) *out_invariant = bin.Rhs().Clone();
        return true;
      }
    }
    return FindHoistableInvariantInExpression(bin.Lhs(), in_condition, out_invariant) ||
           FindHoistableInvariantInExpression(bin.Rhs(), in_condition, out_invariant);
  }
  if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
    const auto& assign = static_cast<const AssignmentExpression&>(expr);
    return FindHoistableInvariantInExpression(assign.Lhs(), in_condition, out_invariant) ||
           FindHoistableInvariantInExpression(assign.Rhs(), in_condition, out_invariant);
  }
  if (expr.Kind() == CExpressionKind::kCastExpression) {
    return FindHoistableInvariantInExpression(static_cast<const CastExpression&>(expr).Operand(),
                                              in_condition, out_invariant);
  }
  return false;
}

bool FindHoistableLoopInvariantInStatement(const CStatement& stmt,
                                           std::unique_ptr<CExpression>* out_invariant) {
  switch (stmt.Kind()) {
    case CStatementKind::kDoWhileStatement: {
      const auto& dw = static_cast<const DoWhileStatement&>(stmt);
      return FindHoistableInvariantInExpression(dw.Condition(), /*in_condition=*/true,
                                                out_invariant) ||
             FindHoistableLoopInvariantInStatement(dw.Body(), out_invariant);
    }
    case CStatementKind::kWhileStatement: {
      const auto& wh = static_cast<const WhileStatement&>(stmt);
      return FindHoistableInvariantInExpression(wh.Condition(), /*in_condition=*/true,
                                                out_invariant) ||
             FindHoistableLoopInvariantInStatement(wh.Body(), out_invariant);
    }
    case CStatementKind::kForStatement: {
      const auto& f = static_cast<const ForStatement&>(stmt);
      if (f.Condition() != nullptr && FindHoistableInvariantInExpression(
                                          *f.Condition(), /*in_condition=*/true, out_invariant)) {
        return true;
      }
      return FindHoistableLoopInvariantInStatement(f.Body(), out_invariant);
    }
    case CStatementKind::kCompoundStatement: {
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (FindHoistableLoopInvariantInStatement(*child, out_invariant)) return true;
      }
      return false;
    }
    case CStatementKind::kExpressionStatement:
      return FindHoistableInvariantInExpression(
          static_cast<const ExpressionStatement&>(stmt).Expression(), /*in_condition=*/false,
          out_invariant);
    default:
      return false;
  }
}

void CollectActiveRegisterAnnotationsInStatement(const CStatement& stmt,
                                                 std::set<std::string>* out) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        CollectActiveRegisterAnnotationsInStatement(*child, out);
      }
      break;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(stmt);
      if (decl.RegisterAnnotation().has_value()) {
        out->insert(*decl.RegisterAnnotation());
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      CollectActiveRegisterAnnotationsInStatement(if_stmt.ThenBranch(), out);
      if (if_stmt.ElseBranch() != nullptr) {
        CollectActiveRegisterAnnotationsInStatement(*if_stmt.ElseBranch(), out);
      }
      break;
    }
    case CStatementKind::kWhileStatement:
      CollectActiveRegisterAnnotationsInStatement(static_cast<const WhileStatement&>(stmt).Body(),
                                                  out);
      break;
    case CStatementKind::kDoWhileStatement:
      CollectActiveRegisterAnnotationsInStatement(static_cast<const DoWhileStatement&>(stmt).Body(),
                                                  out);
      break;
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(stmt);
      if (for_stmt.Init() != nullptr) {
        CollectActiveRegisterAnnotationsInStatement(*for_stmt.Init(), out);
      }
      CollectActiveRegisterAnnotationsInStatement(for_stmt.Body(), out);
      break;
    }
    case CStatementKind::kSwitchStatement:
      for (const auto& sc : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (sc.body != nullptr) {
          CollectActiveRegisterAnnotationsInStatement(*sc.body, out);
        }
      }
      break;
    default:
      break;
  }
}

bool IsFloatingPointRegisterAnnotation(std::string_view reg) {
  return reg.size() >= 3 && reg[0] == '$' && reg[1] == 'f';
}

bool IsCallerSavedScratchRegisterAnnotation(std::string_view reg) {
  return reg == "$1" || reg == "$2" || reg == "$3" || reg == "$8" || reg == "$9" || reg == "$10" ||
         reg == "$11" || reg == "$12" || reg == "$13" || reg == "$14" || reg == "$15" ||
         reg == "$24" || reg == "$25" || reg == "$f0" || reg == "$f1" || reg == "$f2" ||
         reg == "$f3" || reg == "$f4" || reg == "$f5" || reg == "$f6" || reg == "$f7" ||
         reg == "$f8" || reg == "$f9" || reg == "$f10" || reg == "$f11" || reg == "$f12" ||
         reg == "$f13" || reg == "$f14" || reg == "$f15" || reg == "$f16" || reg == "$f17" ||
         reg == "$f18" || reg == "$f19";
}

std::unique_ptr<CStatement> StripRegisterAnnotationsInStatement(const CStatement& stmt,
                                                                bool scratch_only) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> new_stmts;
      const auto& children = static_cast<const CompoundStatement&>(stmt).Statements();
      new_stmts.reserve(children.size());
      for (const auto& child : children) {
        new_stmts.push_back(StripRegisterAnnotationsInStatement(*child, scratch_only));
      }
      return std::make_unique<CompoundStatement>(std::move(new_stmts));
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(stmt);
      std::optional<std::string> reg_ann = decl.RegisterAnnotation();
      if (reg_ann.has_value() && *reg_ann != "$30" &&
          (!scratch_only || IsCallerSavedScratchRegisterAnnotation(*reg_ann))) {
        reg_ann = std::nullopt;
      }
      return CStatement::VariableDeclaration(
          decl.Type(), decl.Name(), decl.Initializer() ? decl.Initializer()->Clone() : nullptr,
          decl.ArraySize(), reg_ann);
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      return CStatement::If(if_stmt.Condition().Clone(),
                            StripRegisterAnnotationsInStatement(if_stmt.ThenBranch(), scratch_only),
                            if_stmt.ElseBranch() ? StripRegisterAnnotationsInStatement(
                                                       *if_stmt.ElseBranch(), scratch_only)
                                                 : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& wh = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(wh.Condition().Clone(),
                               StripRegisterAnnotationsInStatement(wh.Body(), scratch_only));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dw = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(StripRegisterAnnotationsInStatement(dw.Body(), scratch_only),
                                 dw.Condition().Clone());
    }
    case CStatementKind::kForStatement: {
      const auto& f = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          f.Init() ? StripRegisterAnnotationsInStatement(*f.Init(), scratch_only) : nullptr,
          f.Condition() ? f.Condition()->Clone() : nullptr, f.Step() ? f.Step()->Clone() : nullptr,
          StripRegisterAnnotationsInStatement(f.Body(), scratch_only));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& sw = static_cast<const SwitchStatement&>(stmt);
      std::vector<SwitchCase> new_cases;
      new_cases.reserve(sw.Cases().size());
      for (const auto& sc : sw.Cases()) {
        SwitchCase next_case;
        next_case.case_values = sc.case_values;
        next_case.is_default = sc.is_default;
        if (sc.body != nullptr) {
          next_case.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              StripRegisterAnnotationsInStatement(*sc.body, scratch_only).release()));
        }
        new_cases.push_back(std::move(next_case));
      }
      return CStatement::Switch(sw.Condition().Clone(), std::move(new_cases));
    }
    default:
      return stmt.Clone();
  }
}

int MutationTruncationPriorityTier(const AstMutationStep& step) {
  switch (step.kind()) {
    case AST_MUTATION_REORDER_LOCAL_DECLS:
    case AST_MUTATION_COALESCE_PARAM_COPY:
    case AST_MUTATION_DIRECT_TYPED_GLOBAL:
    case AST_MUTATION_GLOBAL_ARRAY_ACCESS:
    case AST_MUTATION_VOLATILE_GLOBAL_ACCESS:
    case AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS:
    case AST_MUTATION_TOGGLE_TRAILING_RETURN:
      return 0;
    case AST_MUTATION_INLINE_SINGLE_USE_TEMP:
    case AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN:
    case AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX:
      return (step.param() == 1 || step.param() == 3) ? 0 : 1;
    case AST_MUTATION_TOGGLE_REGISTER_ANNOTATION:
      return step.param() >= 2 ? 0 : 1;
    case AST_MUTATION_COMMUTE_BINARY_OP:
    case AST_MUTATION_TOGGLE_UNSIGNED_CAST:
    case AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT:
      return 2;
    default:
      return 1;
  }
}

struct ToggleableVarDeclInfo {
  uint32_t site_index = 0;
  std::string name;
  std::optional<std::string> reg_annotation;
  std::optional<std::string> default_reg;
};

void CollectToggleableVarDeclsInStatement(const CStatement& stmt,
                                          std::vector<ToggleableVarDeclInfo>* out) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        CollectToggleableVarDeclsInStatement(*child, out);
      }
      break;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(stmt);
      if (!decl.ArraySize().has_value()) {
        auto default_reg = InferDefaultRegisterAnnotation(decl.Name());
        if (decl.RegisterAnnotation().has_value() || default_reg.has_value()) {
          ToggleableVarDeclInfo info;
          info.site_index = static_cast<uint32_t>(out->size());
          info.name = decl.Name();
          info.reg_annotation = decl.RegisterAnnotation();
          info.default_reg = std::move(default_reg);
          out->push_back(std::move(info));
        }
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      CollectToggleableVarDeclsInStatement(if_stmt.ThenBranch(), out);
      if (if_stmt.ElseBranch() != nullptr) {
        CollectToggleableVarDeclsInStatement(*if_stmt.ElseBranch(), out);
      }
      break;
    }
    case CStatementKind::kWhileStatement:
      CollectToggleableVarDeclsInStatement(static_cast<const WhileStatement&>(stmt).Body(), out);
      break;
    case CStatementKind::kDoWhileStatement:
      CollectToggleableVarDeclsInStatement(static_cast<const DoWhileStatement&>(stmt).Body(), out);
      break;
    case CStatementKind::kForStatement: {
      const auto& f = static_cast<const ForStatement&>(stmt);
      if (f.Init() != nullptr) {
        CollectToggleableVarDeclsInStatement(*f.Init(), out);
      }
      CollectToggleableVarDeclsInStatement(f.Body(), out);
      break;
    }
    case CStatementKind::kSwitchStatement:
      for (const auto& sc : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (sc.body != nullptr) {
          CollectToggleableVarDeclsInStatement(*sc.body, out);
        }
      }
      break;
    default:
      break;
  }
}

std::unique_ptr<CStatement> RewriteToggleableVarDeclAnnotationsInStatement(
    const CStatement& stmt, uint32_t* next_site,
    const std::map<uint32_t, std::optional<std::string>>& new_annotations) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> new_stmts;
      const auto& children = static_cast<const CompoundStatement&>(stmt).Statements();
      new_stmts.reserve(children.size());
      for (const auto& child : children) {
        new_stmts.push_back(
            RewriteToggleableVarDeclAnnotationsInStatement(*child, next_site, new_annotations));
      }
      return std::make_unique<CompoundStatement>(std::move(new_stmts));
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& decl = static_cast<const VariableDeclarationStatement&>(stmt);
      std::optional<std::string> reg_ann = decl.RegisterAnnotation();
      if (!decl.ArraySize().has_value()) {
        auto default_reg = InferDefaultRegisterAnnotation(decl.Name());
        if (reg_ann.has_value() || default_reg.has_value()) {
          uint32_t site = (*next_site)++;
          auto it = new_annotations.find(site);
          if (it != new_annotations.end()) {
            reg_ann = it->second;
          }
        }
      }
      return CStatement::VariableDeclaration(
          decl.Type(), decl.Name(), decl.Initializer() ? decl.Initializer()->Clone() : nullptr,
          decl.ArraySize(), reg_ann);
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      auto new_then = RewriteToggleableVarDeclAnnotationsInStatement(if_stmt.ThenBranch(),
                                                                     next_site, new_annotations);
      std::unique_ptr<CStatement> new_else =
          if_stmt.ElseBranch() != nullptr ? RewriteToggleableVarDeclAnnotationsInStatement(
                                                *if_stmt.ElseBranch(), next_site, new_annotations)
                                          : nullptr;
      return CStatement::If(if_stmt.Condition().Clone(), std::move(new_then), std::move(new_else));
    }
    case CStatementKind::kWhileStatement: {
      const auto& wh = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(
          wh.Condition().Clone(),
          RewriteToggleableVarDeclAnnotationsInStatement(wh.Body(), next_site, new_annotations));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dw = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          RewriteToggleableVarDeclAnnotationsInStatement(dw.Body(), next_site, new_annotations),
          dw.Condition().Clone());
    }
    case CStatementKind::kForStatement: {
      const auto& f = static_cast<const ForStatement&>(stmt);
      std::unique_ptr<CStatement> new_init = f.Init() != nullptr
                                                 ? RewriteToggleableVarDeclAnnotationsInStatement(
                                                       *f.Init(), next_site, new_annotations)
                                                 : nullptr;
      auto new_body =
          RewriteToggleableVarDeclAnnotationsInStatement(f.Body(), next_site, new_annotations);
      return CStatement::For(std::move(new_init), f.Condition() ? f.Condition()->Clone() : nullptr,
                             f.Step() ? f.Step()->Clone() : nullptr, std::move(new_body));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& sw = static_cast<const SwitchStatement&>(stmt);
      std::vector<SwitchCase> new_cases;
      new_cases.reserve(sw.Cases().size());
      for (const auto& sc : sw.Cases()) {
        SwitchCase next_case;
        next_case.case_values = sc.case_values;
        next_case.is_default = sc.is_default;
        if (sc.body != nullptr) {
          next_case.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              RewriteToggleableVarDeclAnnotationsInStatement(*sc.body, next_site, new_annotations)
                  .release()));
        }
        new_cases.push_back(std::move(next_case));
      }
      return CStatement::Switch(sw.Condition().Clone(), std::move(new_cases));
    }
    default:
      return stmt.Clone();
  }
}

std::optional<uint32_t> EncodeHardwareRegisterParam(std::string_view reg) {
  if (reg.size() >= 3 && reg[0] == '$' && reg[1] == 'f') {
    uint32_t fp_num = 0;
    for (size_t i = 2; i < reg.size(); ++i) {
      if (reg[i] < '0' || reg[i] > '9') return std::nullopt;
      fp_num = fp_num * 10 + static_cast<uint32_t>(reg[i] - '0');
    }
    if (fp_num <= 31) return 200 + fp_num;
    return std::nullopt;
  }
  if (reg.size() >= 2 && reg[0] == '$') {
    uint32_t gpr_num = 0;
    for (size_t i = 1; i < reg.size(); ++i) {
      if (reg[i] < '0' || reg[i] > '9') return std::nullopt;
      gpr_num = gpr_num * 10 + static_cast<uint32_t>(reg[i] - '0');
    }
    if (gpr_num >= 2 && gpr_num <= 25) return 100 + gpr_num;
  }
  return std::nullopt;
}

std::optional<std::string> DecodeHardwareRegisterParam(uint32_t param) {
  if (param >= 200 && param <= 231) {
    return absl::StrFormat("$f%u", param - 200);
  }
  if (param >= 102 && param <= 125) {
    return absl::StrFormat("$%u", param - 100);
  }
  return std::nullopt;
}

FunctionDeclaration BuildMutatedFunctionDeclaration(
    const FunctionDeclaration& original, std::unique_ptr<CompoundStatement> mutated_body) {
  FunctionOptimizationMode opt_mode = original.OptimizationMode();
  if (opt_mode == FunctionOptimizationMode::kAuto) {
    std::set<std::string> active_regs;
    CollectActiveRegisterAnnotationsInStatement(original.Body(), &active_regs);
    if (!active_regs.empty()) {
      opt_mode = FunctionOptimizationMode::kO2Standard;
    }
  }
  FunctionDeclaration fn(original.ReturnType(), original.Name(), original.Parameters(),
                         std::move(mutated_body), original.IsStatic(), opt_mode);
  fn.SetSuppressAllCseBarriers(original.SuppressAllCseBarriers());
  fn.MutableSuppressedCseBarrierVars() = original.SuppressedCseBarrierVars();
  return fn;
}

int DiffGuidedPriorityBonus(const AstMutationStep& step,
                            const std::vector<ToggleableVarDeclInfo>& decls,
                            const DiffDiagnostic* diff_diagnostic) {
  if (diff_diagnostic == nullptr) return 0;

  auto is_transposed_reg = [&](std::string_view reg) {
    for (const auto& [r1, r2] : diff_diagnostic->transposed_register_pairs) {
      if (r1 == reg || r2 == reg) return true;
    }
    return false;
  };
  auto is_exact_transposed_pair = [&](std::string_view reg_a, std::string_view reg_b) {
    for (const auto& [r1, r2] : diff_diagnostic->transposed_register_pairs) {
      if ((r1 == reg_a && r2 == reg_b) || (r1 == reg_b && r2 == reg_a)) return true;
    }
    return false;
  };

  if (step.kind() == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION) {
    if (step.param() >= 10 && step.param() < 100) {
      uint32_t i = step.site_index();
      uint32_t j = step.param() - 10;
      if (i < decls.size() && j < decls.size() && decls[i].reg_annotation.has_value() &&
          decls[j].reg_annotation.has_value()) {
        if (is_exact_transposed_pair(*decls[i].reg_annotation, *decls[j].reg_annotation)) {
          return -3;
        }
        if (is_transposed_reg(*decls[i].reg_annotation) ||
            is_transposed_reg(*decls[j].reg_annotation)) {
          return -2;
        }
      }
    } else if (step.param() >= 100 && step.param() < 300) {
      return -3;
    } else if (step.param() == 0 || step.param() == 1) {
      uint32_t i = step.site_index();
      if (i < decls.size()) {
        const std::string& reg =
            decls[i].reg_annotation.has_value()
                ? *decls[i].reg_annotation
                : (decls[i].default_reg.has_value() ? *decls[i].default_reg : "");
        if (!reg.empty() && is_transposed_reg(reg)) {
          return -2;
        }
      }
    } else if (step.param() == 4 || step.param() == 5) {
      if (diff_diagnostic->has_unfilled_delay_slot || diff_diagnostic->has_adjacent_insn_swap) {
        return -3;
      }
      return -1;
    } else if (step.param() == 2 || step.param() == 3) {
      if (!diff_diagnostic->transposed_register_pairs.empty()) {
        return -2;
      }
    }
  }

  if (step.kind() == AST_MUTATION_REORDER_LOCAL_DECLS &&
      !diff_diagnostic->transposed_register_pairs.empty()) {
    return -2;
  }

  if (diff_diagnostic->has_adjacent_insn_swap || diff_diagnostic->has_unfilled_delay_slot) {
    if (step.kind() == AST_MUTATION_SWAP_ADJACENT_STATEMENTS ||
        step.kind() == AST_MUTATION_SPLIT_SINGLE_USE_TEMP ||
        step.kind() == AST_MUTATION_HOIST_WHILE_GUARD ||
        step.kind() == AST_MUTATION_HOIST_LOOP_INVARIANT ||
        step.kind() == AST_MUTATION_INVERT_IF_ELSE) {
      return -2;
    }
  }

  if (diff_diagnostic->has_extra_or_missing_move) {
    if (step.kind() == AST_MUTATION_PROPAGATE_REGISTER_COPY ||
        step.kind() == AST_MUTATION_COALESCE_PARAM_COPY ||
        step.kind() == AST_MUTATION_INLINE_SINGLE_USE_TEMP ||
        step.kind() == AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS ||
        step.kind() == AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR) {
      return -2;
    }
  }

  return 0;
}

struct MutationContext {
  const AstMutationStep* step_to_apply = nullptr;
  std::vector<AstMutationStep>* discovered = nullptr;
  size_t max_mutations = 64;
  std::set<std::string> local_variables;
  std::set<std::string> active_register_annotations;
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
  uint32_t toggle_register_annotation_index = 0;
  uint32_t toggle_store_flag_index = 0;
  uint32_t hoist_loop_invariant_index = 0;

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
        const bool is_address_of_operand =
            cast.Operand().Kind() == CExpressionKind::kUnaryExpression &&
            static_cast<const UnaryExpression&>(cast.Operand()).IsPrefix() &&
            static_cast<const UnaryExpression&>(cast.Operand()).Op() == "&";
        uint32_t site = context->cast_index++;
        if (!is_address_of_operand) {
          context->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 0);
        }
        context->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 1);
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, &parameter)) {
          if (parameter == 0 && !is_address_of_operand) {
            return new_operand;
          }
          if (parameter == 1) {
            CType flipped = (cast.TargetType().name == "u32") ? CType::S32() : CType::U32();
            return CExpression::Cast(flipped, std::move(new_operand));
          }
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
        context->Record(AST_MUTATION_REORDER_LOCAL_DECLS, site, 0);
        const bool can_swap_explicit_registers =
            !declaration1.ArraySize().has_value() && !declaration2.ArraySize().has_value() &&
            declaration1.RegisterAnnotation().has_value() &&
            declaration2.RegisterAnnotation().has_value() &&
            *declaration1.RegisterAnnotation() != *declaration2.RegisterAnnotation() &&
            *declaration1.RegisterAnnotation() != "$30" &&
            *declaration2.RegisterAnnotation() != "$30" &&
            IsFloatingPointRegisterAnnotation(*declaration1.RegisterAnnotation()) ==
                IsFloatingPointRegisterAnnotation(*declaration2.RegisterAnnotation());
        if (can_swap_explicit_registers) {
          context->Record(AST_MUTATION_REORDER_LOCAL_DECLS, site, 1);
          context->Record(AST_MUTATION_REORDER_LOCAL_DECLS, site, 2);
        }
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_REORDER_LOCAL_DECLS, site, &parameter)) {
          if (parameter == 1 && can_swap_explicit_registers) {
            auto init1 = declaration1.Initializer() != nullptr
                             ? TraverseExpression(*declaration1.Initializer(), context)
                             : nullptr;
            auto init2 = declaration2.Initializer() != nullptr
                             ? TraverseExpression(*declaration2.Initializer(), context)
                             : nullptr;
            new_statements.push_back(CStatement::VariableDeclaration(
                declaration1.Type(), declaration1.Name(), std::move(init1),
                declaration1.ArraySize(), declaration2.RegisterAnnotation()));
            new_statements.push_back(CStatement::VariableDeclaration(
                declaration2.Type(), declaration2.Name(), std::move(init2),
                declaration2.ArraySize(), declaration1.RegisterAnnotation()));
            ++i;
            continue;
          }
          if (parameter == 2 && can_swap_explicit_registers) {
            context->active_register_annotations.erase(*declaration1.RegisterAnnotation());
            context->active_register_annotations.erase(*declaration2.RegisterAnnotation());
            auto init1 = declaration1.Initializer() != nullptr
                             ? TraverseExpression(*declaration1.Initializer(), context)
                             : nullptr;
            auto init2 = declaration2.Initializer() != nullptr
                             ? TraverseExpression(*declaration2.Initializer(), context)
                             : nullptr;
            new_statements.push_back(CStatement::VariableDeclaration(
                declaration2.Type(), declaration2.Name(), std::move(init2),
                declaration2.ArraySize(), std::nullopt));
            new_statements.push_back(CStatement::VariableDeclaration(
                declaration1.Type(), declaration1.Name(), std::move(init1),
                declaration1.ArraySize(), std::nullopt));
            ++i;
            continue;
          }
          if (parameter == 0) {
            new_statements.push_back(TraverseStatement(*statements[i + 1], context));
            new_statements.push_back(TraverseStatement(*statements[i], context));
            ++i;
            continue;
          }
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
    if (MatchGeneralSplittableStatement(*statements[i], nullptr, "", nullptr)) {
      std::vector<std::string> eligible_candidates;
      for (const auto& cand_var : context->local_variables) {
        if (CountIdentifierOccurrencesInStatement(*statements[i], cand_var) > 0) continue;
        if (context->root_body != nullptr &&
            IsVariableArrayInStatement(*context->root_body, cand_var)) {
          continue;
        }
        if (IsVariableDeadAfter(statements, i, cand_var, is_top_level_block, context->root_body)) {
          eligible_candidates.push_back(cand_var);
        }
      }
      bool synthesized_split_var = false;
      if (eligible_candidates.empty() && is_top_level_block) {
        std::string synth_var =
            CountIdentifierOccurrencesInStatement(*statements[i], "v0") == 0 ? "v0" : "v1";
        if (CountIdentifierOccurrencesInStatement(*statements[i], synth_var) == 0 &&
            context->local_variables.count(synth_var) == 0) {
          eligible_candidates.push_back(synth_var);
          synthesized_split_var = true;
        }
      }
      if (!eligible_candidates.empty()) {
        uint32_t site = context->split_temporary_index++;
        size_t num_candidates = std::min<size_t>(eligible_candidates.size(), 2);
        for (size_t v = 0; v < num_candidates; ++v) {
          context->Record(AST_MUTATION_SPLIT_SINGLE_USE_TEMP, site, static_cast<uint32_t>(v));
        }
        bool can_hoist_before_prev_store = false;
        std::unique_ptr<CExpression> probe_subexpr;
        if (!new_statements.empty() &&
            MatchGeneralSplittableStatement(*statements[i], &probe_subexpr, eligible_candidates[0],
                                            nullptr) &&
            probe_subexpr != nullptr &&
            new_statements.back()->Kind() == CStatementKind::kExpressionStatement) {
          const auto& prev_es = static_cast<const ExpressionStatement&>(*new_statements.back());
          if (prev_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& prev_a = static_cast<const AssignmentExpression&>(prev_es.Expression());
            const bool prev_indirect = prev_a.Lhs().Kind() != CExpressionKind::kIdentifier;
            if (prev_indirect && !MayAliasMemoryExpressions(*probe_subexpr, prev_a.Lhs()) &&
                CountIdentifierOccurrencesInStatement(*new_statements.back(),
                                                      eligible_candidates[0]) == 0) {
              can_hoist_before_prev_store = true;
              context->Record(AST_MUTATION_SPLIT_SINGLE_USE_TEMP, site, 2);
            }
          }
        }
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_SPLIT_SINGLE_USE_TEMP, site, &parameter)) {
          size_t chosen_idx = (parameter < eligible_candidates.size()) ? parameter : 0;
          const std::string& chosen_var = eligible_candidates[chosen_idx];
          std::unique_ptr<CExpression> subexpr;
          std::unique_ptr<CStatement> replaced_stmt;
          if (MatchGeneralSplittableStatement(*statements[i], &subexpr, chosen_var,
                                              &replaced_stmt) &&
              subexpr != nullptr && replaced_stmt != nullptr) {
            if (synthesized_split_var) {
              size_t decl_pos = 0;
              while (decl_pos < new_statements.size() &&
                     new_statements[decl_pos]->Kind() ==
                         CStatementKind::kVariableDeclarationStatement) {
                ++decl_pos;
              }
              auto default_reg = InferDefaultRegisterAnnotation(chosen_var);
              std::optional<std::string> reg_ann = std::nullopt;
              if (default_reg.has_value() &&
                  context->active_register_annotations.count(*default_reg) == 0) {
                reg_ann = default_reg;
                context->active_register_annotations.insert(*default_reg);
              }
              new_statements.insert(new_statements.begin() + static_cast<std::ptrdiff_t>(decl_pos),
                                    CStatement::VariableDeclaration(
                                        CType::S32(), chosen_var, nullptr, std::nullopt, reg_ann));
              context->local_variables.insert(chosen_var);
            }
            auto temp_assign = CStatement::Expression(CExpression::Assignment(
                "=", CExpression::Identifier(chosen_var), TraverseExpression(*subexpr, context)));
            if (parameter == 2 && can_hoist_before_prev_store && !new_statements.empty()) {
              auto prev_stmt = std::move(new_statements.back());
              new_statements.back() = std::move(temp_assign);
              new_statements.push_back(std::move(prev_stmt));
            } else {
              new_statements.push_back(std::move(temp_assign));
            }
            new_statements.push_back(TraverseStatement(*replaced_stmt, context));
            continue;
          }
        }
      }
    }

    // 5b. Check hoisting loop-invariant constant or address out of loop
    // (`AST_MUTATION_HOIST_LOOP_INVARIANT`)
    if (statements[i]->Kind() == CStatementKind::kDoWhileStatement ||
        statements[i]->Kind() == CStatementKind::kWhileStatement ||
        statements[i]->Kind() == CStatementKind::kForStatement) {
      std::unique_ptr<CExpression> invariant_expr;
      if (FindHoistableLoopInvariantInStatement(*statements[i], &invariant_expr) &&
          invariant_expr != nullptr) {
        std::vector<std::string> eligible_candidates;
        for (const auto& cand_var : context->local_variables) {
          if (CountIdentifierOccurrencesInStatement(*statements[i], cand_var) > 0) continue;
          if (context->root_body != nullptr &&
              IsVariableArrayInStatement(*context->root_body, cand_var)) {
            continue;
          }
          if (IsVariableDeadAfter(statements, i, cand_var, is_top_level_block,
                                  context->root_body)) {
            eligible_candidates.push_back(cand_var);
          }
        }
        bool synthesized_hoist_var = false;
        if (eligible_candidates.empty() && is_top_level_block) {
          for (int t = 0; t <= 9; ++t) {
            std::string cand_t = absl::StrFormat("temp_t%d", t);
            auto def_reg = InferDefaultRegisterAnnotation(cand_t);
            if (context->local_variables.count(cand_t) == 0 &&
                (!def_reg.has_value() ||
                 context->active_register_annotations.count(*def_reg) == 0) &&
                CountIdentifierOccurrencesInStatement(*statements[i], cand_t) == 0) {
              eligible_candidates.push_back(cand_t);
              synthesized_hoist_var = true;
              break;
            }
          }
        }
        if (!eligible_candidates.empty()) {
          uint32_t site = context->hoist_loop_invariant_index++;
          context->Record(AST_MUTATION_HOIST_LOOP_INVARIANT, site, 0);
          size_t fp_setup_count = 0;
          while (fp_setup_count < new_statements.size()) {
            const auto& back_s = *new_statements[new_statements.size() - 1 - fp_setup_count];
            if (back_s.Kind() != CStatementKind::kExpressionStatement) break;
            const auto& back_e = static_cast<const ExpressionStatement&>(back_s).Expression();
            if (back_e.Kind() != CExpressionKind::kAssignmentExpression) break;
            const auto& back_a = static_cast<const AssignmentExpression&>(back_e);
            if (back_a.Op() != "=" || back_a.Lhs().Kind() != CExpressionKind::kIdentifier) break;
            const std::string& lhs_name =
                static_cast<const IdentifierExpression&>(back_a.Lhs()).Name();
            if (lhs_name.size() >= 2 && lhs_name[0] == 'f' && lhs_name[1] >= '0' &&
                lhs_name[1] <= '9') {
              ++fp_setup_count;
            } else {
              break;
            }
          }
          if (fp_setup_count > 0) {
            context->Record(AST_MUTATION_HOIST_LOOP_INVARIANT, site, 1);
          }
          uint32_t parameter = 0;
          if (context->ShouldApply(AST_MUTATION_HOIST_LOOP_INVARIANT, site, &parameter)) {
            size_t chosen_idx =
                (parameter < eligible_candidates.size() && parameter == 0) ? parameter : 0;
            const std::string& chosen_var = eligible_candidates[chosen_idx];
            if (synthesized_hoist_var) {
              size_t decl_pos = 0;
              while (decl_pos < new_statements.size() &&
                     new_statements[decl_pos]->Kind() ==
                         CStatementKind::kVariableDeclarationStatement) {
                ++decl_pos;
              }
              auto default_reg = InferDefaultRegisterAnnotation(chosen_var);
              std::optional<std::string> reg_ann = std::nullopt;
              if (default_reg.has_value() &&
                  context->active_register_annotations.count(*default_reg) == 0) {
                reg_ann = default_reg;
                context->active_register_annotations.insert(*default_reg);
              }
              new_statements.insert(new_statements.begin() + static_cast<std::ptrdiff_t>(decl_pos),
                                    CStatement::VariableDeclaration(
                                        CType::S32(), chosen_var, nullptr, std::nullopt, reg_ann));
              context->local_variables.insert(chosen_var);
            }
            auto hoisted_assign = CStatement::Expression(CExpression::Assignment(
                "=", CExpression::Identifier(chosen_var), invariant_expr->Clone()));
            if (parameter == 1 && fp_setup_count > 0 && fp_setup_count <= new_statements.size()) {
              new_statements.insert(
                  new_statements.end() - static_cast<std::ptrdiff_t>(fp_setup_count),
                  std::move(hoisted_assign));
            } else {
              new_statements.push_back(std::move(hoisted_assign));
            }
            auto replaced_loop = ReplaceExactSubexpressionInStatement(
                *statements[i], invariant_expr->ToString(), *CExpression::Identifier(chosen_var));
            new_statements.push_back(TraverseStatement(*replaced_loop, context));
            continue;
          }
        }
      }
    }

    // 5c. Check hoisting a leading `temp_sX = rhs;` assignment out of `if (cond) { temp_sX = rhs;
    // ... }`
    if (statements[i]->Kind() == CStatementKind::kIfStatement) {
      const auto& if_stmt = static_cast<const IfStatement&>(*statements[i]);
      if (if_stmt.ElseBranch() == nullptr &&
          if_stmt.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
        const auto& then_stmts =
            static_cast<const CompoundStatement&>(if_stmt.ThenBranch()).Statements();
        if (then_stmts.size() >= 2 &&
            then_stmts[0]->Kind() == CStatementKind::kExpressionStatement) {
          const auto& first_e =
              static_cast<const ExpressionStatement&>(*then_stmts[0]).Expression();
          if (first_e.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& first_a = static_cast<const AssignmentExpression&>(first_e);
            if (first_a.Op() == "=" && first_a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                IsPureExpression(first_a.Rhs()) &&
                !ExpressionReadsOrWritesMemoryOrGlobals(first_a.Rhs())) {
              const std::string& s_var =
                  static_cast<const IdentifierExpression&>(first_a.Lhs()).Name();
              if (absl::StartsWith(s_var, "temp_s") &&
                  CountIdentifierOccurrencesInExpression(if_stmt.Condition(), s_var) == 0 &&
                  IsVariableDeadAfter(statements, i, s_var, is_top_level_block,
                                      context->root_body)) {
                const std::string expected_asm =
                    absl::StrFormat("__asm__(\"\" : \"=r\"(%s) : \"0\"(%s))", s_var, s_var);
                const bool has_following_asm_barrier =
                    then_stmts.size() >= 3 &&
                    then_stmts[1]->Kind() == CStatementKind::kExpressionStatement &&
                    static_cast<const ExpressionStatement&>(*then_stmts[1])
                            .Expression()
                            .ToString() == expected_asm;
                uint32_t site = context->hoist_while_guard_index++;
                context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 0);
                if (has_following_asm_barrier) {
                  context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 1);
                }
                uint32_t parameter = 0;
                if (context->ShouldApply(AST_MUTATION_HOIST_WHILE_GUARD, site, &parameter)) {
                  new_statements.push_back(then_stmts[0]->Clone());
                  std::vector<std::unique_ptr<CStatement>> rem_then;
                  const size_t start_k = (has_following_asm_barrier && parameter == 0) ? 2 : 1;
                  for (size_t k = start_k; k < then_stmts.size(); ++k) {
                    rem_then.push_back(then_stmts[k]->Clone());
                  }
                  auto new_if = CStatement::If(
                      if_stmt.Condition().Clone(),
                      std::make_unique<CompoundStatement>(std::move(rem_then)), nullptr);
                  new_statements.push_back(TraverseStatement(*new_if, context));
                  continue;
                }
              }
            }
          }
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
                                &store_rhs)) {
      std::string stored_var = store_rhs->Kind() == CExpressionKind::kIdentifier
                                   ? static_cast<const IdentifierExpression&>(*store_rhs).Name()
                                   : "";
      if (i + 1 < statements.size()) {
        bool next_is_reload = false;
        std::string reload_lhs_var = stored_var;
        CType next_reload_type = store_cast_type;
        if (statements[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
          const auto& next_expr =
              static_cast<const ExpressionStatement&>(*statements[i + 1]).Expression();
          if (next_expr.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& next_assign = static_cast<const AssignmentExpression&>(next_expr);
            if (next_assign.Op() == "=" &&
                next_assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
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
                    reload_lhs_var =
                        static_cast<const IdentifierExpression&>(next_assign.Lhs()).Name();
                  }
                }
              }
            }
          }
        }

        bool next_reads_var = false;
        if (!stored_var.empty()) {
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
                  "=", CExpression::Identifier(reload_lhs_var), std::move(vol_reload_deref))));
              ++i;
            } else if (parameter == 0 && !stored_var.empty()) {
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
      std::unique_ptr<CExpression> sf_lhs;
      std::string sf_op;
      std::unique_ptr<CExpression> sf_cond;
      if (MatchInlineStoreFlagStep(statement, &sf_lhs, &sf_op, &sf_cond)) {
        uint32_t site = context->toggle_store_flag_index++;
        context->Record(AST_MUTATION_TOGGLE_STORE_FLAG_CONDITIONAL, site, 0);
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_TOGGLE_STORE_FLAG_CONDITIONAL, site, &parameter)) {
          auto then_block = std::make_unique<CompoundStatement>();
          auto lhs_clone = sf_lhs->Clone();
          then_block->AddStatement(CStatement::Expression(CExpression::Assignment(
              "=", std::move(sf_lhs),
              CExpression::Binary(sf_op, std::move(lhs_clone), CExpression::Integer(1)))));
          return CStatement::If(std::move(sf_cond), std::move(then_block));
        }
      }
      const auto& expression_statement = static_cast<const ExpressionStatement&>(statement);
      return CStatement::Expression(TraverseExpression(expression_statement.Expression(), context));
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& declaration = static_cast<const VariableDeclarationStatement&>(statement);
      std::unique_ptr<CExpression> initializer =
          declaration.Initializer() != nullptr
              ? TraverseExpression(*declaration.Initializer(), context)
              : nullptr;
      std::optional<std::string> reg_annotation = declaration.RegisterAnnotation();
      if (!declaration.ArraySize().has_value()) {
        auto default_reg = InferDefaultRegisterAnnotation(declaration.Name());
        if (reg_annotation.has_value() || default_reg.has_value()) {
          uint32_t site = context->toggle_register_annotation_index++;
          const bool can_toggle_default =
              reg_annotation.has_value() ||
              (default_reg.has_value() &&
               context->active_register_annotations.count(*default_reg) == 0);
          if (can_toggle_default) {
            context->Record(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, site, 0);
          }
          std::string alt_reg;
          if (reg_annotation.has_value() && !absl::StartsWith(declaration.Name(), "temp_s") &&
              declaration.Name() != "fp") {
            alt_reg = (*reg_annotation == "$2") ? "$3" : "$2";
          }
          const bool can_swap_to_alt =
              !alt_reg.empty() && context->active_register_annotations.count(alt_reg) == 0;
          if (can_swap_to_alt) {
            context->Record(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, site, 1);
          }
          uint32_t parameter = 0;
          if (context->ShouldApply(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, site, &parameter)) {
            if (parameter == 0 && can_toggle_default) {
              if (reg_annotation.has_value()) {
                context->active_register_annotations.erase(*reg_annotation);
                reg_annotation = std::nullopt;
              } else {
                reg_annotation = default_reg;
                context->active_register_annotations.insert(*default_reg);
              }
            } else if (parameter == 1 && can_swap_to_alt) {
              context->active_register_annotations.erase(*reg_annotation);
              reg_annotation = alt_reg;
              context->active_register_annotations.insert(alt_reg);
            }
          }
        }
      }
      return CStatement::VariableDeclaration(declaration.Type(), declaration.Name(),
                                             std::move(initializer), declaration.ArraySize(),
                                             reg_annotation);
    }
    case CStatementKind::kIfStatement: {
      std::unique_ptr<CExpression> sf_lhs;
      std::string sf_op;
      std::unique_ptr<CExpression> sf_cond;
      if (MatchIfStoreFlagStep(statement, &sf_lhs, &sf_op, &sf_cond)) {
        uint32_t site = context->toggle_store_flag_index++;
        context->Record(AST_MUTATION_TOGGLE_STORE_FLAG_CONDITIONAL, site, 0);
        uint32_t parameter = 0;
        if (context->ShouldApply(AST_MUTATION_TOGGLE_STORE_FLAG_CONDITIONAL, site, &parameter)) {
          auto lhs_clone = sf_lhs->Clone();
          return CStatement::Expression(CExpression::Assignment(
              "=", std::move(sf_lhs),
              CExpression::Binary(sf_op, std::move(lhs_clone), std::move(sf_cond))));
        }
      }
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
      uint32_t site = context->hoist_while_guard_index++;
      context->Record(AST_MUTATION_HOIST_WHILE_GUARD, site, 0);
      uint32_t parameter = 0;
      if (context->ShouldApply(AST_MUTATION_HOIST_WHILE_GUARD, site, &parameter) &&
          parameter == 0) {
        return CStatement::While(std::move(new_condition), std::move(new_body));
      }
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

struct SplitScratchWebCandidate {
  std::string base_name;
  std::string split_name;
};

std::vector<SplitScratchWebCandidate> CollectSplitScratchWebCandidates(
    const ReferencedSymbols& referenced_symbols) {
  std::vector<SplitScratchWebCandidate> candidates;
  for (const auto& var_name : referenced_symbols.local_variables) {
    if (auto base_name = GetSplitScratchWebBaseName(var_name)) {
      if (referenced_symbols.local_variables.count(*base_name) > 0) {
        candidates.push_back(
            SplitScratchWebCandidate{.base_name = *base_name, .split_name = var_name});
      }
    }
  }
  return candidates;
}

}  // namespace

std::vector<AstMutationStep> AstMutator::DiscoverMutations(const FunctionDeclaration& function,
                                                           size_t max_mutations,
                                                           const DiffDiagnostic* diff_diagnostic) {
  std::vector<AstMutationStep> mutations;
  if (max_mutations == 0) {
    return mutations;
  }
  ReferencedSymbols referenced_symbols = CollectReferencedSymbols(function);
  std::vector<ToggleableVarDeclInfo> toggleable_decls;
  CollectToggleableVarDeclsInStatement(function.Body(), &toggleable_decls);

  MutationContext context;
  context.discovered = &mutations;
  context.max_mutations = std::numeric_limits<size_t>::max();
  context.local_variables = referenced_symbols.local_variables;
  CollectActiveRegisterAnnotationsInStatement(function.Body(),
                                              &context.active_register_annotations);
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
  if (context.active_register_annotations.size() > 1) {
    context.Record(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, 0, 2);
    context.Record(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, 0, 3);
  }

  // CSE barrier suppression mutations (`param = 4` for all barriers, `param = 5` per `temp_sX`)
  bool has_pinned_callee_saved = false;
  for (const auto& decl : toggleable_decls) {
    if (absl::StartsWith(decl.name, "temp_s") && decl.reg_annotation.has_value()) {
      has_pinned_callee_saved = true;
      if (function.SuppressedCseBarrierVars().count(decl.name) == 0) {
        context.Record(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, decl.site_index, 5);
      }
    }
  }
  if (has_pinned_callee_saved && !function.SuppressAllCseBarriers()) {
    context.Record(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, 0, 4);
  }

  // General register-pair swaps (`param = 10 + j`, including non-adjacent declarations) and
  // diff-guided direct hardware-register re-pinning (`param = 100 + reg_num` / `200 + fp_num`).
  const size_t max_swap_decl_index = std::min<size_t>(toggleable_decls.size(), 90);
  std::set<std::pair<uint32_t, uint32_t>> emitted_reg_toggle_steps;
  auto record_unique_reg_toggle = [&](uint32_t site, uint32_t param) {
    if (emitted_reg_toggle_steps.insert({site, param}).second) {
      context.Record(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION, site, param);
    }
  };

  for (size_t i = 0; i < max_swap_decl_index; ++i) {
    const auto& d1 = toggleable_decls[i];
    if (!d1.reg_annotation.has_value() || *d1.reg_annotation == "$30") continue;
    const bool d1_fp = IsFloatingPointRegisterAnnotation(*d1.reg_annotation);
    const bool d1_scratch = IsCallerSavedScratchRegisterAnnotation(*d1.reg_annotation);
    const bool d1_callee = absl::StartsWith(d1.name, "temp_s");
    for (size_t j = i + 1; j < max_swap_decl_index; ++j) {
      const auto& d2 = toggleable_decls[j];
      if (!d2.reg_annotation.has_value() || *d2.reg_annotation == "$30") continue;
      if (*d1.reg_annotation == *d2.reg_annotation) continue;
      if (d1_fp != IsFloatingPointRegisterAnnotation(*d2.reg_annotation)) continue;
      const bool d2_scratch = IsCallerSavedScratchRegisterAnnotation(*d2.reg_annotation);
      const bool d2_callee = absl::StartsWith(d2.name, "temp_s");
      bool diff_matched_pair = false;
      if (diff_diagnostic != nullptr) {
        for (const auto& [r1, r2] : diff_diagnostic->transposed_register_pairs) {
          if ((r1 == *d1.reg_annotation && r2 == *d2.reg_annotation) ||
              (r1 == *d2.reg_annotation && r2 == *d1.reg_annotation)) {
            diff_matched_pair = true;
            break;
          }
        }
      }
      if ((d1_scratch && d2_scratch) || (d1_callee && d2_callee) || diff_matched_pair) {
        record_unique_reg_toggle(static_cast<uint32_t>(i), static_cast<uint32_t>(10 + j));
      }
    }
  }

  if (diff_diagnostic != nullptr) {
    for (const auto& [reg_a, reg_b] : diff_diagnostic->transposed_register_pairs) {
      for (size_t i = 0; i < toggleable_decls.size(); ++i) {
        const auto& decl = toggleable_decls[i];
        if (!decl.reg_annotation.has_value()) continue;
        std::string_view peer_reg;
        if (*decl.reg_annotation == reg_a) {
          peer_reg = reg_b;
        } else if (*decl.reg_annotation == reg_b) {
          peer_reg = reg_a;
        }
        if (!peer_reg.empty() && IsFloatingPointRegisterAnnotation(*decl.reg_annotation) ==
                                     IsFloatingPointRegisterAnnotation(peer_reg)) {
          if (auto encoded = EncodeHardwareRegisterParam(peer_reg)) {
            record_unique_reg_toggle(decl.site_index, *encoded);
          }
        }
      }
    }
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
      if (function.ReturnType().name == "void" && function.ReturnType().pointer_depth == 0) {
        context.Record(AST_MUTATION_TOGGLE_TRAILING_RETURN, 0, 0);
      }
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

  // Discover split scratch web merge candidates (`temp_t0_2` -> `temp_t0`)
  auto split_webs = CollectSplitScratchWebCandidates(referenced_symbols);
  for (size_t i = 0; i < split_webs.size(); ++i) {
    context.Record(AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS, static_cast<uint32_t>(i), 0);
  }
  if (split_webs.size() > 1) {
    context.Record(AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS, 0, 1);
  }

  if (diff_diagnostic != nullptr || mutations.size() > max_mutations) {
    std::stable_sort(mutations.begin(), mutations.end(),
                     [&](const AstMutationStep& a, const AstMutationStep& b) {
                       int bonus_a = DiffGuidedPriorityBonus(a, toggleable_decls, diff_diagnostic);
                       int bonus_b = DiffGuidedPriorityBonus(b, toggleable_decls, diff_diagnostic);
                       if (bonus_a != bonus_b) return bonus_a < bonus_b;
                       if (mutations.size() > max_mutations) {
                         return MutationTruncationPriorityTier(a) <
                                MutationTruncationPriorityTier(b);
                       }
                       return false;
                     });
  }
  if (mutations.size() > max_mutations) {
    mutations.resize(max_mutations);
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
  // Preset 5: Local pointer-offset-to-array-index after bulk inline (hoist) without global array
  // promotion
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3),
      make_step(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  // Preset 6: Coalesce parameter copies without bulk temporary inlining
  presets.push_back({
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
    return BuildMutatedFunctionDeclaration(function, std::move(body));
  }

  if (step.kind() == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION) {
    if (step.param() == 2 || step.param() == 3) {
      auto mutated_body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
          StripRegisterAnnotationsInStatement(function.Body(), /*scratch_only=*/step.param() == 2)
              .release()));
      return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
    }
    if (step.param() == 4) {
      auto cloned_body = std::unique_ptr<CompoundStatement>(
          static_cast<CompoundStatement*>(function.Body().Clone().release()));
      FunctionDeclaration fn = BuildMutatedFunctionDeclaration(function, std::move(cloned_body));
      fn.SetSuppressAllCseBarriers(!function.SuppressAllCseBarriers());
      return fn;
    }
    if (step.param() == 5) {
      std::vector<ToggleableVarDeclInfo> decls;
      CollectToggleableVarDeclsInStatement(function.Body(), &decls);
      auto cloned_body = std::unique_ptr<CompoundStatement>(
          static_cast<CompoundStatement*>(function.Body().Clone().release()));
      FunctionDeclaration fn = BuildMutatedFunctionDeclaration(function, std::move(cloned_body));
      if (step.site_index() < decls.size()) {
        const std::string& var_name = decls[step.site_index()].name;
        if (fn.SuppressedCseBarrierVars().count(var_name) > 0) {
          fn.MutableSuppressedCseBarrierVars().erase(var_name);
        } else {
          fn.MutableSuppressedCseBarrierVars().insert(var_name);
        }
      }
      return fn;
    }
    if (step.param() >= 10 && step.param() < 100) {
      std::vector<ToggleableVarDeclInfo> decls;
      CollectToggleableVarDeclsInStatement(function.Body(), &decls);
      uint32_t site_i = step.site_index();
      uint32_t site_j = step.param() - 10;
      if (site_i < decls.size() && site_j < decls.size() && site_i != site_j &&
          decls[site_i].reg_annotation.has_value() && decls[site_j].reg_annotation.has_value() &&
          *decls[site_i].reg_annotation != "$30" && *decls[site_j].reg_annotation != "$30" &&
          IsFloatingPointRegisterAnnotation(*decls[site_i].reg_annotation) ==
              IsFloatingPointRegisterAnnotation(*decls[site_j].reg_annotation)) {
        std::map<uint32_t, std::optional<std::string>> new_annotations;
        new_annotations[site_i] = decls[site_j].reg_annotation;
        new_annotations[site_j] = decls[site_i].reg_annotation;
        uint32_t next_site = 0;
        auto mutated_body = std::unique_ptr<CompoundStatement>(
            static_cast<CompoundStatement*>(RewriteToggleableVarDeclAnnotationsInStatement(
                                                function.Body(), &next_site, new_annotations)
                                                .release()));
        return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
      }
      return function.Clone();
    }
    if (step.param() >= 100 && step.param() < 300) {
      std::vector<ToggleableVarDeclInfo> decls;
      CollectToggleableVarDeclsInStatement(function.Body(), &decls);
      uint32_t site_i = step.site_index();
      auto target_reg = DecodeHardwareRegisterParam(step.param());
      if (site_i < decls.size() && target_reg.has_value() && *target_reg != "$30") {
        std::map<uint32_t, std::optional<std::string>> new_annotations;
        std::optional<std::string> prev_reg = decls[site_i].reg_annotation;
        new_annotations[site_i] = *target_reg;
        for (const auto& other : decls) {
          if (other.site_index != site_i && other.reg_annotation == *target_reg) {
            new_annotations[other.site_index] = prev_reg;
          }
        }
        uint32_t next_site = 0;
        auto mutated_body = std::unique_ptr<CompoundStatement>(
            static_cast<CompoundStatement*>(RewriteToggleableVarDeclAnnotationsInStatement(
                                                function.Body(), &next_site, new_annotations)
                                                .release()));
        return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
      }
      return function.Clone();
    }
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
    return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
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
    return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
  }

  if (step.kind() == AST_MUTATION_COALESCE_PARAM_COPY) {
    FunctionDeclaration current = function.Clone();
    if (step.param() == 1) {
      for (size_t iteration = 0; iteration < 8; ++iteration) {
        auto candidates = CollectParameterCopyCandidates(current);
        if (candidates.empty()) break;
        auto body = ApplyCoalesceParameterCopy(current, candidates[0]);
        current = BuildMutatedFunctionDeclaration(current, std::move(body));
      }
      return current;
    }
    auto candidates = CollectParameterCopyCandidates(current);
    if (step.site_index() >= candidates.size()) return current;
    auto body = ApplyCoalesceParameterCopy(current, candidates[step.site_index()]);
    return BuildMutatedFunctionDeclaration(current, std::move(body));
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
    if (!stmts.empty() && stmts.back()->Kind() == CStatementKind::kReturnStatement) {
      const auto& ret = static_cast<const ReturnStatement&>(*stmts.back());
      const bool is_void_return =
          function.ReturnType().name == "void" && function.ReturnType().pointer_depth == 0;
      if (ret.ReturnValue() != nullptr &&
          ret.ReturnValue()->Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(*ret.ReturnValue()).Name() == "v0" &&
          (!is_void_return || step.param() == 1)) {
        return function.Clone();
      }
    }
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
    return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
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
    return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
  }

  if (step.kind() == AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS) {
    auto candidates = CollectSplitScratchWebCandidates(referenced_symbols);
    std::vector<SplitScratchWebCandidate> selected;
    if (step.param() == 1) {
      selected = candidates;
    } else if (step.site_index() < candidates.size()) {
      selected.push_back(candidates[step.site_index()]);
    }
    if (selected.empty()) return function.Clone();
    std::unique_ptr<CStatement> current_stmt = function.Body().Clone();
    for (const auto& cand : selected) {
      current_stmt = ReplaceIdentifierInStatement(*current_stmt, cand.split_name,
                                                  *CExpression::Identifier(cand.base_name),
                                                  /*replace_assignment_lhs=*/true);
    }
    auto mutated_body =
        std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(current_stmt.release()));
    mutated_body = PruneUnusedLocalDeclarations(*mutated_body);
    return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
  }

  MutationContext context;
  context.step_to_apply = &step;
  context.local_variables = referenced_symbols.local_variables;
  CollectActiveRegisterAnnotationsInStatement(function.Body(),
                                              &context.active_register_annotations);
  context.root_body = &function.Body();
  auto mutated_body = TraverseCompound(function.Body(), &context, /*is_top_level_block=*/true);
  if (step.kind() == AST_MUTATION_INLINE_SINGLE_USE_TEMP ||
      step.kind() == AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN ||
      step.kind() == AST_MUTATION_PROPAGATE_REGISTER_COPY ||
      step.kind() == AST_MUTATION_HOIST_WHILE_GUARD) {
    mutated_body = PruneUnusedLocalDeclarations(*mutated_body);
  }
  return BuildMutatedFunctionDeclaration(function, std::move(mutated_body));
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
      if (step.param() == 0) {
        return absl::StrFormat("ReorderLocalDecls(site=%u)", step.site_index());
      }
      return absl::StrFormat("ReorderLocalDecls(site=%u, param=%u)", step.site_index(),
                             step.param());
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
    case AST_MUTATION_TOGGLE_REGISTER_ANNOTATION:
      return absl::StrFormat("ToggleRegisterAnnotation(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS:
      return absl::StrFormat("MergeSplitScratchWebs(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_TOGGLE_STORE_FLAG_CONDITIONAL:
      return absl::StrFormat("ToggleStoreFlagConditional(site=%u, param=%u)", step.site_index(),
                             step.param());
    case AST_MUTATION_HOIST_LOOP_INVARIANT:
      return absl::StrFormat("HoistLoopInvariant(site=%u, param=%u)", step.site_index(),
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
