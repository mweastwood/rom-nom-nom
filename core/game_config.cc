#include "core/game_config.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/game_config.pb.h"
#include "google/protobuf/text_format.h"

namespace rom_nom_nom {

absl::Status ValidateGameConfig(const GameConfig& config) {
  if (config.game_name().empty()) {
    return absl::InvalidArgumentError("GameConfig.game_name cannot be empty");
  }
  if (config.sha1().empty()) {
    return absl::InvalidArgumentError("GameConfig.sha1 cannot be empty");
  }
  if (config.basename().empty()) {
    return absl::InvalidArgumentError("GameConfig.basename cannot be empty");
  }
  if (config.segments().empty()) {
    return absl::InvalidArgumentError("GameConfig must contain at least one segment");
  }

  uint32_t prev_rom_start = 0;
  bool has_prev_seg = false;
  for (int i = 0; i < config.segments_size(); ++i) {
    const Segment& seg = config.segments(i);
    if (seg.name().empty()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Segment at index %d has an empty name", i));
    }
    if (seg.type() == SEGMENT_TYPE_UNSPECIFIED) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Segment '%s' has unspecified type", seg.name()));
    }
    if (seg.type() == SEGMENT_BSS) {
      // BSS segment has no ROM footprint; rom_start is not part of the physical cartridge layout.
      continue;
    }
    if (has_prev_seg && seg.rom_start() <= prev_rom_start) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Segment '%s' rom_start (0x%X) must be strictly greater than "
                          "previous segment start (0x%X)",
                          seg.name(), seg.rom_start(), prev_rom_start));
    }
    prev_rom_start = seg.rom_start();
    has_prev_seg = true;

    uint32_t prev_sub_start = 0;
    bool has_prev_sub = false;
    for (int j = 0; j < seg.subsegments_size(); ++j) {
      const Subsegment& sub = seg.subsegments(j);
      if (sub.type() == SUBSEGMENT_TYPE_UNSPECIFIED) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Subsegment '%s' in segment '%s' has unspecified type", sub.name(), seg.name()));
      }
      if (sub.type() == SUBSEGMENT_BSS) {
        // BSS subsegments exist only in RAM and have no ROM cartridge footprint.
        continue;
      }
      if (has_prev_sub && sub.rom_start() <= prev_sub_start) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Subsegment '%s' rom_start (0x%X) in segment '%s' must be strictly "
                            "greater than previous subsegment start (0x%X)",
                            sub.name(), sub.rom_start(), seg.name(), prev_sub_start));
      }
      prev_sub_start = sub.rom_start();
      has_prev_sub = true;
    }
  }

  absl::flat_hash_set<uint32_t> seen_addresses;
  absl::flat_hash_set<std::string_view> seen_names;
  for (int i = 0; i < config.symbols_size(); ++i) {
    const SymbolRegistryEntry& entry = config.symbols(i);
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

absl::StatusOr<GameConfig> ParseGameConfig(std::string_view textproto_content) {
  GameConfig config;
  google::protobuf::TextFormat::Parser parser;
  if (!parser.ParseFromString(std::string(textproto_content), &config)) {
    return absl::InvalidArgumentError("Failed to parse GameConfig textproto content: syntax error");
  }

  auto status = ValidateGameConfig(config);
  if (!status.ok()) {
    return status;
  }
  return config;
}

absl::StatusOr<GameConfig> LoadGameConfig(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return absl::NotFoundError(absl::StrFormat("Failed to open config file: %s", path.string()));
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return ParseGameConfig(buffer.str());
}

std::vector<ResolvedSegment> ResolveSegments(const GameConfig& config,
                                             std::optional<size_t> total_rom_size) {
  std::vector<ResolvedSegment> resolved;
  resolved.reserve(config.segments_size());

  for (int i = 0; i < config.segments_size(); ++i) {
    const Segment& seg = config.segments(i);
    ResolvedSegment rseg;
    rseg.proto = &seg;
    rseg.name = seg.name();
    rseg.type = seg.type();
    rseg.rom_start = seg.rom_start();
    rseg.rom_end = seg.rom_end();

    if (seg.type() != SEGMENT_BSS && rseg.rom_end == 0) {
      uint32_t inferred_end =
          total_rom_size.has_value() ? static_cast<uint32_t>(*total_rom_size) : 0;
      for (int k = i + 1; k < config.segments_size(); ++k) {
        if (config.segments(k).type() != SEGMENT_BSS &&
            config.segments(k).rom_start() > rseg.rom_start) {
          inferred_end = config.segments(k).rom_start();
          if (total_rom_size.has_value()) {
            inferred_end = std::min(inferred_end, static_cast<uint32_t>(*total_rom_size));
          }
          break;
        }
      }
      rseg.rom_end = inferred_end;
    }

    rseg.vram_start = seg.vram();
    rseg.bss_size = seg.bss_size();
    rseg.vram_end = (rseg.vram_start != 0) ? (rseg.vram_start + rseg.RomSize() + rseg.bss_size) : 0;

    rseg.subsegments.reserve(seg.subsegments_size());
    for (int j = 0; j < seg.subsegments_size(); ++j) {
      const Subsegment& sub = seg.subsegments(j);
      ResolvedSubsegment rsub;
      rsub.proto = &sub;
      rsub.name = sub.name();
      rsub.type = sub.type();
      rsub.rom_start = sub.rom_start();

      if (sub.type() == SUBSEGMENT_BSS) {
        rsub.rom_end = sub.rom_start();
        uint32_t bss_size = sub.bss_size() > 0 ? sub.bss_size() : seg.bss_size();
        if (sub.bss_size() == 0) {
          for (const auto& other_seg : config.segments()) {
            if (other_seg.name() == sub.name() && other_seg.bss_size() > 0) {
              bss_size = other_seg.bss_size();
              break;
            }
          }
        }
        rsub.bss_size = bss_size;
        if (sub.vram() != 0) {
          rsub.vram_start = sub.vram();
        } else if (rseg.vram_start != 0) {
          rsub.vram_start = (sub.rom_start() > rseg.rom_start)
                                ? (rseg.vram_start + (sub.rom_start() - rseg.rom_start))
                                : (rseg.vram_start + rseg.RomSize());
        }
        rsub.vram_end = rsub.vram_start + rsub.bss_size;
        if (rseg.bss_size == 0 && rsub.bss_size > 0) {
          rseg.bss_size = rsub.bss_size;
        }
      } else {
        uint32_t sub_end = rseg.rom_end;
        for (int k = j + 1; k < seg.subsegments_size(); ++k) {
          if (seg.subsegments(k).type() != SUBSEGMENT_BSS && seg.subsegments(k).rom_start() > 0) {
            sub_end = seg.subsegments(k).rom_start();
            break;
          }
        }
        rsub.rom_end = sub_end;
        rsub.bss_size = sub.bss_size();
        if (sub.vram() != 0) {
          rsub.vram_start = sub.vram();
        } else if (rseg.vram_start != 0) {
          rsub.vram_start = rseg.vram_start + (rsub.rom_start - rseg.rom_start);
        }
        if (rsub.bss_size > 0) {
          rsub.vram_end = rsub.vram_start + rsub.bss_size;
        } else if (rsub.RomSize() > 0) {
          rsub.vram_end = rsub.vram_start + rsub.RomSize();
        } else if (j + 1 < seg.subsegments_size()) {
          const auto& next_sub = seg.subsegments(j + 1);
          rsub.vram_end = next_sub.vram() != 0
                              ? next_sub.vram()
                              : (rseg.vram_start != 0
                                     ? rseg.vram_start + (next_sub.rom_start() - rseg.rom_start)
                                     : 0);
        } else {
          rsub.vram_end = rseg.vram_end;
        }
      }

      if (rseg.vram_start != 0) {
        rseg.vram_end = std::max(rseg.vram_end, rsub.vram_end);
      }
      rseg.subsegments.push_back(std::move(rsub));
    }

    resolved.push_back(std::move(rseg));
  }

  return resolved;
}

std::optional<uint32_t> VramToRomOffset(const GameConfig& config, uint32_t vram,
                                        std::optional<size_t> total_rom_size) {
  std::vector<ResolvedSegment> segments = ResolveSegments(config, total_rom_size);
  for (const auto& seg : segments) {
    if (seg.type == SEGMENT_BSS || seg.vram_start == 0) {
      continue;
    }
    for (const auto& sub : seg.subsegments) {
      if (sub.type == SUBSEGMENT_BSS || sub.vram_start == 0 || sub.RomSize() == 0) {
        continue;
      }
      if (vram >= sub.vram_start && vram < sub.vram_start + sub.RomSize()) {
        return sub.rom_start + (vram - sub.vram_start);
      }
    }
    if (seg.RomSize() > 0 && vram >= seg.vram_start && vram < seg.vram_start + seg.RomSize()) {
      return seg.rom_start + (vram - seg.vram_start);
    }
  }
  return std::nullopt;
}

std::vector<std::string> ResolveCFlags(const GameConfig& config, std::string_view object_stem) {
  auto it = config.c_flags().find(object_stem);
  if (it == config.c_flags().end()) {
    it = config.c_flags().find("default");
  }
  if (it == config.c_flags().end()) {
    return {};
  }
  return std::vector<std::string>(it->second.flags().begin(), it->second.flags().end());
}

SectionVramClassification ClassifyVramAddress(const GameConfig& config, uint32_t vram_address) {
  for (const auto& segment : ResolveSegments(config)) {
    if (segment.vram_start == 0) {
      continue;
    }
    if (vram_address < segment.vram_start || vram_address >= segment.vram_end) {
      continue;
    }

    for (const auto& subsegment : segment.subsegments) {
      if (vram_address >= subsegment.vram_start && vram_address < subsegment.vram_end) {
        switch (subsegment.type) {
          case SUBSEGMENT_DATA:
          case SUBSEGMENT_RODATA:
          case SUBSEGMENT_BSS:
          case SUBSEGMENT_BIN:
            return SectionVramClassification::kData;
          case SUBSEGMENT_C:
          case SUBSEGMENT_ASM:
            return SectionVramClassification::kCode;
          default:
            break;
        }
      }
    }

    // Fall back to segment-level classification
    switch (segment.type) {
      case SEGMENT_CODE:
        return SectionVramClassification::kCode;
      case SEGMENT_DATA:
      case SEGMENT_RODATA:
      case SEGMENT_BSS:
        return SectionVramClassification::kData;
      default:
        break;
    }
  }

  return SectionVramClassification::kUnknown;
}

absl::StatusOr<TuningCache> ParseTuningCache(std::string_view textproto_content) {
  TuningCache cache;
  google::protobuf::TextFormat::Parser parser;
  if (!parser.ParseFromString(textproto_content, &cache)) {
    return absl::InvalidArgumentError("Failed to parse TuningCache textproto.");
  }
  return cache;
}

absl::StatusOr<TuningCache> LoadTuningCache(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return absl::NotFoundError(
        absl::StrFormat("Failed to open TuningCache file: %s", path.string()));
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return ParseTuningCache(buffer.str());
}

absl::Status SaveTuningCache(const TuningCache& cache, const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::string text;
  if (!google::protobuf::TextFormat::PrintToString(cache, &text)) {
    return absl::InternalError("Failed to serialize TuningCache to textproto.");
  }
  std::ofstream file(path);
  if (!file.is_open()) {
    return absl::InternalError(
        absl::StrFormat("Failed to open TuningCache file for writing: %s", path.string()));
  }
  file << text;
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
