#ifndef SPLITTER_CONFIG_H_
#define SPLITTER_CONFIG_H_

#include <filesystem>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "splitter/config.pb.h"

namespace rom_nom_nom {

// Validates semantic integrity of a SplitConfig (non-overlapping segments,
// monotonic ROM ranges, non-empty fields, valid types).
absl::Status ValidateSplitConfig(const SplitConfig& config);

// Parses and validates a SplitConfig from a textproto-formatted string.
absl::StatusOr<SplitConfig> ParseSplitConfig(std::string_view textproto_content);

// Loads, parses, and validates a SplitConfig from a .textproto file on disk.
absl::StatusOr<SplitConfig> LoadSplitConfig(const std::filesystem::path& path);

}  // namespace rom_nom_nom

#endif  // SPLITTER_CONFIG_H_
