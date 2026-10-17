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

bool IsPureExpression(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kIntegerLiteral:
    case CExpressionKind::kFloatLiteral:
    case CExpressionKind::kStringLiteral:
    case CExpressionKind::kIdentifier:
      return true;
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.Op() == "++" || u.Op() == "--") return false;
      return IsPureExpression(u.Operand());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return IsPureExpression(b.Lhs()) && IsPureExpression(b.Rhs());
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return IsPureExpression(c.Operand());
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return IsPureExpression(m.Object());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      return IsPureExpression(a.Array()) && IsPureExpression(a.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return IsPureExpression(t.Condition()) && IsPureExpression(t.TrueExpression()) &&
             IsPureExpression(t.FalseExpression());
    }
    case CExpressionKind::kAssignmentExpression:
    case CExpressionKind::kCallExpression:
      return false;
  }
  return false;
}

std::optional<std::string> CommutedBinaryOp(std::string_view op) {
  if (op == "+" || op == "*" || op == "&" || op == "|" || op == "^" || op == "==" || op == "!=") {
    return std::string(op);
  }
  if (op == "<") return ">";
  if (op == ">") return "<";
  if (op == "<=") return ">=";
  if (op == ">=") return "<=";
  return std::nullopt;
}

bool IsCommutableBinary(const BinaryExpression& bin) {
  return CommutedBinaryOp(bin.Op()).has_value() && IsPureExpression(bin.Lhs()) &&
         IsPureExpression(bin.Rhs());
}

std::unique_ptr<CExpression> NegateCondition(const CExpression& cond) {
  if (cond.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& u = static_cast<const UnaryExpression&>(cond);
    if (u.IsPrefix() && u.Op() == "!") {
      return u.Operand().Clone();
    }
  }
  if (cond.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& b = static_cast<const BinaryExpression&>(cond);
    if (b.Op() == "==") return CExpression::Binary("!=", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == "!=") return CExpression::Binary("==", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == "<") return CExpression::Binary(">=", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == ">=") return CExpression::Binary("<", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == ">") return CExpression::Binary("<=", b.Lhs().Clone(), b.Rhs().Clone());
    if (b.Op() == "<=") return CExpression::Binary(">", b.Lhs().Clone(), b.Rhs().Clone());
  }
  return CExpression::Unary("!", cond.Clone(), /*is_prefix=*/true);
}

bool IsToggableCompoundAssignment(const AssignmentExpression& assign) {
  if (assign.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  const std::string& lhs_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  if (assign.Op() == "+=" || assign.Op() == "-=" || assign.Op() == "&=" || assign.Op() == "|=" ||
      assign.Op() == "^=") {
    return true;
  }
  if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
    if ((bin.Op() == "+" || bin.Op() == "-" || bin.Op() == "&" || bin.Op() == "|" ||
         bin.Op() == "^") &&
        bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(bin.Lhs()).Name() == lhs_name) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<CExpression> ToggleCompoundAssignment(const AssignmentExpression& assign) {
  if (assign.Op() == "+=" || assign.Op() == "-=" || assign.Op() == "&=" || assign.Op() == "|=" ||
      assign.Op() == "^=") {
    std::string bin_op(assign.Op().substr(0, 1));
    return CExpression::Assignment(
        "=", assign.Lhs().Clone(),
        CExpression::Binary(bin_op, assign.Lhs().Clone(), assign.Rhs().Clone()));
  }
  const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
  std::string comp_op = bin.Op() + "=";
  return CExpression::Assignment(comp_op, assign.Lhs().Clone(), bin.Rhs().Clone());
}

bool IsToggableIntegerCast(const CastExpression& cast) {
  const auto& t = cast.TargetType();
  return t.pointer_depth == 0 && !t.is_const && (t.name == "s32" || t.name == "u32");
}

bool ExtractFuncPtrCastCalleeIdent(std::string_view text, std::string_view* out_prefix,
                                   std::string_view* out_ident) {
  if (text.size() < 10 || text.substr(0, 2) != "((" || text.back() != ')') return false;
  size_t pos = text.find("(*)())");
  if (pos == std::string_view::npos) return false;
  size_t ident_start = pos + 6;
  if (ident_start + 1 >= text.size()) return false;
  std::string_view ident = text.substr(ident_start, text.size() - ident_start - 1);
  for (char c : ident) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  if (out_prefix != nullptr) *out_prefix = text.substr(0, ident_start);
  if (out_ident != nullptr) *out_ident = ident;
  return true;
}

int CountIdentifierOccurrencesInExpr(const CExpression& expr, std::string_view name) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const std::string& id_name = static_cast<const IdentifierExpression&>(expr).Name();
      if (id_name == name) return 1;
      std::string_view prefix;
      std::string_view inner_id;
      if (ExtractFuncPtrCastCalleeIdent(id_name, &prefix, &inner_id) && inner_id == name) {
        return 1;
      }
      return 0;
    }
    case CExpressionKind::kUnaryExpression:
      return CountIdentifierOccurrencesInExpr(static_cast<const UnaryExpression&>(expr).Operand(),
                                              name);
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(b.Lhs(), name) +
             CountIdentifierOccurrencesInExpr(b.Rhs(), name);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(a.Lhs(), name) +
             CountIdentifierOccurrencesInExpr(a.Rhs(), name);
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      int total = CountIdentifierOccurrencesInExpr(c.Callee(), name);
      for (const auto& arg : c.Arguments()) {
        total += CountIdentifierOccurrencesInExpr(*arg, name);
      }
      return total;
    }
    case CExpressionKind::kCastExpression:
      return CountIdentifierOccurrencesInExpr(static_cast<const CastExpression&>(expr).Operand(),
                                              name);
    case CExpressionKind::kMemberAccessExpression:
      return CountIdentifierOccurrencesInExpr(
          static_cast<const MemberAccessExpression&>(expr).Object(), name);
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(arr.Array(), name) +
             CountIdentifierOccurrencesInExpr(arr.Index(), name);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CountIdentifierOccurrencesInExpr(t.Condition(), name) +
             CountIdentifierOccurrencesInExpr(t.TrueExpression(), name) +
             CountIdentifierOccurrencesInExpr(t.FalseExpression(), name);
    }
    default:
      return 0;
  }
}

int CountIdentifierOccurrencesInStmt(const CStatement& stmt, std::string_view name) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      int total = 0;
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        total += CountIdentifierOccurrencesInStmt(*s, name);
      }
      return total;
    }
    case CStatementKind::kExpressionStatement:
      return CountIdentifierOccurrencesInExpr(
          static_cast<const ExpressionStatement&>(stmt).Expression(), name);
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      int total = (vd.Name() == name) ? 1 : 0;
      if (vd.Initializer() != nullptr) {
        total += CountIdentifierOccurrencesInExpr(*vd.Initializer(), name);
      }
      return total;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      int total = CountIdentifierOccurrencesInExpr(ifs.Condition(), name) +
                  CountIdentifierOccurrencesInStmt(ifs.ThenBranch(), name);
      if (ifs.ElseBranch() != nullptr) {
        total += CountIdentifierOccurrencesInStmt(*ifs.ElseBranch(), name);
      }
      return total;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CountIdentifierOccurrencesInExpr(ws.Condition(), name) +
             CountIdentifierOccurrencesInStmt(ws.Body(), name);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CountIdentifierOccurrencesInStmt(dws.Body(), name) +
             CountIdentifierOccurrencesInExpr(dws.Condition(), name);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      int total = CountIdentifierOccurrencesInStmt(fs.Body(), name);
      if (fs.Init() != nullptr) {
        total += CountIdentifierOccurrencesInStmt(*fs.Init(), name);
      }
      if (fs.Condition() != nullptr) {
        total += CountIdentifierOccurrencesInExpr(*fs.Condition(), name);
      }
      if (fs.Step() != nullptr) {
        total += CountIdentifierOccurrencesInExpr(*fs.Step(), name);
      }
      return total;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr ? CountIdentifierOccurrencesInExpr(*rs.ReturnValue(), name)
                                         : 0;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      int total = CountIdentifierOccurrencesInExpr(ss.Condition(), name);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) total += CountIdentifierOccurrencesInStmt(*c.body, name);
      }
      return total;
    }
    default:
      return 0;
  }
}

void CollectIdentifiersInExpr(const CExpression& expr, std::set<std::string>* out) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const std::string& id_name = static_cast<const IdentifierExpression&>(expr).Name();
      std::string_view prefix;
      std::string_view inner_id;
      if (ExtractFuncPtrCastCalleeIdent(id_name, &prefix, &inner_id)) {
        out->insert(std::string(inner_id));
      } else {
        out->insert(id_name);
      }
      break;
    }
    case CExpressionKind::kUnaryExpression:
      CollectIdentifiersInExpr(static_cast<const UnaryExpression&>(expr).Operand(), out);
      break;
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      CollectIdentifiersInExpr(b.Lhs(), out);
      CollectIdentifiersInExpr(b.Rhs(), out);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      CollectIdentifiersInExpr(a.Lhs(), out);
      CollectIdentifiersInExpr(a.Rhs(), out);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::string_view prefix;
      std::string_view inner_id;
      if (c.Callee().Kind() == CExpressionKind::kIdentifier &&
          ExtractFuncPtrCastCalleeIdent(static_cast<const IdentifierExpression&>(c.Callee()).Name(),
                                        &prefix, &inner_id)) {
        out->insert(std::string(inner_id));
      }
      for (const auto& arg : c.Arguments()) {
        CollectIdentifiersInExpr(*arg, out);
      }
      break;
    }
    case CExpressionKind::kCastExpression:
      CollectIdentifiersInExpr(static_cast<const CastExpression&>(expr).Operand(), out);
      break;
    case CExpressionKind::kMemberAccessExpression:
      CollectIdentifiersInExpr(static_cast<const MemberAccessExpression&>(expr).Object(), out);
      break;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      CollectIdentifiersInExpr(arr.Array(), out);
      CollectIdentifiersInExpr(arr.Index(), out);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      CollectIdentifiersInExpr(t.Condition(), out);
      CollectIdentifiersInExpr(t.TrueExpression(), out);
      CollectIdentifiersInExpr(t.FalseExpression(), out);
      break;
    }
    default:
      break;
  }
}

