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

// Classification of a VRAM address based on the memory map defined in SplitConfig.
enum class SectionVramClassification {
  kUnknown,
  kCode,
  kData,
};

// Classifies a given VRAM address as code or data based on the segment/subsegment layout in
// SplitConfig.
SectionVramClassification ClassifyVramAddress(const SplitConfig& config, uint32_t vram_address);

// Loads, parses, and validates a SplitConfig from a .textproto file on disk.
absl::StatusOr<SplitConfig> LoadSplitConfig(const std::filesystem::path& path);

}  // namespace rom_nom_nom

#endif  // SPLITTER_CONFIG_H_
