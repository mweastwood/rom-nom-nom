#include "lifter/c_emitter.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "core/c_ast.h"
#include "core/process.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

namespace {

bool IsCompilerIntrinsic(std::string_view name) {
  return absl::StartsWith(name, "__n64_") || absl::StartsWith(name, "__builtin_");
}

std::unique_ptr<CExpression> NormalizeArraySymbolsInExpr(
    const CExpression& expr, const std::set<std::string>& array_symbols,
    const std::set<std::string>& local_vars, const std::map<std::string, CType>& module_data_types,
    const std::set<std::string>& external_function_names) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      if (local_vars.count(id.Name()) == 0) {
        if (id.SymbolType().has_value() && !id.IsArraySymbol()) {
          bool is_func = external_function_names.count(id.Name()) > 0;
          auto type_it = module_data_types.find(id.Name());
          bool type_conflict =
              is_func || (type_it != module_data_types.end() &&
                          type_it->second.ToString() != id.SymbolType()->ToString());
          if (type_conflict) {
            std::unique_ptr<CExpression> addr =
                (!is_func && array_symbols.count(id.Name()) > 0)
                    ? CExpression::Identifier(id.Name())
                    : CExpression::Unary("&", CExpression::Identifier(id.Name()));
            return CExpression::Unary(
                "*", CExpression::Cast(id.SymbolType()->MakePointer(), std::move(addr)));
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
      const auto& u = static_cast<const UnaryExpression&>(expr);
      if (u.IsPrefix() && u.Op() == "&" && u.Operand().Kind() == CExpressionKind::kIdentifier) {
        return expr.Clone();
      }
      return CExpression::Unary(
          u.Op(),
          NormalizeArraySymbolsInExpr(u.Operand(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          u.IsPrefix());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      std::unique_ptr<CExpression> new_idx = NormalizeArraySymbolsInExpr(
          a.Index(), array_symbols, local_vars, module_data_types, external_function_names);
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
                                      external_function_names),
          std::move(new_idx));
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(
          b.Op(),
          NormalizeArraySymbolsInExpr(b.Lhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          NormalizeArraySymbolsInExpr(b.Rhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      return CExpression::Assignment(
          a.Op(),
          NormalizeArraySymbolsInExpr(a.Lhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          NormalizeArraySymbolsInExpr(a.Rhs(), array_symbols, local_vars, module_data_types,
                                      external_function_names));
    }
    case CExpressionKind::kCallExpression: {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        args.push_back(NormalizeArraySymbolsInExpr(*arg, array_symbols, local_vars,
                                                   module_data_types, external_function_names));
      }
      std::unique_ptr<CExpression> new_callee =
          c.Callee().Kind() == CExpressionKind::kIdentifier
              ? c.Callee().Clone()
              : NormalizeArraySymbolsInExpr(c.Callee(), array_symbols, local_vars,
                                            module_data_types, external_function_names);
      return CExpression::Call(std::move(new_callee), std::move(args));
    }
    case CExpressionKind::kCastExpression: {
      const auto& c = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(
          c.TargetType(), NormalizeArraySymbolsInExpr(c.Operand(), array_symbols, local_vars,
                                                      module_data_types, external_function_names));
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(
          NormalizeArraySymbolsInExpr(m.Object(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          m.MemberName(), m.IsArrow());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          NormalizeArraySymbolsInExpr(t.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          NormalizeArraySymbolsInExpr(t.TrueExpression(), array_symbols, local_vars,
                                      module_data_types, external_function_names),
          NormalizeArraySymbolsInExpr(t.FalseExpression(), array_symbols, local_vars,
                                      module_data_types, external_function_names));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> NormalizeArraySymbolsInStmt(
    const CStatement& stmt, const std::set<std::string>& array_symbols,
    const std::set<std::string>& local_vars, const std::map<std::string, CType>& module_data_types,
    const std::set<std::string>& external_function_names) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      std::vector<std::unique_ptr<CStatement>> stmts;
      stmts.reserve(comp.Statements().size());
      for (const auto& s : comp.Statements()) {
        stmts.push_back(NormalizeArraySymbolsInStmt(*s, array_symbols, local_vars,
                                                    module_data_types, external_function_names));
      }
      return std::make_unique<CompoundStatement>(std::move(stmts));
    }
    case CStatementKind::kExpressionStatement:
      return CStatement::Expression(NormalizeArraySymbolsInExpr(
          static_cast<const ExpressionStatement&>(stmt).Expression(), array_symbols, local_vars,
          module_data_types, external_function_names));
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      std::unique_ptr<CExpression> init =
          vd.Initializer() != nullptr
              ? NormalizeArraySymbolsInExpr(*vd.Initializer(), array_symbols, local_vars,
                                            module_data_types, external_function_names)
              : nullptr;
      return CStatement::VariableDeclaration(vd.Type(), vd.Name(), std::move(init), vd.ArraySize());
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(
          NormalizeArraySymbolsInExpr(ifs.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          NormalizeArraySymbolsInStmt(ifs.ThenBranch(), array_symbols, local_vars,
                                      module_data_types, external_function_names),
          ifs.ElseBranch() != nullptr
              ? NormalizeArraySymbolsInStmt(*ifs.ElseBranch(), array_symbols, local_vars,
                                            module_data_types, external_function_names)
              : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(
          NormalizeArraySymbolsInExpr(ws.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          NormalizeArraySymbolsInStmt(ws.Body(), array_symbols, local_vars, module_data_types,
                                      external_function_names));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          NormalizeArraySymbolsInStmt(dws.Body(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
          NormalizeArraySymbolsInExpr(dws.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          fs.Init() != nullptr
              ? NormalizeArraySymbolsInStmt(*fs.Init(), array_symbols, local_vars,
                                            module_data_types, external_function_names)
              : nullptr,
          fs.Condition() != nullptr
              ? NormalizeArraySymbolsInExpr(*fs.Condition(), array_symbols, local_vars,
                                            module_data_types, external_function_names)
              : nullptr,
          fs.Step() != nullptr
              ? NormalizeArraySymbolsInExpr(*fs.Step(), array_symbols, local_vars,
                                            module_data_types, external_function_names)
              : nullptr,
          NormalizeArraySymbolsInStmt(fs.Body(), array_symbols, local_vars, module_data_types,
                                      external_function_names));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() == nullptr) return CStatement::Return();
      return CStatement::Return(NormalizeArraySymbolsInExpr(*rs.ReturnValue(), array_symbols,
                                                            local_vars, module_data_types,
                                                            external_function_names));
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
                                          external_function_names)
                  .release()));
        }
        cases.push_back(std::move(sc));
      }
      return CStatement::Switch(
          NormalizeArraySymbolsInExpr(ss.Condition(), array_symbols, local_vars, module_data_types,
                                      external_function_names),
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

  for (const auto& function : functions) {
    ReferencedSymbols symbols = CollectReferencedSymbols(function);
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

  std::set<std::string> all_function_names = external_function_names;
  all_function_names.insert(defined_function_names.begin(), defined_function_names.end());

  std::string combined_function_code;
  for (size_t i = 0; i < functions.size(); ++i) {
    if (functions.size() > 1 || !array_data_symbols.empty()) {
      auto norm_body = std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(
          NormalizeArraySymbolsInStmt(functions[i].Body(), array_data_symbols,
                                      per_func_symbols[i].local_variables, inferred_data_types,
                                      all_function_names)
              .release()));
      FunctionDeclaration norm_fn(functions[i].ReturnType(), functions[i].Name(),
                                  functions[i].Parameters(), std::move(norm_body),
                                  functions[i].IsStatic());
      combined_function_code += norm_fn.ToString();
    } else {
      combined_function_code += functions[i].ToString();
    }
    if (combined_function_code.back() != '\n') {
      combined_function_code += "\n";
    }
  }

  output += combined_function_code;
  return output;
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