// Matches `*(T *)&sym` where T is a scalar type (`u8`, `s8`, `u16`, `s16`, `u32`, `s32`, `f32`,
// `f64`).
bool ExtractGlobalCastTarget(const CExpression& expr, std::string* out_sym = nullptr,
                             CType* out_type = nullptr) {
  if (expr.Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& deref = static_cast<const UnaryExpression&>(expr);
  if (!deref.IsPrefix() || deref.Op() != "*") return false;
  if (deref.Operand().Kind() != CExpressionKind::kCastExpression) return false;
  const auto& cast = static_cast<const CastExpression&>(deref.Operand());
  const CType& ptr_type = cast.TargetType();
  if (ptr_type.pointer_depth != 1) return false;
  if (cast.Operand().Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& addr_of = static_cast<const UnaryExpression&>(cast.Operand());
  if (!addr_of.IsPrefix() || addr_of.Op() != "&") return false;
  if (addr_of.Operand().Kind() != CExpressionKind::kIdentifier) return false;
  const auto& id = static_cast<const IdentifierExpression&>(addr_of.Operand());
  if (out_sym != nullptr) *out_sym = id.Name();
  if (out_type != nullptr) {
    *out_type = CType{.name = ptr_type.name, .is_const = ptr_type.is_const, .pointer_depth = 0};
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
bool MatchPointerOffsetDereference(const CExpression& expr, CType* out_ptr_type = nullptr,
                                   const CExpression** out_base = nullptr,
                                   int64_t* out_elem_index = nullptr) {
  if (expr.Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& deref = static_cast<const UnaryExpression&>(expr);
  if (!deref.IsPrefix() || deref.Op() != "*") return false;
  if (deref.Operand().Kind() != CExpressionKind::kCastExpression) return false;
  const auto& cast = static_cast<const CastExpression&>(deref.Operand());
  const CType& ptr_type = cast.TargetType();
  if (ptr_type.pointer_depth != 1) return false;
  auto elem_size_opt = TypeByteSize(ptr_type.name);
  if (!elem_size_opt.has_value() || *elem_size_opt <= 0) return false;
  int64_t elem_size = *elem_size_opt;

  if (cast.Operand().Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& bin = static_cast<const BinaryExpression&>(cast.Operand());
  if (bin.Op() != "+" && bin.Op() != "-") return false;
  if (bin.Rhs().Kind() != CExpressionKind::kIntegerLiteral) return false;
  // Exclude `(u8 *)&sym + offset` so global byte-indexed array patterns stay handled by global
  // array mutations.
  if (bin.Lhs().Kind() == CExpressionKind::kCastExpression) {
    const auto& lhs_cast = static_cast<const CastExpression&>(bin.Lhs());
    if (lhs_cast.Operand().Kind() == CExpressionKind::kUnaryExpression &&
        static_cast<const UnaryExpression&>(lhs_cast.Operand()).Op() == "&") {
      return false;
    }
  }

  int64_t byte_offset = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
  if (bin.Op() == "-") byte_offset = -byte_offset;
  if (byte_offset % elem_size != 0) return false;

  if (out_ptr_type != nullptr) *out_ptr_type = ptr_type;
  if (out_base != nullptr) *out_base = &bin.Lhs();
  if (out_elem_index != nullptr) *out_elem_index = byte_offset / elem_size;
  return true;
}

bool ExprReadsIndirectMemory(const CExpression& expr) {
  std::string sym;
  if (ExtractGlobalCastTarget(expr, &sym)) {
    return false;
  }
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.IsPrefix() && u.Op() == "*") return true;
      if (u.IsPrefix() && u.Op() == "&") return false;
      return ExprReadsIndirectMemory(u.Operand());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      if (a.Array().Kind() == CExpressionKind::kIdentifier) {
        return ExprReadsIndirectMemory(a.Index());
      }
      return true;
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return ExprReadsIndirectMemory(b.Lhs()) || ExprReadsIndirectMemory(b.Rhs());
    }
    case CExpressionKind::kCastExpression:
      return ExprReadsIndirectMemory(static_cast<const CastExpression&>(expr).Operand());
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return m.IsArrow() || ExprReadsIndirectMemory(m.Object());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExprReadsIndirectMemory(t.Condition()) ||
             ExprReadsIndirectMemory(t.TrueExpression()) ||
             ExprReadsIndirectMemory(t.FalseExpression());
    }
    default:
      return false;
  }
}

bool ExtractLhsTargetIdentifier(const CExpression& lhs, std::string* out_name,
                                bool* out_is_indirect_mem) {
  if (out_is_indirect_mem != nullptr) *out_is_indirect_mem = false;
  if (lhs.Kind() == CExpressionKind::kIdentifier) {
    if (out_name != nullptr) *out_name = static_cast<const IdentifierExpression&>(lhs).Name();
    return true;
  }
  std::string sym;
  if (ExtractGlobalCastTarget(lhs, &sym)) {
    if (out_name != nullptr) *out_name = sym;
    return true;
  }
  if (lhs.Kind() == CExpressionKind::kArrayIndexExpression) {
    const auto& arr = static_cast<const ArrayIndexExpression&>(lhs);
    if (arr.Array().Kind() == CExpressionKind::kIdentifier &&
        !ExprReadsIndirectMemory(arr.Index())) {
      if (out_name != nullptr) {
        *out_name = static_cast<const IdentifierExpression&>(arr.Array()).Name();
      }
      return true;
    }
  }
  if (out_is_indirect_mem != nullptr) *out_is_indirect_mem = true;
  return false;
}

bool StmtContainsJumpOrLabel(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kGotoStatement:
    case CStatementKind::kLabelStatement:
    case CStatementKind::kBreakStatement:
    case CStatementKind::kContinueStatement:
    case CStatementKind::kReturnStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (StmtContainsJumpOrLabel(*s)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StmtContainsJumpOrLabel(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StmtContainsJumpOrLabel(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement:
      return StmtContainsJumpOrLabel(static_cast<const WhileStatement&>(stmt).Body());
    case CStatementKind::kDoWhileStatement:
      return StmtContainsJumpOrLabel(static_cast<const DoWhileStatement&>(stmt).Body());
    case CStatementKind::kForStatement:
      return StmtContainsJumpOrLabel(static_cast<const ForStatement&>(stmt).Body());
    case CStatementKind::kSwitchStatement:
      return true;
    default:
      return false;
  }
}

bool StmtContainsGotoOrLabel(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kGotoStatement:
    case CStatementKind::kLabelStatement:
      return true;
    case CStatementKind::kCompoundStatement: {
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (StmtContainsGotoOrLabel(*s)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StmtContainsGotoOrLabel(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StmtContainsGotoOrLabel(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement:
      return StmtContainsGotoOrLabel(static_cast<const WhileStatement&>(stmt).Body());
    case CStatementKind::kDoWhileStatement:
      return StmtContainsGotoOrLabel(static_cast<const DoWhileStatement&>(stmt).Body());
    case CStatementKind::kForStatement:
      return StmtContainsGotoOrLabel(static_cast<const ForStatement&>(stmt).Body());
    case CStatementKind::kSwitchStatement: {
      for (const auto& c : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (c.body != nullptr && StmtContainsGotoOrLabel(*c.body)) return true;
      }
      return false;
    }
    default:
      return false;
  }
}

int CountNonDeclOccurrencesInStmt(const CStatement& stmt, std::string_view name) {
  if (stmt.Kind() == CStatementKind::kVariableDeclarationStatement) {
    const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
    return vd.Initializer() != nullptr ? CountIdentifierOccurrencesInExpr(*vd.Initializer(), name)
                                       : 0;
  }
  if (stmt.Kind() == CStatementKind::kCompoundStatement) {
    int total = 0;
    for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
      total += CountNonDeclOccurrencesInStmt(*s, name);
    }
    return total;
  }
  return CountIdentifierOccurrencesInStmt(stmt, name);
}

bool IsUnconditionalOverwriteOfVar(const CStatement& stmt, std::string_view var_name) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  return assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
         static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == var_name &&
         CountIdentifierOccurrencesInExpr(assign.Rhs(), var_name) == 0;
}

bool IsVarDeadAfter(const std::vector<std::unique_ptr<CStatement>>& stmts, size_t after_idx,
                    std::string_view var_name, bool is_top_level_block,
                    const CompoundStatement* root_body = nullptr) {
  for (size_t k = after_idx + 1; k < stmts.size(); ++k) {
    if (IsUnconditionalOverwriteOfVar(*stmts[k], var_name)) {
      return true;
    }
    if (stmts[k]->Kind() == CStatementKind::kWhileStatement) {
      const auto& ws = static_cast<const WhileStatement&>(*stmts[k]);
      if (CountIdentifierOccurrencesInExpr(ws.Condition(), var_name) == 0 &&
          ws.Body().Kind() == CStatementKind::kCompoundStatement) {
        const auto& body_stmts = static_cast<const CompoundStatement&>(ws.Body()).Statements();
        if (!body_stmts.empty() && IsUnconditionalOverwriteOfVar(*body_stmts.front(), var_name)) {
          if (ws.Condition().Kind() == CExpressionKind::kIntegerLiteral &&
              static_cast<const IntegerLiteralExpression&>(ws.Condition()).Value() != 0) {
            return true;
          }
        }
      }
    }
    if (CountIdentifierOccurrencesInStmt(*stmts[k], var_name) > 0) {
      return false;
    }
    if (stmts[k]->Kind() == CStatementKind::kReturnStatement) {
      return true;
    }
    if (StmtContainsJumpOrLabel(*stmts[k])) {
      return false;
    }
  }
  if (is_top_level_block) return true;
  if (root_body != nullptr && !StmtContainsGotoOrLabel(*root_body) &&
      CountNonDeclOccurrencesInStmt(*root_body, var_name) <= 2) {
    return true;
  }
  return false;
}

std::unique_ptr<CExpression> ReplaceIdentifierInExpr(const CExpression& expr,
                                                     std::string_view target_name,
                                                     const CExpression& replacement) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      if (id.Name() == target_name) {
        return replacement.Clone();
      }
      std::string_view prefix;
      std::string_view inner_id;
      if (ExtractFuncPtrCastCalleeIdent(id.Name(), &prefix, &inner_id) && inner_id == target_name) {
        return CExpression::Identifier(std::string(prefix) + replacement.ToString() + ")");
      }
      return expr.Clone();
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(
          u.Op(), ReplaceIdentifierInExpr(u.Operand(), target_name, replacement), u.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(b.Op(), ReplaceIdentifierInExpr(b.Lhs(), target_name, replacement),
                                 ReplaceIdentifierInExpr(b.Rhs(), target_name, replacement));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      std::unique_ptr<CExpression> new_lhs =
          (a.Lhs().Kind() == CExpressionKind::kIdentifier &&
           static_cast<const IdentifierExpression&>(a.Lhs()).Name() == target_name && a.Op() == "=")
              ? a.Lhs().Clone()
              : ReplaceIdentifierInExpr(a.Lhs(), target_name, replacement);
      return CExpression::Assignment(a.Op(), std::move(new_lhs),
                                     ReplaceIdentifierInExpr(a.Rhs(), target_name, replacement));
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        args.push_back(ReplaceIdentifierInExpr(*arg, target_name, replacement));
      }
      return CExpression::Call(ReplaceIdentifierInExpr(c.Callee(), target_name, replacement),
                               std::move(args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(c.TargetType(),
                               ReplaceIdentifierInExpr(c.Operand(), target_name, replacement));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(
          ReplaceIdentifierInExpr(m.Object(), target_name, replacement), m.MemberName(),
          m.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(
          ReplaceIdentifierInExpr(arr.Array(), target_name, replacement),
          ReplaceIdentifierInExpr(arr.Index(), target_name, replacement));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          ReplaceIdentifierInExpr(t.Condition(), target_name, replacement),
          ReplaceIdentifierInExpr(t.TrueExpression(), target_name, replacement),
          ReplaceIdentifierInExpr(t.FalseExpression(), target_name, replacement));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> ReplaceIdentifierInSingleStmt(const CStatement& stmt,
                                                          std::string_view target_name,
                                                          const CExpression& replacement) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      return CStatement::Expression(
          ReplaceIdentifierInExpr(es.Expression(), target_name, replacement));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return rs.Clone();
      return CStatement::Return(
          ReplaceIdentifierInExpr(*rs.ReturnValue(), target_name, replacement));
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(ReplaceIdentifierInExpr(ifs.Condition(), target_name, replacement),
                            ifs.ThenBranch().Clone(),
                            ifs.ElseBranch() ? ifs.ElseBranch()->Clone() : nullptr);
    }
    default:
      return stmt.Clone();
  }
}

struct InlineCandidateInfo {
  size_t use_idx = 0;
  std::string var_name;
  const CExpression* rhs = nullptr;
  bool can_sink_to_use = false;
  bool can_hoist_to_def = false;
};

bool AnalyzeInlineSingleUseTempAt(const std::vector<std::unique_ptr<CStatement>>& stmts, size_t idx,
                                  bool is_top_level_block, InlineCandidateInfo* out_info,
                                  const CompoundStatement* root_body = nullptr) {
  if (idx + 1 >= stmts.size()) return false;
  if (stmts[idx]->Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& es = static_cast<const ExpressionStatement&>(*stmts[idx]);
  if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assign = static_cast<const AssignmentExpression&>(es.Expression());
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  if (!IsPureExpression(assign.Rhs())) return false;

  const std::string& var_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  const CExpression& rhs = assign.Rhs();

  std::set<std::string> rhs_ids;
  CollectIdentifiersInExpr(rhs, &rhs_ids);
  const bool rhs_reads_indirect_mem = ExprReadsIndirectMemory(rhs);

  // Find the first subsequent statement `use_idx` that references `var_name`.
  std::optional<size_t> found_use_idx;
  bool can_sink = true;
  for (size_t m = idx + 1; m < stmts.size(); ++m) {
    if (CountIdentifierOccurrencesInStmt(*stmts[m], var_name) > 0) {
      found_use_idx = m;
      break;
    }
    // Check if `stmts[m]` is a pure assignment so we can potentially cross it.
    if (stmts[m]->Kind() != CStatementKind::kExpressionStatement) {
      return false;
    }
    const auto& m_expr = static_cast<const ExpressionStatement&>(*stmts[m]).Expression();
    if (m_expr.Kind() != CExpressionKind::kAssignmentExpression) {
      return false;
    }
    const auto& m_assign = static_cast<const AssignmentExpression&>(m_expr);
    if (m_assign.Op() != "=" || !IsPureExpression(m_assign.Lhs()) ||
        !IsPureExpression(m_assign.Rhs())) {
      return false;
    }
    std::string m_written_id;
    bool m_writes_indirect = false;
    if (ExtractLhsTargetIdentifier(m_assign.Lhs(), &m_written_id, &m_writes_indirect)) {
      if (rhs_ids.count(m_written_id) > 0) {
        can_sink = false;
      }
    }
    if (m_writes_indirect && (rhs_reads_indirect_mem || !rhs_ids.empty())) {
      can_sink = false;
    }
  }

  if (!found_use_idx.has_value()) return false;
  const size_t use_idx = *found_use_idx;
  const CStatement& use_stmt = *stmts[use_idx];

  int use_count = 0;
  bool use_kills_var = false;
  if (use_stmt.Kind() == CStatementKind::kExpressionStatement) {
    const auto& u_es = static_cast<const ExpressionStatement&>(use_stmt);
    if (u_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& u_assign = static_cast<const AssignmentExpression&>(u_es.Expression());
      if (u_assign.Op() == "=" && u_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(u_assign.Lhs()).Name() == var_name) {
        use_count = CountIdentifierOccurrencesInExpr(u_assign.Rhs(), var_name);
        use_kills_var = true;
      } else {
        use_count = CountIdentifierOccurrencesInExpr(u_es.Expression(), var_name);
      }
    } else {
      use_count = CountIdentifierOccurrencesInExpr(u_es.Expression(), var_name);
    }
  } else if (use_stmt.Kind() == CStatementKind::kReturnStatement) {
    const auto& rs = static_cast<const ReturnStatement&>(use_stmt);
    if (rs.ReturnValue() != nullptr) {
      use_count = CountIdentifierOccurrencesInExpr(*rs.ReturnValue(), var_name);
    }
    use_kills_var = true;
  } else if (use_stmt.Kind() == CStatementKind::kIfStatement) {
    const auto& ifs = static_cast<const IfStatement&>(use_stmt);
    if (CountIdentifierOccurrencesInStmt(ifs.ThenBranch(), var_name) == 0 &&
        (ifs.ElseBranch() == nullptr ||
         CountIdentifierOccurrencesInStmt(*ifs.ElseBranch(), var_name) == 0)) {
      use_count = CountIdentifierOccurrencesInExpr(ifs.Condition(), var_name);
    }
  }

  if (use_count != 1) return false;
  if (!use_kills_var && !IsVarDeadAfter(stmts, use_idx, var_name, is_top_level_block, root_body)) {
    return false;
  }

  // Check if `use_stmt` can be hoisted up to `idx` when `use_idx > idx + 1`.
  bool can_hoist = (use_idx == idx + 1);
  if (use_idx > idx + 1 && use_stmt.Kind() == CStatementKind::kExpressionStatement) {
    const auto& u_es = static_cast<const ExpressionStatement&>(use_stmt);
    if (u_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& u_assign = static_cast<const AssignmentExpression&>(u_es.Expression());
      if (u_assign.Op() == "=" && IsPureExpression(u_assign.Lhs()) &&
          IsPureExpression(u_assign.Rhs())) {
        std::string u_written_id;
        bool u_writes_indirect = false;
        if (ExtractLhsTargetIdentifier(u_assign.Lhs(), &u_written_id, &u_writes_indirect) &&
            !u_writes_indirect) {
          auto placeholder_stmt = ReplaceIdentifierInSingleStmt(
              use_stmt, var_name, *CExpression::Identifier("__inline_placeholder__"));
          const auto& ph_expr =
              static_cast<const ExpressionStatement&>(*placeholder_stmt).Expression();
          std::set<std::string> u_ids;
          CollectIdentifiersInExpr(ph_expr, &u_ids);
          u_ids.erase("__inline_placeholder__");
          bool u_reads_indirect = ExprReadsIndirectMemory(ph_expr);

          can_hoist = true;
          for (size_t m = idx + 1; m < use_idx; ++m) {
            const auto& m_assign = static_cast<const AssignmentExpression&>(
                static_cast<const ExpressionStatement&>(*stmts[m]).Expression());
            std::string m_written_id;
            bool m_writes_indirect = false;
            ExtractLhsTargetIdentifier(m_assign.Lhs(), &m_written_id, &m_writes_indirect);
            if (!m_written_id.empty() && u_ids.count(m_written_id) > 0) {
              can_hoist = false;
              break;
            }
            if (CountIdentifierOccurrencesInStmt(*stmts[m], u_written_id) > 0) {
              can_hoist = false;
              break;
            }
            if (m_writes_indirect && (u_reads_indirect || !u_ids.empty())) {
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
    out_info->use_idx = use_idx;
    out_info->var_name = var_name;
    out_info->rhs = &rhs;
    out_info->can_sink_to_use = can_sink;
    out_info->can_hoist_to_def = can_hoist;
  }
  return true;
}

bool CanSwapAdjacentStatements(const std::vector<std::unique_ptr<CStatement>>& stmts, size_t idx) {
  if (idx + 1 >= stmts.size()) return false;
  if (stmts[idx]->Kind() != CStatementKind::kExpressionStatement ||
      stmts[idx + 1]->Kind() != CStatementKind::kExpressionStatement) {
    return false;
  }
  const auto& e1 = static_cast<const ExpressionStatement&>(*stmts[idx]).Expression();
  const auto& e2 = static_cast<const ExpressionStatement&>(*stmts[idx + 1]).Expression();
  if (e1.Kind() != CExpressionKind::kAssignmentExpression ||
      e2.Kind() != CExpressionKind::kAssignmentExpression) {
    return false;
  }
  const auto& a1 = static_cast<const AssignmentExpression&>(e1);
  const auto& a2 = static_cast<const AssignmentExpression&>(e2);
  if (a1.Op() != "=" || a2.Op() != "=" || !IsPureExpression(a1.Lhs()) ||
      !IsPureExpression(a1.Rhs()) || !IsPureExpression(a2.Lhs()) || !IsPureExpression(a2.Rhs())) {
    return false;
  }

  std::string w1;
  bool ind_w1 = false;
  ExtractLhsTargetIdentifier(a1.Lhs(), &w1, &ind_w1);

  std::string w2;
  bool ind_w2 = false;
  ExtractLhsTargetIdentifier(a2.Lhs(), &w2, &ind_w2);

  if (ind_w1 && ind_w2) return false;
  if (!w1.empty() && !w2.empty() && w1 == w2) return false;
  if (!w1.empty() && CountIdentifierOccurrencesInExpr(e2, w1) > 0) return false;
  if (!w2.empty() && CountIdentifierOccurrencesInExpr(e1, w2) > 0) return false;
  if (ind_w1 && ExprReadsIndirectMemory(e2)) return false;
  if (ind_w2 && ExprReadsIndirectMemory(e1)) return false;

  return true;
}

bool IsDeadAssignmentAt(const std::vector<std::unique_ptr<CStatement>>& stmts, size_t idx,
                        bool is_top_level_block, const std::set<std::string>& local_vars,
                        bool* out_is_call = nullptr, const CompoundStatement* root_body = nullptr) {
  if (stmts[idx]->Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& expr = static_cast<const ExpressionStatement&>(*stmts[idx]).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  const std::string& var_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  if (local_vars.count(var_name) == 0) return false;

  const CExpression* rhs = &assign.Rhs();
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

  if (!IsVarDeadAfter(stmts, idx, var_name, is_top_level_block, root_body)) {
    return false;
  }
  if (out_is_call != nullptr) *out_is_call = is_call;
  return true;
}

std::unique_ptr<CompoundStatement> PruneUnusedLocalDecls(const CompoundStatement& body) {
  const auto& stmts = body.Statements();
  std::vector<std::unique_ptr<CStatement>> new_stmts;
  new_stmts.reserve(stmts.size());

  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() == CStatementKind::kVariableDeclarationStatement) {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(*stmts[i]);
      if ((vd.Initializer() == nullptr || IsPureExpression(*vd.Initializer())) &&
          !vd.ArraySize().has_value()) {
        int uses = 0;
        for (size_t j = 0; j < stmts.size(); ++j) {
          if (j == i) continue;
          uses += CountIdentifierOccurrencesInStmt(*stmts[j], vd.Name());
        }
        if (uses == 0) {
          continue;
        }
      }
    }
    new_stmts.push_back(stmts[i]->Clone());
  }
  if (!new_stmts.empty() && new_stmts.back()->Kind() == CStatementKind::kReturnStatement &&
      static_cast<const ReturnStatement&>(*new_stmts.back()).ReturnValue() == nullptr) {
    new_stmts.pop_back();
  }
  return std::make_unique<CompoundStatement>(std::move(new_stmts));
}

// Collects global symbols that are only ever accessed via `*(T *)&sym` with a consistent width.
struct DirectTypedGlobalCandidate {
  std::string symbol_name;
  CType resolved_type;
};

void ScanDirectTypedGlobalsInExpr(const CExpression& expr, const std::set<std::string>& local_vars,
                                  std::map<std::string, std::set<std::string>>* cast_types,
                                  std::set<std::string>* non_cast_uses) {
  std::string sym;
  CType t;
  if (ExtractGlobalCastTarget(expr, &sym, &t)) {
    if (local_vars.count(sym) == 0) {
      (*cast_types)[sym].insert(t.name);
    }
    return;
  }
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      if (local_vars.count(id.Name()) == 0) {
        non_cast_uses->insert(id.Name());
      }
      break;
    }
    case CExpressionKind::kUnaryExpression:
      ScanDirectTypedGlobalsInExpr(static_cast<const UnaryExpression&>(expr).Operand(), local_vars,
                                   cast_types, non_cast_uses);
      break;
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      ScanDirectTypedGlobalsInExpr(b.Lhs(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInExpr(b.Rhs(), local_vars, cast_types, non_cast_uses);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      ScanDirectTypedGlobalsInExpr(a.Lhs(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInExpr(a.Rhs(), local_vars, cast_types, non_cast_uses);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      for (const auto& arg : c.Arguments()) {
        ScanDirectTypedGlobalsInExpr(*arg, local_vars, cast_types, non_cast_uses);
      }
      break;
    }
    case CExpressionKind::kCastExpression:
      ScanDirectTypedGlobalsInExpr(static_cast<const CastExpression&>(expr).Operand(), local_vars,
                                   cast_types, non_cast_uses);
      break;
    case CExpressionKind::kMemberAccessExpression:
      ScanDirectTypedGlobalsInExpr(static_cast<const MemberAccessExpression&>(expr).Object(),
                                   local_vars, cast_types, non_cast_uses);
      break;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      ScanDirectTypedGlobalsInExpr(arr.Array(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInExpr(arr.Index(), local_vars, cast_types, non_cast_uses);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      ScanDirectTypedGlobalsInExpr(tern.Condition(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInExpr(tern.TrueExpression(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInExpr(tern.FalseExpression(), local_vars, cast_types, non_cast_uses);
      break;
    }
    default:
      break;
  }
}

void ScanDirectTypedGlobalsInStmt(const CStatement& stmt, const std::set<std::string>& local_vars,
                                  std::map<std::string, std::set<std::string>>* cast_types,
                                  std::set<std::string>* non_cast_uses) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        ScanDirectTypedGlobalsInStmt(*s, local_vars, cast_types, non_cast_uses);
      }
      break;
    case CStatementKind::kExpressionStatement:
      ScanDirectTypedGlobalsInExpr(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                   local_vars, cast_types, non_cast_uses);
      break;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      if (vd.Initializer() != nullptr) {
        ScanDirectTypedGlobalsInExpr(*vd.Initializer(), local_vars, cast_types, non_cast_uses);
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      ScanDirectTypedGlobalsInExpr(ifs.Condition(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInStmt(ifs.ThenBranch(), local_vars, cast_types, non_cast_uses);
      if (ifs.ElseBranch() != nullptr) {
        ScanDirectTypedGlobalsInStmt(*ifs.ElseBranch(), local_vars, cast_types, non_cast_uses);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      ScanDirectTypedGlobalsInExpr(ws.Condition(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInStmt(ws.Body(), local_vars, cast_types, non_cast_uses);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      ScanDirectTypedGlobalsInStmt(dws.Body(), local_vars, cast_types, non_cast_uses);
      ScanDirectTypedGlobalsInExpr(dws.Condition(), local_vars, cast_types, non_cast_uses);
      break;
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr) {
        ScanDirectTypedGlobalsInStmt(*fs.Init(), local_vars, cast_types, non_cast_uses);
      }
      if (fs.Condition() != nullptr) {
        ScanDirectTypedGlobalsInExpr(*fs.Condition(), local_vars, cast_types, non_cast_uses);
      }
      if (fs.Step() != nullptr) {
        ScanDirectTypedGlobalsInExpr(*fs.Step(), local_vars, cast_types, non_cast_uses);
      }
      ScanDirectTypedGlobalsInStmt(fs.Body(), local_vars, cast_types, non_cast_uses);
      break;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr) {
        ScanDirectTypedGlobalsInExpr(*rs.ReturnValue(), local_vars, cast_types, non_cast_uses);
      }
      break;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      ScanDirectTypedGlobalsInExpr(ss.Condition(), local_vars, cast_types, non_cast_uses);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) {
          ScanDirectTypedGlobalsInStmt(*c.body, local_vars, cast_types, non_cast_uses);
        }
      }
      break;
    }
    default:
      break;
  }
}

std::vector<DirectTypedGlobalCandidate> CollectDirectTypedGlobalCandidates(
    const FunctionDeclaration& func, const ReferencedSymbols& ref_syms) {
  std::map<std::string, std::set<std::string>> cast_types;
  std::set<std::string> non_cast_uses;
  ScanDirectTypedGlobalsInStmt(func.Body(), ref_syms.local_variables, &cast_types, &non_cast_uses);

  std::vector<DirectTypedGlobalCandidate> candidates;
  for (const auto& [sym, types] : cast_types) {
    if (non_cast_uses.count(sym) > 0) continue;
    if (types.size() == 1) {
      candidates.push_back({sym, CType::Named(*types.begin())});
    } else if (types.size() == 2 && types.count("s16") > 0 && types.count("u16") > 0) {
      candidates.push_back({sym, CType::S16()});
    } else if (types.size() == 2 && types.count("s8") > 0 && types.count("u8") > 0) {
      candidates.push_back({sym, CType::S8()});
    }
  }
  return candidates;
}

std::unique_ptr<CExpression> RewriteDirectTypedGlobalInExpr(
    const CExpression& expr, const std::map<std::string, CType>& target_globals) {
  std::string sym;
  CType t;
  if (ExtractGlobalCastTarget(expr, &sym, &t)) {
    auto it = target_globals.find(sym);
    if (it != target_globals.end()) {
      return CExpression::Identifier(sym, it->second);
    }
  }
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(u.Op(), RewriteDirectTypedGlobalInExpr(u.Operand(), target_globals),
                                u.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(b.Op(), RewriteDirectTypedGlobalInExpr(b.Lhs(), target_globals),
                                 RewriteDirectTypedGlobalInExpr(b.Rhs(), target_globals));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return CExpression::Assignment(a.Op(),
                                     RewriteDirectTypedGlobalInExpr(a.Lhs(), target_globals),
                                     RewriteDirectTypedGlobalInExpr(a.Rhs(), target_globals));
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        args.push_back(RewriteDirectTypedGlobalInExpr(*arg, target_globals));
      }
      return CExpression::Call(RewriteDirectTypedGlobalInExpr(c.Callee(), target_globals),
                               std::move(args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(c.TargetType(),
                               RewriteDirectTypedGlobalInExpr(c.Operand(), target_globals));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(RewriteDirectTypedGlobalInExpr(m.Object(), target_globals),
                                       m.MemberName(), m.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(RewriteDirectTypedGlobalInExpr(arr.Array(), target_globals),
                                     RewriteDirectTypedGlobalInExpr(arr.Index(), target_globals));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          RewriteDirectTypedGlobalInExpr(tern.Condition(), target_globals),
          RewriteDirectTypedGlobalInExpr(tern.TrueExpression(), target_globals),
          RewriteDirectTypedGlobalInExpr(tern.FalseExpression(), target_globals));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> RewriteDirectTypedGlobalInStmt(
    const CStatement& stmt, const std::map<std::string, CType>& target_globals) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> stmts;
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        stmts.push_back(RewriteDirectTypedGlobalInStmt(*s, target_globals));
      }
      return std::make_unique<CompoundStatement>(std::move(stmts));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(RewriteDirectTypedGlobalInExpr(
          static_cast<const ExpressionStatement&>(stmt).Expression(), target_globals));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return CStatement::VariableDeclaration(
          vd.Type(), vd.Name(),
          vd.Initializer() ? RewriteDirectTypedGlobalInExpr(*vd.Initializer(), target_globals)
                           : nullptr,
          vd.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(RewriteDirectTypedGlobalInExpr(ifs.Condition(), target_globals),
                            RewriteDirectTypedGlobalInStmt(ifs.ThenBranch(), target_globals),
                            ifs.ElseBranch()
                                ? RewriteDirectTypedGlobalInStmt(*ifs.ElseBranch(), target_globals)
                                : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(RewriteDirectTypedGlobalInExpr(ws.Condition(), target_globals),
                               RewriteDirectTypedGlobalInStmt(ws.Body(), target_globals));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(RewriteDirectTypedGlobalInStmt(dws.Body(), target_globals),
                                 RewriteDirectTypedGlobalInExpr(dws.Condition(), target_globals));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          fs.Init() ? RewriteDirectTypedGlobalInStmt(*fs.Init(), target_globals) : nullptr,
          fs.Condition() ? RewriteDirectTypedGlobalInExpr(*fs.Condition(), target_globals)
                         : nullptr,
          fs.Step() ? RewriteDirectTypedGlobalInExpr(*fs.Step(), target_globals) : nullptr,
          RewriteDirectTypedGlobalInStmt(fs.Body(), target_globals));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(RewriteDirectTypedGlobalInExpr(*rs.ReturnValue(), target_globals));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      std::vector<SwitchCase> cases;
      for (const auto& c : ss.Cases()) {
        SwitchCase sc;
        sc.case_values = c.case_values;
        sc.is_default = c.is_default;
        if (c.body != nullptr) {
          sc.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              RewriteDirectTypedGlobalInStmt(*c.body, target_globals).release()));
        }
        cases.push_back(std::move(sc));
      }
      return CStatement::Switch(RewriteDirectTypedGlobalInExpr(ss.Condition(), target_globals),
                                std::move(cases));
    }
    default:
      return stmt.Clone();
  }
}

// Rewrites uses of `ptr_var` (where `ptr_var = &sym;`) and 32-bit global `sym` into `sym[idx]` /
// `&sym[idx]`.
std::unique_ptr<CExpression> RewriteGlobalArrayInExpr(
    const CExpression& expr, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& addr_temp_to_sym) {
  // 1. Check `*(s32 *)ptr_var` -> `sym[0]`
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& deref = static_cast<const UnaryExpression&>(expr);
    if (deref.IsPrefix() && deref.Op() == "*" &&
        deref.Operand().Kind() == CExpressionKind::kCastExpression) {
      const auto& cast = static_cast<const CastExpression&>(deref.Operand());
      if (cast.TargetType().pointer_depth == 1 &&
          (cast.TargetType().name == "s32" || cast.TargetType().name == "u32")) {
        if (cast.Operand().Kind() == CExpressionKind::kIdentifier) {
          const auto& id = static_cast<const IdentifierExpression&>(cast.Operand());
          auto it = addr_temp_to_sym.find(id.Name());
          if (it != addr_temp_to_sym.end()) {
            return CExpression::ArrayIndex(
                CExpression::Identifier(it->second, CType::S32(), /*is_array_symbol=*/true),
                CExpression::Integer(0));
          }
        } else if (cast.Operand().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& bin = static_cast<const BinaryExpression&>(cast.Operand());
          if ((bin.Op() == "+" || bin.Op() == "-") &&
              bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
              bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
            const auto& id = static_cast<const IdentifierExpression&>(bin.Lhs());
            int64_t off = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
            if (bin.Op() == "-") off = -off;
            auto it = addr_temp_to_sym.find(id.Name());
            if (it != addr_temp_to_sym.end() && (off % 4) == 0) {
              return CExpression::ArrayIndex(
                  CExpression::Identifier(it->second, CType::S32(), /*is_array_symbol=*/true),
                  CExpression::Integer(off / 4));
            }
          }
        }
      }
    }
  }

  // 2. Check `(ptr_var + offset)` -> `&sym[offset / 4]`
  if (expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(expr);
    if ((bin.Op() == "+" || bin.Op() == "-") && bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
        bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
      const auto& id = static_cast<const IdentifierExpression&>(bin.Lhs());
      int64_t off = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
      if (bin.Op() == "-") off = -off;
      auto it = addr_temp_to_sym.find(id.Name());
      if (it != addr_temp_to_sym.end() && (off % 4) == 0) {
        return CExpression::Unary(
            "&",
            CExpression::ArrayIndex(
                CExpression::Identifier(it->second, CType::S32(), /*is_array_symbol=*/true),
                CExpression::Integer(off / 4)),
            /*is_prefix=*/true);
      }
    }
  }

  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      auto it = addr_temp_to_sym.find(id.Name());
      if (it != addr_temp_to_sym.end()) {
        return CExpression::Unary(
            "&",
            CExpression::ArrayIndex(
                CExpression::Identifier(it->second, CType::S32(), /*is_array_symbol=*/true),
                CExpression::Integer(0)),
            /*is_prefix=*/true);
      }
      if (array_globals.count(id.Name()) > 0) {
        return CExpression::ArrayIndex(
            CExpression::Identifier(id.Name(), CType::S32(), /*is_array_symbol=*/true),
            CExpression::Integer(0));
      }
      return expr.Clone();
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.IsPrefix() && u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
        return expr.Clone();
      }
      return CExpression::Unary(
          u.Op(), RewriteGlobalArrayInExpr(u.Operand(), array_globals, addr_temp_to_sym),
          u.IsPrefix());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      if (a.Array().Kind() == CExpressionKind::kIdentifier) {
        const auto& id = static_cast<const IdentifierExpression&>(a.Array());
        // Toggle back to scalar `sym` if `sym[0]` is toggled individually.
        if (array_globals.count(id.Name()) > 0 &&
            a.Index().Kind() == CExpressionKind::kIntegerLiteral &&
            static_cast<const IntegerLiteralExpression&>(a.Index()).Value() == 0) {
          return CExpression::Identifier(id.Name());
        }
        return CExpression::ArrayIndex(
            a.Array().Clone(),
            RewriteGlobalArrayInExpr(a.Index(), array_globals, addr_temp_to_sym));
      }
      return CExpression::ArrayIndex(
          RewriteGlobalArrayInExpr(a.Array(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInExpr(a.Index(), array_globals, addr_temp_to_sym));
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(
          b.Op(), RewriteGlobalArrayInExpr(b.Lhs(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInExpr(b.Rhs(), array_globals, addr_temp_to_sym));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return CExpression::Assignment(
          a.Op(), RewriteGlobalArrayInExpr(a.Lhs(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInExpr(a.Rhs(), array_globals, addr_temp_to_sym));
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        args.push_back(RewriteGlobalArrayInExpr(*arg, array_globals, addr_temp_to_sym));
      }
      std::unique_ptr<CExpression> new_callee =
          c.Callee().Kind() == CExpressionKind::kIdentifier
              ? c.Callee().Clone()
              : RewriteGlobalArrayInExpr(c.Callee(), array_globals, addr_temp_to_sym);
      return CExpression::Call(std::move(new_callee), std::move(args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(
          c.TargetType(), RewriteGlobalArrayInExpr(c.Operand(), array_globals, addr_temp_to_sym));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(
          RewriteGlobalArrayInExpr(m.Object(), array_globals, addr_temp_to_sym), m.MemberName(),
          m.IsArrow());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          RewriteGlobalArrayInExpr(tern.Condition(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInExpr(tern.TrueExpression(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInExpr(tern.FalseExpression(), array_globals, addr_temp_to_sym));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> RewriteGlobalArrayInStmt(
    const CStatement& stmt, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& addr_temp_to_sym) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      std::vector<std::unique_ptr<CStatement>> stmts;
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s->Kind() == CStatementKind::kExpressionStatement) {
          const auto& expr = static_cast<const ExpressionStatement&>(*s).Expression();
          if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& assign = static_cast<const AssignmentExpression&>(expr);
            if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                addr_temp_to_sym.count(
                    static_cast<const IdentifierExpression&>(assign.Lhs()).Name()) > 0) {
              continue;
            }
          }
        }
        stmts.push_back(RewriteGlobalArrayInStmt(*s, array_globals, addr_temp_to_sym));
      }
      return std::make_unique<CompoundStatement>(std::move(stmts));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(
          RewriteGlobalArrayInExpr(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                   array_globals, addr_temp_to_sym));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      return CStatement::VariableDeclaration(
          vd.Type(), vd.Name(),
          vd.Initializer()
              ? RewriteGlobalArrayInExpr(*vd.Initializer(), array_globals, addr_temp_to_sym)
              : nullptr,
          vd.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(
          RewriteGlobalArrayInExpr(ifs.Condition(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInStmt(ifs.ThenBranch(), array_globals, addr_temp_to_sym),
          ifs.ElseBranch()
              ? RewriteGlobalArrayInStmt(*ifs.ElseBranch(), array_globals, addr_temp_to_sym)
              : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(
          RewriteGlobalArrayInExpr(ws.Condition(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInStmt(ws.Body(), array_globals, addr_temp_to_sym));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          RewriteGlobalArrayInStmt(dws.Body(), array_globals, addr_temp_to_sym),
          RewriteGlobalArrayInExpr(dws.Condition(), array_globals, addr_temp_to_sym));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          fs.Init() ? RewriteGlobalArrayInStmt(*fs.Init(), array_globals, addr_temp_to_sym)
                    : nullptr,
          fs.Condition()
              ? RewriteGlobalArrayInExpr(*fs.Condition(), array_globals, addr_temp_to_sym)
              : nullptr,
          fs.Step() ? RewriteGlobalArrayInExpr(*fs.Step(), array_globals, addr_temp_to_sym)
                    : nullptr,
          RewriteGlobalArrayInStmt(fs.Body(), array_globals, addr_temp_to_sym));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(
          RewriteGlobalArrayInExpr(*rs.ReturnValue(), array_globals, addr_temp_to_sym));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      std::vector<SwitchCase> cases;
      for (const auto& c : ss.Cases()) {
        SwitchCase sc;
        sc.case_values = c.case_values;
        sc.is_default = c.is_default;
        if (c.body != nullptr) {
          sc.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              RewriteGlobalArrayInStmt(*c.body, array_globals, addr_temp_to_sym).release()));
        }
        cases.push_back(std::move(sc));
      }
      return CStatement::Switch(
          RewriteGlobalArrayInExpr(ss.Condition(), array_globals, addr_temp_to_sym),
          std::move(cases));
    }
    default:
      return stmt.Clone();
  }
}

std::vector<std::string> Collect32BitGlobalCandidates(const FunctionDeclaration& func,
                                                      const ReferencedSymbols& ref_syms,
                                                      bool non_zero_or_ptr_only) {
  std::map<std::string, std::set<std::string>> cast_types;
  std::set<std::string> non_cast_uses;
  ScanDirectTypedGlobalsInStmt(func.Body(), ref_syms.local_variables, &cast_types, &non_cast_uses);

  std::set<std::string> zero_only_assigned;
  if (non_zero_or_ptr_only) {
    for (const auto& s : func.Body().Statements()) {
      if (s->Kind() != CStatementKind::kExpressionStatement) continue;
      const auto& expr = static_cast<const ExpressionStatement&>(*s).Expression();
      if (expr.Kind() != CExpressionKind::kAssignmentExpression) continue;
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(assign.Rhs()).Value() == 0) {
        const std::string& name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
        if (CountIdentifierOccurrencesInStmt(func.Body(), name) == 1) {
          zero_only_assigned.insert(name);
        }
      }
    }
  }

  std::vector<std::string> candidates;
  for (const auto& sym : ref_syms.external_data) {
    if (cast_types.count(sym) > 0 && non_cast_uses.count(sym) == 0) continue;
    auto type_it = ref_syms.inferred_data_types.find(sym);
    if (type_it != ref_syms.inferred_data_types.end() && type_it->second.name != "s32" &&
        type_it->second.name != "u32") {
      continue;
    }
    if (non_zero_or_ptr_only && zero_only_assigned.count(sym) > 0) continue;
    candidates.push_back(sym);
  }
  return candidates;
}

std::map<std::string, std::string> CollectSingleAssignGlobalAddrTemps(
    const FunctionDeclaration& func, const std::set<std::string>& target_globals) {
  std::map<std::string, std::string> addr_temps;
  const auto& stmts = func.Body().Statements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() != CStatementKind::kExpressionStatement) continue;
    const auto& expr = static_cast<const ExpressionStatement&>(*stmts[i]).Expression();
    if (expr.Kind() != CExpressionKind::kAssignmentExpression) continue;
    const auto& assign = static_cast<const AssignmentExpression&>(expr);
    if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) continue;
    if (assign.Rhs().Kind() != CExpressionKind::kUnaryExpression) continue;
    const auto& u = static_cast<const UnaryExpression&>(assign.Rhs());
    if (!u.IsPrefix() || u.Op() != "&" || u.Operand().Kind() != CExpressionKind::kIdentifier) {
      continue;
    }
    const std::string& var_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
    const std::string& sym_name = static_cast<const IdentifierExpression&>(u.Operand()).Name();
    if (target_globals.count(sym_name) == 0) continue;

    // Ensure `var_name` is never reassigned anywhere else in `func.Body()`.
    bool reassigned = false;
    for (size_t j = 0; j < stmts.size(); ++j) {
      if (j == i) continue;
      if (stmts[j]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& je = static_cast<const ExpressionStatement&>(*stmts[j]).Expression();
        if (je.Kind() == CExpressionKind::kAssignmentExpression &&
            static_cast<const AssignmentExpression&>(je).Lhs().Kind() ==
                CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(
                static_cast<const AssignmentExpression&>(je).Lhs())
                    .Name() == var_name) {
          reassigned = true;
          break;
        }
      }
    }
    if (!reassigned) {
      addr_temps[var_name] = sym_name;
    }
  }
  return addr_temps;
}

struct ParamCopyCandidate {
  size_t stmt_idx = 0;
  std::string temp_var;
  std::string param_name;
};

std::vector<ParamCopyCandidate> CollectParamCopyCandidates(const FunctionDeclaration& func) {
  std::set<std::string> param_names;
  for (const auto& p : func.Parameters()) {
    param_names.insert(p.name);
  }
  std::vector<ParamCopyCandidate> candidates;
  const auto& stmts = func.Body().Statements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() != CStatementKind::kExpressionStatement) continue;
    const auto& expr = static_cast<const ExpressionStatement&>(*stmts[i]).Expression();
    if (expr.Kind() != CExpressionKind::kAssignmentExpression) continue;
    const auto& assign = static_cast<const AssignmentExpression&>(expr);
    if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
        assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
      continue;
    }
    const std::string& lhs_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
    const std::string& rhs_name = static_cast<const IdentifierExpression&>(assign.Rhs()).Name();
    if (lhs_name == rhs_name || param_names.count(rhs_name) == 0 ||
        param_names.count(lhs_name) > 0) {
      continue;
    }
    // Check if `rhs_name` is dead immediately after `i` (either not used again or overwritten
    // before any read).
    if (!IsVarDeadAfter(stmts, i, rhs_name, /*is_top_level_block=*/true)) {
      continue;
    }
    candidates.push_back({i, lhs_name, rhs_name});
  }
  return candidates;
}

std::unique_ptr<CompoundStatement> ApplyCoalesceParamCopy(const FunctionDeclaration& func,
                                                          const ParamCopyCandidate& cand) {
  const auto& stmts = func.Body().Statements();
  std::vector<std::unique_ptr<CStatement>> work;
  work.reserve(stmts.size());
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (i == cand.stmt_idx) continue;
    work.push_back(stmts[i]->Clone());
  }

  // First inline any single-use assignments to `cand.param_name` (e.g. `arg0 = temp_s2;
  // call(arg0)`) so they don't collide with `cand.temp_var` being renamed to `cand.param_name`.
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < work.size(); ++i) {
      InlineCandidateInfo info;
      if (AnalyzeInlineSingleUseTempAt(work, i, /*is_top_level_block=*/true, &info) &&
          info.var_name == cand.param_name && info.can_sink_to_use) {
        work[info.use_idx] =
            ReplaceIdentifierInSingleStmt(*work[info.use_idx], info.var_name, *info.rhs);
        work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        break;
      }
    }
  }

  // Now replace all occurrences of `cand.temp_var` with `cand.param_name`.
  auto param_id = CExpression::Identifier(cand.param_name);
  for (size_t i = 0; i < work.size(); ++i) {
    if (work[i]->Kind() == CStatementKind::kVariableDeclarationStatement &&
        static_cast<const VariableDeclarationStatement&>(*work[i]).Name() == cand.temp_var) {
      continue;
    }
    if (work[i]->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*work[i]);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        std::unique_ptr<CExpression> new_lhs =
            (a.Lhs().Kind() == CExpressionKind::kIdentifier &&
             static_cast<const IdentifierExpression&>(a.Lhs()).Name() == cand.temp_var)
                ? param_id->Clone()
                : ReplaceIdentifierInExpr(a.Lhs(), cand.temp_var, *param_id);
        work[i] = CStatement::Expression(
            CExpression::Assignment(a.Op(), std::move(new_lhs),
                                    ReplaceIdentifierInExpr(a.Rhs(), cand.temp_var, *param_id)));
        continue;
      }
    }
    work[i] = ReplaceIdentifierInSingleStmt(*work[i], cand.temp_var, *param_id);
  }

  return PruneUnusedLocalDecls(CompoundStatement(std::move(work)));
}

struct MutationContext {
  const AstMutationStep* step_to_apply = nullptr;
  std::vector<AstMutationStep>* discovered = nullptr;
  size_t max_mutations = 64;
  std::set<std::string> local_vars;
  const CompoundStatement* root_body = nullptr;

  uint32_t commute_index = 0;
  uint32_t invert_if_index = 0;
  uint32_t reorder_decl_index = 0;
  uint32_t compound_assign_index = 0;
  uint32_t while_for_index = 0;
  uint32_t cast_index = 0;
  uint32_t inline_temp_index = 0;
  uint32_t ptr_offset_index = 0;
  uint32_t swap_stmt_index = 0;
  uint32_t dead_assign_index = 0;

  void Record(AstMutationKind kind, uint32_t site_index, uint32_t param = 0) {
    if (discovered == nullptr || discovered->size() >= max_mutations) return;
    AstMutationStep s;
    s.set_kind(kind);
    s.set_site_index(site_index);
    s.set_param(param);
    discovered->push_back(std::move(s));
  }

  bool ShouldApply(AstMutationKind kind, uint32_t site_index, uint32_t* out_param = nullptr) const {
    if (step_to_apply == nullptr) return false;
    if (step_to_apply->kind() == kind && step_to_apply->site_index() == site_index) {
      if (out_param != nullptr) *out_param = step_to_apply->param();
      return true;
    }
    return false;
  }

  bool ShouldApplyAllSites(AstMutationKind kind, uint32_t expected_param = 1) const {
    return step_to_apply != nullptr && step_to_apply->kind() == kind &&
           step_to_apply->param() == expected_param;
  }
};

std::unique_ptr<CExpression> TraverseExpression(const CExpression& expr, MutationContext* ctx) {
  CType ptr_type;
  const CExpression* base_expr = nullptr;
  int64_t elem_idx = 0;
  if (MatchPointerOffsetDereference(expr, &ptr_type, &base_expr, &elem_idx)) {
    auto new_base = TraverseExpression(*base_expr, ctx);
    uint32_t site = ctx->ptr_offset_index++;
    ctx->Record(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, site, 0);
    uint32_t param = 0;
    if ((ctx->ShouldApply(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, site, &param) &&
         param == 0) ||
        ctx->ShouldApplyAllSites(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 1)) {
      return CExpression::ArrayIndex(CExpression::Cast(ptr_type, std::move(new_base)),
                                     CExpression::Integer(elem_idx));
    }
    const auto& deref = static_cast<const UnaryExpression&>(expr);
    const auto& cast = static_cast<const CastExpression&>(deref.Operand());
    const auto& bin = static_cast<const BinaryExpression&>(cast.Operand());
    return CExpression::Unary(
        "*",
        CExpression::Cast(ptr_type,
                          CExpression::Binary(bin.Op(), std::move(new_base), bin.Rhs().Clone())),
        /*is_prefix=*/true);
  }

  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(u.Op(), TraverseExpression(u.Operand(), ctx), u.IsPrefix());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      auto new_lhs = TraverseExpression(b.Lhs(), ctx);
      auto new_rhs = TraverseExpression(b.Rhs(), ctx);
      if (IsCommutableBinary(b)) {
        uint32_t site = ctx->commute_index++;
        ctx->Record(AST_MUTATION_COMMUTE_BINARY_OP, site);
        if (ctx->ShouldApply(AST_MUTATION_COMMUTE_BINARY_OP, site)) {
          return CExpression::Binary(*CommutedBinaryOp(b.Op()), std::move(new_rhs),
                                     std::move(new_lhs));
        }
      }
      return CExpression::Binary(b.Op(), std::move(new_lhs), std::move(new_rhs));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      auto new_lhs = TraverseExpression(a.Lhs(), ctx);
      auto new_rhs = TraverseExpression(a.Rhs(), ctx);
      AssignmentExpression current(a.Op(), std::move(new_lhs), std::move(new_rhs));
      if (IsToggableCompoundAssignment(current)) {
        uint32_t site = ctx->compound_assign_index++;
        ctx->Record(AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT, site);
        if (ctx->ShouldApply(AST_MUTATION_TOGGLE_COMPOUND_ASSIGNMENT, site)) {
          return ToggleCompoundAssignment(current);
        }
      }
      return current.Clone();
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      auto new_callee = TraverseExpression(c.Callee(), ctx);
      std::vector<std::unique_ptr<CExpression>> new_args;
      new_args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        new_args.push_back(TraverseExpression(*arg, ctx));
      }
      return CExpression::Call(std::move(new_callee), std::move(new_args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      auto new_op = TraverseExpression(c.Operand(), ctx);
      if (IsToggableIntegerCast(c)) {
        uint32_t site = ctx->cast_index++;
        ctx->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 0);
        ctx->Record(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, 1);
        uint32_t param = 0;
        if (ctx->ShouldApply(AST_MUTATION_TOGGLE_UNSIGNED_CAST, site, &param)) {
          if (param == 0) {
            return new_op;
          }
          CType flipped = (c.TargetType().name == "u32") ? CType::S32() : CType::U32();
          return CExpression::Cast(flipped, std::move(new_op));
        }
      }
      return CExpression::Cast(c.TargetType(), std::move(new_op));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(TraverseExpression(m.Object(), ctx), m.MemberName(),
                                       m.IsArrow());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(TraverseExpression(a.Array(), ctx),
                                     TraverseExpression(a.Index(), ctx));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(TraverseExpression(t.Condition(), ctx),
                                  TraverseExpression(t.TrueExpression(), ctx),
                                  TraverseExpression(t.FalseExpression(), ctx));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> TraverseStatement(const CStatement& stmt, MutationContext* ctx,
                                              bool is_top_level_block = false);

std::unique_ptr<CompoundStatement> TraverseCompound(const CompoundStatement& comp,
                                                    MutationContext* ctx,
                                                    bool is_top_level_block = false) {
  const auto& stmts = comp.Statements();
  std::vector<std::unique_ptr<CStatement>> new_stmts;
  new_stmts.reserve(stmts.size());

  for (size_t i = 0; i < stmts.size(); ++i) {
    // 1. Check adjacent VariableDeclarationStatements for reordering
    if (i + 1 < stmts.size() && stmts[i]->Kind() == CStatementKind::kVariableDeclarationStatement &&
        stmts[i + 1]->Kind() == CStatementKind::kVariableDeclarationStatement) {
      const auto& d1 = static_cast<const VariableDeclarationStatement&>(*stmts[i]);
      const auto& d2 = static_cast<const VariableDeclarationStatement&>(*stmts[i + 1]);
      bool independent = (d2.Initializer() == nullptr ||
                          CountIdentifierOccurrencesInExpr(*d2.Initializer(), d1.Name()) == 0) &&
                         (d1.Initializer() == nullptr ||
                          CountIdentifierOccurrencesInExpr(*d1.Initializer(), d2.Name()) == 0);
      if (independent) {
        uint32_t site = ctx->reorder_decl_index++;
        ctx->Record(AST_MUTATION_REORDER_LOCAL_DECLS, site);
        if (ctx->ShouldApply(AST_MUTATION_REORDER_LOCAL_DECLS, site)) {
          new_stmts.push_back(TraverseStatement(*stmts[i + 1], ctx));
          new_stmts.push_back(TraverseStatement(*stmts[i], ctx));
          ++i;
          continue;
        }
      }
    }

    // 2. Check dead assignment / dead call return elimination
    bool is_dead_call = false;
    if (IsDeadAssignmentAt(stmts, i, is_top_level_block, ctx->local_vars, &is_dead_call,
                           ctx->root_body)) {
      uint32_t site = ctx->dead_assign_index++;
      ctx->Record(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, site, 0);
      uint32_t param = 0;
      if ((ctx->ShouldApply(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, site, &param) && param == 0) ||
          ctx->ShouldApplyAllSites(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 1)) {
        if (is_dead_call) {
          const auto& assign = static_cast<const AssignmentExpression&>(
              static_cast<const ExpressionStatement&>(*stmts[i]).Expression());
          const CExpression* rhs = &assign.Rhs();
          if (rhs->Kind() == CExpressionKind::kCastExpression) {
            rhs = &static_cast<const CastExpression&>(*rhs).Operand();
          }
          new_stmts.push_back(CStatement::Expression(TraverseExpression(*rhs, ctx)));
        }
        continue;
      }
    }

    // 3. Check single-use temporary inlining across `stmts[i]` and `stmts[info.use_idx]`
    InlineCandidateInfo inline_info;
    if (AnalyzeInlineSingleUseTempAt(stmts, i, is_top_level_block, &inline_info, ctx->root_body)) {
      uint32_t site = ctx->inline_temp_index++;
      if (inline_info.can_sink_to_use) {
        ctx->Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site, 0);
      }
      if (inline_info.can_hoist_to_def && inline_info.use_idx > i + 1) {
        ctx->Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site, 2);
      }
      uint32_t param = 0;
      if (ctx->ShouldApply(AST_MUTATION_INLINE_SINGLE_USE_TEMP, site, &param)) {
        auto replaced = ReplaceIdentifierInSingleStmt(*stmts[inline_info.use_idx],
                                                      inline_info.var_name, *inline_info.rhs);
        if (param == 2 && inline_info.can_hoist_to_def) {
          new_stmts.push_back(TraverseStatement(*replaced, ctx));
          for (size_t m = i + 1; m < inline_info.use_idx; ++m) {
            new_stmts.push_back(TraverseStatement(*stmts[m], ctx));
          }
          i = inline_info.use_idx;
          continue;
        } else if (inline_info.can_sink_to_use) {
          for (size_t m = i + 1; m < inline_info.use_idx; ++m) {
            new_stmts.push_back(TraverseStatement(*stmts[m], ctx));
          }
          new_stmts.push_back(TraverseStatement(*replaced, ctx));
          i = inline_info.use_idx;
          continue;
        }
      }
    }

    // 4. Check swapping adjacent independent statements
    if (CanSwapAdjacentStatements(stmts, i)) {
      uint32_t site = ctx->swap_stmt_index++;
      ctx->Record(AST_MUTATION_SWAP_ADJACENT_STATEMENTS, site);
      if (ctx->ShouldApply(AST_MUTATION_SWAP_ADJACENT_STATEMENTS, site)) {
        new_stmts.push_back(TraverseStatement(*stmts[i + 1], ctx));
        new_stmts.push_back(TraverseStatement(*stmts[i], ctx));
        ++i;
        continue;
      }
    }

    new_stmts.push_back(TraverseStatement(*stmts[i], ctx));
  }

  return std::make_unique<CompoundStatement>(std::move(new_stmts));
}

std::unique_ptr<CStatement> TraverseStatement(const CStatement& stmt, MutationContext* ctx,
                                              bool is_top_level_block) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      return TraverseCompound(static_cast<const CompoundStatement&>(stmt), ctx, is_top_level_block);
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      return CStatement::Expression(TraverseExpression(es.Expression(), ctx));
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      std::unique_ptr<CExpression> init =
          vd.Initializer() != nullptr ? TraverseExpression(*vd.Initializer(), ctx) : nullptr;
      return CStatement::VariableDeclaration(vd.Type(), vd.Name(), std::move(init), vd.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      auto new_cond = TraverseExpression(ifs.Condition(), ctx);
      auto new_then = TraverseStatement(ifs.ThenBranch(), ctx);
      std::unique_ptr<CStatement> new_else =
          ifs.ElseBranch() != nullptr ? TraverseStatement(*ifs.ElseBranch(), ctx) : nullptr;
      if (new_else != nullptr) {
        uint32_t site = ctx->invert_if_index++;
        ctx->Record(AST_MUTATION_INVERT_IF_ELSE, site);
        if (ctx->ShouldApply(AST_MUTATION_INVERT_IF_ELSE, site)) {
          return CStatement::If(NegateCondition(*new_cond), std::move(new_else),
                                std::move(new_then));
        }
      }
      return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      auto new_cond = TraverseExpression(ws.Condition(), ctx);
      auto new_body = TraverseStatement(ws.Body(), ctx);
      uint32_t site = ctx->while_for_index++;
      ctx->Record(AST_MUTATION_WHILE_TO_FOR_LOOP, site);
      if (ctx->ShouldApply(AST_MUTATION_WHILE_TO_FOR_LOOP, site)) {
        return CStatement::For(nullptr, std::move(new_cond), nullptr, std::move(new_body));
      }
      return CStatement::While(std::move(new_cond), std::move(new_body));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      auto new_body = TraverseStatement(dws.Body(), ctx);
      auto new_cond = TraverseExpression(dws.Condition(), ctx);
      return CStatement::DoWhile(std::move(new_body), std::move(new_cond));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      std::unique_ptr<CStatement> new_init =
          fs.Init() != nullptr ? TraverseStatement(*fs.Init(), ctx) : nullptr;
      std::unique_ptr<CExpression> new_cond =
          fs.Condition() != nullptr ? TraverseExpression(*fs.Condition(), ctx) : nullptr;
      std::unique_ptr<CExpression> new_step =
          fs.Step() != nullptr ? TraverseExpression(*fs.Step(), ctx) : nullptr;
      auto new_body = TraverseStatement(fs.Body(), ctx);
      if (new_init == nullptr && new_step == nullptr && new_cond != nullptr) {
        uint32_t site = ctx->while_for_index++;
        ctx->Record(AST_MUTATION_WHILE_TO_FOR_LOOP, site);
        if (ctx->ShouldApply(AST_MUTATION_WHILE_TO_FOR_LOOP, site)) {
          return CStatement::While(std::move(new_cond), std::move(new_body));
        }
      }
      return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                             std::move(new_body));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(TraverseExpression(*rs.ReturnValue(), ctx));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      auto new_cond = TraverseExpression(ss.Condition(), ctx);
      std::vector<SwitchCase> new_cases;
      new_cases.reserve(ss.Cases().size());
      for (const auto& c : ss.Cases()) {
        SwitchCase sc;
        sc.case_values = c.case_values;
        sc.is_default = c.is_default;
        if (c.body != nullptr) {
          sc.body = TraverseCompound(*c.body, ctx);
        }
        new_cases.push_back(std::move(sc));
      }
      return CStatement::Switch(std::move(new_cond), std::move(new_cases));
    }
    default:
      return stmt.Clone();
  }
}

