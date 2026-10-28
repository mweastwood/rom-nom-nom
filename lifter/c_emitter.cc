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
    std::map<std::string, std::map<std::string, std::pair<CType, int>>>* cast_deref_uses,
    std::set<std::string>* uncast_addr_arith_symbols = nullptr,
    std::map<std::string, std::map<std::string, std::pair<CType, int>>>* id_symbol_type_uses =
        nullptr) {
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
        if (id_symbol_type_uses != nullptr && !id.IsArraySymbol()) {
          CType sym_type = id.SymbolType().value_or(CType::S32());
          auto& entry = (*id_symbol_type_uses)[id.Name()][sym_type.ToString()];
          entry.first = sym_type;
          entry.second++;
        }
      }
      break;
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.IsPrefix() && u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
        break;
      }
      CollectGlobalCastDerefsInExpr(u.Operand(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
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
                if (uncast_addr_arith_symbols != nullptr) {
                  uncast_addr_arith_symbols->insert(id.Name());
                }
              }
            }
          }
        };
        check_uncast_addr_of(b.Lhs());
        check_uncast_addr_of(b.Rhs());
      }
      CollectGlobalCastDerefsInExpr(b.Lhs(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInExpr(b.Rhs(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
        const auto& lhs_id = static_cast<const IdentifierExpression&>(a.Lhs());
        if (local_vars.count(lhs_id.Name()) == 0 && !lhs_id.IsArraySymbol()) {
          std::string t_str = lhs_id.SymbolType().value_or(CType::S32()).ToString();
          if (t_str == "s32" && a.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
              static_cast<const IntegerLiteralExpression&>(a.Rhs()).Value() == 0) {
            if (id_symbol_type_uses != nullptr) {
              auto& z_entry = (*id_symbol_type_uses)[lhs_id.Name()]["s32_zero_store"];
              z_entry.first = CType::S32();
              z_entry.second++;
            }
            (*total_id_uses)[lhs_id.Name()]++;
            CollectGlobalCastDerefsInExpr(a.Rhs(), local_vars, total_id_uses, cast_deref_uses,
                                          uncast_addr_arith_symbols, id_symbol_type_uses);
            break;
          }
        }
      }
      CollectGlobalCastDerefsInExpr(a.Lhs(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInExpr(a.Rhs(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      CollectGlobalCastDerefsInExpr(c.Callee(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      for (const auto& arg : c.Arguments()) {
        if (arg != nullptr) {
          CollectGlobalCastDerefsInExpr(*arg, local_vars, total_id_uses, cast_deref_uses,
                                        uncast_addr_arith_symbols, id_symbol_type_uses);
        }
      }
      break;
    }
    case CExpressionKind::kCastExpression:
      CollectGlobalCastDerefsInExpr(static_cast<const CastExpression&>(expr).Operand(), local_vars,
                                    total_id_uses, cast_deref_uses, uncast_addr_arith_symbols,
                                    id_symbol_type_uses);
      break;
    case CExpressionKind::kMemberAccessExpression:
      CollectGlobalCastDerefsInExpr(static_cast<const MemberAccessExpression&>(expr).Object(),
                                    local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      CollectGlobalCastDerefsInExpr(a.Array(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInExpr(a.Index(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      CollectGlobalCastDerefsInExpr(t.Condition(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInExpr(t.TrueExpression(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInExpr(t.FalseExpression(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    }
    default:
      break;
  }
}

void CollectGlobalCastDerefsInStmt(
    const CStatement& stmt, const std::set<std::string>& local_vars,
    std::map<std::string, int>* total_id_uses,
    std::map<std::string, std::map<std::string, std::pair<CType, int>>>* cast_deref_uses,
    std::set<std::string>* uncast_addr_arith_symbols = nullptr,
    std::map<std::string, std::map<std::string, std::pair<CType, int>>>* id_symbol_type_uses =
        nullptr) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr) {
          CollectGlobalCastDerefsInStmt(*s, local_vars, total_id_uses, cast_deref_uses,
                                        uncast_addr_arith_symbols, id_symbol_type_uses);
        }
      }
      break;
    case CStatementKind::kExpressionStatement:
      CollectGlobalCastDerefsInExpr(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                    local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      if (vd.Initializer() != nullptr) {
        CollectGlobalCastDerefsInExpr(*vd.Initializer(), local_vars, total_id_uses, cast_deref_uses,
                                      uncast_addr_arith_symbols, id_symbol_type_uses);
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      CollectGlobalCastDerefsInExpr(ifs.Condition(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInStmt(ifs.ThenBranch(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      if (ifs.ElseBranch() != nullptr) {
        CollectGlobalCastDerefsInStmt(*ifs.ElseBranch(), local_vars, total_id_uses, cast_deref_uses,
                                      uncast_addr_arith_symbols, id_symbol_type_uses);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      CollectGlobalCastDerefsInExpr(ws.Condition(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInStmt(ws.Body(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      CollectGlobalCastDerefsInStmt(dws.Body(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      CollectGlobalCastDerefsInExpr(dws.Condition(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr) {
        CollectGlobalCastDerefsInStmt(*fs.Init(), local_vars, total_id_uses, cast_deref_uses,
                                      uncast_addr_arith_symbols, id_symbol_type_uses);
      }
      if (fs.Condition() != nullptr) {
        CollectGlobalCastDerefsInExpr(*fs.Condition(), local_vars, total_id_uses, cast_deref_uses,
                                      uncast_addr_arith_symbols, id_symbol_type_uses);
      }
      if (fs.Step() != nullptr) {
        CollectGlobalCastDerefsInExpr(*fs.Step(), local_vars, total_id_uses, cast_deref_uses,
                                      uncast_addr_arith_symbols, id_symbol_type_uses);
      }
      CollectGlobalCastDerefsInStmt(fs.Body(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      break;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr) {
        CollectGlobalCastDerefsInExpr(*rs.ReturnValue(), local_vars, total_id_uses, cast_deref_uses,
                                      uncast_addr_arith_symbols, id_symbol_type_uses);
      }
      break;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      CollectGlobalCastDerefsInExpr(ss.Condition(), local_vars, total_id_uses, cast_deref_uses,
                                    uncast_addr_arith_symbols, id_symbol_type_uses);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) {
          CollectGlobalCastDerefsInStmt(*c.body, local_vars, total_id_uses, cast_deref_uses,
                                        uncast_addr_arith_symbols, id_symbol_type_uses);
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
              if (!is_func && array_symbols.count(id.Name()) == 0 &&
                  ((type_it->second.ToString() == "s8" && accessed_type.ToString() == "u8") ||
                   (type_it->second.ToString() == "s16" && accessed_type.ToString() == "u16"))) {
                return CExpression::Cast(accessed_type, std::make_unique<IdentifierExpression>(
                                                            id.Name(), type_it->second));
              }
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
        if (prom_it != promoted_scalar_globals.end()) {
          if (prom_it->second.ToString() == matched->second.ToString()) {
            return std::make_unique<IdentifierExpression>(matched->first, matched->second);
          }
          if ((prom_it->second.ToString() == "s8" && matched->second.ToString() == "u8") ||
              (prom_it->second.ToString() == "s16" && matched->second.ToString() == "u16")) {
            return CExpression::Cast(matched->second, std::make_unique<IdentifierExpression>(
                                                          matched->first, prom_it->second));
          }
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
      if (a.Op() == "=") {
        if (auto matched = MatchScalarCastDerefGlobal(a.Lhs(), local_vars); matched.has_value()) {
          auto prom_it = promoted_scalar_globals.find(matched->first);
          if (prom_it != promoted_scalar_globals.end() &&
              (prom_it->second.ToString() == matched->second.ToString() ||
               (prom_it->second.ToString() == "s8" && matched->second.ToString() == "u8") ||
               (prom_it->second.ToString() == "s16" && matched->second.ToString() == "u16"))) {
            return CExpression::Assignment(
                a.Op(), std::make_unique<IdentifierExpression>(matched->first, prom_it->second),
                NormalizeArraySymbolsInExpr(a.Rhs(), array_symbols, local_vars, module_data_types,
                                            external_function_names, promoted_scalar_globals));
          }
        } else if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          const auto& lhs_id = static_cast<const IdentifierExpression&>(a.Lhs());
          if (local_vars.count(lhs_id.Name()) == 0 && !lhs_id.IsArraySymbol()) {
            CType accessed_type = lhs_id.SymbolType().value_or(CType::S32());
            auto prom_it = promoted_scalar_globals.find(lhs_id.Name());
            if (prom_it != promoted_scalar_globals.end() &&
                ((prom_it->second.ToString() == "s8" && accessed_type.ToString() == "u8") ||
                 (prom_it->second.ToString() == "s16" && accessed_type.ToString() == "u16") ||
                 (prom_it->second.ToString() == "f32" && accessed_type.ToString() == "s32" &&
                  a.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
                  static_cast<const IntegerLiteralExpression&>(a.Rhs()).Value() == 0))) {
              return CExpression::Assignment(
                  a.Op(), std::make_unique<IdentifierExpression>(lhs_id.Name(), prom_it->second),
                  NormalizeArraySymbolsInExpr(a.Rhs(), array_symbols, local_vars, module_data_types,
                                              external_function_names, promoted_scalar_globals));
            }
          }
        }
      }
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
      if (auto matched = MatchScalarCastDerefGlobal(c.Operand(), local_vars); matched.has_value()) {
        auto prom_it = promoted_scalar_globals.find(matched->first);
        if (prom_it != promoted_scalar_globals.end() &&
            c.TargetType().ToString() == prom_it->second.ToString() &&
            ((prom_it->second.ToString() == "s8" && matched->second.ToString() == "u8") ||
             (prom_it->second.ToString() == "s16" && matched->second.ToString() == "u16"))) {
          return std::make_unique<IdentifierExpression>(matched->first, prom_it->second);
        }
      }
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

bool IsPinnedCalleeSavedGprVar(std::string_view v) {
  return v == "temp_fp" ||
         (v.size() == 7 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7');
}

bool ExprContainsNonIntrinsicCall(const CExpression& expr);
bool ExprReadsVar(const CExpression& expr, std::string_view var);
bool StmtContainsNonIntrinsicCall(const CStatement& stmt);
bool StmtReadsVar(const CStatement& stmt, std::string_view var);
bool CalleeSavedCopyCrossesCallWithReadInBlock(const CompoundStatement& comp, size_t start_idx,
                                               std::string_view var,
                                               bool require_split_prone = false);

bool ExprHasCseSplitProneRead(const CExpression& expr, std::string_view var) {
  switch (expr.Kind()) {
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.Op() == "*" || absl::StartsWith(u.Op(), "*(")) {
        const CExpression* inner = &u.Operand();
        while (inner->Kind() == CExpressionKind::kCastExpression) {
          inner = &static_cast<const CastExpression*>(inner)->Operand();
        }
        if (inner->Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression*>(inner)->Name() == var) {
          return true;
        }
      }
      return ExprHasCseSplitProneRead(u.Operand(), var);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      if (b.Op() == "*" && (ExprReadsVar(b.Lhs(), var) || ExprReadsVar(b.Rhs(), var))) {
        return true;
      }
      return ExprHasCseSplitProneRead(b.Lhs(), var) || ExprHasCseSplitProneRead(b.Rhs(), var);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(a.Lhs()).Name() != var) {
        const CExpression* rhs = &a.Rhs();
        while (rhs->Kind() == CExpressionKind::kCastExpression) {
          rhs = &static_cast<const CastExpression*>(rhs)->Operand();
        }
        if (rhs->Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression*>(rhs)->Name() == var) {
          return true;
        }
      }
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
        return ExprHasCseSplitProneRead(a.Rhs(), var);
      }
      return ExprHasCseSplitProneRead(a.Lhs(), var) || ExprHasCseSplitProneRead(a.Rhs(), var);
    }
    case CExpressionKind::kCastExpression:
      return ExprHasCseSplitProneRead(static_cast<const CastExpression&>(expr).Operand(), var);
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExprHasCseSplitProneRead(*arg, var)) return true;
      }
      return false;
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& ai = static_cast<const ArrayIndexExpression&>(expr);
      return ExprHasCseSplitProneRead(ai.Array(), var) || ExprHasCseSplitProneRead(ai.Index(), var);
    }
    case CExpressionKind::kMemberAccessExpression:
      return ExprHasCseSplitProneRead(static_cast<const MemberAccessExpression&>(expr).Object(),
                                      var);
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExprHasCseSplitProneRead(t.Condition(), var) ||
             ExprHasCseSplitProneRead(t.TrueExpression(), var) ||
             ExprHasCseSplitProneRead(t.FalseExpression(), var);
    }
    default:
      return false;
  }
}

bool StmtHasCseSplitProneRead(const CStatement& stmt, std::string_view var) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement:
      return ExprHasCseSplitProneRead(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                      var);
    case CStatementKind::kCompoundStatement: {
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr && StmtHasCseSplitProneRead(*s, var)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExprHasCseSplitProneRead(ifs.Condition(), var) ||
             StmtHasCseSplitProneRead(ifs.ThenBranch(), var) ||
             (ifs.ElseBranch() != nullptr && StmtHasCseSplitProneRead(*ifs.ElseBranch(), var));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExprHasCseSplitProneRead(ws.Condition(), var) ||
             StmtHasCseSplitProneRead(ws.Body(), var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StmtHasCseSplitProneRead(dws.Body(), var) ||
             ExprHasCseSplitProneRead(dws.Condition(), var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StmtHasCseSplitProneRead(*fs.Init(), var)) ||
             (fs.Condition() != nullptr && ExprHasCseSplitProneRead(*fs.Condition(), var)) ||
             (fs.Step() != nullptr && ExprHasCseSplitProneRead(*fs.Step(), var)) ||
             StmtHasCseSplitProneRead(fs.Body(), var);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExprHasCseSplitProneRead(ss.Condition(), var)) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StmtHasCseSplitProneRead(*c.body, var)) return true;
      }
      return false;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr && ExprHasCseSplitProneRead(*rs.ReturnValue(), var);
    }
    default:
      return false;
  }
}

bool IsInPlaceConstAddSub(const CExpression& rhs, std::string_view lhs) {
  if (rhs.Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  const auto& b = static_cast<const BinaryExpression&>(rhs);
  return (b.Op() == "+" || b.Op() == "-") && b.Lhs().Kind() == CExpressionKind::kIdentifier &&
         static_cast<const IdentifierExpression&>(b.Lhs()).Name() == lhs &&
         b.Rhs().Kind() == CExpressionKind::kIntegerLiteral;
}

bool IsCseConflictProneCalleeSavedRhs(const CExpression& rhs,
                                      const std::set<std::string>& external_data_symbols,
                                      const CompoundStatement& comp, size_t idx,
                                      std::string_view lhs) {
  if (ExprReadsVar(rhs, lhs)) {
    if (IsInPlaceConstAddSub(rhs, lhs)) {
      return CalleeSavedCopyCrossesCallWithReadInBlock(comp, idx, lhs,
                                                       /*require_split_prone=*/true);
    }
    return false;
  }
  if (rhs.Kind() == CExpressionKind::kIntegerLiteral ||
      rhs.Kind() == CExpressionKind::kFloatLiteral) {
    return false;
  }
  if (rhs.Kind() == CExpressionKind::kCallExpression) {
    return CalleeSavedCopyCrossesCallWithReadInBlock(comp, idx, lhs,
                                                     /*require_split_prone=*/true);
  }
  if (rhs.Kind() == CExpressionKind::kIdentifier) {
    std::string name(static_cast<const IdentifierExpression&>(rhs).Name());
    if (name == "v0" || name == "v1") {
      return CalleeSavedCopyCrossesCallWithReadInBlock(comp, idx, lhs,
                                                       /*require_split_prone=*/true);
    }
    return external_data_symbols.count(name) == 0;
  }
  if (rhs.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& u = static_cast<const UnaryExpression&>(rhs);
    if (u.Op() == "*" || absl::StartsWith(u.Op(), "*(")) {
      return false;
    }
  }
  if (ExprReadsVar(rhs, "v0") || ExprReadsVar(rhs, "v1")) {
    return false;
  }
  if (rhs.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& b = static_cast<const BinaryExpression&>(rhs);
    if (b.Op() == "+" || b.Op() == "-") {
      if (b.Lhs().Kind() == CExpressionKind::kIdentifier &&
          absl::StartsWith(static_cast<const IdentifierExpression&>(b.Lhs()).Name(), "arg")) {
        return false;
      }
      if (idx + 1 < comp.Statements().size() && comp.Statements()[idx + 1] != nullptr &&
          comp.Statements()[idx + 1]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& next_es = static_cast<const ExpressionStatement&>(*comp.Statements()[idx + 1]);
        if (ExprContainsNonIntrinsicCall(next_es.Expression()) &&
            ExprReadsVar(next_es.Expression(), lhs)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool ExtractDirectSymbolAddressRhs(const CExpression& expr,
                                   const std::set<std::string>& external_data_symbols,
                                   std::string* out_sym, bool* out_has_s32_cast) {
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& un = static_cast<const UnaryExpression&>(expr);
    if (un.Op() == "&" && un.Operand().Kind() == CExpressionKind::kIdentifier) {
      std::string name(static_cast<const IdentifierExpression&>(un.Operand()).Name());
      if (external_data_symbols.count(name) > 0) {
        *out_sym = std::move(name);
        *out_has_s32_cast = false;
        return true;
      }
    }
  } else if (expr.Kind() == CExpressionKind::kCastExpression) {
    const auto& ce = static_cast<const CastExpression&>(expr);
    if (ce.TargetType().ToString() == "s32" &&
        ce.Operand().Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(ce.Operand());
      if (un.Op() == "&" && un.Operand().Kind() == CExpressionKind::kIdentifier) {
        std::string name(static_cast<const IdentifierExpression&>(un.Operand()).Name());
        if (external_data_symbols.count(name) > 0) {
          *out_sym = std::move(name);
          *out_has_s32_cast = true;
          return true;
        }
      }
    }
  }
  return false;
}

bool ExprContainsNonIntrinsicCall(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (call.Callee().Kind() == CExpressionKind::kIdentifier) {
        std::string_view name = static_cast<const IdentifierExpression&>(call.Callee()).Name();
        if (!IsCompilerIntrinsic(name)) {
          return true;
        }
      } else {
        return true;
      }
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExprContainsNonIntrinsicCall(*arg)) return true;
      }
      return false;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return ExprContainsNonIntrinsicCall(a.Lhs()) || ExprContainsNonIntrinsicCall(a.Rhs());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return ExprContainsNonIntrinsicCall(b.Lhs()) || ExprContainsNonIntrinsicCall(b.Rhs());
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return ExprContainsNonIntrinsicCall(u.Operand());
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return ExprContainsNonIntrinsicCall(c.Operand());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& ai = static_cast<const ArrayIndexExpression&>(expr);
      return ExprContainsNonIntrinsicCall(ai.Array()) || ExprContainsNonIntrinsicCall(ai.Index());
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& ma = static_cast<const MemberAccessExpression&>(expr);
      return ExprContainsNonIntrinsicCall(ma.Object());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExprContainsNonIntrinsicCall(t.Condition()) ||
             ExprContainsNonIntrinsicCall(t.TrueExpression()) ||
             ExprContainsNonIntrinsicCall(t.FalseExpression());
    }
    default:
      return false;
  }
}

bool ExprReadsVar(const CExpression& expr, std::string_view var) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier:
      return static_cast<const IdentifierExpression&>(expr).Name() == var;
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
        return ExprReadsVar(a.Rhs(), var);
      }
      return ExprReadsVar(a.Lhs(), var) || ExprReadsVar(a.Rhs(), var);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return ExprReadsVar(b.Lhs(), var) || ExprReadsVar(b.Rhs(), var);
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return ExprReadsVar(u.Operand(), var);
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return ExprReadsVar(c.Operand(), var);
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExprReadsVar(*arg, var)) return true;
      }
      return false;
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& ai = static_cast<const ArrayIndexExpression&>(expr);
      return ExprReadsVar(ai.Array(), var) || ExprReadsVar(ai.Index(), var);
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& ma = static_cast<const MemberAccessExpression&>(expr);
      return ExprReadsVar(ma.Object(), var);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExprReadsVar(t.Condition(), var) || ExprReadsVar(t.TrueExpression(), var) ||
             ExprReadsVar(t.FalseExpression(), var);
    }
    default:
      return false;
  }
}

bool StmtReadsVar(const CStatement& stmt, std::string_view var) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement:
      return ExprReadsVar(static_cast<const ExpressionStatement&>(stmt).Expression(), var);
    case CStatementKind::kCompoundStatement: {
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr && StmtReadsVar(*s, var)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExprReadsVar(ifs.Condition(), var) || StmtReadsVar(ifs.ThenBranch(), var) ||
             (ifs.ElseBranch() != nullptr && StmtReadsVar(*ifs.ElseBranch(), var));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExprReadsVar(ws.Condition(), var) || StmtReadsVar(ws.Body(), var);
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StmtReadsVar(dws.Body(), var) || ExprReadsVar(dws.Condition(), var);
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StmtReadsVar(*fs.Init(), var)) ||
             (fs.Condition() != nullptr && ExprReadsVar(*fs.Condition(), var)) ||
             (fs.Step() != nullptr && ExprReadsVar(*fs.Step(), var)) ||
             StmtReadsVar(fs.Body(), var);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExprReadsVar(ss.Condition(), var)) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StmtReadsVar(*c.body, var)) return true;
      }
      return false;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr && ExprReadsVar(*rs.ReturnValue(), var);
    }
    default:
      return false;
  }
}

bool StmtContainsNonIntrinsicCall(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement:
      return ExprContainsNonIntrinsicCall(
          static_cast<const ExpressionStatement&>(stmt).Expression());
    case CStatementKind::kCompoundStatement: {
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr && StmtContainsNonIntrinsicCall(*s)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExprContainsNonIntrinsicCall(ifs.Condition()) ||
             StmtContainsNonIntrinsicCall(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StmtContainsNonIntrinsicCall(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExprContainsNonIntrinsicCall(ws.Condition()) ||
             StmtContainsNonIntrinsicCall(ws.Body());
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StmtContainsNonIntrinsicCall(dws.Body()) ||
             ExprContainsNonIntrinsicCall(dws.Condition());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StmtContainsNonIntrinsicCall(*fs.Init())) ||
             (fs.Condition() != nullptr && ExprContainsNonIntrinsicCall(*fs.Condition())) ||
             (fs.Step() != nullptr && ExprContainsNonIntrinsicCall(*fs.Step())) ||
             StmtContainsNonIntrinsicCall(fs.Body());
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExprContainsNonIntrinsicCall(ss.Condition())) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StmtContainsNonIntrinsicCall(*c.body)) return true;
      }
      return false;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr && ExprContainsNonIntrinsicCall(*rs.ReturnValue());
    }
    default:
      return false;
  }
}

bool StmtOverwritesVar(const CStatement& stmt, std::string_view var) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(a.Lhs()).Name() == var) {
          return true;
        }
      }
      return false;
    }
    case CStatementKind::kCompoundStatement: {
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr && StmtOverwritesVar(*s, var)) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StmtOverwritesVar(ifs.ThenBranch(), var) ||
             (ifs.ElseBranch() != nullptr && StmtOverwritesVar(*ifs.ElseBranch(), var));
    }
    case CStatementKind::kWhileStatement:
      return StmtOverwritesVar(static_cast<const WhileStatement&>(stmt).Body(), var);
    case CStatementKind::kDoWhileStatement:
      return StmtOverwritesVar(static_cast<const DoWhileStatement&>(stmt).Body(), var);
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StmtOverwritesVar(*fs.Init(), var)) ||
             StmtOverwritesVar(fs.Body(), var);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StmtOverwritesVar(*c.body, var)) return true;
      }
      return false;
    }
    default:
      return false;
  }
}

