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
#include "core/c_ast_visitor.h"

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
  class PurityVisitor : public AstVisitor {
   public:
    bool is_pure = true;

    void VisitUnaryExpression(const UnaryExpression& expr) override {
      if (expr.Op() == "++" || expr.Op() == "--") {
        is_pure = false;
        Stop();
        return;
      }
      AstVisitor::VisitUnaryExpression(expr);
    }

    void VisitAssignmentExpression(const AssignmentExpression& /*expr*/) override {
      is_pure = false;
      Stop();
    }

    void VisitCallExpression(const CallExpression& /*expr*/) override {
      is_pure = false;
      Stop();
    }
  };

  PurityVisitor visitor;
  visitor.VisitExpression(expression);
  return visitor.is_pure;
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

namespace {

class ContainsCallVisitor : public AstVisitor {
 public:
  explicit ContainsCallVisitor(bool ignore_math_intrinsics)
      : ignore_math_intrinsics_(ignore_math_intrinsics) {}

  bool found = false;

  void VisitCallExpression(const CallExpression& expr) override {
    if (!ignore_math_intrinsics_ || !IsInlineAsmIntrinsicCall(expr)) {
      found = true;
      Stop();
      return;
    }
    AstVisitor::VisitCallExpression(expr);
  }

 private:
  bool ignore_math_intrinsics_;
};

class CountIdentifierOccurrencesVisitor : public AstVisitor {
 public:
  explicit CountIdentifierOccurrencesVisitor(std::string_view name) : name_(name) {}

  int total = 0;

  void VisitVariableDeclarationStatement(const VariableDeclarationStatement& stmt) override {
    if (stmt.Name() == name_) ++total;
    AstVisitor::VisitVariableDeclarationStatement(stmt);
  }

  void VisitIdentifierExpression(const IdentifierExpression& expr) override {
    const std::string& identifier_name = expr.Name();
    if (identifier_name == name_) {
      ++total;
      return;
    }
    std::string_view prefix;
    std::string_view inner_identifier;
    if (ExtractFunctionPointerCastCalleeIdentifier(identifier_name, &prefix, &inner_identifier) &&
        inner_identifier == name_) {
      ++total;
    }
  }

 private:
  std::string_view name_;
};

class VariableReferenceVisitor : public AstVisitor {
 public:
  explicit VariableReferenceVisitor(std::string_view target_var) : target_var_(target_var) {}

  bool found = false;

  void VisitIdentifierExpression(const IdentifierExpression& expr) override {
    if (expr.Name() == target_var_) {
      found = true;
      Stop();
    }
  }

 private:
  std::string_view target_var_;
};

class TakesAddressOfVisitor : public AstVisitor {
 public:
  TakesAddressOfVisitor(std::string_view target_var, bool ignore_assigned_float_bitcast_lhs)
      : target_var_(target_var),
        ignore_assigned_float_bitcast_lhs_(ignore_assigned_float_bitcast_lhs) {}

  bool found = false;

  void VisitUnaryExpression(const UnaryExpression& expr) override {
    if (expr.IsPrefix() &&
        (expr.Op() == "&" || absl::EndsWith(expr.Op(), "*&") ||
         expr.Op().find('&') != std::string::npos) &&
        expr.Operand().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(expr.Operand()).Name() == target_var_) {
      found = true;
      Stop();
      return;
    }
    AstVisitor::VisitUnaryExpression(expr);
  }

  void VisitAssignmentExpression(const AssignmentExpression& expr) override {
    if (!ignore_assigned_float_bitcast_lhs_ || expr.Op() != "=" ||
        ExtractAssignedLhsVar(expr.Lhs()).empty()) {
      VisitExpression(expr.Lhs());
      if (ShouldStop()) return;
    }
    VisitExpression(expr.Rhs());
  }

 private:
  std::string_view target_var_;
  bool ignore_assigned_float_bitcast_lhs_;
};

class CountVariableReadUsesVisitor : public AstVisitor {
 public:
  explicit CountVariableReadUsesVisitor(std::string_view var) : var_(var) {}

  int total = 0;

  void VisitVariableDeclarationStatement(const VariableDeclarationStatement& /*stmt*/) override {
    // Historically CountVariableReadUsesInStatement ignores VariableDeclarationStatement.
  }