std::unique_ptr<CStatement> InlineOneTempInStmt(const CStatement& stmt,
                                                const CompoundStatement& root_body,
                                                bool prefer_hoist, bool is_top_level,
                                                bool* inlined_any);

std::unique_ptr<CompoundStatement> InlineOneTempInCompound(const CompoundStatement& comp,
                                                           const CompoundStatement& root_body,
                                                           bool prefer_hoist, bool is_top_level,
                                                           bool* inlined_any) {
  const auto& stmts = comp.Statements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    InlineCandidateInfo info;
    if (AnalyzeInlineSingleUseTempAt(stmts, i, is_top_level, &info, &root_body)) {
      std::vector<std::unique_ptr<CStatement>> next_stmts;
      next_stmts.reserve(stmts.size() - 1);
      for (size_t k = 0; k < i; ++k) {
        next_stmts.push_back(stmts[k]->Clone());
      }
      auto replaced = ReplaceIdentifierInSingleStmt(*stmts[info.use_idx], info.var_name, *info.rhs);
      if (prefer_hoist && info.can_hoist_to_def) {
        next_stmts.push_back(std::move(replaced));
        for (size_t m = i + 1; m < info.use_idx; ++m) {
          next_stmts.push_back(stmts[m]->Clone());
        }
      } else if (info.can_sink_to_use) {
        for (size_t m = i + 1; m < info.use_idx; ++m) {
          next_stmts.push_back(stmts[m]->Clone());
        }
        next_stmts.push_back(std::move(replaced));
      } else {
        next_stmts.push_back(std::move(replaced));
        for (size_t m = i + 1; m < info.use_idx; ++m) {
          next_stmts.push_back(stmts[m]->Clone());
        }
      }
      for (size_t k = info.use_idx + 1; k < stmts.size(); ++k) {
        next_stmts.push_back(stmts[k]->Clone());
      }
      *inlined_any = true;
      return std::make_unique<CompoundStatement>(std::move(next_stmts));
    }
  }

  std::vector<std::unique_ptr<CStatement>> next_stmts;
  next_stmts.reserve(stmts.size());
  for (const auto& s : stmts) {
    if (!*inlined_any) {
      next_stmts.push_back(
          InlineOneTempInStmt(*s, root_body, prefer_hoist, /*is_top_level=*/false, inlined_any));
    } else {
      next_stmts.push_back(s->Clone());
    }
  }
  return std::make_unique<CompoundStatement>(std::move(next_stmts));
}