bool StmtContainsCallFollowedByRead(const CStatement& stmt, std::string_view var,
                                    bool require_split_prone) {
  const auto check_read = [&](const CStatement& s) {
    return require_split_prone ? StmtHasCseSplitProneRead(s, var) : StmtReadsVar(s, var);
  };
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      bool seen_call = false;
      for (const auto& s : comp.Statements()) {
        if (s == nullptr) continue;
        if (seen_call && check_read(*s)) return true;
        if (StmtContainsCallFollowedByRead(*s, var, require_split_prone)) return true;
        if (StmtOverwritesVar(*s, var)) return false;
        if (StmtContainsNonIntrinsicCall(*s)) seen_call = true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ExprContainsNonIntrinsicCall(ifs.Condition())) {
        if (check_read(ifs.ThenBranch()) ||
            (ifs.ElseBranch() != nullptr && check_read(*ifs.ElseBranch()))) {
          return true;
        }
      }
      return StmtContainsCallFollowedByRead(ifs.ThenBranch(), var, require_split_prone) ||
             (ifs.ElseBranch() != nullptr &&
              StmtContainsCallFollowedByRead(*ifs.ElseBranch(), var, require_split_prone));
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExprContainsNonIntrinsicCall(ss.Condition())) {
        for (const auto& c : ss.Cases()) {
          if (c.body != nullptr && check_read(*c.body)) return true;
        }
      }
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr &&
            StmtContainsCallFollowedByRead(*c.body, var, require_split_prone)) {
          return true;
        }
      }
      return false;
    }
    case CStatementKind::kWhileStatement:
      return StmtContainsCallFollowedByRead(static_cast<const WhileStatement&>(stmt).Body(), var,
                                            require_split_prone);
    case CStatementKind::kDoWhileStatement:
      return StmtContainsCallFollowedByRead(static_cast<const DoWhileStatement&>(stmt).Body(), var,
                                            require_split_prone);
    case CStatementKind::kForStatement:
      return StmtContainsCallFollowedByRead(static_cast<const ForStatement&>(stmt).Body(), var,
                                            require_split_prone);
    default:
      return false;
  }
}

