#ifndef LIFTER_FUNCTION_LOADER_H_
#define LIFTER_FUNCTION_LOADER_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "differ/target_extractor.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

struct FunctionLoaderOptions {
  std::filesystem::path repo_root = ".";
  std::string game_name = "harvest-moon-64";
};

// Represents a loaded function ready for decompilation lifting.
struct LoadedFunction {
  std::string name;
  uint32_t vram = 0;
  std::vector<Instruction> instructions;
  const SymbolIndex* symbol_index = nullptr;
};

// Loads target functions from ROM binaries, disassembly, or in-memory sequences.
class FunctionLoader {
 public:
  static absl::StatusOr<std::unique_ptr<FunctionLoader>> Create(FunctionLoaderOptions options = {});

  explicit FunctionLoader(std::unique_ptr<TargetExtractor> extractor);

  // Loads a function by name or VRAM address (e.g. "MessageInit", "0x800266C0", "func_XXXXXXXX").
  absl::StatusOr<LoadedFunction> LoadFunction(std::string_view func_name_or_addr) const;

  // Convenience helper for tests or direct word decoding.
  static absl::StatusOr<LoadedFunction> FromWords(std::string name,
                                                  absl::Span<const uint32_t> words,
                                                  uint32_t base_vram = 0x80000000,
                                                  const SymbolIndex* symbol_index = nullptr);

  const TargetExtractor* Extractor() const { return extractor_.get(); }

 private:
  std::unique_ptr<TargetExtractor> extractor_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_FUNCTION_LOADER_H_