std::unique_ptr<CStatement> InlineOneTempInStmt(const CStatement& stmt,
                                                const CompoundStatement& root_body,
                                                bool prefer_hoist, bool is_top_level,
                                                bool* inlined_any) {
  if (*inlined_any) return stmt.Clone();
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      return InlineOneTempInCompound(static_cast<const CompoundStatement&>(stmt), root_body,
                                     prefer_hoist, is_top_level, inlined_any);
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      auto new_then = InlineOneTempInStmt(ifs.ThenBranch(), root_body, prefer_hoist,
                                          /*is_top_level=*/false, inlined_any);
      std::unique_ptr<CStatement> new_else =
          ifs.ElseBranch() != nullptr
              ? InlineOneTempInStmt(*ifs.ElseBranch(), root_body, prefer_hoist,
                                    /*is_top_level=*/false, inlined_any)
              : nullptr;
      return CStatement::If(ifs.Condition().Clone(), std::move(new_then), std::move(new_else));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      auto new_body = InlineOneTempInStmt(ws.Body(), root_body, prefer_hoist,
                                          /*is_top_level=*/false, inlined_any);
      return CStatement::While(ws.Condition().Clone(), std::move(new_body));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      auto new_body = InlineOneTempInStmt(dws.Body(), root_body, prefer_hoist,
                                          /*is_top_level=*/false, inlined_any);
      return CStatement::DoWhile(std::move(new_body), dws.Condition().Clone());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      auto new_body = InlineOneTempInStmt(fs.Body(), root_body, prefer_hoist,
                                          /*is_top_level=*/false, inlined_any);
      return CStatement::For(fs.Init() ? fs.Init()->Clone() : nullptr,
                             fs.Condition() ? fs.Condition()->Clone() : nullptr,
                             fs.Step() ? fs.Step()->Clone() : nullptr, std::move(new_body));
    }
    default:
      return stmt.Clone();
  }
}