bool CalleeSavedCopyCrossesCallWithReadInBlock(const CompoundStatement& comp, size_t start_idx,
                                               std::string_view var, bool require_split_prone) {
  const auto check_read = [&](const CStatement& s) {
    return require_split_prone ? StmtHasCseSplitProneRead(s, var) : StmtReadsVar(s, var);
  };
  bool seen_call = false;
  for (size_t k = start_idx + 1; k < comp.Statements().size(); ++k) {
    const auto& next_s = comp.Statements()[k];
    if (next_s == nullptr) continue;
    if (next_s->Kind() == CStatementKind::kVariableDeclarationStatement) continue;
    if (!seen_call) {
      if (next_s->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es = static_cast<const ExpressionStatement&>(*next_s);
        if (ExprContainsNonIntrinsicCall(es.Expression())) {
          seen_call = true;
        }
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(a.Lhs()).Name() == var) {
            return false;
          }
        }
        continue;
      }
      if (next_s->Kind() == CStatementKind::kIfStatement ||
          next_s->Kind() == CStatementKind::kSwitchStatement ||
          next_s->Kind() == CStatementKind::kWhileStatement ||
          next_s->Kind() == CStatementKind::kDoWhileStatement ||
          next_s->Kind() == CStatementKind::kForStatement) {
        if (StmtContainsCallFollowedByRead(*next_s, var, require_split_prone)) {
          return true;
        }
        if (StmtOverwritesVar(*next_s, var)) {
          return false;
        }
        if (StmtContainsNonIntrinsicCall(*next_s)) {
          seen_call = true;
        }
        continue;
      }
      return false;
    }
    if (check_read(*next_s)) {
      return true;
    }
    if (next_s->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*next_s);
      if (es.Expression().Kind() == CExpressionKind::kCallExpression &&
          static_cast<const CallExpression&>(es.Expression()).Arguments().empty()) {
        return false;
      }
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(a.Lhs()).Name() == var) {
          return false;
        }
      }
    }
    if (StmtOverwritesVar(*next_s, var)) {
      return false;
    }
    if (next_s->Kind() == CStatementKind::kReturnStatement ||
        next_s->Kind() == CStatementKind::kGotoStatement ||
        next_s->Kind() == CStatementKind::kBreakStatement ||
        next_s->Kind() == CStatementKind::kContinueStatement) {
      break;
    }
  }
  return false;
}

