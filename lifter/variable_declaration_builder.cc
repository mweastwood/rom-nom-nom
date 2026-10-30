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
#include "lifter/ast_type_inference_pass.h"
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

bool IsSinglePrecisionFprVarName(std::string_view name) {
  if (name.size() < 2 || name[0] != 'f' || !std::isdigit(static_cast<unsigned char>(name[1]))) {
    return false;
  }
  for (size_t i = 1; i < name.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
      return false;
    }
  }
  return true;
}

bool IsEligibleGprForFloatBitcastAsm(std::string_view name) {
  return name == "v0" || absl::StartsWith(name, "v0_w") || name == "v1" ||
         absl::StartsWith(name, "v1_w") || name == "fp" || name == "temp_fp" || name == "temp_at" ||
         absl::StartsWith(name, "temp_at_w") || absl::StartsWith(name, "temp_t") ||
         absl::StartsWith(name, "temp_s") || absl::StartsWith(name, "temp_a") ||
         (name.size() == 4 && absl::StartsWith(name, "arg") && name[3] >= '0' && name[3] <= '3');
}

bool IsInlineAsmEligibleFloatGprBitcastStmt(const CStatement& root_body, const CStatement& stmt) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& es = static_cast<const ExpressionStatement&>(stmt);
  if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
  if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier ||
      a.Rhs().Kind() != CExpressionKind::kUnaryExpression) {
    return false;
  }
  std::string_view lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
  const auto& u = static_cast<const UnaryExpression&>(a.Rhs());
  if (u.Operand().Kind() != CExpressionKind::kIdentifier) return false;
  std::string_view rhs = static_cast<const IdentifierExpression&>(u.Operand()).Name();
  if ((u.Op() == "*(s32*)&" || u.Op() == "*(s32 *)&") && IsEligibleGprForFloatBitcastAsm(lhs) &&
      IsSinglePrecisionFprVarName(rhs)) {
    return !((rhs == "f12" || rhs == "f14") && !StatementModifiesVariable(root_body, rhs));
  }
  return (u.Op() == "*(f32*)&" || u.Op() == "*(f32 *)&") && IsSinglePrecisionFprVarName(lhs) &&
         IsEligibleGprForFloatBitcastAsm(rhs) && !absl::StartsWith(rhs, "arg");
}

bool StatementTakesNonInlineAsmAddressOfImpl(const CStatement& root_body, const CStatement& s,
                                             std::string_view target_var) {
  switch (s.Kind()) {
    case CStatementKind::kExpressionStatement:
      if (IsInlineAsmEligibleFloatGprBitcastStmt(root_body, s)) {
        return false;
      }
      return StatementTakesAddressOf(s, target_var);
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(s);
      for (const auto& child : comp.Statements()) {
        if (child != nullptr &&
            StatementTakesNonInlineAsmAddressOfImpl(root_body, *child, target_var)) {
          return true;
        }
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(s);
      if (ExpressionTakesAddressOf(ifs.Condition(), target_var) ||
          StatementTakesNonInlineAsmAddressOfImpl(root_body, ifs.ThenBranch(), target_var)) {
        return true;
      }
      return ifs.ElseBranch() != nullptr &&
             StatementTakesNonInlineAsmAddressOfImpl(root_body, *ifs.ElseBranch(), target_var);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(s);
      return ExpressionTakesAddressOf(ws.Condition(), target_var) ||
             StatementTakesNonInlineAsmAddressOfImpl(root_body, ws.Body(), target_var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(s);
      return StatementTakesNonInlineAsmAddressOfImpl(root_body, dws.Body(), target_var) ||
             ExpressionTakesAddressOf(dws.Condition(), target_var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(s);
      if (fs.Init() != nullptr &&
          StatementTakesNonInlineAsmAddressOfImpl(root_body, *fs.Init(), target_var)) {
        return true;
      }
      if (fs.Condition() != nullptr && ExpressionTakesAddressOf(*fs.Condition(), target_var)) {
        return true;
      }
      if (fs.Step() != nullptr && ExpressionTakesAddressOf(*fs.Step(), target_var)) {
        return true;
      }
      return StatementTakesNonInlineAsmAddressOfImpl(root_body, fs.Body(), target_var);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(s);
      if (ExpressionTakesAddressOf(ss.Condition(), target_var)) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr &&
            StatementTakesNonInlineAsmAddressOfImpl(root_body, *c.body, target_var)) {
          return true;
        }
      }
      return false;
    }
    default:
      return StatementTakesAddressOf(s, target_var);
  }
}

bool StatementTakesNonInlineAsmAddressOf(const CStatement& root_body, std::string_view target_var,
                                         bool is_o2) {
  if (!is_o2) {
    return StatementTakesAddressOf(root_body, target_var);
  }
  return StatementTakesNonInlineAsmAddressOfImpl(root_body, root_body, target_var);
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

bool ExpressionContainsNonPowerOfTwoConstDivMod(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      if ((bin.Op() == "/" || bin.Op() == "%") &&
          bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t val = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        uint64_t abs_val = (val < 0) ? static_cast<uint64_t>(-val) : static_cast<uint64_t>(val);
        if (abs_val > 1 && (abs_val & (abs_val - 1)) != 0) {
          return true;
        }
      }
      return ExpressionContainsNonPowerOfTwoConstDivMod(bin.Lhs()) ||
             ExpressionContainsNonPowerOfTwoConstDivMod(bin.Rhs());
    }
    case CExpressionKind::kUnaryExpression:
      return ExpressionContainsNonPowerOfTwoConstDivMod(
          static_cast<const UnaryExpression&>(expr).Operand());
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return ExpressionContainsNonPowerOfTwoConstDivMod(assign.Lhs()) ||
             ExpressionContainsNonPowerOfTwoConstDivMod(assign.Rhs());
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (ExpressionContainsNonPowerOfTwoConstDivMod(call.Callee())) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExpressionContainsNonPowerOfTwoConstDivMod(*arg)) return true;
      }
      return false;
    }
    case CExpressionKind::kCastExpression:
      return ExpressionContainsNonPowerOfTwoConstDivMod(
          static_cast<const CastExpression&>(expr).Operand());
    case CExpressionKind::kMemberAccessExpression:
      return ExpressionContainsNonPowerOfTwoConstDivMod(
          static_cast<const MemberAccessExpression&>(expr).Object());
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return ExpressionContainsNonPowerOfTwoConstDivMod(arr.Array()) ||
             ExpressionContainsNonPowerOfTwoConstDivMod(arr.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return ExpressionContainsNonPowerOfTwoConstDivMod(tern.Condition()) ||
             ExpressionContainsNonPowerOfTwoConstDivMod(tern.TrueExpression()) ||
             ExpressionContainsNonPowerOfTwoConstDivMod(tern.FalseExpression());
    }
    default:
      return false;
  }
}