std::unique_ptr<CompoundStatement> ApplyBulkInlineTemps(const CompoundStatement& body,
                                                        bool prefer_hoist) {
  auto current =
      std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(body.Clone().release()));
  for (size_t iter = 0; iter < 32; ++iter) {
    bool inlined_any = false;
    current = InlineOneTempInCompound(*current, *current, prefer_hoist, /*is_top_level=*/true,
                                      &inlined_any);
    if (!inlined_any) break;
  }
  return PruneUnusedLocalDecls(*current);
}

}  // namespace

std::vector<AstMutationStep> AstMutator::DiscoverMutations(const FunctionDeclaration& func,
                                                           size_t max_mutations) {
  std::vector<AstMutationStep> mutations;
  ReferencedSymbols ref_syms = CollectReferencedSymbols(func);

  MutationContext ctx;
  ctx.discovered = &mutations;
  ctx.max_mutations = max_mutations;
  ctx.local_vars = ref_syms.local_variables;
  ctx.root_body = &func.Body();
  (void)TraverseCompound(func.Body(), &ctx, /*is_top_level_block=*/true);

  // Add bulk single-use temp inlining if multiple inline sites exist
  if (ctx.inline_temp_index > 1) {
    ctx.Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 1);
    ctx.Record(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3);
  }
  if (ctx.dead_assign_index > 1) {
    ctx.Record(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1);
  }
  if (ctx.ptr_offset_index > 1) {
    ctx.Record(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 0, 1);
  }

  // Discover direct typed global candidates (`*(T *)&D_...` -> `extern T D_...;`)
  auto typed_globals = CollectDirectTypedGlobalCandidates(func, ref_syms);
  for (size_t i = 0; i < typed_globals.size(); ++i) {
    ctx.Record(AST_MUTATION_DIRECT_TYPED_GLOBAL, static_cast<uint32_t>(i), 0);
  }
  if (typed_globals.size() > 1) {
    ctx.Record(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1);
  }

  // Discover 32-bit global array promotion candidates (`D_...` -> `D_...[0]`)
  auto array_globals = Collect32BitGlobalCandidates(func, ref_syms, /*non_zero_or_ptr_only=*/false);
  for (size_t i = 0; i < array_globals.size(); ++i) {
    ctx.Record(AST_MUTATION_GLOBAL_ARRAY_ACCESS, static_cast<uint32_t>(i), 0);
  }
  if (!array_globals.empty()) {
    ctx.Record(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 1);
    ctx.Record(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 2);
  }

  // Discover parameter copy coalescing candidates (`temp_sX = argY` -> `argY`)
  auto param_copies = CollectParamCopyCandidates(func);
  for (size_t i = 0; i < param_copies.size(); ++i) {
    ctx.Record(AST_MUTATION_COALESCE_PARAM_COPY, static_cast<uint32_t>(i), 0);
  }
  if (param_copies.size() > 1) {
    ctx.Record(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1);
  }

  return mutations;
}