bool ExprContainsNarrowSignCastToFloatOfVar(const CExpression& expr, std::string_view var) {
  switch (expr.Kind()) {
    case CExpressionKind::kCastExpression: {
      const auto& ce = static_cast<const CastExpression&>(expr);
      std::string outer_type = ce.TargetType().ToString();
      if ((outer_type == "f32" || outer_type == "f64") &&
          ce.Operand().Kind() == CExpressionKind::kCastExpression) {
        const auto& inner_ce = static_cast<const CastExpression&>(ce.Operand());
        std::string inner_type = inner_ce.TargetType().ToString();
        if ((inner_type == "s8" || inner_type == "s16") &&
            inner_ce.Operand().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(inner_ce.Operand()).Name() == var) {
          return true;
        }
      }
      return ExprContainsNarrowSignCastToFloatOfVar(ce.Operand(), var);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return ExprContainsNarrowSignCastToFloatOfVar(a.Lhs(), var) ||
             ExprContainsNarrowSignCastToFloatOfVar(a.Rhs(), var);
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return ExprContainsNarrowSignCastToFloatOfVar(b.Lhs(), var) ||
             ExprContainsNarrowSignCastToFloatOfVar(b.Rhs(), var);
    }
    case CExpressionKind::kUnaryExpression:
      return ExprContainsNarrowSignCastToFloatOfVar(
          static_cast<const UnaryExpression&>(expr).Operand(), var);
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExprContainsNarrowSignCastToFloatOfVar(*arg, var)) return true;
      }
      return false;
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& ai = static_cast<const ArrayIndexExpression&>(expr);
      return ExprContainsNarrowSignCastToFloatOfVar(ai.Array(), var) ||
             ExprContainsNarrowSignCastToFloatOfVar(ai.Index(), var);
    }
    case CExpressionKind::kMemberAccessExpression:
      return ExprContainsNarrowSignCastToFloatOfVar(
          static_cast<const MemberAccessExpression&>(expr).Object(), var);
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExprContainsNarrowSignCastToFloatOfVar(t.Condition(), var) ||
             ExprContainsNarrowSignCastToFloatOfVar(t.TrueExpression(), var) ||
             ExprContainsNarrowSignCastToFloatOfVar(t.FalseExpression(), var);
    }
    default:
      return false;
  }
}

bool GlobalLoadHasNarrowToFloatAndSecondReadInBlock(const CompoundStatement& comp, size_t start_idx,
                                                    std::string_view var) {
  bool seen_narrow_to_float = false;
  int read_stmts = 0;
  for (size_t i = start_idx + 1; i < comp.Statements().size(); ++i) {
    const auto& next_s = comp.Statements()[i];
    if (next_s == nullptr) continue;
    if (next_s->Kind() != CStatementKind::kExpressionStatement) {
      break;
    }
    const auto& es = static_cast<const ExpressionStatement&>(*next_s);
    if (ExprReadsVar(es.Expression(), var)) {
      ++read_stmts;
      if (ExprContainsNarrowSignCastToFloatOfVar(es.Expression(), var)) {
        seen_narrow_to_float = true;
      }
    }
    if (StmtOverwritesVar(*next_s, var) || ExprContainsNonIntrinsicCall(es.Expression())) {
      break;
    }
  }
  return seen_narrow_to_float && read_stmts >= 2;
}

