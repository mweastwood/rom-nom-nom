#ifndef LIFTER_LIFTER_PIPELINE_H_
#define LIFTER_LIFTER_PIPELINE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "core/c_ast.h"
#include "lifter/function_loader.h"

namespace rom_nom_nom {

// Configuration options for the lifter pipeline.
struct LifterPipelineOptions {
  std::filesystem::path repo_root = ".";
  std::string game_name = "harvest-moon-64";
  std::filesystem::path config_path;
  std::filesystem::path symbols_path;
  std::filesystem::path rom_path;
  std::filesystem::path asm_dir;
  std::vector<std::string> includes = {"types.h"};
  bool format_with_clang = true;
};

// Result of a decompilation lifting pass.
struct LifterResult {
  std::string function_name;
  uint32_t vram = 0;
  size_t instruction_count = 0;
  std::unique_ptr<FunctionDeclaration> ast;
  std::string c_code;
};

// End-to-end pipeline orchestrating MIPS disassembly to formatted C source code.
class LifterPipeline {
 public:
  static absl::StatusOr<std::unique_ptr<LifterPipeline>> Create(LifterPipelineOptions options = {});

  explicit LifterPipeline(LifterPipelineOptions options,
                          std::unique_ptr<FunctionLoader> loader = nullptr);

  // Decompiles a function given by name or VRAM address (e.g. "InterpolateInit", "0x800266C0").
  absl::StatusOr<LifterResult> Decompile(std::string_view func_name_or_addr) const;

  // Decompiles multiple functions in order into a single formatted translation unit.
  absl::StatusOr<std::string> DecompileFunctions(
      const std::vector<std::string>& func_names,
      const std::vector<std::string>& includes = {}) const;

  // Resolves all function names registered within a module / subsegment.
  absl::StatusOr<std::vector<std::string>> GetFunctionsInModule(std::string_view module_name) const;

  // Decompiles an entire module into a single formatted translation unit.
  absl::StatusOr<std::string> DecompileModule(std::string_view module_name,
                                              const std::vector<std::string>& includes = {}) const;

  // Discovers all code modules registered in the split configuration.
  absl::StatusOr<std::vector<std::string>> GetAllModules() const;

  // Decompiles all modules in the game and writes each to <output_dir>/<module_name>.c.
  absl::StatusOr<std::vector<std::filesystem::path>> DecompileAllModules(
      const std::filesystem::path& output_dir) const;

  // Decompiles an already loaded or synthesized function directly.
  absl::StatusOr<LifterResult> DecompileFunction(
      const LoadedFunction& loaded_func,
      const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr) const;

  const LifterPipelineOptions& Options() const { return options_; }
  const FunctionLoader* Loader() const { return loader_.get(); }

 private:
  LifterPipelineOptions options_;
  std::unique_ptr<FunctionLoader> loader_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_LIFTER_PIPELINE_H_
