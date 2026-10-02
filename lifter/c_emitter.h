#ifndef LIFTER_C_EMITTER_H_
#define LIFTER_C_EMITTER_H_

#include <string>
#include <string_view>
#include <vector>

#include "core/c_ast.h"

namespace rom_nom_nom {

// Configuration options for C code emission and formatting.
struct CEmitterOptions {
  std::vector<std::string> includes = {"common.h"};
  bool format_with_clang = true;
  std::string clang_format_style = "file";
};

// Represents a complete C translation unit (source file).
struct CTranslationUnit {
  std::vector<std::string> includes;
  std::vector<FunctionDeclaration> functions;

  std::string ToString() const;
};

// Emits clean, Google-style C source code from C AST representations.
class CEmitter {
 public:
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