std::vector<std::vector<AstMutationStep>> AstMutator::GenerateCanonicalPresets(
    const FunctionDeclaration& func) {
  auto make_step = [](AstMutationKind kind, uint32_t site, uint32_t param) {
    AstMutationStep s;
    s.set_kind(kind);
    s.set_site_index(site);
    s.set_param(param);
    return s;
  };

  std::vector<std::vector<AstMutationStep>> presets;
  // Preset 1: Dead assign removal + direct typed globals + bulk inline (sink) + coalesce params
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 1),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  // Preset 2: Dead assign removal + direct typed globals + bulk inline (hoist) + coalesce params
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  // Preset 3: Struct/Array promotion (smart non-zero globals + pointer offsets) + bulk inline
  // (hoist)
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 2),
      make_step(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 3),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  // Preset 4: Struct/Array promotion (all globals + pointer offsets) + bulk inline (sink)
  presets.push_back({
      make_step(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN, 0, 1),
      make_step(AST_MUTATION_DIRECT_TYPED_GLOBAL, 0, 1),
      make_step(AST_MUTATION_GLOBAL_ARRAY_ACCESS, 0, 1),
      make_step(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX, 0, 1),
      make_step(AST_MUTATION_INLINE_SINGLE_USE_TEMP, 0, 1),
      make_step(AST_MUTATION_COALESCE_PARAM_COPY, 0, 1),
  });
  (void)func;
  return presets;
}