  void VisitIdentifierExpression(const IdentifierExpression& expr) override {
    const std::string& id_name = expr.Name();
    if (id_name == var_) {
      ++total;
      return;
    }
    if (absl::StartsWith(id_name, "((") && absl::EndsWith(id_name, ")")) {
      std::string suffix = absl::StrCat(")", var_, ")");
      if (absl::EndsWith(id_name, suffix)) ++total;
    }
  }

  void VisitAssignmentExpression(const AssignmentExpression& expr) override {
    if (expr.Op() != "=" || ExtractAssignedLhsVar(expr.Lhs()).empty()) {
      VisitExpression(expr.Lhs());
    }
    VisitExpression(expr.Rhs());
  }

 private:
  std::string_view var_;
};

class ModifiesVariableVisitor : public AstVisitor {
 public:
  explicit ModifiesVariableVisitor(std::string_view target_var) : target_var_(target_var) {}

  bool found = false;

  void VisitAssignmentExpression(const AssignmentExpression& expr) override {
    if (expr.Lhs().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(expr.Lhs()).Name() == target_var_) {
      found = true;
      Stop();
      return;
    }
    AstVisitor::VisitAssignmentExpression(expr);
  }

  void VisitUnaryExpression(const UnaryExpression& expr) override {
    if ((expr.Op() == "++" || expr.Op() == "--") &&
        expr.Operand().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(expr.Operand()).Name() == target_var_) {
      found = true;
      Stop();
      return;
    }
    AstVisitor::VisitUnaryExpression(expr);
  }

 private:
  std::string_view target_var_;
};

class CollectUsedVariablesVisitor : public AstVisitor {
 public:
  CollectUsedVariablesVisitor(std::vector<std::string>* vars,
                              absl::flat_hash_set<std::string>* seen)
      : vars_(vars), seen_(seen) {}

  void VisitVariableDeclarationStatement(const VariableDeclarationStatement& /*stmt*/) override {
    // CollectUsedVariables only inspects non-declaration statements.
  }

  void VisitIdentifierExpression(const IdentifierExpression& expr) override {
    std::string name = expr.Name();
    if (absl::StartsWith(name, "((") && absl::EndsWith(name, ")") && name.size() > 4) {
      size_t inner_close = name.rfind(')', name.size() - 2);
      if (inner_close != std::string::npos && inner_close + 1 < name.size() - 1) {
        name = name.substr(inner_close + 1, name.size() - inner_close - 2);
      }
    }
    if (!name.empty() && seen_->insert(name).second) {
      vars_->push_back(name);
    }
  }

  void VisitCallExpression(const CallExpression& expr) override {
    if (expr.Callee().Kind() != CExpressionKind::kIdentifier ||
        absl::StartsWith(static_cast<const IdentifierExpression&>(expr.Callee()).Name(), "((")) {
      VisitExpression(expr.Callee());
    }
    for (const auto& arg : expr.Arguments()) {
      if (arg != nullptr) VisitExpression(*arg);
    }
  }

 private:
  std::vector<std::string>* vars_;
  absl::flat_hash_set<std::string>* seen_;
};

class CollectIdentifiersVisitor : public AstVisitor {
 public:
  explicit CollectIdentifiersVisitor(std::set<std::string>* out) : out_(out) {}

  void VisitIdentifierExpression(const IdentifierExpression& expr) override {
    const std::string& identifier_name = expr.Name();
    std::string_view prefix;
    std::string_view inner_identifier;
    if (ExtractFunctionPointerCastCalleeIdentifier(identifier_name, &prefix, &inner_identifier)) {
      out_->insert(std::string(inner_identifier));
    } else {
      out_->insert(identifier_name);
    }
  }

  void VisitCallExpression(const CallExpression& expr) override {
    std::string_view prefix;
    std::string_view inner_identifier;
    if (expr.Callee().Kind() == CExpressionKind::kIdentifier &&
        ExtractFunctionPointerCastCalleeIdentifier(
            static_cast<const IdentifierExpression&>(expr.Callee()).Name(), &prefix,
            &inner_identifier)) {
      out_->insert(std::string(inner_identifier));
    }
    for (const auto& argument : expr.Arguments()) {
      if (argument != nullptr) VisitExpression(*argument);
    }
  }

 private:
  std::set<std::string>* out_;
};

}  // namespace

bool ExpressionContainsCall(const CExpression& expr, bool ignore_math_intrinsics) {
  ContainsCallVisitor visitor(ignore_math_intrinsics);
  visitor.VisitExpression(expr);
  return visitor.found;
}

