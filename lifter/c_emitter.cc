#include "lifter/c_emitter.h"

#include <unistd.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "core/c_ast.h"
#include "core/process.h"
#include "lifter/ast_codegen_lowerer.h"

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

  LoweredTranslationUnit lowered = AstCodegenLowerer::LowerTranslationUnit(
      functions, symbol_index, header_provided_functions, parent_module_context.get());

  std::string combined_function_code;
  for (const auto& norm_fn : lowered.functions) {
    combined_function_code += norm_fn.ToString();
    if (!combined_function_code.empty() && combined_function_code.back() != '\n') {
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

  if (!lowered.external_function_names.empty()) {
    for (const auto& external_function : lowered.external_function_names) {
      CType return_type = CType::S32();
      auto it = lowered.inferred_return_types.find(external_function);
      if (it != lowered.inferred_return_types.end()) {
        return_type = it->second;
      }
      output += absl::StrFormat("extern %s %s();\n", return_type.ToString(), external_function);
    }
    output += "\n";
  }

  if (!lowered.external_data_symbols.empty()) {
    for (const auto& data_symbol : lowered.external_data_symbols) {
      CType data_type = CType::S32();
      auto it = lowered.inferred_data_types.find(data_symbol);
      if (it != lowered.inferred_data_types.end()) {
        data_type = it->second;
      }
      std::string array_suffix = lowered.array_data_symbols.count(data_symbol) > 0 ? "[]" : "";
      const char* volatile_qual =
          (lowered.volatile_data_symbols.count(data_symbol) > 0 && array_suffix.empty())
              ? "volatile "
              : "";
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