FunctionDeclaration AstMutator::ApplyMutation(const FunctionDeclaration& func,
                                              const AstMutationStep& step) {
  ReferencedSymbols ref_syms = CollectReferencedSymbols(func);

  if (step.kind() == AST_MUTATION_INLINE_SINGLE_USE_TEMP &&
      (step.param() == 1 || step.param() == 3)) {
    auto body = ApplyBulkInlineTemps(func.Body(), /*prefer_hoist=*/step.param() == 3);
    return FunctionDeclaration(func.ReturnType(), func.Name(), func.Parameters(), std::move(body),
                               func.IsStatic());
  }

  if (step.kind() == AST_MUTATION_DIRECT_TYPED_GLOBAL) {
    auto candidates = CollectDirectTypedGlobalCandidates(func, ref_syms);
    std::map<std::string, CType> target_map;
    if (step.param() == 1) {
      for (const auto& c : candidates) {
        target_map[c.symbol_name] = c.resolved_type;
      }
    } else if (step.site_index() < candidates.size()) {
      const auto& c = candidates[step.site_index()];
      target_map[c.symbol_name] = c.resolved_type;
    }
    if (target_map.empty()) return func.Clone();
    auto mutated_body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
        RewriteDirectTypedGlobalInStmt(func.Body(), target_map).release()));
    return FunctionDeclaration(func.ReturnType(), func.Name(), func.Parameters(),
                               std::move(mutated_body), func.IsStatic());
  }

  if (step.kind() == AST_MUTATION_GLOBAL_ARRAY_ACCESS) {
    auto candidates = Collect32BitGlobalCandidates(func, ref_syms,
                                                   /*non_zero_or_ptr_only=*/step.param() == 2);
    std::set<std::string> target_set;
    if (step.param() == 1 || step.param() == 2) {
      target_set.insert(candidates.begin(), candidates.end());
    } else if (step.site_index() < candidates.size()) {
      target_set.insert(candidates[step.site_index()]);
    }
    if (target_set.empty()) return func.Clone();
    auto addr_temps = CollectSingleAssignGlobalAddrTemps(func, target_set);
    auto mutated_body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
        RewriteGlobalArrayInStmt(func.Body(), target_set, addr_temps).release()));
    mutated_body = PruneUnusedLocalDecls(*mutated_body);
    return FunctionDeclaration(func.ReturnType(), func.Name(), func.Parameters(),
                               std::move(mutated_body), func.IsStatic());
  }

  if (step.kind() == AST_MUTATION_COALESCE_PARAM_COPY) {
    FunctionDeclaration current = func.Clone();
    if (step.param() == 1) {
      for (size_t iter = 0; iter < 8; ++iter) {
        auto cands = CollectParamCopyCandidates(current);
        if (cands.empty()) break;
        auto body = ApplyCoalesceParamCopy(current, cands[0]);
        current = FunctionDeclaration(current.ReturnType(), current.Name(), current.Parameters(),
                                      std::move(body), current.IsStatic());
      }
      return current;
    }
    auto cands = CollectParamCopyCandidates(current);
    if (step.site_index() >= cands.size()) return current;
    auto body = ApplyCoalesceParamCopy(current, cands[step.site_index()]);
    return FunctionDeclaration(current.ReturnType(), current.Name(), current.Parameters(),
                               std::move(body), current.IsStatic());
  }

  MutationContext ctx;
  ctx.step_to_apply = &step;
  ctx.local_vars = ref_syms.local_variables;
  ctx.root_body = &func.Body();
  auto mutated_body = TraverseCompound(func.Body(), &ctx, /*is_top_level_block=*/true);
  if (step.kind() == AST_MUTATION_INLINE_SINGLE_USE_TEMP ||
      step.kind() == AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN) {
    mutated_body = PruneUnusedLocalDecls(*mutated_body);
  }
  return FunctionDeclaration(func.ReturnType(), func.Name(), func.Parameters(),
                             std::move(mutated_body), func.IsStatic());
}

FunctionDeclaration AstMutator::ApplyMutations(const FunctionDeclaration& func,
                                               absl::Span<const AstMutationStep> steps) {
  FunctionDeclaration current = func.Clone();
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