bool StatementContainsCall(const CStatement& stmt, bool ignore_math_intrinsics) {
  ContainsCallVisitor visitor(ignore_math_intrinsics);
  visitor.VisitStatement(stmt);
  return visitor.found;
}

int CountIdentifierOccurrencesInExpression(const CExpression& expression, std::string_view name) {
  CountIdentifierOccurrencesVisitor visitor(name);
  visitor.VisitExpression(expression);
  return visitor.total;
}

int CountIdentifierOccurrencesInStatement(const CStatement& statement, std::string_view name) {
  CountIdentifierOccurrencesVisitor visitor(name);
  visitor.VisitStatement(statement);
  return visitor.total;
}

bool ExpressionUsesVariable(const CExpression& expr, std::string_view target_var) {
  VariableReferenceVisitor visitor(target_var);
  visitor.VisitExpression(expr);
  return visitor.found;
}

bool StatementReferencesVariable(const CStatement& stmt, std::string_view target_var) {
  VariableReferenceVisitor visitor(target_var);
  visitor.VisitStatement(stmt);
  return visitor.found;
}

bool ExpressionTakesAddressOf(const CExpression& expr, std::string_view target_var,
                              bool ignore_assigned_float_bitcast_lhs) {
  TakesAddressOfVisitor visitor(target_var, ignore_assigned_float_bitcast_lhs);
  visitor.VisitExpression(expr);
  return visitor.found;
}

bool StatementTakesAddressOf(const CStatement& stmt, std::string_view target_var,
                             bool ignore_assigned_float_bitcast_lhs) {
  TakesAddressOfVisitor visitor(target_var, ignore_assigned_float_bitcast_lhs);
  visitor.VisitStatement(stmt);
  return visitor.found;
}

std::string_view ExtractFloatBitcastLhsVar(const CExpression& lhs) {
  if (lhs.Kind() != CExpressionKind::kUnaryExpression) return {};
  const auto& deref = static_cast<const UnaryExpression&>(lhs);
  if (deref.Op() != "*" || !deref.IsPrefix()) return {};
  if (deref.Operand().Kind() != CExpressionKind::kCastExpression) return {};
  const auto& cast = static_cast<const CastExpression&>(deref.Operand());
  if ((cast.TargetType().name != "f32" && cast.TargetType().name != "f64") ||
      cast.TargetType().pointer_depth != 1) {
    return {};
  }
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
  CountVariableReadUsesVisitor visitor(var);
  visitor.VisitExpression(expression);
  return visitor.total;
}

int CountVariableReadUsesInStatement(const CStatement& statement, std::string_view var) {
  CountVariableReadUsesVisitor visitor(var);
  visitor.VisitStatement(statement);
  return visitor.total;
}