std::unique_ptr<CStatement> DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
    const CStatement& stmt, const std::set<std::string>& external_data_symbols, bool has_calls) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      std::vector<std::unique_ptr<CStatement>> stmts;
      stmts.reserve(comp.Statements().size());
      std::map<std::string, std::vector<std::string>> active_sym_vars;
      for (size_t idx = 0; idx < comp.Statements().size(); ++idx) {
        const auto& s = comp.Statements()[idx];
        if (s == nullptr) continue;
        if (s->Kind() == CStatementKind::kExpressionStatement) {
          const auto& es = static_cast<const ExpressionStatement&>(*s);
          bool emit_post_barrier = false;
          std::string post_barrier_var;
          if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
            if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
              std::string lhs(static_cast<const IdentifierExpression&>(a.Lhs()).Name());
              std::string rhs_sym;
              bool has_s32_cast = false;
              const bool is_sym_addr = ExtractDirectSymbolAddressRhs(a.Rhs(), external_data_symbols,
                                                                     &rhs_sym, &has_s32_cast);
              for (auto& [sym_key, var_list] : active_sym_vars) {
                if (!is_sym_addr || sym_key != rhs_sym) {
                  var_list.erase(std::remove(var_list.begin(), var_list.end(), lhs),
                                 var_list.end());
                }
              }
              if (IsPinnedCalleeSavedGprVar(lhs) && is_sym_addr) {
                auto& var_list = active_sym_vars[rhs_sym];
                var_list.erase(std::remove(var_list.begin(), var_list.end(), lhs), var_list.end());
                if (!var_list.empty()) {
                  const std::string& prev_var = var_list.back();
                  stmts.push_back(CStatement::Expression(CExpression::Identifier(absl::StrFormat(
                      "__asm__(\"\" : \"=r\"(%s) : \"0\"(%s))", prev_var, prev_var))));
                }
                var_list.push_back(lhs);
              } else if (has_calls && IsPinnedCalleeSavedGprVar(lhs) && !is_sym_addr &&
                         IsCseConflictProneCalleeSavedRhs(a.Rhs(), external_data_symbols, comp, idx,
                                                          lhs) &&
                         CalleeSavedCopyCrossesCallWithReadInBlock(comp, idx, lhs)) {
                emit_post_barrier = true;
                post_barrier_var = lhs;
              } else if (a.Rhs().Kind() == CExpressionKind::kIdentifier &&
                         external_data_symbols.count(std::string(
                             static_cast<const IdentifierExpression&>(a.Rhs()).Name())) > 0 &&
                         GlobalLoadHasNarrowToFloatAndSecondReadInBlock(comp, idx, lhs)) {
                emit_post_barrier = true;
                post_barrier_var = lhs;
              }
            }
          }
          stmts.push_back(s->Clone());
          if (emit_post_barrier) {
            stmts.push_back(CStatement::Expression(CExpression::Identifier(absl::StrFormat(
                "__asm__(\"\" : \"=r\"(%s) : \"0\"(%s))", post_barrier_var, post_barrier_var))));
          }
          continue;
        }
        if (s->Kind() != CStatementKind::kVariableDeclarationStatement) {
          active_sym_vars.clear();
        }
        auto rewritten =
            DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(*s, external_data_symbols, has_calls);
        if (has_calls && rewritten != nullptr &&
            rewritten->Kind() == CStatementKind::kDoWhileStatement) {
          const auto& dws = static_cast<const DoWhileStatement&>(*rewritten);
          if (dws.Body().Kind() == CStatementKind::kCompoundStatement) {
            const auto& body_comp = static_cast<const CompoundStatement&>(dws.Body());
            std::vector<std::unique_ptr<CStatement>> new_body_stmts;
            new_body_stmts.reserve(body_comp.Statements().size() + 1);
            bool modified_loop = false;
            for (size_t b_idx = 0; b_idx < body_comp.Statements().size(); ++b_idx) {
              const auto& bs = body_comp.Statements()[b_idx];
              new_body_stmts.push_back(bs != nullptr ? bs->Clone() : nullptr);
              if (bs != nullptr && bs->Kind() == CStatementKind::kExpressionStatement) {
                const auto& bes = static_cast<const ExpressionStatement&>(*bs);
                if (bes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& ba = static_cast<const AssignmentExpression&>(bes.Expression());
                  if (ba.Op() == "=" && ba.Lhs().Kind() == CExpressionKind::kIdentifier) {
                    std::string blhs(static_cast<const IdentifierExpression&>(ba.Lhs()).Name());
                    if (IsPinnedCalleeSavedGprVar(blhs) &&
                        ((IsCseConflictProneCalleeSavedRhs(ba.Rhs(), external_data_symbols,
                                                           body_comp, b_idx, blhs) &&
                          CalleeSavedCopyCrossesCallWithReadInBlock(comp, idx, blhs)) ||
                         (IsInPlaceConstAddSub(ba.Rhs(), blhs) &&
                          CalleeSavedCopyCrossesCallWithReadInBlock(
                              comp, idx, blhs, /*require_split_prone=*/true))) &&
                        !CalleeSavedCopyCrossesCallWithReadInBlock(body_comp, b_idx, blhs)) {
                      bool overwritten_later_in_loop = false;
                      for (size_t rem = b_idx + 1; rem < body_comp.Statements().size(); ++rem) {
                        if (body_comp.Statements()[rem] != nullptr &&
                            (StmtContainsNonIntrinsicCall(*body_comp.Statements()[rem]) ||
                             StmtOverwritesVar(*body_comp.Statements()[rem], blhs))) {
                          overwritten_later_in_loop = true;
                          break;
                        }
                      }
                      if (!overwritten_later_in_loop) {
                        new_body_stmts.push_back(
                            CStatement::Expression(CExpression::Identifier(absl::StrFormat(
                                "__asm__(\"\" : \"=r\"(%s) : \"0\"(%s))", blhs, blhs))));
                        modified_loop = true;
                      }
                    }
                  }
                }
              }
            }
            if (modified_loop) {
              rewritten = CStatement::DoWhile(
                  std::make_unique<CompoundStatement>(std::move(new_body_stmts)),
                  dws.Condition().Clone());
            }
          }
        }
        stmts.push_back(std::move(rewritten));
      }
      return std::make_unique<CompoundStatement>(std::move(stmts));
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(ifs.Condition().Clone(),
                            DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
                                ifs.ThenBranch(), external_data_symbols, has_calls),
                            ifs.ElseBranch() != nullptr
                                ? DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
                                      *ifs.ElseBranch(), external_data_symbols, has_calls)
                                : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(ws.Condition().Clone(),
                               DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
                                   ws.Body(), external_data_symbols, has_calls));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
                                     dws.Body(), external_data_symbols, has_calls),
                                 dws.Condition().Clone());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(fs.Init() != nullptr ? fs.Init()->Clone() : nullptr,
                             fs.Condition() != nullptr ? fs.Condition()->Clone() : nullptr,
                             fs.Step() != nullptr ? fs.Step()->Clone() : nullptr,
                             DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
                                 fs.Body(), external_data_symbols, has_calls));
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
          sc.body = std::unique_ptr<CompoundStatement>(
              static_cast<CompoundStatement*>(DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
                                                  *c.body, external_data_symbols, has_calls)
                                                  .release()));
        }
        cases.push_back(std::move(sc));
      }
      return CStatement::Switch(ss.Condition().Clone(), std::move(cases));
    }
    default:
      return stmt.Clone();
  }
}

bool StmtContainsCall(const CStatement& stmt);

