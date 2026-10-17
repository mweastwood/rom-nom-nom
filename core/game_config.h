#ifndef CORE_GAME_CONFIG_H_
#define CORE_GAME_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/game_config.pb.h"

namespace rom_nom_nom {

// Resolved subsegment layout with implicit ROM/VRAM boundaries and BSS sizes filled in.
struct ResolvedSubsegment {
  const Subsegment* proto = nullptr;
  std::string name;
  SubsegmentType type = SUBSEGMENT_TYPE_UNSPECIFIED;
  uint32_t rom_start = 0;
  uint32_t rom_end = 0;
  uint32_t vram_start = 0;
  uint32_t vram_end = 0;
  uint32_t bss_size = 0;

  uint32_t RomSize() const { return rom_end > rom_start ? rom_end - rom_start : 0; }
};

// Resolved segment layout with implicit ROM/VRAM boundaries, BSS sizes, and subsegments.
struct ResolvedSegment {
  const Segment* proto = nullptr;
  std::string name;
  SegmentType type = SEGMENT_TYPE_UNSPECIFIED;
  uint32_t rom_start = 0;
  uint32_t rom_end = 0;
  uint32_t vram_start = 0;
  uint32_t vram_end = 0;
  uint32_t bss_size = 0;
  std::vector<ResolvedSubsegment> subsegments;

  uint32_t RomSize() const { return rom_end > rom_start ? rom_end - rom_start : 0; }
};

// Validates semantic integrity of a GameConfig (non-overlapping segments,
// monotonic ROM ranges, non-empty fields, valid types, and valid embedded symbols).
absl::Status ValidateGameConfig(const GameConfig& config);

// Parses and validates a GameConfig from a textproto-formatted string.
absl::StatusOr<GameConfig> ParseGameConfig(std::string_view textproto_content);

// Resolves all segments and subsegments in a GameConfig, computing implicit
// ROM end offsets, derived subsegment VRAM addresses, and BSS sizes.
std::vector<ResolvedSegment> ResolveSegments(const GameConfig& config,
                                             std::optional<size_t> total_rom_size = std::nullopt);

// Translates a VRAM address to a physical ROM offset using the segments in GameConfig.
// Returns std::nullopt if the VRAM address does not map to a ROM-backed segment.
std::optional<uint32_t> VramToRomOffset(const GameConfig& config, uint32_t vram,
                                        std::optional<size_t> total_rom_size = std::nullopt);

// Resolves compiler flags for a given object stem from GameConfig.c_flags,
// falling back to the "default" key if no per-file override exists.
std::vector<std::string> ResolveCFlags(const GameConfig& config, std::string_view object_stem);

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