bool StatementContainsNonPowerOfTwoConstDivMod(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsNonPowerOfTwoConstDivMod(*child)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement:
      return ExpressionContainsNonPowerOfTwoConstDivMod(
          static_cast<const ExpressionStatement&>(stmt).Expression());
    case CStatementKind::kReturnStatement: {
      const auto& ret = static_cast<const ReturnStatement&>(stmt);
      return ret.ReturnValue() != nullptr &&
             ExpressionContainsNonPowerOfTwoConstDivMod(*ret.ReturnValue());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ExpressionContainsNonPowerOfTwoConstDivMod(ifs.Condition()) ||
          StatementContainsNonPowerOfTwoConstDivMod(ifs.ThenBranch())) {
        return true;
      }
      return ifs.ElseBranch() != nullptr &&
             StatementContainsNonPowerOfTwoConstDivMod(*ifs.ElseBranch());
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExpressionContainsNonPowerOfTwoConstDivMod(ws.Condition()) ||
             StatementContainsNonPowerOfTwoConstDivMod(ws.Body());
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StatementContainsNonPowerOfTwoConstDivMod(dws.Body()) ||
             ExpressionContainsNonPowerOfTwoConstDivMod(dws.Condition());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr && StatementContainsNonPowerOfTwoConstDivMod(*fs.Init())) {
        return true;
      }
      if (fs.Condition() != nullptr &&
          ExpressionContainsNonPowerOfTwoConstDivMod(*fs.Condition())) {
        return true;
      }
      if (fs.Step() != nullptr && ExpressionContainsNonPowerOfTwoConstDivMod(*fs.Step())) {
        return true;
      }
      return StatementContainsNonPowerOfTwoConstDivMod(fs.Body());
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExpressionContainsNonPowerOfTwoConstDivMod(ss.Condition())) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StatementContainsNonPowerOfTwoConstDivMod(*c.body)) return true;
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

bool ExpressionContainsNarrowSignedDeref(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      if (un.IsPrefix() && un.Op() == "*" &&
          un.Operand().Kind() == CExpressionKind::kCastExpression) {
        const auto& cast = static_cast<const CastExpression&>(un.Operand());
        if (cast.TargetType().pointer_depth == 1 &&
            (cast.TargetType().name == "s16" || cast.TargetType().name == "s8")) {
          return true;
        }
      }
      return ExpressionContainsNarrowSignedDeref(un.Operand());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return ExpressionContainsNarrowSignedDeref(bin.Lhs()) ||
             ExpressionContainsNarrowSignedDeref(bin.Rhs());
    }
    case CExpressionKind::kCastExpression:
      return ExpressionContainsNarrowSignedDeref(
          static_cast<const CastExpression&>(expr).Operand());
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return ExpressionContainsNarrowSignedDeref(a.Lhs()) ||
             ExpressionContainsNarrowSignedDeref(a.Rhs());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExpressionContainsNarrowSignedDeref(t.Condition()) ||
             ExpressionContainsNarrowSignedDeref(t.TrueExpression()) ||
             ExpressionContainsNarrowSignedDeref(t.FalseExpression());
    }
    default:
      return false;
  }
}

uint32_t CountNarrowSignedTernaryInExpr(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      const uint32_t self_count = ExpressionContainsNarrowSignedDeref(t.Condition()) ? 1u : 0u;
      return self_count + CountNarrowSignedTernaryInExpr(t.Condition()) +
             CountNarrowSignedTernaryInExpr(t.TrueExpression()) +
             CountNarrowSignedTernaryInExpr(t.FalseExpression());
    }
    case CExpressionKind::kUnaryExpression:
      return CountNarrowSignedTernaryInExpr(static_cast<const UnaryExpression&>(expr).Operand());
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return CountNarrowSignedTernaryInExpr(bin.Lhs()) + CountNarrowSignedTernaryInExpr(bin.Rhs());
    }
    case CExpressionKind::kCastExpression:
      return CountNarrowSignedTernaryInExpr(static_cast<const CastExpression&>(expr).Operand());
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return CountNarrowSignedTernaryInExpr(a.Lhs()) + CountNarrowSignedTernaryInExpr(a.Rhs());
    }
    default:
      return 0u;
  }
}

bool StatementContainsAnyLoop(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kWhileStatement:
    case CStatementKind::kDoWhileStatement:
    case CStatementKind::kForStatement:
      return true;
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsAnyLoop(*child)) return true;
      }
      return false;
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StatementContainsAnyLoop(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StatementContainsAnyLoop(*ifs.ElseBranch()));
    }
    case CStatementKind::kSwitchStatement:
      for (const auto& c : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (c.body != nullptr && StatementContainsAnyLoop(*c.body)) return true;
      }
      return false;
    default:
      return false;
  }
}

bool ForStatementAllocatesGccLoopTestSpillSlot(const ForStatement& fs) {
  if (fs.Init() == nullptr || fs.Init()->Kind() != CStatementKind::kExpressionStatement ||
      fs.Condition() == nullptr || fs.Condition()->Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  const auto& init_expr = static_cast<const ExpressionStatement&>(*fs.Init()).Expression();
  if (init_expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return false;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(init_expr);
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
      assign.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
      static_cast<const IntegerLiteralExpression&>(assign.Rhs()).Value() != 0) {
    return false;
  }
  const std::string& loop_var = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  const auto& cond = static_cast<const BinaryExpression&>(*fs.Condition());
  if (cond.Op() != "<" && cond.Op() != ">=" && cond.Op() != ">" && cond.Op() != "<=") {
    return false;
  }
  auto strip_casts = [](const CExpression* e) {
    while (e != nullptr && e->Kind() == CExpressionKind::kCastExpression) {
      e = &static_cast<const CastExpression&>(*e).Operand();
    }
    return e;
  };
  const CExpression* lhs = strip_casts(&cond.Lhs());
  const CExpression* rhs = strip_casts(&cond.Rhs());
  if (lhs != nullptr && lhs->Kind() == CExpressionKind::kIdentifier &&
      static_cast<const IdentifierExpression&>(*lhs).Name() == loop_var) {
    return rhs != nullptr && rhs->Kind() != CExpressionKind::kIntegerLiteral;
  }
  if (rhs != nullptr && rhs->Kind() == CExpressionKind::kIdentifier &&
      static_cast<const IdentifierExpression&>(*rhs).Name() == loop_var) {
    return lhs != nullptr && lhs->Kind() != CExpressionKind::kIntegerLiteral;
  }
  return false;
}

uint32_t CountForStatementsImpl(const CStatement& stmt, bool has_loop) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      uint32_t count = 0;
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          count += CountForStatementsImpl(*child, has_loop);
        }
      }
      return count;
    }
    case CStatementKind::kExpressionStatement:
      return has_loop ? CountNarrowSignedTernaryInExpr(
                            static_cast<const ExpressionStatement&>(stmt).Expression())
                      : 0u;
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return (has_loop ? CountNarrowSignedTernaryInExpr(ifs.Condition()) : 0u) +
             CountForStatementsImpl(ifs.ThenBranch(), has_loop) +
             (ifs.ElseBranch() != nullptr ? CountForStatementsImpl(*ifs.ElseBranch(), has_loop)
                                          : 0u);
    }
    case CStatementKind::kWhileStatement:
      return CountForStatementsImpl(static_cast<const WhileStatement&>(stmt).Body(), has_loop);
    case CStatementKind::kDoWhileStatement:
      return CountForStatementsImpl(static_cast<const DoWhileStatement&>(stmt).Body(), has_loop);
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (ForStatementAllocatesGccLoopTestSpillSlot(fs) ? 1u : 0u) +
             CountForStatementsImpl(fs.Body(), has_loop);
    }
    case CStatementKind::kSwitchStatement: {
      uint32_t count = 0;
      for (const auto& c : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (c.body != nullptr) {
          count += CountForStatementsImpl(*c.body, has_loop);
        }
      }
      return count;
    }
    default:
      return 0u;
  }
}

uint32_t CountForStatements(const CStatement& stmt) {
  return CountForStatementsImpl(stmt, StatementContainsAnyLoop(stmt));
}

