#ifndef CORE_SYMBOL_REGISTRY_H_
#define CORE_SYMBOL_REGISTRY_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/game_config.pb.h"

namespace rom_nom_nom {

// Validates semantic integrity of a SymbolRegistry (no duplicate addresses or names,
// non-empty names, valid symbol types).
absl::Status ValidateSymbolRegistry(const SymbolRegistry& registry);

// Parses and validates a SymbolRegistry from a textproto-formatted string.
absl::StatusOr<SymbolRegistry> ParseSymbolRegistry(std::string_view textproto_content);

// Loads, parses, and validates a SymbolRegistry from a .textproto file on disk.
absl::StatusOr<SymbolRegistry> LoadSymbolRegistry(const std::filesystem::path& path);

// Indexed query engine providing fast O(1) lookups and label synthesis
// over a SymbolRegistry or GameConfig.
class SymbolIndex {
 public:
  // Factory methods
  static absl::StatusOr<SymbolIndex> LoadFromTextproto(const std::filesystem::path& path);
  static absl::StatusOr<SymbolIndex> ParseFromTextproto(std::string_view textproto_content);
  static absl::StatusOr<SymbolIndex> FromProto(SymbolRegistry registry);
  static absl::StatusOr<SymbolIndex> FromGameConfig(const GameConfig& config);

  SymbolIndex() = default;

  // Move-only semantics to maintain stable pointer indexing over registry_
  SymbolIndex(SymbolIndex&& other) noexcept;
  SymbolIndex& operator=(SymbolIndex&& other) noexcept;
  SymbolIndex(const SymbolIndex&) = delete;
  SymbolIndex& operator=(const SymbolIndex&) = delete;

  // Access to underlying protobuf message
  const SymbolRegistry& Proto() const { return registry_; }

  // Fast O(1) lookups
  const SymbolRegistryEntry* FindByAddress(uint32_t address) const;
  const SymbolRegistryEntry* FindByName(std::string_view name) const;
  bool HasAddress(uint32_t address) const;
  bool HasName(std::string_view name) const;

  // Resolves a name for an address, synthesizing standard labels if unregistered:
  // - SYMBOL_FUNC:  "func_XXXXXXXX"
  // - SYMBOL_LABEL: ".LXXXXXXXX"
  // - Other:        "D_XXXXXXXX"
  std::string LookupOrSynthesizeName(uint32_t address,
                                     SymbolType fallback_type = SYMBOL_FUNC) const;

  // Returns all registered symbol entries sorted ascending by address.
  const std::vector<const SymbolRegistryEntry*>& SortedEntries() const { return sorted_entries_; }

  // Formats all registered symbols as GNU ld PROVIDE statements (`PROVIDE(name = 0xXXXXXXXX);`).
  std::string GenerateSymbolsScript() const;

  size_t Size() const { return by_address_.size(); }
  bool Empty() const { return by_address_.empty(); }

 private:
  explicit SymbolIndex(SymbolRegistry registry);
  void RebuildIndex();

  SymbolRegistry registry_;
  absl::flat_hash_map<uint32_t, const SymbolRegistryEntry*> by_address_;
  absl::flat_hash_map<std::string_view, const SymbolRegistryEntry*> by_name_;
  std::vector<const SymbolRegistryEntry*> sorted_entries_;
};

using SymbolRegistryIndex = SymbolIndex;

// Resolves a symbol name or address representation to a 32-bit VRAM address.
// Checks registered symbols in `symbols` (if non-null), `0x...` hex literals,
// `.LXXXXXXXX` local labels, `func_XXXXXXXX` / `D_XXXXXXXX` / `*_XXXXXXXX`
// synthesized labels, and 8-digit hex strings.
std::optional<uint32_t> ResolveSymbolOrSynthesizedAddress(std::string_view name_or_addr,
                                                          const SymbolIndex* symbols = nullptr);

// Options for unified SymbolIndex loading across overrides, explicit paths, and GameConfig.
struct SymbolLoadOptions {
  std::string symbols_text_override;
  std::filesystem::path symbols_path;
  const GameConfig* config = nullptr;
  std::filesystem::path repo_root;
  std::string game_name;
};

// Loads a SymbolIndex according to priority:
// 1. symbols_text_override
// 2. symbols_path
// 3. config inline symbols + symbol_files
// 4. fallback configs/<game>.symbols.textproto or symbols/<game>.textproto discovery
absl::StatusOr<SymbolIndex> LoadSymbolIndex(const SymbolLoadOptions& options);

}  // namespace rom_nom_nom

#endif  // CORE_SYMBOL_REGISTRY_H_