bool ExprContainsCall(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kCallExpression:
      return true;
    case CExpressionKind::kUnaryExpression:
      return ExprContainsCall(static_cast<const UnaryExpression&>(expr).Operand());
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return ExprContainsCall(b.Lhs()) || ExprContainsCall(b.Rhs());
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return ExprContainsCall(a.Lhs()) || ExprContainsCall(a.Rhs());
    }
    case CExpressionKind::kCastExpression:
      return ExprContainsCall(static_cast<const CastExpression&>(expr).Operand());
    case CExpressionKind::kMemberAccessExpression:
      return ExprContainsCall(static_cast<const MemberAccessExpression&>(expr).Object());
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      return ExprContainsCall(a.Array()) || ExprContainsCall(a.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return ExprContainsCall(t.Condition()) || ExprContainsCall(t.TrueExpression()) ||
             ExprContainsCall(t.FalseExpression());
    }
    default:
      return false;
  }
}

bool StmtContainsCall(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr && StmtContainsCall(*s)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement:
      return ExprContainsCall(static_cast<const ExpressionStatement&>(stmt).Expression());
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return ExprContainsCall(ifs.Condition()) || StmtContainsCall(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StmtContainsCall(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return ExprContainsCall(ws.Condition()) || StmtContainsCall(ws.Body());
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return StmtContainsCall(dws.Body()) || ExprContainsCall(dws.Condition());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StmtContainsCall(*fs.Init())) ||
             (fs.Condition() != nullptr && ExprContainsCall(*fs.Condition())) ||
             (fs.Step() != nullptr && ExprContainsCall(*fs.Step())) || StmtContainsCall(fs.Body());
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      if (ExprContainsCall(ss.Condition())) return true;
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr && StmtContainsCall(*c.body)) return true;
      }
      return false;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return rs.ReturnValue() != nullptr && ExprContainsCall(*rs.ReturnValue());
    }
    default:
      return false;
  }
}

void CollectExprIdentifiers(const CExpression& expr, std::set<std::string>* out_ids) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier:
      out_ids->insert(static_cast<const IdentifierExpression&>(expr).Name());
      break;
    case CExpressionKind::kUnaryExpression:
      CollectExprIdentifiers(static_cast<const UnaryExpression&>(expr).Operand(), out_ids);
      break;
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      CollectExprIdentifiers(b.Lhs(), out_ids);
      CollectExprIdentifiers(b.Rhs(), out_ids);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      CollectExprIdentifiers(a.Lhs(), out_ids);
      CollectExprIdentifiers(a.Rhs(), out_ids);
      break;
    }
    case CExpressionKind::kCastExpression:
      CollectExprIdentifiers(static_cast<const CastExpression&>(expr).Operand(), out_ids);
      break;
    case CExpressionKind::kMemberAccessExpression:
      CollectExprIdentifiers(static_cast<const MemberAccessExpression&>(expr).Object(), out_ids);
      break;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      CollectExprIdentifiers(a.Array(), out_ids);
      CollectExprIdentifiers(a.Index(), out_ids);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      CollectExprIdentifiers(t.Condition(), out_ids);
      CollectExprIdentifiers(t.TrueExpression(), out_ids);
      CollectExprIdentifiers(t.FalseExpression(), out_ids);
      break;
    }
    default:
      break;
  }
}

bool StmtModifiesIdentifier(const CStatement& stmt, const std::string& name) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr && StmtModifiesIdentifier(*s, name)) return true;
      }
      return false;
    case CStatementKind::kExpressionStatement: {
      const auto& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
      if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
        std::set<std::string> lhs_ids;
        CollectExprIdentifiers(static_cast<const AssignmentExpression&>(expr).Lhs(), &lhs_ids);
        if (lhs_ids.count(name) > 0) return true;
      }
      return false;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StmtModifiesIdentifier(ifs.ThenBranch(), name) ||
             (ifs.ElseBranch() != nullptr && StmtModifiesIdentifier(*ifs.ElseBranch(), name));
    }
    case CStatementKind::kWhileStatement:
      return StmtModifiesIdentifier(static_cast<const WhileStatement&>(stmt).Body(), name);
    case CStatementKind::kDoWhileStatement:
      return StmtModifiesIdentifier(static_cast<const DoWhileStatement&>(stmt).Body(), name);
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StmtModifiesIdentifier(*fs.Init(), name)) ||
             StmtModifiesIdentifier(fs.Body(), name);
    }
    case CStatementKind::kSwitchStatement:
      for (const auto& c : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (c.body != nullptr && StmtModifiesIdentifier(*c.body, name)) return true;
      }
      return false;
    default:
      return false;
  }
}

void CollectSpinLoopVolatileSymbolsInStmt(const CStatement& stmt,
                                          const std::set<std::string>& local_vars,
                                          std::set<std::string>* volatile_symbols) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr) {
          CollectSpinLoopVolatileSymbolsInStmt(*s, local_vars, volatile_symbols);
        }
      }
      break;
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      CollectSpinLoopVolatileSymbolsInStmt(ifs.ThenBranch(), local_vars, volatile_symbols);
      if (ifs.ElseBranch() != nullptr) {
        CollectSpinLoopVolatileSymbolsInStmt(*ifs.ElseBranch(), local_vars, volatile_symbols);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      if (!StmtContainsCall(ws)) {
        std::set<std::string> cond_ids;
        CollectExprIdentifiers(ws.Condition(), &cond_ids);
        std::string sole_global;
        bool has_local = false;
        for (const auto& id : cond_ids) {
          if (local_vars.count(id) > 0) {
            has_local = true;
            break;
          }
          if (sole_global.empty()) {
            sole_global = id;
          } else {
            has_local = true;
            break;
          }
        }
        if (!has_local && !sole_global.empty() && !StmtModifiesIdentifier(ws.Body(), sole_global)) {
          volatile_symbols->insert(sole_global);
        }
      }
      CollectSpinLoopVolatileSymbolsInStmt(ws.Body(), local_vars, volatile_symbols);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      if (!StmtContainsCall(dws) && dws.Body().Kind() == CStatementKind::kCompoundStatement) {
        const auto& comp = static_cast<const CompoundStatement&>(dws.Body());
        if (comp.Statements().size() == 1 && comp.Statements()[0] != nullptr &&
            comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement) {
          const auto& expr =
              static_cast<const ExpressionStatement&>(*comp.Statements()[0]).Expression();
          if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& assign = static_cast<const AssignmentExpression&>(expr);
            if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
              std::string lhs = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
              std::set<std::string> cond_ids;
              CollectExprIdentifiers(dws.Condition(), &cond_ids);
              if (local_vars.count(lhs) > 0 && cond_ids.size() == 1 && cond_ids.count(lhs) == 1) {
                std::set<std::string> rhs_ids;
                CollectExprIdentifiers(assign.Rhs(), &rhs_ids);
                if (rhs_ids.size() == 1 && local_vars.count(*rhs_ids.begin()) == 0) {
                  volatile_symbols->insert(*rhs_ids.begin());
                }
              }
            }
          }
        }
      }
      CollectSpinLoopVolatileSymbolsInStmt(dws.Body(), local_vars, volatile_symbols);
      break;
    }
    case CStatementKind::kForStatement:
      CollectSpinLoopVolatileSymbolsInStmt(static_cast<const ForStatement&>(stmt).Body(),
                                           local_vars, volatile_symbols);
      break;
    case CStatementKind::kSwitchStatement:
      for (const auto& c : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (c.body != nullptr) {
          CollectSpinLoopVolatileSymbolsInStmt(*c.body, local_vars, volatile_symbols);
        }
      }
      break;
    default:
      break;
  }
}

