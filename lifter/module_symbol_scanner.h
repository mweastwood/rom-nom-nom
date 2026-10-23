#ifndef LIFTER_MODULE_SYMBOL_SCANNER_H_
#define LIFTER_MODULE_SYMBOL_SCANNER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/game_config.pb.h"
#include "core/symbol_registry.h"
#include "lifter/function_loader.h"

namespace rom_nom_nom {

// Discovers code modules, resolves per-module function names from split assembly
// and symbol registries, and generates linker scripts for lifted C translation units.
class ModuleSymbolScanner {
 public:
  // Discovers all code modules (C or ASM subsegments) registered in the split configuration.
  static std::vector<std::string> DiscoverAllCodeModules(const GameConfig& config);

  // Resolves all function names registered within a module / subsegment using
  // nonmatchings directories, standalone module assembly files, or symbol ranges.
  static absl::StatusOr<std::vector<std::string>> DiscoverFunctionsInModule(
      std::string_view module_name, const GameConfig& config, const SymbolIndex& symbols,
      const std::filesystem::path& repo_root, std::string_view game_name,
      const std::filesystem::path& asm_dir);

  // Writes lifted_symbols.ld to output_dir, anchoring data/BSS symbols relative to subsegment
  // base labels and resolving unexported internal code/data symbols.
  static void WriteLiftedSymbolsLinkerScript(
      const std::filesystem::path& output_dir, const FunctionLoader* loader,
      const SymbolIndex* symbols, const absl::flat_hash_set<std::string>& defined_function_names,
      absl::Span<const std::pair<std::string, std::string>> undefined_auto_symbols);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_MODULE_SYMBOL_SCANNER_H_
