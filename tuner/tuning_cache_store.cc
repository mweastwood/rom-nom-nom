#include "tuner/tuning_cache_store.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/endian.h"
#include "core/sha1.h"

namespace rom_nom_nom {

std::string ComputeFunctionTuningHash(absl::Span<const uint32_t> target_raw_words,
                                      std::string_view baseline_c_code) {
  std::vector<uint8_t> payload;
  payload.reserve(target_raw_words.size() * 4 + 1 + baseline_c_code.size());
  for (uint32_t word : target_raw_words) {
    uint8_t buf[4];
    WriteBigEndian32(buf, word);
    payload.insert(payload.end(), buf, buf + 4);
  }
  payload.push_back(0xFF);
  payload.insert(payload.end(), baseline_c_code.begin(), baseline_c_code.end());
  return ComputeSha1Hex(payload);
}

TuningCacheStore::TuningCacheStore(TuningCache cache) : game_name_(cache.game_name()) {
  for (const auto& entry : cache.entries()) {
    entries_[std::string(entry.function_name())] = entry;
  }
  for (const auto& [mod_name, flags_proto] : cache.module_c_flags()) {
    module_c_flags_[mod_name] =
        std::vector<std::string>(flags_proto.flags().begin(), flags_proto.flags().end());
  }
}

absl::StatusOr<TuningCacheStore> TuningCacheStore::LoadFromFile(const std::filesystem::path& path) {
  auto cache_or = LoadTuningCache(path);
  if (!cache_or.ok()) {
    return cache_or.status();
  }
  return TuningCacheStore(std::move(*cache_or));
}

TuningCacheStore TuningCacheStore::FromGameConfig(const GameConfig& config,
                                                  const std::filesystem::path& repo_root) {
  TuningCacheStore store;
  store.game_name_ = std::string(config.game_name());

  std::error_code ec;
  if (!config.tuning_cache_file().empty()) {
    std::filesystem::path p(config.tuning_cache_file());
    if (p.is_relative() && !repo_root.empty()) {
      p = repo_root / p;
    }
    if (std::filesystem::exists(p, ec)) {
      auto loaded_or = LoadFromFile(p);
      if (loaded_or.ok()) {
        store = std::move(*loaded_or);
      }
    }
  } else if (!config.basename().empty() && !repo_root.empty()) {
    std::filesystem::path default_cache =
        repo_root / "config" / (std::string(config.basename()) + ".tuning.textproto");
    if (std::filesystem::exists(default_cache, ec)) {
      auto loaded_or = LoadFromFile(default_cache);
      if (loaded_or.ok()) {
        store = std::move(*loaded_or);
      }
    }
  }

  for (const auto& entry : config.tuning_entries()) {
    store.Upsert(entry);
  }
  return store;
}

const FunctionTuningEntry* TuningCacheStore::Lookup(std::string_view function_name,
                                                    std::string_view expected_sha1) const {
  auto it = entries_.find(function_name);
  if (it == entries_.end()) {
    return nullptr;
  }
  if (!expected_sha1.empty() && it->second.input_sha1() != expected_sha1) {
    return nullptr;
  }
  return &it->second;
}

void TuningCacheStore::Upsert(FunctionTuningEntry entry) {
  std::string key(entry.function_name());
  entries_[std::move(key)] = std::move(entry);
}

bool TuningCacheStore::Remove(std::string_view function_name) {
  return entries_.erase(function_name) > 0;
}

void TuningCacheStore::SetModuleCFlags(std::string_view module_name,
                                       const std::vector<std::string>& flags) {
  module_c_flags_[std::string(module_name)] = flags;
}

std::vector<std::string> TuningCacheStore::GetModuleCFlags(std::string_view module_name) const {
  auto it = module_c_flags_.find(module_name);
  if (it == module_c_flags_.end()) {
    return {};
  }
  return it->second;
}

TuningCache TuningCacheStore::ToProto(std::string_view game_name) const {
  TuningCache out;
  if (!game_name.empty()) {
    out.set_game_name(std::string(game_name));
  } else if (!game_name_.empty()) {
    out.set_game_name(game_name_);
  }

  std::vector<FunctionTuningEntry> sorted_entries;
  sorted_entries.reserve(entries_.size());
  for (const auto& [_, entry] : entries_) {
    sorted_entries.push_back(entry);
  }
  std::sort(sorted_entries.begin(), sorted_entries.end(),
            [](const FunctionTuningEntry& a, const FunctionTuningEntry& b) {
              if (a.module_name() != b.module_name()) return a.module_name() < b.module_name();
              return a.function_name() < b.function_name();
            });
  for (auto& entry : sorted_entries) {
    *out.add_entries() = std::move(entry);
  }

  for (const auto& [mod_name, flags] : module_c_flags_) {
    CompilerFlags cf;
    for (const auto& f : flags) {
      cf.add_flags(f);
    }
    (*out.mutable_module_c_flags())[mod_name] = std::move(cf);
  }
  return out;
}

absl::Status TuningCacheStore::SaveToFile(const std::filesystem::path& path,
                                          std::string_view game_name) const {
  return SaveTuningCache(ToProto(game_name), path);
}

}  // namespace rom_nom_nom
