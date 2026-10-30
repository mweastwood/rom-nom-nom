#ifndef LIFTER_C_EMITTER_H_
#define LIFTER_C_EMITTER_H_

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/c_ast.h"
#include "core/symbol_registry.h"
#include "lifter/ast_codegen_lowerer.h"

namespace rom_nom_nom {

// Configuration options for C code emission and formatting.
struct CEmitterOptions {
  std::vector<std::string> includes = {"types.h"};
  bool format_with_clang = true;
  std::string clang_format_style = "file";
};

// Options for generating the standard `types.h` header.
struct TypesHeaderOptions {
  bool include_unaligned_structs = true;
  bool include_math_intrinsics = true;
  std::string extra_declarations;
  std::function<bool(std::string_view)> should_skip_math_intrinsic;
  std::set<std::string>* emitted_math_intrinsics = nullptr;
};

// Represents a complete C translation unit (source file).
struct CTranslationUnit {
  std::vector<std::string> includes;
  std::vector<FunctionDeclaration> functions;
  const SymbolIndex* symbol_index = nullptr;
  std::set<std::string> header_provided_functions;
  std::shared_ptr<const ModuleSymbolContext> parent_module_context;

  std::string ToString() const;
};

// Emits clean, Google-style C source code from C AST representations.
class CEmitter {
 public:
  // Emits the standard N64 `types.h` header content.
  static std::string EmitTypesHeader(const TypesHeaderOptions& options = {});

  // Returns the list of standard inline math intrinsic names provided by `types.h`.
  static std::vector<std::string_view> StandardMathIntrinsicNames();

  // Emits a single function declaration into formatted C code.
  static std::string EmitFunction(const FunctionDeclaration& func,
                                  const CEmitterOptions& options = {});

  // Emits a full translation unit into formatted C code.
  static std::string EmitTranslationUnit(const CTranslationUnit& tu,
                                         const CEmitterOptions& options = {});

  // Runs clang-format on the given C source string, falling back to original code on error.
  static std::string FormatCode(std::string_view unformatted_code, std::string_view style = "file");
};

}  // namespace rom_nom_nom

#endif  // LIFTER_C_EMITTER_H_
