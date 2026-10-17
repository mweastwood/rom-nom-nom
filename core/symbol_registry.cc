#include "core/symbol_registry.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/game_config.pb.h"
#include "google/protobuf/text_format.h"

namespace rom_nom_nom {
namespace {

bool IsHexDigits(std::string_view s, size_t expected_len = 0) {
  if (expected_len > 0 && s.size() != expected_len) {
    return false;
  }
  if (s.empty() || s.size() > 8) {
    return false;
  }
  for (char c : s) {
    if (!absl::ascii_isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

}  // namespace

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
    GameConfig game_config;
    if (parser.ParseFromString(std::string(textproto_content), &game_config)) {
      absl::flat_hash_set<uint32_t> seen_addresses;
      absl::flat_hash_set<std::string> seen_names;
      for (const auto& sym : game_config.symbols()) {
        seen_addresses.insert(sym.address());
        seen_names.insert(std::string(sym.name()));
        *registry.add_entries() = sym;
      }
      for (const auto& sym_file : game_config.symbol_files()) {
        if (std::filesystem::exists(sym_file)) {
          auto file_reg_or = LoadSymbolRegistry(sym_file);
          if (file_reg_or.ok()) {
            for (const auto& sym : file_reg_or->entries()) {
              if (seen_addresses.insert(sym.address()).second &&
                  seen_names.insert(std::string(sym.name())).second) {
                *registry.add_entries() = sym;
              }
            }
          }
        }
      }
      auto status = ValidateSymbolRegistry(registry);
      if (!status.ok()) {
        return status;
      }
      return registry;
    }
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
  sorted_entries_.clear();
  by_address_.reserve(registry_.entries_size());
  by_name_.reserve(registry_.entries_size());
  sorted_entries_.reserve(registry_.entries_size());

  for (const auto& entry : registry_.entries()) {
    by_address_[entry.address()] = &entry;
    by_name_[entry.name()] = &entry;
    sorted_entries_.push_back(&entry);
  }
  std::sort(sorted_entries_.begin(), sorted_entries_.end(),
            [](const SymbolRegistryEntry* a, const SymbolRegistryEntry* b) {
              return a->address() < b->address();
            });
}

absl::StatusOr<SymbolIndex> SymbolIndex::FromProto(SymbolRegistry registry) {
  auto status = ValidateSymbolRegistry(registry);
  if (!status.ok()) {
    return status;
  }
  return SymbolIndex(std::move(registry));
}

absl::StatusOr<SymbolIndex> SymbolIndex::FromGameConfig(const GameConfig& config) {
  SymbolRegistry registry;
  registry.mutable_entries()->Reserve(config.symbols_size());
  absl::flat_hash_set<uint32_t> seen_addresses;
  absl::flat_hash_set<std::string> seen_names;
  for (const auto& sym : config.symbols()) {
    seen_addresses.insert(sym.address());
    seen_names.insert(std::string(sym.name()));
    *registry.add_entries() = sym;
  }
  for (const auto& sym_file : config.symbol_files()) {
    if (std::filesystem::exists(sym_file)) {
      auto file_reg_or = LoadSymbolRegistry(sym_file);
      if (file_reg_or.ok()) {
        for (const auto& sym : file_reg_or->entries()) {
          if (seen_addresses.insert(sym.address()).second &&
              seen_names.insert(std::string(sym.name())).second) {
            *registry.add_entries() = sym;
          }
        }
      }
    }
  }
  return FromProto(std::move(registry));
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

std::string SymbolIndex::GenerateSymbolsScript() const {
  std::string out;
  absl::StrAppend(&out, "/* Auto-generated global symbol definitions */\n");
  for (const auto* entry : SortedEntries()) {
    absl::StrAppendFormat(&out, "PROVIDE(%s = 0x%08X);\n", entry->name(), entry->address());
  }
  return out;
}

std::optional<uint32_t> ResolveSymbolOrSynthesizedAddress(std::string_view name_or_addr,
                                                          const SymbolIndex* symbols) {
  if (name_or_addr.empty()) {
    return std::nullopt;
  }

  if (symbols != nullptr) {
    const SymbolRegistryEntry* entry = symbols->FindByName(name_or_addr);
    if (entry != nullptr) {
      return entry->address();
    }
  }

  if (absl::StartsWith(name_or_addr, "0x") || absl::StartsWith(name_or_addr, "0X")) {
    std::string_view hex = name_or_addr.substr(2);
    uint32_t addr = 0;
    if (IsHexDigits(hex) && absl::SimpleHexAtoi(hex, &addr)) {
      return addr;
    }
  }

  if (absl::StartsWith(name_or_addr, ".L") && name_or_addr.size() == 10) {
    std::string_view hex = name_or_addr.substr(2);
    uint32_t addr = 0;
    if (IsHexDigits(hex, 8) && absl::SimpleHexAtoi(hex, &addr)) {
      return addr;
    }
  }

  size_t last_underscore = name_or_addr.rfind('_');
  if (last_underscore != std::string_view::npos && last_underscore + 1 < name_or_addr.size() &&
      name_or_addr.size() - (last_underscore + 1) == 8) {
    std::string_view hex = name_or_addr.substr(last_underscore + 1);
    uint32_t addr = 0;
    if (IsHexDigits(hex, 8) && absl::SimpleHexAtoi(hex, &addr)) {
      return addr;
    }
  }

  if (IsHexDigits(name_or_addr, 8)) {
    bool has_digit = false;
    for (char c : name_or_addr) {
      if (absl::ascii_isdigit(static_cast<unsigned char>(c))) {
        has_digit = true;
        break;
      }
    }
    uint32_t addr = 0;
    if (has_digit && absl::SimpleHexAtoi(name_or_addr, &addr)) {
      return addr;
    }
  }

  return std::nullopt;
}

absl::StatusOr<SymbolIndex> LoadSymbolIndex(const SymbolLoadOptions& options) {
  if (!options.symbols_text_override.empty()) {
    return SymbolIndex::ParseFromTextproto(options.symbols_text_override);
  }

  if (!options.symbols_path.empty()) {
    return SymbolIndex::LoadFromTextproto(options.symbols_path);
  }

  if (options.config != nullptr &&
      (options.config->symbols_size() > 0 || options.config->symbol_files_size() > 0)) {
    SymbolRegistry registry;
    registry.mutable_entries()->Reserve(options.config->symbols_size());
    absl::flat_hash_set<uint32_t> seen_addresses;
    absl::flat_hash_set<std::string> seen_names;
    for (const auto& sym : options.config->symbols()) {
      seen_addresses.insert(sym.address());
      seen_names.insert(std::string(sym.name()));
      *registry.add_entries() = sym;
    }
    for (const auto& sym_file : options.config->symbol_files()) {
      std::filesystem::path p(sym_file);
      std::error_code ec;
      if (!std::filesystem::exists(p, ec) && p.is_relative() && !options.repo_root.empty()) {
        p = options.repo_root / p;
      }
      if (std::filesystem::exists(p, ec)) {
        auto file_reg_or = LoadSymbolRegistry(p);
        if (file_reg_or.ok()) {
          for (const auto& sym : file_reg_or->entries()) {
            if (seen_addresses.insert(sym.address()).second &&
                seen_names.insert(std::string(sym.name())).second) {
              *registry.add_entries() = sym;
            }
          }
        }
      }
    }
    if (registry.entries_size() > 0) {
      return SymbolIndex::FromProto(std::move(registry));
    }
  }

  std::string game_name = options.game_name;
  if (game_name.empty() && options.config != nullptr) {
    game_name = options.config->basename().empty() ? std::string(options.config->game_name())
                                                   : std::string(options.config->basename());
  }

  if (!game_name.empty()) {
    const std::vector<std::filesystem::path> rel_candidates = {
        std::filesystem::path("configs") / absl::StrCat(game_name, ".symbols.textproto"),
        std::filesystem::path("config") / absl::StrCat(game_name, ".symbols.textproto"),
        std::filesystem::path("symbols") / absl::StrCat(game_name, ".textproto"),
        std::filesystem::path("configs") / absl::StrCat(game_name, "_symbols.textproto"),
    };
    for (const auto& rel : rel_candidates) {
      std::filesystem::path cand = options.repo_root.empty() ? rel : (options.repo_root / rel);
      std::error_code ec;
      if (std::filesystem::exists(cand, ec)) {
        auto idx_or = SymbolIndex::LoadFromTextproto(cand);
        if (idx_or.ok()) {
          return idx_or;
        }
      }
    }
  }

  return SymbolIndex{};
}

}  // namespace rom_nom_nom