bool StatementHasLoopConditionUsingVar(const CStatement& stmt, std::string_view var) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementHasLoopConditionUsingVar(*child, var)) {
          return true;
        }
      }
      return false;
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StatementHasLoopConditionUsingVar(ifs.ThenBranch(), var) ||
             (ifs.ElseBranch() != nullptr &&
              StatementHasLoopConditionUsingVar(*ifs.ElseBranch(), var));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CountVariableReadUsesInExpression(ws.Condition(), var) > 0 ||
             StatementHasLoopConditionUsingVar(ws.Body(), var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CountVariableReadUsesInExpression(dws.Condition(), var) > 0 ||
             StatementHasLoopConditionUsingVar(dws.Body(), var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Condition() != nullptr &&
              CountVariableReadUsesInExpression(*fs.Condition(), var) > 0) ||
             StatementHasLoopConditionUsingVar(fs.Body(), var);
    }
    default:
      return false;
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

// Returns true if `var_name` is assigned a binary expression whose LHS and RHS are both binary
// expressions (`var = (a op1 b) op2 (c op3 d)`). In GCC 2.7.2 `expr.c` (`validate_subtarget`),
// pinning `var` to a hard register prevents GCC from using `var` as the `subtarget` for the LHS
// subexpression `(a op1 b)`.
bool IsAssignedCompoundBinaryBothSides(const CStatement& stmt, std::string_view var_name) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && IsAssignedCompoundBinaryBothSides(*child, var_name)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement: {
      const auto& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
      if (expr.Kind() != CExpressionKind::kAssignmentExpression) return false;
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
          static_cast<const IdentifierExpression&>(assign.Lhs()).Name() != var_name ||
          assign.Rhs().Kind() != CExpressionKind::kBinaryExpression) {
        return false;
      }
      const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
      return bin.Lhs().Kind() == CExpressionKind::kBinaryExpression &&
             bin.Rhs().Kind() == CExpressionKind::kBinaryExpression;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return IsAssignedCompoundBinaryBothSides(ifs.ThenBranch(), var_name) ||
             (ifs.ElseBranch() != nullptr &&
              IsAssignedCompoundBinaryBothSides(*ifs.ElseBranch(), var_name));
    }
    case CStatementKind::kWhileStatement:
      return IsAssignedCompoundBinaryBothSides(static_cast<const WhileStatement&>(stmt).Body(),
                                               var_name);
    case CStatementKind::kDoWhileStatement:
      return IsAssignedCompoundBinaryBothSides(static_cast<const DoWhileStatement&>(stmt).Body(),
                                               var_name);
    case CStatementKind::kForStatement:
      return IsAssignedCompoundBinaryBothSides(static_cast<const ForStatement&>(stmt).Body(),
                                               var_name);
    default:
      return false;
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

bool NonAddrTakenSpVarWillSpillInGcc(const CompoundStatement& body, std::string_view var_name) {
  int calls_before_first_ref = 0;
  bool seen_first_ref = false;
  int def_count = 0;
  bool single_const_def = false;
  int call_read_stmts = 0;
  int reg_copy_read_stmts = 0;
  int total_read_uses = 0;

  for (const auto& stmt_ptr : body.Statements()) {
    if (stmt_ptr == nullptr) continue;
    const CStatement& stmt = *stmt_ptr;
    const bool refs = StatementReferencesVariable(stmt, var_name);
    if (!seen_first_ref) {
      if (refs) {
        seen_first_ref = true;
      } else if (StatementHasCall(stmt)) {
        ++calls_before_first_ref;
      }
    }
    if (!refs) continue;

    if (StatementModifiesVariable(stmt, var_name)) {
      ++def_count;
      if (stmt.Kind() == CStatementKind::kExpressionStatement) {
        const auto& es = static_cast<const ExpressionStatement&>(stmt);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& assign = static_cast<const AssignmentExpression&>(es.Expression());
          if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == var_name &&
              assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
            single_const_def = true;
          }
        }
      }
    }
    const int reads = CountVariableReadUsesInStatement(stmt, var_name);
    if (reads > 0) {
      total_read_uses += reads;
      if (StatementHasCall(stmt)) {
        ++call_read_stmts;
      } else if (stmt.Kind() == CStatementKind::kExpressionStatement) {
        const auto& es = static_cast<const ExpressionStatement&>(stmt);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& assign = static_cast<const AssignmentExpression&>(es.Expression());
          if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
              assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(assign.Rhs()).Name() == var_name) {
            ++reg_copy_read_stmts;
          }
        }
      }
    }
  }

  if (calls_before_first_ref >= 4) {
    return false;
  }
  if (def_count == 1 && single_const_def && total_read_uses == 2 && call_read_stmts == 1 &&
      reg_copy_read_stmts == 1) {
    return false;
  }
  return true;
}

uint32_t ComputeUnreferencedSpGapSize(const StackFrameAnalysis& frame_analysis,
                                      const absl::flat_hash_set<std::string>& double_vars,
                                      const CompoundStatement& body,
                                      std::vector<std::string>* local_vars_to_declare) {
  if (frame_analysis.has_frame_pointer || frame_analysis.sp_buffer_size != 0) {
    return 0;
  }
  uint32_t local_frame_top = 0;
  uint32_t local_frame_bottom = 0;
  if (frame_analysis.prologue.min_saved_reg_offset != 0xFFFFFFFF) {
    local_frame_top = frame_analysis.prologue.min_saved_reg_offset & ~7u;
    local_frame_bottom = (static_cast<uint32_t>(frame_analysis.outgoing_args_size) + 7u) & ~7u;
  } else if (!frame_analysis.has_calls && frame_analysis.frame_size > 0) {
    local_frame_top = static_cast<uint32_t>(frame_analysis.frame_size) & ~7u;
    local_frame_bottom = 0;
  }
  if (local_frame_top <= local_frame_bottom) {
    return 0;
  }

  const uint32_t target_local_size = local_frame_top - local_frame_bottom;
  bool has_sp_buffer = false;
  bool any_non_sp_local_addr_taken = false;
  size_t sp_locals_in_gap = 0;
  absl::flat_hash_set<int32_t> non_addr_taken_sp_slots_in_gap;
  uint32_t addr_taken_sp_bytes = 0;
  int callee_saved_gprs = 0;

  for (const auto& v : *local_vars_to_declare) {
    if (v == "sp") {
      has_sp_buffer = true;
      break;
    }
    if (IsCalleeSavedGprVarName(v)) {
      callee_saved_gprs += double_vars.contains(v) ? 2 : 1;
    }
    const bool is_addr_taken = StatementTakesNonInlineAsmAddressOf(body, v, /*is_o2=*/true);
    if (absl::StartsWith(v, "var_sp_") && !absl::StartsWith(v, "var_sp_neg_")) {
      int32_t off = 0;
      if (absl::SimpleAtoi(v.substr(7), &off) && off >= static_cast<int32_t>(local_frame_bottom) &&
          off < static_cast<int32_t>(local_frame_top)) {
        ++sp_locals_in_gap;
        if (is_addr_taken) {
          addr_taken_sp_bytes += ((!absl::StartsWith(v, "var_sp_") && absl::EndsWith(v, "_64")) ||
                                  double_vars.contains(v))
                                     ? 8u
                                     : 4u;
        } else if (NonAddrTakenSpVarWillSpillInGcc(body, v)) {
          non_addr_taken_sp_slots_in_gap.insert((off - static_cast<int32_t>(local_frame_bottom)) /
                                                8);
        }
      } else if (is_addr_taken) {
        any_non_sp_local_addr_taken = true;
      }
    } else if (is_addr_taken) {
      any_non_sp_local_addr_taken = true;
    }
  }

  int prologue_saved_callee_gprs = 0;
  for (const auto& [reg, off] : frame_analysis.prologue.saved_reg_offsets) {
    (void)off;
    if ((reg >= Register::kS0 && reg <= Register::kS7) || reg == Register::kFp) {
      ++prologue_saved_callee_gprs;
    }
  }
  callee_saved_gprs = std::max(callee_saved_gprs, prologue_saved_callee_gprs);

  int prologue_saved_callee_fpr_pairs = 0;
  for (int f_num = 20; f_num <= 30; f_num += 2) {
    FpRegister fp_even = static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (f_num - 20));
    FpRegister fp_odd =
        static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (f_num + 1 - 20));
    if (frame_analysis.prologue.saved_fpr_offsets.contains(fp_even) ||
        frame_analysis.prologue.saved_fpr_offsets.contains(fp_odd)) {
      ++prologue_saved_callee_fpr_pairs;
    }
  }

  if (has_sp_buffer || any_non_sp_local_addr_taken || StatementContainsS64PointerDeref(body)) {
    return 0;
  }
  uint32_t unreferenced_sp_gap_size = 0;
  const uint32_t for_loop_spill_bytes = 8u * CountForStatements(body);
  if (sp_locals_in_gap == 0 || (frame_analysis.prologue.min_saved_reg_offset == 0xFFFFFFFF &&
                                !frame_analysis.has_calls && addr_taken_sp_bytes == 0)) {
    if (target_local_size > for_loop_spill_bytes) {
      unreferenced_sp_gap_size = target_local_size - for_loop_spill_bytes;
    }
  } else if (callee_saved_gprs >= 8 || prologue_saved_callee_fpr_pairs >= 6) {
    uint32_t max_spill_bytes = ((addr_taken_sp_bytes + 7u) & ~7u) +
                               static_cast<uint32_t>(non_addr_taken_sp_slots_in_gap.size()) * 8u +
                               for_loop_spill_bytes;
    if (addr_taken_sp_bytes > 8u && !non_addr_taken_sp_slots_in_gap.empty() &&
        callee_saved_gprs >= 9) {
      max_spill_bytes += 8u;
    }
    if (target_local_size > max_spill_bytes) {
      unreferenced_sp_gap_size = target_local_size - max_spill_bytes;
    }
  } else if (callee_saved_gprs < 8) {
    uint32_t gcc_local_bytes = ((addr_taken_sp_bytes + 7u) & ~7u) + for_loop_spill_bytes;
    if (StatementContainsMaskedSignBitFloatFix(body)) {
      gcc_local_bytes += 8u;
    }
    if (target_local_size > gcc_local_bytes) {
      unreferenced_sp_gap_size = target_local_size - gcc_local_bytes;
    }
  }
  if (unreferenced_sp_gap_size > 0) {
    local_vars_to_declare->push_back("sp");
  }
  return unreferenced_sp_gap_size;
}

