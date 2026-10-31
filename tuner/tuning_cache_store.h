#ifndef TUNER_TUNING_CACHE_STORE_H_
#define TUNER_TUNING_CACHE_STORE_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/game_config.h"

namespace rom_nom_nom {

// Computes a deterministic SHA-1 content hash over a function's retail target instruction words
// and its baseline lifted C representation. Any change to the target binary or lifter output
// for that function invalidates only that function's cache entry.
std::string ComputeFunctionTuningHash(absl::Span<const uint32_t> target_raw_words,
                                      std::string_view baseline_c_code);

// In-memory store and persistence wrapper around `TuningCache` (.textproto).
class TuningCacheStore {
 public:
  TuningCacheStore() = default;
  explicit TuningCacheStore(TuningCache cache);

  static absl::StatusOr<TuningCacheStore> LoadFromFile(const std::filesystem::path& path);
  static TuningCacheStore FromGameConfig(const GameConfig& config,
                                         const std::filesystem::path& repo_root = {});

  // Looks up a cached tuning recipe for `function_name` whose `input_sha1` matches `expected_sha1`.
  // Returns nullptr on cache miss or hash mismatch.
  const FunctionTuningEntry* Lookup(std::string_view function_name,
                                    std::string_view expected_sha1) const;

  bool ContainsFunction(std::string_view function_name) const {
    return entries_.contains(function_name);
  }

  // Inserts or replaces a per-function tuning entry.
  void Upsert(FunctionTuningEntry entry);

  // Removes a per-function tuning entry if present. Returns true if an entry was removed.
  bool Remove(std::string_view function_name);

  // Sets or updates module-level compiler flags.
  void SetModuleCFlags(std::string_view module_name, const std::vector<std::string>& flags);
  std::vector<std::string> GetModuleCFlags(std::string_view module_name) const;

  // Exports a sorted, deterministic `TuningCache` protobuf message.
  TuningCache ToProto(std::string_view game_name = "") const;

  // Saves the deterministic `TuningCache` to a `.textproto` file on disk.
  absl::Status SaveToFile(const std::filesystem::path& path, std::string_view game_name = "") const;

  size_t Size() const { return entries_.size(); }
  bool Empty() const { return entries_.empty() && module_c_flags_.empty(); }

 private:
  std::string game_name_;
  absl::flat_hash_map<std::string, FunctionTuningEntry> entries_;
  absl::flat_hash_map<std::string, std::vector<std::string>> module_c_flags_;
};

}  // namespace rom_nom_nom

#endif  // TUNER_TUNING_CACHE_STORE_H_
