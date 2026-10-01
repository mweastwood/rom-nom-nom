#ifndef DIFFER_RELOCATION_APPLIER_H_
#define DIFFER_RELOCATION_APPLIER_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "differ/elf_reader.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

// Result of applying relocations to a function.
struct RelocationApplyResult {
  std::vector<uint32_t> patched_words;
  std::vector<std::string> unresolved_symbols;
};

// Function signature for resolving symbol names to 32-bit VRAM addresses.
using SymbolLookupFn = std::function<std::optional<uint32_t>(std::string_view)>;

class RelocationApplier {
 public:
  // Applies relocations to the raw words of an ElfFunction using a custom lookup function.
  static RelocationApplyResult Apply(const ElfFunction& function,
                                     const SymbolLookupFn& symbol_lookup);

  // Convenience overload: uses an address map.
  static RelocationApplyResult Apply(const ElfFunction& function,
                                     const absl::flat_hash_map<std::string, uint32_t>& symbol_map);

  // Convenience overload: uses a SymbolIndex.
  static RelocationApplyResult Apply(const ElfFunction& function, const SymbolIndex& symbol_index);

  // Low-level helper: applies relocations to a vector of words given a list of relocations.
  static RelocationApplyResult ApplyRelocations(absl::Span<const uint32_t> words,
                                                absl::Span<const ElfRelocation> relocations,
                                                const SymbolLookupFn& symbol_lookup);
};

}  // namespace rom_nom_nom

#endif  // DIFFER_RELOCATION_APPLIER_H_