void PromoteFramePointerPostIncrementStackVars(
    const std::vector<std::string>& narrow_candidates, std::unique_ptr<CompoundStatement>* body,
    absl::flat_hash_map<std::string, CType>* narrow_cond_inc_var_types) {
  for (const auto& var_name : narrow_candidates) {
    if (!absl::StartsWith(var_name, "var_sp_") || narrow_cond_inc_var_types->contains(var_name)) {
      continue;
    }
    std::optional<CType> inferred_ptr_type;
    bool valid_ptr_var = true;
    int post_inc_uses = 0;
    std::function<void(const CExpression&)> scan_expr = [&](const CExpression& e) {
      if (!valid_ptr_var) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (!un.IsPrefix() && (un.Op() == "++" || un.Op() == "--") &&
            un.Operand().Kind() == CExpressionKind::kCastExpression) {
          const auto& cast = static_cast<const CastExpression&>(un.Operand());
          if (cast.TargetType().pointer_depth == 1 &&
              cast.Operand().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(cast.Operand()).Name() == var_name) {
            if (inferred_ptr_type.has_value() &&
                inferred_ptr_type->ToString() != cast.TargetType().ToString()) {
              valid_ptr_var = false;
            } else {
              inferred_ptr_type = cast.TargetType();
              ++post_inc_uses;
            }
            return;
          }
        }
        scan_expr(un.Operand());
        return;
      }
      if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(a.Lhs()).Name() == var_name) {
          scan_expr(a.Rhs());
          return;
        }
        scan_expr(a.Lhs());
        scan_expr(a.Rhs());
        return;
      }
      if (e.Kind() == CExpressionKind::kIdentifier) {
        if (static_cast<const IdentifierExpression&>(e).Name() == var_name) {
          valid_ptr_var = false;
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& b = static_cast<const BinaryExpression&>(e);
        scan_expr(b.Lhs());
        scan_expr(b.Rhs());
        return;
      }
      if (e.Kind() == CExpressionKind::kCastExpression) {
        scan_expr(static_cast<const CastExpression&>(e).Operand());
        return;
      }
      if (e.Kind() == CExpressionKind::kCallExpression) {
        const auto& c = static_cast<const CallExpression&>(e);
        scan_expr(c.Callee());
        for (const auto& arg : c.Arguments()) {
          if (arg != nullptr) scan_expr(*arg);
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kTernaryExpression) {
        const auto& t = static_cast<const TernaryExpression&>(e);
        scan_expr(t.Condition());
        scan_expr(t.TrueExpression());
        scan_expr(t.FalseExpression());
        return;
      }
      if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        const auto& arr = static_cast<const ArrayIndexExpression&>(e);
        scan_expr(arr.Array());
        scan_expr(arr.Index());
        return;
      }
      if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        scan_expr(static_cast<const MemberAccessExpression&>(e).Object());
        return;
      }
    };
    std::function<void(const CStatement&)> scan_stmt = [&](const CStatement& s) {
      if (!valid_ptr_var) return;
      switch (s.Kind()) {
        case CStatementKind::kCompoundStatement:
          for (const auto& ch : static_cast<const CompoundStatement&>(s).Statements()) {
            if (ch != nullptr) scan_stmt(*ch);
          }
          break;
        case CStatementKind::kExpressionStatement:
          scan_expr(static_cast<const ExpressionStatement&>(s).Expression());
          break;
        case CStatementKind::kIfStatement: {
          const auto& ifs = static_cast<const IfStatement&>(s);
          scan_expr(ifs.Condition());
          scan_stmt(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) scan_stmt(*ifs.ElseBranch());
          break;
        }
        case CStatementKind::kWhileStatement: {
          const auto& ws = static_cast<const WhileStatement&>(s);
          scan_expr(ws.Condition());
          scan_stmt(ws.Body());
          break;
        }
        case CStatementKind::kDoWhileStatement: {
          const auto& dws = static_cast<const DoWhileStatement&>(s);
          scan_stmt(dws.Body());
          scan_expr(dws.Condition());
          break;
        }
        case CStatementKind::kForStatement: {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr) scan_stmt(*fs.Init());
          if (fs.Condition() != nullptr) scan_expr(*fs.Condition());
          if (fs.Step() != nullptr) scan_expr(*fs.Step());
          scan_stmt(fs.Body());
          break;
        }
        case CStatementKind::kReturnStatement: {
          const auto& rs = static_cast<const ReturnStatement&>(s);
          if (rs.ReturnValue() != nullptr) scan_expr(*rs.ReturnValue());
          break;
        }
        case CStatementKind::kSwitchStatement: {
          const auto& ss = static_cast<const SwitchStatement&>(s);
          scan_expr(ss.Condition());
          for (const auto& c : ss.Cases()) {
            if (c.body != nullptr) scan_stmt(*c.body);
          }
          break;
        }
        default:
          break;
      }
    };
    scan_stmt(**body);
    if (valid_ptr_var && post_inc_uses > 0 && inferred_ptr_type.has_value()) {
      const CType ptr_type = *inferred_ptr_type;
      std::function<std::unique_ptr<CExpression>(const CExpression&)> rewrite_expr =
          [&](const CExpression& e) -> std::unique_ptr<CExpression> {
        if (e.Kind() == CExpressionKind::kUnaryExpression) {
          const auto& un = static_cast<const UnaryExpression&>(e);
          if (!un.IsPrefix() && (un.Op() == "++" || un.Op() == "--") &&
              un.Operand().Kind() == CExpressionKind::kCastExpression) {
            const auto& cast = static_cast<const CastExpression&>(un.Operand());
            if (cast.Operand().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(cast.Operand()).Name() == var_name) {
              return CExpression::Unary(un.Op(), CExpression::Identifier(var_name),
                                        /*is_prefix=*/false);
            }
          }
          return CExpression::Unary(un.Op(), rewrite_expr(un.Operand()), un.IsPrefix());
        }
        if (e.Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(e);
          auto new_rhs = rewrite_expr(a.Rhs());
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(a.Lhs()).Name() == var_name) {
            return CExpression::Assignment("=", a.Lhs().Clone(),
                                           CExpression::Cast(ptr_type, std::move(new_rhs)));
          }
          return CExpression::Assignment(a.Op(), rewrite_expr(a.Lhs()), std::move(new_rhs));
        }
        if (e.Kind() == CExpressionKind::kBinaryExpression) {
          const auto& b = static_cast<const BinaryExpression&>(e);
          return CExpression::Binary(b.Op(), rewrite_expr(b.Lhs()), rewrite_expr(b.Rhs()));
        }
        if (e.Kind() == CExpressionKind::kCastExpression) {
          const auto& c = static_cast<const CastExpression&>(e);
          return CExpression::Cast(c.TargetType(), rewrite_expr(c.Operand()));
        }
        if (e.Kind() == CExpressionKind::kCallExpression) {
          const auto& c = static_cast<const CallExpression&>(e);
          std::vector<std::unique_ptr<CExpression>> args;
          for (const auto& arg : c.Arguments()) {
            args.push_back(arg != nullptr ? rewrite_expr(*arg) : nullptr);
          }
          return CExpression::Call(rewrite_expr(c.Callee()), std::move(args));
        }
        if (e.Kind() == CExpressionKind::kTernaryExpression) {
          const auto& t = static_cast<const TernaryExpression&>(e);
          return CExpression::Ternary(rewrite_expr(t.Condition()), rewrite_expr(t.TrueExpression()),
                                      rewrite_expr(t.FalseExpression()));
        }
        return e.Clone();
      };
      std::function<std::unique_ptr<CStatement>(const CStatement&)> rewrite_stmt =
          [&](const CStatement& s) -> std::unique_ptr<CStatement> {
        switch (s.Kind()) {
          case CStatementKind::kCompoundStatement: {
            auto comp = std::make_unique<CompoundStatement>();
            for (const auto& ch : static_cast<const CompoundStatement&>(s).Statements()) {
              if (ch != nullptr) comp->AddStatement(rewrite_stmt(*ch));
            }
            return comp;
          }
          case CStatementKind::kExpressionStatement:
            return CStatement::Expression(
                rewrite_expr(static_cast<const ExpressionStatement&>(s).Expression()));
          case CStatementKind::kIfStatement: {
            const auto& ifs = static_cast<const IfStatement&>(s);
            return CStatement::If(
                rewrite_expr(ifs.Condition()), rewrite_stmt(ifs.ThenBranch()),
                ifs.ElseBranch() != nullptr ? rewrite_stmt(*ifs.ElseBranch()) : nullptr);
          }
          case CStatementKind::kWhileStatement: {
            const auto& ws = static_cast<const WhileStatement&>(s);
            return CStatement::While(rewrite_expr(ws.Condition()), rewrite_stmt(ws.Body()));
          }
          case CStatementKind::kDoWhileStatement: {
            const auto& dws = static_cast<const DoWhileStatement&>(s);
            return CStatement::DoWhile(rewrite_stmt(dws.Body()), rewrite_expr(dws.Condition()));
          }
          case CStatementKind::kForStatement: {
            const auto& fs = static_cast<const ForStatement&>(s);
            return CStatement::For(
                fs.Init() != nullptr ? rewrite_stmt(*fs.Init()) : nullptr,
                fs.Condition() != nullptr ? rewrite_expr(*fs.Condition()) : nullptr,
                fs.Step() != nullptr ? rewrite_expr(*fs.Step()) : nullptr, rewrite_stmt(fs.Body()));
          }
          case CStatementKind::kReturnStatement: {
            const auto& rs = static_cast<const ReturnStatement&>(s);
            return CStatement::Return(rs.ReturnValue() != nullptr ? rewrite_expr(*rs.ReturnValue())
                                                                  : nullptr);
          }
          default:
            return s.Clone();
        }
      };
      *body = std::unique_ptr<CompoundStatement>(
          static_cast<CompoundStatement*>(rewrite_stmt(**body).release()));
      (*narrow_cond_inc_var_types)[var_name] = ptr_type;
    }
  }
}

