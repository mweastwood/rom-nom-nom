#include "splitter/symbol_registry.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "google/protobuf/text_format.h"
#include "splitter/symbol_registry.pb.h"

namespace rom_nom_nom {

absl::Status ValidateSymbolRegistry(const SymbolRegistry& registry) {
  absl::flat_hash_set<uint32_t> seen_addresses;
  absl::flat_hash_set<std::string_view> seen_names;

  for (int i = 0; i < registry.entries_size(); ++i) {
    const SymbolRegistryEntry& entry = registry.entries(i);
    if (entry.name().empty()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("SymbolRegistryEntry at index %d has an empty name", i));
    }
    if (entry.type() == SYMBOL_TYPE_UNSPECIFIED) {
      return absl::InvalidArgumentError(absl::StrFormat("Symbol '%s' (0x%08X) has unspecified type",
                                                        entry.name(), entry.address()));
    }
    if (!seen_addresses.insert(entry.address()).second) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Duplicate symbol address: 0x%08X (symbol '%s')", entry.address(), entry.name()));
    }
    if (!seen_names.insert(entry.name()).second) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Duplicate symbol name: '%s'", entry.name()));
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<SymbolRegistry> ParseSymbolRegistry(std::string_view textproto_content) {
  SymbolRegistry registry;
  google::protobuf::TextFormat::Parser parser;
  if (!parser.ParseFromString(std::string(textproto_content), &registry)) {
    return absl::InvalidArgumentError("Failed to parse SymbolRegistry textproto: syntax error");
  }

  auto status = ValidateSymbolRegistry(registry);
  if (!status.ok()) {
    return status;
  }
  return registry;
}

absl::StatusOr<SymbolRegistry> LoadSymbolRegistry(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return absl::NotFoundError(absl::StrFormat("Failed to open symbol file: %s", path.string()));
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return ParseSymbolRegistry(buffer.str());
}

// --- SymbolIndex Implementation ---

SymbolIndex::SymbolIndex(SymbolRegistry registry) : registry_(std::move(registry)) {
  RebuildIndex();
}

SymbolIndex::SymbolIndex(SymbolIndex&& other) noexcept : registry_(std::move(other.registry_)) {
  RebuildIndex();
}

SymbolIndex& SymbolIndex::operator=(SymbolIndex&& other) noexcept {
  if (this != &other) {
    registry_ = std::move(other.registry_);
    RebuildIndex();
  }
  return *this;
}

void SymbolIndex::RebuildIndex() {
  by_address_.clear();
  by_name_.clear();
  by_address_.reserve(registry_.entries_size());
  by_name_.reserve(registry_.entries_size());

  for (const auto& entry : registry_.entries()) {
    by_address_[entry.address()] = &entry;
    by_name_[entry.name()] = &entry;
  }
}

absl::StatusOr<SymbolIndex> SymbolIndex::FromProto(SymbolRegistry registry) {
  auto status = ValidateSymbolRegistry(registry);
  if (!status.ok()) {
    return status;
  }
  return SymbolIndex(std::move(registry));
}

absl::StatusOr<SymbolIndex> SymbolIndex::ParseFromTextproto(std::string_view textproto_content) {
  auto registry_or = ParseSymbolRegistry(textproto_content);
  if (!registry_or.ok()) {
    return registry_or.status();
  }
  return SymbolIndex(std::move(*registry_or));
}

absl::StatusOr<SymbolIndex> SymbolIndex::LoadFromTextproto(const std::filesystem::path& path) {
  auto registry_or = LoadSymbolRegistry(path);
  if (!registry_or.ok()) {
    return registry_or.status();
  }
  return SymbolIndex(std::move(*registry_or));
}

const SymbolRegistryEntry* SymbolIndex::FindByAddress(uint32_t address) const {
  auto it = by_address_.find(address);
  return it != by_address_.end() ? it->second : nullptr;
}

const SymbolRegistryEntry* SymbolIndex::FindByName(std::string_view name) const {
  auto it = by_name_.find(name);
  return it != by_name_.end() ? it->second : nullptr;
}

bool SymbolIndex::HasAddress(uint32_t address) const {
  return by_address_.contains(address);
}

bool SymbolIndex::HasName(std::string_view name) const {
  return by_name_.contains(name);
}

std::string SymbolIndex::LookupOrSynthesizeName(uint32_t address, SymbolType fallback_type) const {
  const SymbolRegistryEntry* entry = FindByAddress(address);
  if (entry != nullptr) {
    return std::string(entry->name());
  }

  switch (fallback_type) {
    case SYMBOL_FUNC:
      return absl::StrFormat("func_%08X", address);
    case SYMBOL_LABEL:
      return absl::StrFormat(".L%08X", address);
    default:
      return absl::StrFormat("D_%08X", address);
  }
}

std::vector<const SymbolRegistryEntry*> SymbolIndex::SortedEntries() const {
  std::vector<const SymbolRegistryEntry*> entries;
  entries.reserve(by_address_.size());
  for (const auto& [_, entry] : by_address_) {
    entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(),
            [](const SymbolRegistryEntry* a, const SymbolRegistryEntry* b) {
              return a->address() < b->address();
            });
  return entries;
}

}  // namespace rom_nom_nom
