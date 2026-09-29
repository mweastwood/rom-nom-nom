#ifndef SPLITTER_SYMBOL_REGISTRY_H_
#define SPLITTER_SYMBOL_REGISTRY_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "splitter/symbol_registry.pb.h"

namespace rom_nom_nom {

// Validates semantic integrity of a SymbolRegistry (no duplicate addresses or names,
// non-empty names, valid symbol types).
absl::Status ValidateSymbolRegistry(const SymbolRegistry& registry);

// Parses and validates a SymbolRegistry from a textproto-formatted string.
absl::StatusOr<SymbolRegistry> ParseSymbolRegistry(std::string_view textproto_content);

// Loads, parses, and validates a SymbolRegistry from a .textproto file on disk.
absl::StatusOr<SymbolRegistry> LoadSymbolRegistry(const std::filesystem::path& path);

// Indexed query engine providing fast O(1) lookups and label synthesis
// over a SymbolRegistry.
class SymbolIndex {
 public:
  // Factory methods
  static absl::StatusOr<SymbolIndex> LoadFromTextproto(const std::filesystem::path& path);
  static absl::StatusOr<SymbolIndex> ParseFromTextproto(std::string_view textproto_content);
  static absl::StatusOr<SymbolIndex> FromProto(SymbolRegistry registry);

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
  std::vector<const SymbolRegistryEntry*> SortedEntries() const;

  size_t Size() const { return by_address_.size(); }
  bool Empty() const { return by_address_.empty(); }

 private:
  explicit SymbolIndex(SymbolRegistry registry);
  void RebuildIndex();

  SymbolRegistry registry_;
  absl::flat_hash_map<uint32_t, const SymbolRegistryEntry*> by_address_;
  absl::flat_hash_map<std::string_view, const SymbolRegistryEntry*> by_name_;
};

using SymbolRegistryIndex = SymbolIndex;

}  // namespace rom_nom_nom

#endif  // SPLITTER_SYMBOL_REGISTRY_H_