bool StatementContainsLabel(const CStatement& stmt) {
  class LabelVisitor : public AstVisitor {
   public:
    bool found = false;
    void VisitLabelStatement(const LabelStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
  };
  LabelVisitor visitor;
  visitor.VisitStatement(stmt);
  return visitor.found;
}

bool StatementContainsControlFlowJump(const CStatement& stmt) {
  class ControlFlowJumpVisitor : public AstVisitor {
   public:
    bool found = false;
    void VisitBreakStatement(const BreakStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
    void VisitContinueStatement(const ContinueStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
    void VisitGotoStatement(const GotoStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
    void VisitLabelStatement(const LabelStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
  };
  ControlFlowJumpVisitor visitor;
  visitor.VisitStatement(stmt);
  return visitor.found;
}

bool StatementContainsLoop(const CStatement& stmt) {
  class LoopVisitor : public AstVisitor {
   public:
    bool found = false;
    void VisitWhileStatement(const WhileStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
    void VisitDoWhileStatement(const DoWhileStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
    void VisitForStatement(const ForStatement& /*stmt*/) override {
      found = true;
      Stop();
    }
  };
  LoopVisitor visitor;
  visitor.VisitStatement(stmt);
  return visitor.found;
}

bool ExpressionModifiesVariable(const CExpression& expr, std::string_view target_var) {
  ModifiesVariableVisitor visitor(target_var);
  visitor.VisitExpression(expr);
  return visitor.found;
}

bool StatementModifiesVariable(const CStatement& stmt, std::string_view target_var) {
  ModifiesVariableVisitor visitor(target_var);
  visitor.VisitStatement(stmt);
  return visitor.found;
}

void CollectVariablesInExpression(const CExpression& expr, std::vector<std::string>* vars,
                                  absl::flat_hash_set<std::string>* seen) {
  CollectUsedVariablesVisitor visitor(vars, seen);
  visitor.VisitExpression(expr);
}

void CollectUsedVariables(const CStatement& stmt, std::vector<std::string>* vars,
                          absl::flat_hash_set<std::string>* seen) {
  CollectUsedVariablesVisitor visitor(vars, seen);
  visitor.VisitStatement(stmt);
}

void CollectIdentifiersInExpression(const CExpression& expression, std::set<std::string>* out) {
  CollectIdentifiersVisitor visitor(out);
  visitor.VisitExpression(expression);
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
    auto new_op = SubstituteIdentifier(cast.Operand(), target, replacement);
    if (cast.TargetType().name == "f32" && cast.TargetType().pointer_depth == 0 &&
        new_op->Kind() == CExpressionKind::kCastExpression) {
      const auto& inner_cast = static_cast<const CastExpression&>(*new_op);
      if (inner_cast.TargetType().name == "f64" && inner_cast.TargetType().pointer_depth == 0 &&
          inner_cast.Operand().Kind() == CExpressionKind::kCastExpression) {
        const auto& u32_cast = static_cast<const CastExpression&>(inner_cast.Operand());
        if (u32_cast.TargetType().name == "u32" && u32_cast.TargetType().pointer_depth == 0) {
          return CExpression::Cast(cast.TargetType(), inner_cast.Operand().Clone());
        }
      }
    }
    return CExpression::Cast(cast.TargetType(), std::move(new_op));
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
  class SubstituteStatementMutator : public AstMutator {
   public:
    SubstituteStatementMutator(std::string_view from, std::string_view to)
        : from_(from), to_id_(CExpression::Identifier(std::string(to))) {}

    void MutateVariableDeclarationStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                            VariableDeclarationStatement* /*decl_stmt*/) override {
      // Historically SubstituteIdentifierInStatement leaves VariableDeclarationStatement untouched.
    }

    void MutateExpression(std::unique_ptr<CExpression>* expr) override {
      if (expr == nullptr || *expr == nullptr) return;
      *expr = SubstituteIdentifier(**expr, from_, *to_id_);
      MarkChanged();
    }

   private:
    std::string_view from_;
    std::unique_ptr<CExpression> to_id_;
  };

  SubstituteStatementMutator mutator(from, to);
  mutator.MutateStatement(&stmt);
  return stmt;
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

namespace {

bool IsStackParamOrLocalNameHelper(std::string_view id_name) {
  if (absl::StartsWith(id_name, "var_sp_")) return true;
  if (id_name.size() >= 4 && absl::StartsWith(id_name, "arg") && id_name != "arg0" &&
      id_name != "arg1" && id_name != "arg2" && id_name != "arg3") {
    return true;
  }
  return false;
}

bool HasMemoryDerefHelper(const CExpression& expr) {
  bool found = false;
  std::function<void(const CExpression&)> check = [&](const CExpression& e) {
    if (found) return;
    if (e.Kind() == CExpressionKind::kIdentifier) {
      std::string_view id_name = static_cast<const IdentifierExpression&>(e).Name();
      if (IsStackParamOrLocalNameHelper(id_name)) {
        found = true;
      }
      return;
    }
    if (e.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(e);
      if (un.Op().find('*') != std::string::npos) {
        if (un.Op() == "*(s32*)&" || un.Op() == "*(u32*)&" || un.Op() == "*(f32*)&" ||
            un.Op() == "*(s32 *)&" || un.Op() == "*(u32 *)&" || un.Op() == "*(f32 *)&") {
          if (un.Operand().Kind() == CExpressionKind::kIdentifier) {
            std::string_view id_name =
                static_cast<const IdentifierExpression&>(un.Operand()).Name();
            if (!IsStackParamOrLocalNameHelper(id_name) && !absl::StartsWith(id_name, "D_") &&
                !absl::StartsWith(id_name, "g_") && !absl::StartsWith(id_name, "RO_") &&
                !absl::StartsWith(id_name, "B_")) {
              return;
            }
          } else {
            check(un.Operand());
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
    } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
      check(static_cast<const AssignmentExpression&>(e).Lhs());
      check(static_cast<const AssignmentExpression&>(e).Rhs());
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

bool ExtractSimpleDerefRangeHelper(const CExpression& e, std::string_view* out_base,
                                   int64_t* out_offset, int64_t* out_size,
                                   bool* out_is_addr_of = nullptr, bool allow_ident_deref = false) {
  if (out_is_addr_of != nullptr) *out_is_addr_of = false;
  if (allow_ident_deref && e.Kind() == CExpressionKind::kIdentifier) {
    std::string_view id_name = static_cast<const IdentifierExpression&>(e).Name();
    if (IsStackParamOrLocalNameHelper(id_name) || absl::StartsWith(id_name, "D_") ||
        absl::StartsWith(id_name, "g_") || absl::StartsWith(id_name, "s_") ||
        absl::StartsWith(id_name, "k_") || absl::StartsWith(id_name, "RO_") ||
        absl::StartsWith(id_name, "B_")) {
      *out_base = id_name;
      *out_offset = 0;
      *out_size = absl::EndsWith(id_name, "_64") ? 8 : 4;
      if (out_is_addr_of != nullptr) *out_is_addr_of = true;
      return true;
    }
    return false;
  }
  if (e.Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& un = static_cast<const UnaryExpression&>(e);
  int64_t size = 4;
  const CExpression* addr_root = nullptr;
  if (un.Op() == "*" && un.Operand().Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    std::string ts = cast.TargetType().ToString();
    if (ts == "s8*" || ts == "u8*" || ts == "s8 *" || ts == "u8 *") {
      size = 1;
    } else if (ts == "s16*" || ts == "u16*" || ts == "s16 *" || ts == "u16 *") {
      size = 2;
    } else if (ts == "s64*" || ts == "u64*" || ts == "f64*" || ts == "s64 *" || ts == "u64 *" ||
               ts == "f64 *") {
      size = 8;
    }
    addr_root = &cast.Operand();
  } else if (un.Op() == "*(s8*)" || un.Op() == "*(u8*)" || un.Op() == "*(s8 *)" ||
             un.Op() == "*(u8 *)") {
    size = 1;
    addr_root = &un.Operand();
  } else if (un.Op() == "*(s16*)" || un.Op() == "*(u16*)" || un.Op() == "*(s16 *)" ||
             un.Op() == "*(u16 *)") {
    size = 2;
    addr_root = &un.Operand();
  } else if (un.Op() == "*(s32*)" || un.Op() == "*(u32*)" || un.Op() == "*(f32*)" ||
             un.Op() == "*(s32 *)" || un.Op() == "*(u32 *)" || un.Op() == "*(f32 *)") {
    size = 4;
    addr_root = &un.Operand();
  } else if (un.Op() == "*(s64*)" || un.Op() == "*(u64*)" || un.Op() == "*(f64*)" ||
             un.Op() == "*(s64 *)" || un.Op() == "*(u64 *)" || un.Op() == "*(f64 *)") {
    size = 8;
    addr_root = &un.Operand();
  } else {
    return false;
  }
  const CExpression* addr_ptr = addr_root;
  while (addr_ptr->Kind() == CExpressionKind::kCastExpression) {
    addr_ptr = &static_cast<const CastExpression&>(*addr_ptr).Operand();
  }
  const CExpression& addr = *addr_ptr;
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
      const CExpression* lhs_ptr = &bin.Lhs();
      while (lhs_ptr->Kind() == CExpressionKind::kCastExpression) {
        lhs_ptr = &static_cast<const CastExpression&>(*lhs_ptr).Operand();
      }
      const CExpression& lhs = *lhs_ptr;
      if (lhs.Kind() == CExpressionKind::kIdentifier) {
        *out_base = static_cast<const IdentifierExpression&>(lhs).Name();
        *out_offset = (bin.Op() == "-") ? -off : off;
        *out_size = size;
        return true;
      }
      if (lhs.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& lhs_un = static_cast<const UnaryExpression&>(lhs);
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

bool AreProvablyDisjointDerefsHelper(const CExpression& a, const CExpression& b,
                                     bool allow_addr_of_vs_ptr = false) {
  std::string_view base_a;
  std::string_view base_b;
  int64_t off_a = 0;
  int64_t off_b = 0;
  int64_t size_a = 0;
  int64_t size_b = 0;
  bool addr_of_a = false;
  bool addr_of_b = false;
  if (!ExtractSimpleDerefRangeHelper(a, &base_a, &off_a, &size_a, &addr_of_a,
                                     allow_addr_of_vs_ptr) ||
      !ExtractSimpleDerefRangeHelper(b, &base_b, &off_b, &size_b, &addr_of_b,
                                     allow_addr_of_vs_ptr)) {
    return false;
  }
  if (base_a == base_b) {
    return (off_a + size_a <= off_b) || (off_b + size_b <= off_a);
  }
  const bool is_stack_a = IsStackParamOrLocalNameHelper(base_a) || base_a == "sp";
  const bool is_stack_b = IsStackParamOrLocalNameHelper(base_b) || base_b == "sp";
  if ((is_stack_a && addr_of_b && !is_stack_b) || (is_stack_b && addr_of_a && !is_stack_a)) {
    return true;
  }
  if (addr_of_a && addr_of_b) {
    return true;
  }
  if ((addr_of_a != addr_of_b) && (size_a != size_b || allow_addr_of_vs_ptr)) {
    return true;
  }
  return false;
}

bool AreAllDerefsProvablyDisjointHelper(const CExpression& expr, const CExpression& store_lhs,
                                        bool allow_addr_of_vs_ptr = false) {
  bool found_deref = false;
  bool all_disjoint = true;
  std::function<void(const CExpression&)> walk = [&](const CExpression& e) {
    if (!all_disjoint) return;
    if (e.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(e);
      if (un.Op() == "*(s32*)&" || un.Op() == "*(u32*)&" || un.Op() == "*(f32*)&" ||
          un.Op() == "*(s32 *)&" || un.Op() == "*(u32 *)&" || un.Op() == "*(f32 *)&" ||
          un.Op() == "*(s64*)&" || un.Op() == "*(u64*)&" || un.Op() == "*(f64*)&") {
        if (un.Operand().Kind() == CExpressionKind::kIdentifier) {
          std::string_view id_name = static_cast<const IdentifierExpression&>(un.Operand()).Name();
          if (!IsStackParamOrLocalNameHelper(id_name) && !absl::StartsWith(id_name, "D_") &&
              !absl::StartsWith(id_name, "g_") && !absl::StartsWith(id_name, "RO_") &&
              !absl::StartsWith(id_name, "B_")) {
            return;
          }
          if (!allow_addr_of_vs_ptr) {
            all_disjoint = false;
            return;
          }
          found_deref = true;
          if (!AreProvablyDisjointDerefsHelper(un.Operand(), store_lhs, allow_addr_of_vs_ptr)) {
            all_disjoint = false;
          }
          return;
        }
        walk(un.Operand());
        return;
      }
      if (un.Op() == "*" || absl::StartsWith(un.Op(), "*(s") || absl::StartsWith(un.Op(), "*(u") ||
          absl::StartsWith(un.Op(), "*(f")) {
        found_deref = true;
        if (!AreProvablyDisjointDerefsHelper(e, store_lhs, allow_addr_of_vs_ptr)) {
          all_disjoint = false;
        }
        return;
      }
      walk(un.Operand());
    } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(e);
      walk(bin.Lhs());
      walk(bin.Rhs());
    } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& assign = static_cast<const AssignmentExpression&>(e);
      walk(assign.Lhs());
      walk(assign.Rhs());
    } else if (e.Kind() == CExpressionKind::kCastExpression) {
      walk(static_cast<const CastExpression&>(e).Operand());
    } else if (e.Kind() == CExpressionKind::kIdentifier) {
      std::string_view id_name = static_cast<const IdentifierExpression&>(e).Name();
      if (allow_addr_of_vs_ptr && IsStackParamOrLocalNameHelper(id_name)) {
        found_deref = true;
        if (!AreProvablyDisjointDerefsHelper(e, store_lhs, allow_addr_of_vs_ptr)) {
          all_disjoint = false;
        }
      }
      return;
    } else if (e.Kind() == CExpressionKind::kIntegerLiteral ||
               e.Kind() == CExpressionKind::kFloatLiteral) {
      return;
    } else {
      all_disjoint = false;
    }
  };
  walk(expr);
  return found_deref && all_disjoint;
}

}  // namespace

bool MayAliasMemoryExpressions(const CExpression& load_expr, const CExpression& store_lhs) {
  if (!HasMemoryDerefHelper(load_expr)) {
    return false;
  }
  return !AreAllDerefsProvablyDisjointHelper(load_expr, store_lhs, /*allow_addr_of_vs_ptr=*/true);
}

}  // namespace rom_nom_nom