void CountAddressTakenExternalDataSymbolsInExpr(const CExpression& expr,
                                                const std::set<std::string>& external_data_symbols,
                                                std::map<std::string, int>* counts) {
  std::string sym;
  bool has_cast = false;
  if (ExtractDirectSymbolAddressRhs(expr, external_data_symbols, &sym, &has_cast)) {
    (*counts)[sym]++;
    return;
  }
  switch (expr.Kind()) {
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      CountAddressTakenExternalDataSymbolsInExpr(a.Lhs(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInExpr(a.Rhs(), external_data_symbols, counts);
      break;
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      CountAddressTakenExternalDataSymbolsInExpr(b.Lhs(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInExpr(b.Rhs(), external_data_symbols, counts);
      break;
    }
    case CExpressionKind::kUnaryExpression:
      CountAddressTakenExternalDataSymbolsInExpr(
          static_cast<const UnaryExpression&>(expr).Operand(), external_data_symbols, counts);
      break;
    case CExpressionKind::kCastExpression:
      CountAddressTakenExternalDataSymbolsInExpr(static_cast<const CastExpression&>(expr).Operand(),
                                                 external_data_symbols, counts);
      break;
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      CountAddressTakenExternalDataSymbolsInExpr(call.Callee(), external_data_symbols, counts);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr) {
          CountAddressTakenExternalDataSymbolsInExpr(*arg, external_data_symbols, counts);
        }
      }
      break;
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& ai = static_cast<const ArrayIndexExpression&>(expr);
      CountAddressTakenExternalDataSymbolsInExpr(ai.Array(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInExpr(ai.Index(), external_data_symbols, counts);
      break;
    }
    case CExpressionKind::kMemberAccessExpression:
      CountAddressTakenExternalDataSymbolsInExpr(
          static_cast<const MemberAccessExpression&>(expr).Object(), external_data_symbols, counts);
      break;
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      CountAddressTakenExternalDataSymbolsInExpr(t.Condition(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInExpr(t.TrueExpression(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInExpr(t.FalseExpression(), external_data_symbols,
                                                 counts);
      break;
    }
    default:
      break;
  }
}

void CountAddressTakenExternalDataSymbolsInStmt(const CStatement& stmt,
                                                const std::set<std::string>& external_data_symbols,
                                                std::map<std::string, int>* counts) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (s != nullptr) {
          CountAddressTakenExternalDataSymbolsInStmt(*s, external_data_symbols, counts);
        }
      }
      break;
    case CStatementKind::kExpressionStatement:
      CountAddressTakenExternalDataSymbolsInExpr(
          static_cast<const ExpressionStatement&>(stmt).Expression(), external_data_symbols,
          counts);
      break;
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      CountAddressTakenExternalDataSymbolsInExpr(ifs.Condition(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInStmt(ifs.ThenBranch(), external_data_symbols, counts);
      if (ifs.ElseBranch() != nullptr) {
        CountAddressTakenExternalDataSymbolsInStmt(*ifs.ElseBranch(), external_data_symbols,
                                                   counts);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      CountAddressTakenExternalDataSymbolsInExpr(ws.Condition(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInStmt(ws.Body(), external_data_symbols, counts);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      CountAddressTakenExternalDataSymbolsInStmt(dws.Body(), external_data_symbols, counts);
      CountAddressTakenExternalDataSymbolsInExpr(dws.Condition(), external_data_symbols, counts);
      break;
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr) {
        CountAddressTakenExternalDataSymbolsInStmt(*fs.Init(), external_data_symbols, counts);
      }
      if (fs.Condition() != nullptr) {
        CountAddressTakenExternalDataSymbolsInExpr(*fs.Condition(), external_data_symbols, counts);
      }
      if (fs.Step() != nullptr) {
        CountAddressTakenExternalDataSymbolsInExpr(*fs.Step(), external_data_symbols, counts);
      }
      CountAddressTakenExternalDataSymbolsInStmt(fs.Body(), external_data_symbols, counts);
      break;
    }
    case CStatementKind::kSwitchStatement:
      break;
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr) {
        CountAddressTakenExternalDataSymbolsInExpr(*rs.ReturnValue(), external_data_symbols,
                                                   counts);
      }
      break;
    }
    default:
      break;
  }
}

bool IsO2FunctionBody(const CompoundStatement& body) {
  bool has_register_decl = false;
  bool has_sp_array = false;
  bool has_other_unpinned_local = false;
  for (const auto& s : body.Statements()) {
    if (s == nullptr || s->Kind() != CStatementKind::kVariableDeclarationStatement) continue;
    const auto& vd = static_cast<const VariableDeclarationStatement&>(*s);
    if (vd.RegisterAnnotation().has_value() && !vd.RegisterAnnotation()->empty()) {
      has_register_decl = true;
    } else if (vd.Name() == "sp" && vd.ArraySize().has_value()) {
      has_sp_array = true;
    } else if (vd.Name() != "v0" && vd.Name() != "v1") {
      has_other_unpinned_local = true;
    } else if (StmtReadsVar(body, vd.Name())) {
      has_other_unpinned_local = true;
    }
  }
  return has_register_decl || (has_sp_array && !has_other_unpinned_local);
}

bool ExtractNormalizedSymbolArg(const CExpression& expr,
                                const std::set<std::string>& dup_addr_symbols,
                                std::string* out_sym) {
  if (expr.Kind() == CExpressionKind::kIdentifier) {
    std::string name(static_cast<const IdentifierExpression&>(expr).Name());
    if (dup_addr_symbols.count(name) > 0) {
      *out_sym = std::move(name);
      return true;
    }
  } else if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& u = static_cast<const UnaryExpression&>(expr);
    if (u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
      std::string name(static_cast<const IdentifierExpression&>(u.Operand()).Name());
      if (dup_addr_symbols.count(name) > 0) {
        *out_sym = std::move(name);
        return true;
      }
    }
  }
  return false;
}

std::unique_ptr<CExpression> RewriteCallForDuplicateSymbolArgs(
    const CallExpression& call, const std::set<std::string>& dup_addr_symbols) {
  if (call.Callee().Kind() == CExpressionKind::kIdentifier &&
      IsCompilerIntrinsic(static_cast<const IdentifierExpression&>(call.Callee()).Name())) {
    return call.Clone();
  }
  for (const auto& arg : call.Arguments()) {
    if (arg != nullptr && ExprContainsNonIntrinsicCall(*arg)) {
      return call.Clone();
    }
  }
  bool changed = false;
  std::vector<std::unique_ptr<CExpression>> new_args;
  new_args.reserve(call.Arguments().size());
  for (size_t k = 0; k < call.Arguments().size(); ++k) {
    const auto& arg = call.Arguments()[k];
    std::string sym;
    if (arg != nullptr && ExtractNormalizedSymbolArg(*arg, dup_addr_symbols, &sym)) {
      int reg_num = (k < 4) ? static_cast<int>(4 + k) : 2;
      bool conflict = false;
      std::string reg_var_a = (k < 4) ? absl::StrFormat("arg%d", k) : "v0";
      std::string reg_var_b = (k < 4) ? absl::StrFormat("temp_a%d", k) : "v0";
      if (ExprReadsVar(call.Callee(), reg_var_a) || ExprReadsVar(call.Callee(), reg_var_b)) {
        conflict = true;
      }
      for (size_t m = 0; m < call.Arguments().size() && !conflict; ++m) {
        if (m == k || call.Arguments()[m] == nullptr) continue;
        if (ExprReadsVar(*call.Arguments()[m], reg_var_a) ||
            ExprReadsVar(*call.Arguments()[m], reg_var_b)) {
          conflict = true;
        }
      }
      if (!conflict) {
        new_args.push_back(CExpression::Identifier(absl::StrFormat(
            "({ register s32 _p asm(\"$%d\") = (s32)%s; _p; })", reg_num, arg->ToString())));
        changed = true;
        continue;
      }
    }
    new_args.push_back(arg != nullptr ? arg->Clone() : nullptr);
  }
  if (!changed) {
    return call.Clone();
  }
  return CExpression::Call(call.Callee().Clone(), std::move(new_args));
}

std::unique_ptr<CStatement> PreventDuplicateCallArgSymbolCseInStmt(
    const CStatement& stmt, const std::set<std::string>& dup_addr_symbols) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      std::vector<std::unique_ptr<CStatement>> stmts;
      stmts.reserve(comp.Statements().size());
      for (const auto& s : comp.Statements()) {
        if (s != nullptr) {
          stmts.push_back(PreventDuplicateCallArgSymbolCseInStmt(*s, dup_addr_symbols));
        }
      }
      return std::make_unique<CompoundStatement>(std::move(stmts));
    }
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      if (es.Expression().Kind() == CExpressionKind::kCallExpression) {
        return CStatement::Expression(RewriteCallForDuplicateSymbolArgs(
            static_cast<const CallExpression&>(es.Expression()), dup_addr_symbols));
      }
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            a.Rhs().Kind() == CExpressionKind::kCallExpression) {
          return CStatement::Expression(CExpression::Assignment(
              "=", a.Lhs().Clone(),
              RewriteCallForDuplicateSymbolArgs(static_cast<const CallExpression&>(a.Rhs()),
                                                dup_addr_symbols)));
        }
      }
      return stmt.Clone();
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr &&
          rs.ReturnValue()->Kind() == CExpressionKind::kCallExpression) {
        return CStatement::Return(RewriteCallForDuplicateSymbolArgs(
            static_cast<const CallExpression&>(*rs.ReturnValue()), dup_addr_symbols));
      }
      return stmt.Clone();
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(
          ifs.Condition().Clone(),
          PreventDuplicateCallArgSymbolCseInStmt(ifs.ThenBranch(), dup_addr_symbols),
          ifs.ElseBranch() != nullptr
              ? PreventDuplicateCallArgSymbolCseInStmt(*ifs.ElseBranch(), dup_addr_symbols)
              : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(ws.Condition().Clone(),
                               PreventDuplicateCallArgSymbolCseInStmt(ws.Body(), dup_addr_symbols));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          PreventDuplicateCallArgSymbolCseInStmt(dws.Body(), dup_addr_symbols),
          dws.Condition().Clone());
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(fs.Init() != nullptr ? fs.Init()->Clone() : nullptr,
                             fs.Condition() != nullptr ? fs.Condition()->Clone() : nullptr,
                             fs.Step() != nullptr ? fs.Step()->Clone() : nullptr,
                             PreventDuplicateCallArgSymbolCseInStmt(fs.Body(), dup_addr_symbols));
    }
    case CStatementKind::kSwitchStatement:
      return stmt.Clone();
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
  std::set<std::string> uncast_addr_arith_symbols;
  std::map<std::string, std::map<std::string, std::pair<CType, int>>> id_symbol_type_uses;
  std::set<std::string> volatile_data_symbols;

  for (const auto& function : functions) {
    ReferencedSymbols symbols = CollectReferencedSymbols(function);
    CollectGlobalCastDerefsInStmt(function.Body(), symbols.local_variables, &total_id_uses,
                                  &cast_deref_uses, &uncast_addr_arith_symbols,
                                  &id_symbol_type_uses);
    CollectSpinLoopVolatileSymbolsInStmt(function.Body(), symbols.local_variables,
                                         &volatile_data_symbols);
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
    if (array_data_symbols.count(data_symbol) > 0 ||
        uncast_addr_arith_symbols.count(data_symbol) > 0) {
      continue;
    }
    auto cast_it = cast_deref_uses.find(data_symbol);
    auto total_it = total_id_uses.find(data_symbol);
    if (cast_it == cast_deref_uses.end() || cast_it->second.empty() ||
        total_it == total_id_uses.end() || total_it->second <= 0) {
      continue;
    }
    int total_uses = total_it->second;
    std::map<std::string, std::pair<CType, int>> combined_types;
    if (auto id_it = id_symbol_type_uses.find(data_symbol); id_it != id_symbol_type_uses.end()) {
      for (const auto& [t_str, tc] : id_it->second) {
        auto& entry = combined_types[t_str];
        entry.first = tc.first;
        entry.second += tc.second;
      }
    }
    for (const auto& [t_str, tc] : cast_it->second) {
      auto& entry = combined_types[t_str];
      entry.first = tc.first;
      entry.second += tc.second;
    }
    int s32_uses = combined_types.count("s32") ? combined_types["s32"].second : 0;
    int s8_uses = combined_types.count("s8") ? combined_types["s8"].second : 0;
    int u8_uses = combined_types.count("u8") ? combined_types["u8"].second : 0;
    int byte_uses = s8_uses + u8_uses;
    if (byte_uses > 0 &&
        (byte_uses == total_uses ||
         (byte_uses >= 4 && byte_uses + s32_uses == total_uses && byte_uses >= s32_uses * 4))) {
      CType prom_type = (s8_uses >= u8_uses && s8_uses > 0) ? CType::S8() : CType::U8();
      promoted_scalar_globals[data_symbol] = prom_type;
      inferred_data_types[data_symbol] = prom_type;
      continue;
    }
    int s16_uses = combined_types.count("s16") ? combined_types["s16"].second : 0;
    int u16_uses = combined_types.count("u16") ? combined_types["u16"].second : 0;
    int half_uses = s16_uses + u16_uses;
    if (half_uses > 0 &&
        (half_uses == total_uses ||
         (half_uses >= 4 && half_uses + s32_uses == total_uses && half_uses >= s32_uses * 4))) {
      CType prom_type = (s16_uses >= u16_uses && s16_uses > 0) ? CType::S16() : CType::U16();
      promoted_scalar_globals[data_symbol] = prom_type;
      inferred_data_types[data_symbol] = prom_type;
      continue;
    }
    int s32_zero_stores =
        combined_types.count("s32_zero_store") ? combined_types["s32_zero_store"].second : 0;
    for (const char* t_str : {"f32", "f64"}) {
      int t_uses = combined_types.count(t_str) ? combined_types[t_str].second : 0;
      int allowed_zero_stores = (std::string_view(t_str) == "f32") ? s32_zero_stores : 0;
      if (t_uses > 0 && t_uses + allowed_zero_stores == total_uses) {
        CType prom_type = combined_types[t_str].first;
        promoted_scalar_globals[data_symbol] = prom_type;
        inferred_data_types[data_symbol] = prom_type;
        break;
      }
    }
  }

  std::set<std::string> all_function_names = external_function_names;
  all_function_names.insert(defined_function_names.begin(), defined_function_names.end());

  std::string combined_function_code;
  for (size_t i = 0; i < functions.size(); ++i) {
    const bool has_calls = !per_func_symbols[i].external_functions.empty() ||
                           StmtContainsNonIntrinsicCall(functions[i].Body());
    std::set<std::string> dup_addr_symbols;
    if (has_calls && IsO2FunctionBody(functions[i].Body())) {
      std::map<std::string, int> addr_counts;
      CountAddressTakenExternalDataSymbolsInStmt(functions[i].Body(), external_data_symbols,
                                                 &addr_counts);
      for (const auto& [sym, cnt] : addr_counts) {
        if (cnt >= 2) {
          dup_addr_symbols.insert(sym);
        }
      }
    }
    std::unique_ptr<CompoundStatement> disambiguated_body(
        static_cast<CompoundStatement*>(DisambiguateDuplicateCalleeSavedSymbolLoadsInStmt(
                                            functions[i].Body(), external_data_symbols, has_calls)
                                            .release()));
    std::unique_ptr<CompoundStatement> body_to_emit;
    if (functions.size() > 1 || !array_data_symbols.empty() || !promoted_scalar_globals.empty()) {
      body_to_emit = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
          NormalizeArraySymbolsInStmt(*disambiguated_body, array_data_symbols,
                                      per_func_symbols[i].local_variables, inferred_data_types,
                                      all_function_names, promoted_scalar_globals)
              .release()));
    } else {
      body_to_emit = std::move(disambiguated_body);
    }
    if (!dup_addr_symbols.empty()) {
      body_to_emit = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
          PreventDuplicateCallArgSymbolCseInStmt(*body_to_emit, dup_addr_symbols).release()));
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
      const char* volatile_qual =
          (volatile_data_symbols.count(data_symbol) > 0 && array_suffix.empty()) ? "volatile " : "";
      output += absl::StrFormat("extern %s%s %s%s;\n", volatile_qual, data_type.ToString(),
                                data_symbol, array_suffix);
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
