#include "lifter/c_emitter.h"

#include <unistd.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "core/c_ast.h"
#include "core/process.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

namespace {

struct MathIntrinsic {
  std::string_view name;
  std::string_view type;  // "f32" or "f64"
  std::string_view asm_inst;
};

constexpr MathIntrinsic kMathIntrinsics[] = {
    {"N64Sqrtf", "f32", "sqrt.s %0, %1"},
    {"N64Sqrt", "f64", "sqrt.d %0, %1"},
    {"sqrtf", "f32", "sqrt.s %0, %1"},
    {"sqrt", "f64", "sqrt.d %0, %1"},
    {"fabsf", "f32", "abs.s %0, %1"},
    {"fabs", "f64", "abs.d %0, %1"},
    {"ceilf", "f32", "ceil.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"floorf", "f32", "floor.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"roundf", "f32", "round.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"truncf", "f32", "trunc.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"ceil", "f64", "ceil.w.d %0, %1\n\tcvt.d.w %0, %0"},
    {"floor", "f64", "floor.w.d %0, %1\n\tcvt.d.w %0, %0"},
    {"round", "f64", "round.w.d %0, %1\n\tcvt.d.w %0, %0"},
    {"trunc", "f64", "trunc.w.d %0, %1\n\tcvt.d.w %0, %0"},
};

bool IsCompilerIntrinsic(std::string_view name) {
  return absl::StartsWith(name, "__n64_") || absl::StartsWith(name, "__builtin_");
}

void CollectCustomUnalignedBlockSizes(std::string_view code, std::set<int>* out_sizes) {
  constexpr std::string_view kPrefix = "UnalignedBlock";
  size_t pos = 0;
  while ((pos = code.find(kPrefix, pos)) != std::string_view::npos) {
    pos += kPrefix.size();
    size_t end = pos;
    while (end < code.size() && std::isdigit(static_cast<unsigned char>(code[end]))) {
      ++end;
    }
    if (end > pos) {
      int block_size = 0;
      if (absl::SimpleAtoi(code.substr(pos, end - pos), &block_size) && block_size > 0 &&
          block_size != 4 && block_size != 6 && block_size != 8 && block_size != 12 &&
          block_size != 16) {
        out_sizes->insert(block_size);
      }
    }
    pos = end;
  }
}

std::optional<std::pair<std::string, CType>> MatchScalarCastDerefGlobal(
    const CExpression& expr, const std::set<std::string>& local_vars) {
  if (expr.Kind() != CExpressionKind::kUnaryExpression) return std::nullopt;
  const auto& deref = static_cast<const UnaryExpression&>(expr);
  if (!deref.IsPrefix() || deref.Op() != "*") return std::nullopt;
  if (deref.Operand().Kind() != CExpressionKind::kCastExpression) return std::nullopt;
  const auto& cast = static_cast<const CastExpression&>(deref.Operand());
  std::string target_type_str = cast.TargetType().ToString();
  CType scalar_type = CType::S32();
  if (target_type_str == "u8*") {
    scalar_type = CType::U8();
  } else if (target_type_str == "s8*") {
    scalar_type = CType::S8();
  } else if (target_type_str == "u16*") {
    scalar_type = CType::U16();
  } else if (target_type_str == "s16*") {
    scalar_type = CType::S16();
  } else if (target_type_str == "f32*") {
    scalar_type = CType::F32();
  } else if (target_type_str == "f64*") {
    scalar_type = CType::F64();
  } else {
    return std::nullopt;
  }
  if (cast.Operand().Kind() != CExpressionKind::kUnaryExpression) return std::nullopt;
  const auto& addr_of = static_cast<const UnaryExpression&>(cast.Operand());
  if (!addr_of.IsPrefix() || addr_of.Op() != "&") return std::nullopt;
  if (addr_of.Operand().Kind() != CExpressionKind::kIdentifier) return std::nullopt;
  const auto& id = static_cast<const IdentifierExpression&>(addr_of.Operand());
  if (local_vars.count(id.Name()) > 0 || id.IsArraySymbol()) return std::nullopt;
  return std::make_pair(id.Name(), scalar_type);
}

void CollectGlobalCastDerefsInExpr(
    const CExpression& expr, const std::set<std::string>& local_vars,
    std::map<std::string, int>* total_id_uses,
    std::map<std::string, std::map<std::string, std::pair<CType, int>>>* cast_deref_uses) {
  if (auto matched = MatchScalarCastDerefGlobal(expr, local_vars); matched.has_value()) {
    const std::string& sym = matched->first;
    const CType& sc_type = matched->second;
    (*total_id_uses)[sym]++;
    auto& entry = (*cast_deref_uses)[sym][sc_type.ToString()];
    entry.first = sc_type;
    entry.second++;
    return;
  }
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      if (local_vars.count(id.Name()) == 0) {
        (*total_id_uses)[id.Name()]++;
      }
      break;
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.IsPrefix() && u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
        break;
      }
      CollectGlobalCastDerefsInExpr(u.Operand(), local_vars, total_id_uses, cast_deref_uses);
      break;
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      if (b.Op() == "+" || b.Op() == "-") {
        auto check_uncast_addr_of = [&](const CExpression& side) {
          if (side.Kind() == CExpressionKind::kUnaryExpression) {
            const auto& u = static_cast<const UnaryExpression&>(side);
            if (u.IsPrefix() && u.Op() == "&" &&
                u.Operand().Kind() == CExpressionKind::kIdentifier) {
              const auto& id = static_cast<const IdentifierExpression&>(u.Operand());
              if (local_vars.count(id.Name()) == 0) {
                (*total_id_uses)[id.Name()]++;
              }
            }
          }
        };
        check_uncast_addr_of(b.Lhs());
        check_uncast_addr_of(b.Rhs());
      }
      CollectGlobalCastDerefsInExpr(b.Lhs(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInExpr(b.Rhs(), local_vars, total_id_uses, cast_deref_uses);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      CollectGlobalCastDerefsInExpr(a.Lhs(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInExpr(a.Rhs(), local_vars, total_id_uses, cast_deref_uses);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      CollectGlobalCastDerefsInExpr(c.Callee(), local_vars, total_id_uses, cast_deref_uses);
      for (const auto& arg : c.Arguments()) {
        if (arg != nullptr) {
          CollectGlobalCastDerefsInExpr(*arg, local_vars, total_id_uses, cast_deref_uses);
        }
      }
      break;
    }
    case CExpressionKind::kCastExpression:
      CollectGlobalCastDerefsInExpr(static_cast<const CastExpression&>(expr).Operand(), local_vars,
                                    total_id_uses, cast_deref_uses);
      break;
    case CExpressionKind::kMemberAccessExpression:
      CollectGlobalCastDerefsInExpr(static_cast<const MemberAccessExpression&>(expr).Object(),
                                    local_vars, total_id_uses, cast_deref_uses);
      break;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      CollectGlobalCastDerefsInExpr(a.Array(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInExpr(a.Index(), local_vars, total_id_uses, cast_deref_uses);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      CollectGlobalCastDerefsInExpr(t.Condition(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInExpr(t.TrueExpression(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInExpr(t.FalseExpression(), local_vars, total_id_uses,
                                    cast_deref_uses);
      break;
    }
    default:
      break;
  }
}

void CollectGlobalCastDerefsInStmt(
    const CStatement& stmt, const std::set<std::string>& local_vars,
    std::map<std::string, int>* total_id_uses,
    std::map<std::string, std::map<std::string, std::pair<CType, int>>>* cast_deref_uses) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr) {
          CollectGlobalCastDerefsInStmt(*s, local_vars, total_id_uses, cast_deref_uses);
        }
      }
      break;
    case CStatementKind::kExpressionStatement:
      CollectGlobalCastDerefsInExpr(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                    local_vars, total_id_uses, cast_deref_uses);
      break;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      if (vd.Initializer() != nullptr) {
        CollectGlobalCastDerefsInExpr(*vd.Initializer(), local_vars, total_id_uses,
                                      cast_deref_uses);
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      CollectGlobalCastDerefsInExpr(ifs.Condition(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInStmt(ifs.ThenBranch(), local_vars, total_id_uses, cast_deref_uses);
      if (ifs.ElseBranch() != nullptr) {
        CollectGlobalCastDerefsInStmt(*ifs.ElseBranch(), local_vars, total_id_uses,
                                      cast_deref_uses);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      CollectGlobalCastDerefsInExpr(ws.Condition(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInStmt(ws.Body(), local_vars, total_id_uses, cast_deref_uses);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      CollectGlobalCastDerefsInStmt(dws.Body(), local_vars, total_id_uses, cast_deref_uses);
      CollectGlobalCastDerefsInExpr(dws.Condition(), local_vars, total_id_uses, cast_deref_uses);
      break;
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr) {
        CollectGlobalCastDerefsInStmt(*fs.Init(), local_vars, total_id_uses, cast_deref_uses);
      }
      if (fs.Condition() != nullptr) {
        CollectGlobalCastDerefsInExpr(*fs.Condition(), local_vars, total_id_uses, cast_deref_uses);
      }
      if (fs.Step() != nullptr) {
        CollectGlobalCastDerefsInExpr(*fs.Step(), local_vars, total_id_uses, cast_deref_uses);
      }
      CollectGlobalCastDerefsInStmt(fs.Body(), local_vars, total_id_uses, cast_deref_uses);
      break;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr) {
        CollectGlobalCastDerefsInExpr(*rs.ReturnValue(), local_vars, total_id_uses,
                                      cast_deref_uses);
      }
      break;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      CollectGlobalCastDerefsInExpr(ss.Condition(), local_vars, total_id_uses, cast_deref_uses);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) {
          CollectGlobalCastDerefsInStmt(*c.body, local_vars, total_id_uses, cast_deref_uses);
        }
      }
      break;
    }
    default:
      break;
  }
}

std::unique_ptr<CExpression> NormalizeArraySymbolsInExpr(
    const CExpression& expr, const std::set<std::string>& array_symbols,
    const std::set<std::string>& local_vars, const std::map<std::string, CType>& module_data_types,
    const std::set<std::string>& external_function_names,
    const std::map<std::string, CType>& promoted_scalar_globals = {}) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      if (local_vars.count(id.Name()) == 0) {
        if (!id.IsArraySymbol()) {
          bool is_func = external_function_names.count(id.Name()) > 0;
          auto type_it = module_data_types.find(id.Name());
          if (type_it != module_data_types.end() || is_func) {
            CType accessed_type = id.SymbolType().value_or(CType::S32());
            if (is_func || type_it->second.ToString() != accessed_type.ToString()) {
              std::unique_ptr<CExpression> addr =
                  (!is_func && array_symbols.count(id.Name()) > 0)
                      ? CExpression::Identifier(id.Name())
                      : CExpression::Unary("&", CExpression::Identifier(id.Name()));
              return CExpression::Unary(
                  "*", CExpression::Cast(accessed_type.MakePointer(), std::move(addr)));
            }
          }
        }
        if (array_symbols.count(id.Name()) > 0) {
          return CExpression::ArrayIndex(CExpression::Identifier(id.Name()),
                                         CExpression::Integer(0));
        }
      }
      return expr.Clone();
    }
    case CExpressionKind::kUnaryExpression: {
      if (auto matched = MatchScalarCastDerefGlobal(expr, local_vars); matched.has_value()) {
        auto prom_it = promoted_scalar_globals.find(matched->first);
        if (prom_it != promoted_scalar_globals.end() &&
            prom_it->second.ToString() == matched->second.ToString()) {
          return std::make_unique<IdentifierExpression>(matched->first, matched->second);
        }
      }
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.IsPrefix() && u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
        return expr.Clone();
      }
      return CExpression::Unary(
          u.Op(),
          NormalizeArraySymbolsInExpr(u.Operand(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          u.IsPrefix());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      std::unique_ptr<CExpression> new_idx =
          NormalizeArraySymbolsInExpr(a.Index(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals);
      if (a.Array().Kind() == CExpressionKind::kIdentifier) {
        const auto& arr_id = static_cast<const IdentifierExpression&>(a.Array());
        if (local_vars.count(arr_id.Name()) == 0 && arr_id.SymbolType().has_value()) {
          bool is_func = external_function_names.count(arr_id.Name()) > 0;
          auto type_it = module_data_types.find(arr_id.Name());
          bool type_conflict =
              is_func || (type_it != module_data_types.end() &&
                          type_it->second.ToString() != arr_id.SymbolType()->ToString());
          if (type_conflict) {
            std::unique_ptr<CExpression> addr =
                (!is_func && array_symbols.count(arr_id.Name()) > 0)
                    ? CExpression::Identifier(arr_id.Name())
                    : CExpression::Unary("&", CExpression::Identifier(arr_id.Name()));
            return CExpression::ArrayIndex(
                CExpression::Cast(arr_id.SymbolType()->MakePointer(), std::move(addr)),
                std::move(new_idx));
          }
        }
        return CExpression::ArrayIndex(a.Array().Clone(), std::move(new_idx));
      }
      return CExpression::ArrayIndex(
          NormalizeArraySymbolsInExpr(a.Array(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          std::move(new_idx));
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(
          b.Op(),
          NormalizeArraySymbolsInExpr(b.Lhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          NormalizeArraySymbolsInExpr(b.Rhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return CExpression::Assignment(
          a.Op(),
          NormalizeArraySymbolsInExpr(a.Lhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          NormalizeArraySymbolsInExpr(a.Rhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals));
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        args.push_back(NormalizeArraySymbolsInExpr(*arg, array_symbols, local_vars,
                                                   module_data_types, external_function_names,
                                                   promoted_scalar_globals));
      }
      std::unique_ptr<CExpression> new_callee =
          c.Callee().Kind() == CExpressionKind::kIdentifier
              ? c.Callee().Clone()
              : NormalizeArraySymbolsInExpr(c.Callee(), array_symbols, local_vars,
                                            module_data_types, external_function_names,
                                            promoted_scalar_globals);
      return CExpression::Call(std::move(new_callee), std::move(args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(
          c.TargetType(),
          NormalizeArraySymbolsInExpr(c.Operand(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(
          NormalizeArraySymbolsInExpr(m.Object(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          m.MemberName(), m.IsArrow());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          NormalizeArraySymbolsInExpr(t.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          NormalizeArraySymbolsInExpr(t.TrueExpression(), array_symbols, local_vars,
                                      module_data_types, external_function_names,
                                      promoted_scalar_globals),
          NormalizeArraySymbolsInExpr(t.FalseExpression(), array_symbols, local_vars,
                                      module_data_types, external_function_names,
                                      promoted_scalar_globals));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> NormalizeArraySymbolsInStmt(
    const CStatement& stmt, const std::set<std::string>& array_symbols,
    const std::set<std::string>& local_vars, const std::map<std::string, CType>& module_data_types,
    const std::set<std::string>& external_function_names,
    const std::map<std::string, CType>& promoted_scalar_globals = {}) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      std::vector<std::unique_ptr<CStatement>> stmts;
      stmts.reserve(comp.Statements().size());
      for (const auto& s : comp.Statements()) {
        stmts.push_back(NormalizeArraySymbolsInStmt(*s, array_symbols, local_vars,
                                                    module_data_types, external_function_names,
                                                    promoted_scalar_globals));
      }
      return std::make_unique<CompoundStatement>(std::move(stmts));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(NormalizeArraySymbolsInExpr(
          static_cast<const ExpressionStatement&>(stmt).Expression(), array_symbols, local_vars,
          module_data_types, external_function_names, promoted_scalar_globals));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      std::unique_ptr<CExpression> init =
          vd.Initializer() != nullptr
              ? NormalizeArraySymbolsInExpr(*vd.Initializer(), array_symbols, local_vars,
                                            module_data_types, external_function_names,
                                            promoted_scalar_globals)
              : nullptr;
      return CStatement::VariableDeclaration(vd.Type(), vd.Name(), std::move(init), vd.ArraySize(),
                                             vd.RegisterAnnotation());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(
          NormalizeArraySymbolsInExpr(ifs.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          NormalizeArraySymbolsInStmt(ifs.ThenBranch(), array_symbols, local_vars,
                                      module_data_types, external_function_names,
                                      promoted_scalar_globals),
          ifs.ElseBranch() != nullptr
              ? NormalizeArraySymbolsInStmt(*ifs.ElseBranch(), array_symbols, local_vars,
                                            module_data_types, external_function_names,
                                            promoted_scalar_globals)
              : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(
          NormalizeArraySymbolsInExpr(ws.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          NormalizeArraySymbolsInStmt(ws.Body(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          NormalizeArraySymbolsInStmt(dws.Body(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          NormalizeArraySymbolsInExpr(dws.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          fs.Init() != nullptr ? NormalizeArraySymbolsInStmt(
                                     *fs.Init(), array_symbols, local_vars, module_data_types,
                                     external_function_names, promoted_scalar_globals)
                               : nullptr,
          fs.Condition() != nullptr
              ? NormalizeArraySymbolsInExpr(*fs.Condition(), array_symbols, local_vars,
                                            module_data_types, external_function_names,
                                            promoted_scalar_globals)
              : nullptr,
          fs.Step() != nullptr ? NormalizeArraySymbolsInExpr(
                                     *fs.Step(), array_symbols, local_vars, module_data_types,
                                     external_function_names, promoted_scalar_globals)
                               : nullptr,
          NormalizeArraySymbolsInStmt(fs.Body(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(NormalizeArraySymbolsInExpr(
          *rs.ReturnValue(), array_symbols, local_vars, module_data_types, external_function_names,
          promoted_scalar_globals));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      std::vector<SwitchCase> cases;
      cases.reserve(ss.Cases().size());
      for (const auto& c : ss.Cases()) {
        SwitchCase sc;
        sc.case_values = c.case_values;
        sc.is_default = c.is_default;
        if (c.body != nullptr) {
          sc.body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
              NormalizeArraySymbolsInStmt(*c.body, array_symbols, local_vars, module_data_types,
                                          external_function_names, promoted_scalar_globals)
                  .release()));
        }
        cases.push_back(std::move(sc));
      }
      return CStatement::Switch(
          NormalizeArraySymbolsInExpr(ss.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names, promoted_scalar_globals),
          std::move(cases));
    }
    default:
      return stmt.Clone();
  }
}

}  // namespace

std::string CTranslationUnit::ToString() const {
  std::string output;
  for (const auto& include_path : includes) {
    if (!include_path.empty()) {
      if (include_path.front() == '<') {
        output += absl::StrFormat("#include %s\n", include_path);
      } else {
        output += absl::StrFormat("#include \"%s\"\n", include_path);
      }
    }
  }
  if (!includes.empty()) {
    output += "\n";
  }

  std::set<std::string> defined_function_names;
  for (const auto& function : functions) {
    defined_function_names.insert(function.Name());
  }

  std::set<std::string> external_function_names;
  std::set<std::string> external_data_symbols;
  std::map<std::string, CType> inferred_return_types;
  std::map<std::string, CType> inferred_data_types;
  std::set<std::string> array_data_symbols;
  std::vector<ReferencedSymbols> per_func_symbols;
  per_func_symbols.reserve(functions.size());

  std::map<std::string, int> total_id_uses;
  std::map<std::string, std::map<std::string, std::pair<CType, int>>> cast_deref_uses;

  for (const auto& function : functions) {
    ReferencedSymbols symbols = CollectReferencedSymbols(function);
    CollectGlobalCastDerefsInStmt(function.Body(), symbols.local_variables, &total_id_uses,
                                  &cast_deref_uses);
    for (const auto& [name, ret_type] : symbols.inferred_function_return_types) {
      inferred_return_types[name] = ret_type;
    }
    for (const auto& [name, data_type] : symbols.inferred_data_types) {
      inferred_data_types[name] = data_type;
    }
    for (const auto& arr_name : symbols.array_data_symbols) {
      array_data_symbols.insert(arr_name);
    }

    for (const auto& func_name : symbols.external_functions) {
      if (defined_function_names.count(func_name) > 0) {
        continue;
      }
      if (IsCompilerIntrinsic(func_name)) {
        continue;
      }
      if (header_provided_functions.count(func_name) > 0) {
        continue;
      }
      if (symbol_index != nullptr) {
        const auto* entry = symbol_index->FindByName(func_name);
        if (entry != nullptr && entry->type() == SYMBOL_DATA) {
          external_data_symbols.insert(func_name);
          continue;
        }
      }
      external_function_names.insert(func_name);
    }

    for (const auto& data_name : symbols.external_data) {
      if (defined_function_names.count(data_name) > 0) {
        continue;
      }
      if (IsCompilerIntrinsic(data_name)) {
        continue;
      }
      if (header_provided_functions.count(data_name) > 0) {
        continue;
      }
      if (symbol_index != nullptr) {
        const auto* entry = symbol_index->FindByName(data_name);
        if (entry != nullptr && entry->type() == SYMBOL_FUNC) {
          external_function_names.insert(data_name);
          continue;
        }
      }
      external_data_symbols.insert(data_name);
    }
    per_func_symbols.push_back(std::move(symbols));
  }

  for (const auto& func_name : external_function_names) {
    external_data_symbols.erase(func_name);
    array_data_symbols.erase(func_name);
  }

  std::map<std::string, CType> promoted_scalar_globals;
  for (const auto& data_symbol : external_data_symbols) {
    if (array_data_symbols.count(data_symbol) > 0 || inferred_data_types.count(data_symbol) > 0) {
      continue;
    }
    auto cast_it = cast_deref_uses.find(data_symbol);
    auto total_it = total_id_uses.find(data_symbol);
    if (cast_it != cast_deref_uses.end() && cast_it->second.size() == 1 &&
        total_it != total_id_uses.end()) {
      const auto& [type_str, type_and_count] = *cast_it->second.begin();
      if (type_and_count.second == total_it->second && total_it->second > 0) {
        promoted_scalar_globals[data_symbol] = type_and_count.first;
        inferred_data_types[data_symbol] = type_and_count.first;
      }
    }
  }

  std::set<std::string> all_function_names = external_function_names;
  all_function_names.insert(defined_function_names.begin(), defined_function_names.end());

  std::string combined_function_code;
  for (size_t i = 0; i < functions.size(); ++i) {
    std::unique_ptr<CompoundStatement> body_to_emit;
    if (functions.size() > 1 || !array_data_symbols.empty() || !promoted_scalar_globals.empty()) {
      body_to_emit = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
          NormalizeArraySymbolsInStmt(functions[i].Body(), array_data_symbols,
                                      per_func_symbols[i].local_variables, inferred_data_types,
                                      all_function_names, promoted_scalar_globals)
              .release()));
    } else {
      body_to_emit = std::unique_ptr<CompoundStatement>(
          static_cast<CompoundStatement*>(functions[i].Body().Clone().release()));
    }
    FunctionDeclaration norm_fn(functions[i].ReturnType(), functions[i].Name(),
                                functions[i].Parameters(), std::move(body_to_emit),
                                functions[i].IsStatic());
    combined_function_code += norm_fn.ToString();
    if (combined_function_code.back() != '\n') {
      combined_function_code += "\n";
    }
  }

  std::set<int> custom_unaligned_block_sizes;
  CollectCustomUnalignedBlockSizes(combined_function_code, &custom_unaligned_block_sizes);
  if (!custom_unaligned_block_sizes.empty()) {
    for (int block_size : custom_unaligned_block_sizes) {
      output += absl::StrFormat(
          "#ifndef UNALIGNED_BLOCK_%d_DEFINED\n"
          "#define UNALIGNED_BLOCK_%d_DEFINED\n"
          "typedef struct { u8 b[%d]; } UnalignedBlock%d;\n"
          "#endif\n",
          block_size, block_size, block_size, block_size);
    }
    output += "\n";
  }

  if (!external_function_names.empty()) {
    for (const auto& external_function : external_function_names) {
      CType return_type = CType::S32();
      auto it = inferred_return_types.find(external_function);
      if (it != inferred_return_types.end()) {
        return_type = it->second;
      }
      output += absl::StrFormat("extern %s %s();\n", return_type.ToString(), external_function);
    }
    output += "\n";
  }

  if (!external_data_symbols.empty()) {
    for (const auto& data_symbol : external_data_symbols) {
      CType data_type = CType::S32();
      auto it = inferred_data_types.find(data_symbol);
      if (it != inferred_data_types.end()) {
        data_type = it->second;
      }
      std::string array_suffix = array_data_symbols.count(data_symbol) > 0 ? "[]" : "";
      output +=
          absl::StrFormat("extern %s %s%s;\n", data_type.ToString(), data_symbol, array_suffix);
    }
    output += "\n";
  }

  for (const auto& function : functions) {
    output += function.Prototype();
  }
  if (!functions.empty()) {
    output += "\n";
  }

  output += combined_function_code;
  return output;
}

std::vector<std::string_view> CEmitter::StandardMathIntrinsicNames() {
  std::vector<std::string_view> names;
  names.reserve(sizeof(kMathIntrinsics) / sizeof(kMathIntrinsics[0]));
  for (const auto& intrin : kMathIntrinsics) {
    names.push_back(intrin.name);
  }
  return names;
}

std::string CEmitter::EmitTypesHeader(const TypesHeaderOptions& options) {
  std::string out = R"(#ifndef TYPES_H
#define TYPES_H

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;

typedef float f32;
typedef double f64;
)";

  if (options.include_unaligned_structs) {
    out += R"(
typedef struct { u8 b[4]; } UnalignedWord;
typedef struct { u8 b[6]; } UnalignedBlock6;
typedef struct { u8 b[8]; } UnalignedBlock8;
typedef struct { u8 b[12]; } UnalignedBlock12;
typedef struct { u8 b[16]; } UnalignedBlock16;
)";
  }

  if (options.include_math_intrinsics) {
    out += "\n#if defined(__GNUC__) && (defined(__mips__) || defined(__mips))\n";
    for (const auto& intrin : kMathIntrinsics) {
      if (options.should_skip_math_intrinsic && options.should_skip_math_intrinsic(intrin.name)) {
        continue;
      }
      if (options.emitted_math_intrinsics != nullptr) {
        options.emitted_math_intrinsics->insert(std::string(intrin.name));
      }
      out += absl::StrFormat(
          "static inline %s %s(%s x) {\n"
          "  %s res;\n"
          "  __asm__(\"%s\" : \"=f\"(res) : \"f\"(x));\n"
          "  return res;\n"
          "}\n",
          intrin.type, intrin.name, intrin.type, intrin.type, intrin.asm_inst);
    }
    out += R"(#else
#ifdef __cplusplus
#include <cmath>
static inline f32 N64Sqrtf(f32 x) { return std::sqrt(x); }
static inline f64 N64Sqrt(f64 x) { return std::sqrt(x); }
#else
#include <math.h>
static inline f32 N64Sqrtf(f32 x) { return sqrtf(x); }
static inline f64 N64Sqrt(f64 x) { return sqrt(x); }
#endif
#endif
)";
  }

  if (!options.extra_declarations.empty()) {
    out += "\n" + options.extra_declarations + "\n";
  }

  out += "\n#endif  // TYPES_H\n";
  return out;
}

std::string CEmitter::EmitFunction(const FunctionDeclaration& func,
                                   const CEmitterOptions& options) {
  std::string raw_code;
  for (const auto& inc : options.includes) {
    if (!inc.empty()) {
      if (inc.front() == '<') {
        raw_code += absl::StrFormat("#include %s\n", inc);
      } else {
        raw_code += absl::StrFormat("#include \"%s\"\n", inc);
      }
    }
  }
  if (!options.includes.empty()) {
    raw_code += "\n";
  }
  raw_code += func.ToString();
  if (raw_code.empty() || raw_code.back() != '\n') {
    raw_code += "\n";
  }

  if (options.format_with_clang) {
    return FormatCode(raw_code, options.clang_format_style);
  }
  return raw_code;
}

std::string CEmitter::EmitTranslationUnit(const CTranslationUnit& tu,
                                          const CEmitterOptions& options) {
  std::string raw_code = tu.ToString();
  if (options.format_with_clang) {
    return FormatCode(raw_code, options.clang_format_style);
  }
  return raw_code;
}

std::string CEmitter::FormatCode(std::string_view unformatted_code, std::string_view style) {
  std::error_code ec;
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return std::string(unformatted_code);
  }

  std::string temp_filename = absl::StrFormat("temp_lifter_%d_%d.c", getpid(), rand());
  std::filesystem::path temp_file = temp_dir / temp_filename;

  std::ofstream out(temp_file);
  if (!out.is_open()) {
    return std::string(unformatted_code);
  }
  out << unformatted_code;
  out.close();

  ProcessOptions proc_opts;
  proc_opts.program = "clang-format";
  proc_opts.args = {absl::StrFormat("--style=%s", style), temp_file.string()};
  proc_opts.working_directory = std::filesystem::current_path(ec);
  proc_opts.timeout = absl::Seconds(5);

  auto res_or = RunProcess(proc_opts);
  std::filesystem::remove(temp_file, ec);

  if (res_or.ok() && res_or->Ok() && !res_or->stdout_output.empty()) {
    return res_or->stdout_output;
  }

  return std::string(unformatted_code);
}

}  // namespace rom_nom_nom
