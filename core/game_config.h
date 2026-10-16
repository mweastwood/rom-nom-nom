#ifndef CORE_GAME_CONFIG_H_
#define CORE_GAME_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/game_config.pb.h"

namespace rom_nom_nom {

// Validates semantic integrity of a GameConfig (non-overlapping segments,
// monotonic ROM ranges, non-empty fields, valid types, and valid embedded symbols).
absl::Status ValidateGameConfig(const GameConfig& config);

// Parses and validates a GameConfig from a textproto-formatted string.
absl::StatusOr<GameConfig> ParseGameConfig(std::string_view textproto_content);

// Classification of a VRAM address based on the memory map defined in GameConfig.
enum class SectionVramClassification {
  kUnknown,
  kCode,
  kData,
};

// Classifies a given VRAM address as code or data based on the segment/subsegment layout in
// GameConfig.
SectionVramClassification ClassifyVramAddress(const GameConfig& config, uint32_t vram_address);

// Loads, parses, and validates a GameConfig from a .textproto file on disk.
absl::StatusOr<GameConfig> LoadGameConfig(const std::filesystem::path& path);

}  // namespace rom_nom_nom

#endif  // CORE_GAME_CONFIG_H_