bool HoistPostCallFpConstantInit(CStatement* stmt) {
  if (stmt == nullptr) return false;
  switch (stmt->Kind()) {
    case CStatementKind::kCompoundStatement: {
      auto& stmts = static_cast<CompoundStatement*>(stmt)->MutableStatements();
      bool hoisted = false;
      for (size_t i = 0; i < stmts.size(); ++i) {
        if (stmts[i] == nullptr) continue;
        if (stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
            StatementHasCall(*stmts[i]) && CountVariableReadUsesInStatement(*stmts[i], "fp") == 0 &&
            !StatementModifiesVariable(*stmts[i], "fp")) {
          for (size_t j = i + 1; j < stmts.size() && j <= i + 3; ++j) {
            if (stmts[j] == nullptr || stmts[j]->Kind() != CStatementKind::kExpressionStatement ||
                StatementHasCall(*stmts[j])) {
              break;
            }
            const auto& es = static_cast<const ExpressionStatement&>(*stmts[j]);
            if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
              const auto& assign = static_cast<const AssignmentExpression&>(es.Expression());
              if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == "fp" &&
                  assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
                auto fp_stmt = std::move(stmts[j]);
                for (size_t k = j; k > i; --k) {
                  stmts[k] = std::move(stmts[k - 1]);
                }
                stmts[i] = std::move(fp_stmt);
                i = j;
                hoisted = true;
                break;
              }
            }
            if (CountVariableReadUsesInStatement(*stmts[j], "fp") > 0 ||
                StatementModifiesVariable(*stmts[j], "fp")) {
              break;
            }
          }
        } else if (HoistPostCallFpConstantInit(stmts[i].get())) {
          hoisted = true;
        }
      }
      return hoisted;
    }
    case CStatementKind::kIfStatement: {
      auto* ifs = static_cast<IfStatement*>(stmt);
      bool hoisted = HoistPostCallFpConstantInit(ifs->MutableThenBranch());
      if (ifs->MutableElseBranch() != nullptr &&
          HoistPostCallFpConstantInit(ifs->MutableElseBranch())) {
        hoisted = true;
      }
      return hoisted;
    }
    case CStatementKind::kWhileStatement:
      return HoistPostCallFpConstantInit(static_cast<WhileStatement*>(stmt)->MutableBody());
    case CStatementKind::kDoWhileStatement:
      return HoistPostCallFpConstantInit(static_cast<DoWhileStatement*>(stmt)->MutableBody());
    case CStatementKind::kForStatement:
      return HoistPostCallFpConstantInit(static_cast<ForStatement*>(stmt)->MutableBody());
    default:
      return false;
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

  if (frame_analysis.has_frame_pointer && frame_analysis.sp_buffer_size == 0 &&
      frame_analysis.prologue.min_saved_reg_offset != 0xFFFFFFFF) {
    const uint32_t local_frame_top = frame_analysis.prologue.min_saved_reg_offset & ~7u;
    const uint32_t local_frame_bottom =
        frame_analysis.has_calls
            ? ((static_cast<uint32_t>(frame_analysis.outgoing_args_size) + 7u) & ~7u)
            : 0u;
    if (local_frame_top > local_frame_bottom) {
      const uint32_t target_local_size = local_frame_top - local_frame_bottom;
      const bool is_leaf = !StatementHasCall(**body);
      absl::flat_hash_set<int32_t> covered_sp_offsets;
      int32_t max_referenced_sp_end = 0;
      uint32_t non_reg_local_bytes = 0;
      for (const auto& v : local_vars_to_declare) {
        if (v == "sp") continue;
        const uint32_t var_bytes = ((!absl::StartsWith(v, "var_sp_") && absl::EndsWith(v, "_64")) ||
                                    double_vars.contains(v))
                                       ? 8u
                                       : 4u;
        if (absl::StartsWith(v, "var_sp_") && !absl::StartsWith(v, "var_sp_neg_")) {
          int32_t off = 0;
          std::string_view sv = v;
          if (absl::SimpleAtoi(sv.substr(7), &off) &&
              off >= static_cast<int32_t>(local_frame_bottom) &&
              off < static_cast<int32_t>(local_frame_top)) {
            covered_sp_offsets.insert(off);
            if (var_bytes == 8u) {
              covered_sp_offsets.insert(off + 4);
            }
            max_referenced_sp_end =
                std::max(max_referenced_sp_end, off + static_cast<int32_t>(var_bytes));
          }
          non_reg_local_bytes += var_bytes;
          continue;
        }
        if (StatementTakesAddressOf(**body, v)) {
          non_reg_local_bytes += var_bytes;
          continue;
        }
        if (is_leaf) {
          continue;
        }
        bool is_saved_callee_reg = false;
        if (v.size() == 7 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7') {
          Register s_reg = static_cast<Register>(static_cast<int>(Register::kS0) + (v[6] - '0'));
          is_saved_callee_reg = frame_analysis.prologue.saved_reg_offsets.contains(s_reg);
        } else {
          std::string_view base =
              absl::EndsWith(v, "_d") ? std::string_view{v}.substr(0, v.size() - 2) : v;
          int fpr_num = 0;
          if (IsCalleeSavedFprBaseName(base) && absl::SimpleAtoi(base.substr(1), &fpr_num)) {
            FpRegister fp_reg =
                static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (fpr_num - 20));
            is_saved_callee_reg = frame_analysis.prologue.saved_fpr_offsets.contains(fp_reg);
          }
        }
        if (!is_saved_callee_reg) {
          non_reg_local_bytes += var_bytes;
        }
      }
      if (StatementContainsNonPowerOfTwoConstDivMod(**body)) {
        non_reg_local_bytes += 4u;
      }
      for (uint32_t off = local_frame_bottom;
           off < local_frame_top && non_reg_local_bytes + 4u <= target_local_size; off += 4u) {
        if (covered_sp_offsets.contains(static_cast<int32_t>(off))) {
          continue;
        }
        if (static_cast<int32_t>(off) < max_referenced_sp_end ||
            ((non_reg_local_bytes + 7u) & ~7u) < target_local_size) {
          local_vars_to_declare.push_back(absl::StrFormat("var_sp_%d", off));
          covered_sp_offsets.insert(static_cast<int32_t>(off));
          non_reg_local_bytes += 4u;
        }
      }
    }
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
    if (frame_analysis.has_frame_pointer && !sp_vars.empty()) {
      std::sort(sp_vars.begin(), sp_vars.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      std::vector<std::string> reordered;
      reordered.reserve(local_vars_to_declare.size());
      for (const auto& [off, name] : sp_vars) {
        (void)off;
        reordered.push_back(name);
      }
      for (const auto& name : local_vars_to_declare) {
        int64_t off = 0;
        std::string_view sv = name;
        if (absl::StartsWith(sv, "var_sp_") && absl::SimpleAtoi(sv.substr(7), &off)) {
          continue;
        }
        reordered.push_back(name);
      }
      local_vars_to_declare = std::move(reordered);
    } else if (sp_vars.size() >= 2) {
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

  const uint32_t unreferenced_sp_gap_size =
      ComputeUnreferencedSpGapSize(frame_analysis, double_vars, **body, &local_vars_to_declare);

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
  if (unreferenced_sp_gap_size == 0 && !frame_analysis.has_frame_pointer && sp_buffer_size > 0) {
    uint32_t spill_bytes = 8u * CountForStatements(**body);
    int prologue_saved_callee_fpr_pairs = 0;
    for (int f_num = 20; f_num <= 30; f_num += 2) {
      FpRegister fp_even =
          static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (f_num - 20));
      FpRegister fp_odd =
          static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (f_num + 1 - 20));
      if (frame_analysis.prologue.saved_fpr_offsets.contains(fp_even) ||
          frame_analysis.prologue.saved_fpr_offsets.contains(fp_odd)) {
        ++prologue_saved_callee_fpr_pairs;
      }
    }
    if (prologue_saved_callee_fpr_pairs >= 6 &&
        (sp_buffer_size > 64 ||
         std::find(local_vars_to_declare.begin(), local_vars_to_declare.end(), "f18") !=
             local_vars_to_declare.end())) {
      for (const auto& v : local_vars_to_declare) {
        if (absl::EndsWith(v, "_f")) {
          spill_bytes += 8u;
        }
      }
    }
    const uint32_t min_accessed_bytes =
        (static_cast<uint32_t>(std::max<int32_t>(0, frame_analysis.max_sp_access)) + 7u) & ~7u;
    if (spill_bytes > 0 && sp_buffer_size > spill_bytes &&
        sp_buffer_size >= min_accessed_bytes + spill_bytes) {
      sp_buffer_size -= spill_bytes;
    }
  }

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
    return StatementTakesNonInlineAsmAddressOf(**body, target_var,
                                               !frame_analysis.has_frame_pointer);
  };

  absl::flat_hash_map<std::string, int> fp_base_counts;
  for (const auto& vn : local_vars_to_declare) {
    if (is_fp_reg(vn)) {
      std::string base = absl::EndsWith(vn, "_d") ? vn.substr(0, vn.size() - 2) : vn;
      ++fp_base_counts[base];
    }
  }

  const bool is_leaf_func = !StatementHasCall(**body);
  const bool has_s64_pointer_deref = StatementContainsS64PointerDeref(**body);
  const bool returns_fp = IsFloatingPointReturnType(return_type);

  auto is_caller_saved_safe_across_calls = [&](std::string_view r) -> bool {
    return IsCallerSavedSafeAcrossCalls(**body, is_leaf_func, r);
  };

  if (!frame_analysis.has_frame_pointer && !is_leaf_func &&
      frame_analysis.prologue.saved_reg_offsets.contains(Register::kFp) &&
      std::find(local_vars_to_declare.begin(), local_vars_to_declare.end(), "fp") !=
          local_vars_to_declare.end() &&
      is_caller_saved_safe_across_calls("fp")) {
    HoistPostCallFpConstantInit(body->get());
  }

  // MIPS O32 register file capacities and GCC 2.7.2 reload reserve margins:
  // - 16 even FPRs total ($f0..$f30); reserve 4 even FPRs for expression evaluation / conversions.
  // - 16 caller-saved GPRs total ($v0..$v1, $a0..$a3, $t0..$t9); reserve 2 GPRs for reloads.
  // - 18 allocatable GPRs total before hard-register pinning risks spilling fixed registers.
  constexpr int kTotalEvenFprs = 16;
  constexpr int kReservedScratchEvenFprs = 4;
  constexpr int kMaxPinnedEvenFprs = kTotalEvenFprs - kReservedScratchEvenFprs;
  constexpr int kTotalCallerSavedGprs = 16;
  constexpr int kReservedScratchGprs = 2;
  constexpr int kMaxCallerGprPins = kTotalCallerSavedGprs - kReservedScratchGprs;
  constexpr size_t kMaxUnpinnedGprLocalsBeforeSpillRisk = 18;

  size_t unpinned_gpr_locals_count = 0;
  int callee_saved_gpr_count = 0;
  bool has_v0_local = false;
  for (const auto& v : local_vars_to_declare) {
    if (v == "v0") has_v0_local = true;
    if (IsCalleeSavedGprVarName(v)) {
      callee_saved_gpr_count += double_vars.contains(v) ? 2 : 1;
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

  const bool v0_reused_as_working_temp =
      has_v0_local && CountDirectAssignmentsInStatement(**body, "v0") >= 2 &&
      CountNonReturnVariableReadUsesInStatement(**body, "v0") >= 1;

  absl::flat_hash_set<std::string> float_gpr_var_names;
  for (Register r : AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg)) {
    float_gpr_var_names.insert(ExpressionBuilder::RegisterVarName(r));
  }
  for (const auto& v : local_vars_to_declare) {
    if (v.size() == 9 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7' &&
        absl::EndsWith(v, "_f")) {
      float_gpr_var_names.insert(v);
    }
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

  const bool has_callee_saved_gprs = (callee_saved_gpr_count >= 1);
  const bool has_callee_saved_fprs = (callee_saved_fp_count >= 1);

  auto can_pin_abi_fpr_base = [&](std::string_view base) -> bool {
    if (!IsAbiArgOrReturnFprBaseName(base) || (is_leaf_func && !has_callee_saved_fprs)) {
      return false;
    }
    return base == "f12" || base == "f14" || (base == "f0" && !returns_fp);
  };

  absl::flat_hash_set<std::string> unpinned_s_vars;
  if (!is_leaf_func && frame_analysis.frame_size > 0 &&
      !(sp_buffer_size == 0 && has_s64_pointer_deref)) {
    const auto& body_stmts = (*body)->Statements();
    for (size_t i = 0; i < body_stmts.size(); ++i) {
      if (body_stmts[i] == nullptr) continue;
      if (StatementHasCall(*body_stmts[i]) ||
          body_stmts[i]->Kind() != CStatementKind::kExpressionStatement) {
        break;
      }
      const auto& es = static_cast<const ExpressionStatement&>(*body_stmts[i]);
      if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) continue;
      const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
          a.Rhs().Kind() == CExpressionKind::kIdentifier) {
        std::string_view lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
        std::string_view rhs = static_cast<const IdentifierExpression&>(a.Rhs()).Name();
        if (lhs.size() == 7 && absl::StartsWith(lhs, "temp_s") && lhs[6] >= '0' && lhs[6] <= '7' &&
            rhs.size() >= 4 && absl::StartsWith(rhs, "arg") &&
            CountDirectAssignmentsInStatement(**body, lhs) == 1 &&
            !StatementModifiesVariable(**body, rhs) && !has_address_taken(rhs)) {
          bool all_lower_s_unpinned = true;
          int s_idx = lhs[6] - '0';
          for (int k = 0; k < s_idx; ++k) {
            if (!unpinned_s_vars.contains(absl::StrCat("temp_s", k))) {
              all_lower_s_unpinned = false;
              break;
            }
          }
          if (all_lower_s_unpinned) {
            unpinned_s_vars.insert(std::string(lhs));
          }
        }
      }
    }
  }

  std::vector<std::string> narrow_candidates;
  narrow_candidates.reserve(local_vars_to_declare.size());
  for (const auto& var_name : local_vars_to_declare) {
    if (var_name == "sp" || is_fp_reg(var_name) || float_gpr_var_names.contains(var_name) ||
        float_local_var_names.contains(var_name) ||
        (!absl::StartsWith(var_name, "var_sp_") && absl::EndsWith(var_name, "_64")) ||
        s64_local_var_names.contains(var_name) || double_vars.contains(var_name) ||
        has_address_taken(var_name)) {
      continue;
    }
    narrow_candidates.push_back(var_name);
  }
  absl::flat_hash_map<std::string, CType> narrow_cond_inc_var_types =
      InferAndPromoteNarrowCondSteppedVars(narrow_candidates, body);
  if (frame_analysis.has_frame_pointer) {
    PromoteFramePointerPostIncrementStackVars(narrow_candidates, body, &narrow_cond_inc_var_types);
  }
  bool has_narrow_temp_t = false;
  for (const auto& [k, _] : narrow_cond_inc_var_types) {
    if (k.size() == 7 && absl::StartsWith(k, "temp_t") && k[6] >= '0' && k[6] <= '9') {
      has_narrow_temp_t = true;
      break;
    }
  }

  auto get_callee_saved_register_annotation =
      [&](std::string_view var_name) -> std::optional<std::string> {
    if (has_address_taken(var_name)) {
      return std::nullopt;
    }
    if (frame_analysis.has_frame_pointer) {
      if (absl::StartsWith(var_name, "var_sp_") || var_name == "sp") {
        return std::nullopt;
      }
      if (is_leaf_func) {
        return std::string("");
      }
      if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_s") && var_name[6] >= '0' &&
          var_name[6] <= '7') {
        Register s_reg =
            static_cast<Register>(static_cast<int>(Register::kS0) + (var_name[6] - '0'));
        if (frame_analysis.prologue.saved_reg_offsets.contains(s_reg)) {
          return std::string("");
        }
      }
      if (is_fp_reg(var_name)) {
        std::string_view base =
            absl::EndsWith(var_name, "_d") ? var_name.substr(0, var_name.size() - 2) : var_name;
        if (IsCalleeSavedFprBaseName(base) && !pinned_fp_bases.contains(std::string(base))) {
          int fpr_num = 0;
          if (absl::SimpleAtoi(base.substr(1), &fpr_num) &&
              frame_analysis.prologue.saved_fpr_offsets.contains(
                  static_cast<FpRegister>(static_cast<int>(FpRegister::kF20) + (fpr_num - 20)))) {
            pinned_fp_bases.insert(std::string(base));
            return std::string("");
          }
        }
      }
      return std::nullopt;
    }
    if ((var_name == "temp_k0" || var_name == "temp_k1") && !has_s64_pointer_deref) {
      return (var_name == "temp_k0") ? "$26" : "$27";
    }
    if ((var_name.size() == 7 || (var_name.size() == 9 && absl::EndsWith(var_name, "_f"))) &&
        absl::StartsWith(var_name, "temp_s") && var_name[6] >= '0' && var_name[6] <= '7') {
      if (var_name.size() == 9) {
        std::string gpr_counterpart(var_name.substr(0, 7));
        if (std::find(local_vars_to_declare.begin(), local_vars_to_declare.end(),
                      gpr_counterpart) != local_vars_to_declare.end()) {
          return std::nullopt;
        }
      }
      Register s_reg = static_cast<Register>(static_cast<int>(Register::kS0) + (var_name[6] - '0'));
      if (frame_analysis.frame_size == 0 || (sp_buffer_size == 0 && has_s64_pointer_deref) ||
          unpinned_s_vars.contains(std::string(var_name.substr(0, 7))) ||
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
    if (!is_leaf_func && CountVariableReadUsesInStatement(**body, var_name) == 0) {
      return std::nullopt;
    }
    std::optional<std::string> caller_gpr_reg;
    const bool leaf_returns_non_v0_with_working_v0 =
        is_leaf_func && !has_callee_saved_gprs && v0_reused_as_working_temp &&
        return_type.ToString() != "void" && !returns_fp &&
        CountNonReturnVariableReadUsesInStatement(**body, "v0") ==
            CountVariableReadUsesInStatement(**body, "v0") &&
        StatementHasLoopConditionUsingVar(**body, "v0") &&
        !IsAssignedCompoundBinaryBothSides(**body, "v0");
    if (var_name == "v0" &&
        ((is_leaf_func && has_callee_saved_gprs) || (!is_leaf_func && v0_reused_as_working_temp) ||
         leaf_returns_non_v0_with_working_v0)) {
      caller_gpr_reg = "$2";
    } else if (var_name == "v1" && (!is_leaf_func || has_callee_saved_gprs)) {
      caller_gpr_reg = "$3";
    } else if (var_name.size() == 4 && absl::StartsWith(var_name, "arg") && var_name[3] >= '0' &&
               var_name[3] <= '3' && (!is_leaf_func || has_callee_saved_gprs) &&
               (!any_param_modified || (is_leaf_func && has_callee_saved_gprs) ||
                (!is_leaf_func &&
                 CountNonReturnVariableReadUsesInStatement(**body, var_name) == 0 &&
                 CountVariableReadUsesInStatement(**body, var_name) >= 1))) {
      int reg_num = 4 + (var_name[3] - '0');
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    } else if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_a") && var_name[6] >= '0' &&
               var_name[6] <= '3' &&
               !(is_leaf_func && !has_callee_saved_gprs && has_narrow_temp_t) &&
               (!is_leaf_func || has_callee_saved_gprs ||
                (!DoesScratchArgInterfereWithEntryParam(**body, var_name) &&
                 !IsAssignedCompoundBinaryBothSides(**body, var_name)))) {
      int reg_num = 4 + (var_name[6] - '0');
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    } else if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_t") && var_name[6] >= '0' &&
               var_name[6] <= '9' && !narrow_cond_inc_var_types.contains(std::string(var_name))) {
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

  absl::flat_hash_map<std::string, std::optional<std::string>> reg_annotations;
  std::vector<std::string> unpinned_scratch_candidates;
  for (const auto& var_name : local_vars_to_declare) {
    if (var_name == "sp") {
      continue;
    }
    if (is_fp_reg(var_name) || float_gpr_var_names.contains(var_name)) {
      reg_annotations[var_name] = get_callee_saved_register_annotation(var_name);
      continue;
    }
    if (float_local_var_names.contains(var_name) ||
        (!absl::StartsWith(var_name, "var_sp_") && absl::EndsWith(var_name, "_64")) ||
        (s64_local_var_names.contains(var_name) && !double_vars.contains(var_name))) {
      continue;
    }
    if (double_vars.contains(var_name)) {
      reg_annotations[var_name] = get_callee_saved_register_annotation(var_name);
      continue;
    }
    bool is_callee_gpr = IsCalleeSavedGprVarName(var_name);
    bool needs_init = vars_needing_init.contains(var_name);
    std::optional<std::string> reg_annot =
        (!needs_init || is_callee_gpr || frame_analysis.has_frame_pointer)
            ? get_callee_saved_register_annotation(var_name)
            : std::nullopt;
    reg_annotations[var_name] = reg_annot;
    if (!frame_analysis.has_frame_pointer && !reg_annot.has_value() && !needs_init &&
        !is_callee_gpr && !has_address_taken(var_name) &&
        !(has_narrow_temp_t && narrow_cond_inc_var_types.contains(var_name)) &&
        (var_name == "v0" || var_name == "v1" || var_name == "temp_at" ||
         (var_name.size() == 7 &&
          (absl::StartsWith(var_name, "temp_t") || absl::StartsWith(var_name, "temp_a"))))) {
      unpinned_scratch_candidates.push_back(var_name);
    }
  }

  if (!unpinned_scratch_candidates.empty()) {
    absl::flat_hash_map<std::string, std::vector<std::string>> split_vars_map;
    AstPassContext split_ctx{
        .parameters = parameters,
        .frame_size = static_cast<uint32_t>(frame_analysis.frame_size),
        .has_frame_pointer = frame_analysis.has_frame_pointer,
        .returns_v0 = (return_type.ToString() != "void" && !returns_fp),
        .returns_f0 = returns_fp,
        .return_type = return_type,
    };
    ScratchRegisterWebSplitterPass splitter(unpinned_scratch_candidates, &split_vars_map);
    if (splitter.Run(body->get(), split_ctx) && !split_vars_map.empty()) {
      std::vector<std::string> expanded_local_vars;
      expanded_local_vars.reserve(local_vars_to_declare.size() + split_vars_map.size() * 2);
      for (const auto& var_name : local_vars_to_declare) {
        expanded_local_vars.push_back(var_name);
        auto it = split_vars_map.find(var_name);
        if (it != split_vars_map.end()) {
          auto narrow_it = narrow_cond_inc_var_types.find(var_name);
          for (const auto& new_var : it->second) {
            expanded_local_vars.push_back(new_var);
            if (narrow_it != narrow_cond_inc_var_types.end()) {
              narrow_cond_inc_var_types[new_var] = narrow_it->second;
            }
          }
        }
      }
      local_vars_to_declare = std::move(expanded_local_vars);
    }
  }

  auto new_body = std::make_unique<CompoundStatement>();
  for (const auto& var_name : local_vars_to_declare) {
    if (var_name == "sp") {
      new_body->AddStatement(
          CStatement::VariableDeclaration(CType::U8(), "sp", nullptr, sp_buffer_size));
    } else if (is_fp_reg(var_name)) {
      std::optional<std::string> reg_annot = reg_annotations[var_name];
      if (absl::EndsWith(var_name, "_d") || double_vars.contains(var_name)) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name, nullptr,
                                                               std::nullopt, std::move(reg_annot)));
      } else {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name, nullptr,
                                                               std::nullopt, std::move(reg_annot)));
      }
    } else if (float_gpr_var_names.contains(var_name)) {
      std::optional<std::string> reg_annot = reg_annotations[var_name];
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name, nullptr,
                                                             std::nullopt, std::move(reg_annot)));
    } else if (float_local_var_names.contains(var_name)) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name));
    } else if ((!absl::StartsWith(var_name, "var_sp_") && absl::EndsWith(var_name, "_64")) ||
               (s64_local_var_names.contains(var_name) && !double_vars.contains(var_name))) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::Named("s64"), var_name));
    } else if (double_vars.contains(var_name)) {
      std::optional<std::string> reg_annot = reg_annotations[var_name];
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name, nullptr,
                                                             std::nullopt, std::move(reg_annot)));
    } else {
      bool is_callee_gpr = IsCalleeSavedGprVarName(var_name);
      auto init_expr = vars_needing_init.contains(var_name) ? CExpression::Integer(0) : nullptr;
      std::optional<std::string> reg_annot = reg_annotations[var_name];
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
