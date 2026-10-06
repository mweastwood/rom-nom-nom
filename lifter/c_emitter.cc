#include "lifter/c_emitter.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "core/c_ast.h"
#include "core/process.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

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
  std::string combined_function_code;
  for (const auto& function : functions) {
    defined_function_names.insert(function.Name());
    combined_function_code += function.ToString();
    if (combined_function_code.back() != '\n') {
      combined_function_code += "\n";
    }
  }

  std::set<std::string> external_function_names;
  std::set<std::string> external_data_symbols;

  for (const auto& function : functions) {
    ReferencedSymbols symbols = CollectReferencedSymbols(function);

    for (const auto& func_name : symbols.external_functions) {
      if (defined_function_names.count(func_name) > 0) {
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
      if (symbol_index != nullptr) {
        const auto* entry = symbol_index->FindByName(data_name);
        if (entry != nullptr && entry->type() == SYMBOL_FUNC) {
          external_function_names.insert(data_name);
          continue;
        }
      }
      external_data_symbols.insert(data_name);
    }
  }

  for (const auto& func_name : external_function_names) {
    external_data_symbols.erase(func_name);
  }

  if (!external_function_names.empty()) {
    for (const auto& external_function : external_function_names) {
      if (external_function == "sqrtf" || external_function == "fabsf" ||
          external_function == "sqrt" || external_function == "fabs") {
        if (symbol_index != nullptr && symbol_index->FindByName(external_function) != nullptr) {
          if (external_function == "sqrtf" || external_function == "fabsf") {
            output += absl::StrFormat("extern f32 %s(f32);\n", external_function);
          } else {
            output += absl::StrFormat("extern f64 %s(f64);\n", external_function);
          }
        }
        continue;
      }
      output += absl::StrFormat("extern void %s();\n", external_function);
    }
    output += "\n";
  }

  if (!external_data_symbols.empty()) {
    for (const auto& data_symbol : external_data_symbols) {
      output += absl::StrFormat("extern s32 %s;\n", data_symbol);
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
