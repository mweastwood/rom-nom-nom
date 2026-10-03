#include "splitter/config.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "google/protobuf/text_format.h"
#include "splitter/config.pb.h"

namespace rom_nom_nom {

absl::Status ValidateSplitConfig(const SplitConfig& config) {
  if (config.game_name().empty()) {
    return absl::InvalidArgumentError("SplitConfig.game_name cannot be empty");
  }
  if (config.sha1().empty()) {
    return absl::InvalidArgumentError("SplitConfig.sha1 cannot be empty");
  }
  if (config.basename().empty()) {
    return absl::InvalidArgumentError("SplitConfig.basename cannot be empty");
  }
  if (config.segments().empty()) {
    return absl::InvalidArgumentError("SplitConfig must contain at least one segment");
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

  return absl::OkStatus();
}

absl::StatusOr<SplitConfig> ParseSplitConfig(std::string_view textproto_content) {
  SplitConfig config;
  google::protobuf::TextFormat::Parser parser;
  if (!parser.ParseFromString(std::string(textproto_content), &config)) {
    return absl::InvalidArgumentError(
        "Failed to parse SplitConfig textproto content: syntax error");
  }

  auto status = ValidateSplitConfig(config);
  if (!status.ok()) {
    return status;
  }
  return config;
}

absl::StatusOr<SplitConfig> LoadSplitConfig(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return absl::NotFoundError(absl::StrFormat("Failed to open config file: %s", path.string()));
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return ParseSplitConfig(buffer.str());
}

SectionVramClassification ClassifyVramAddress(const SplitConfig& config, uint32_t vram_address) {
  for (const auto& segment : config.segments()) {
    if (segment.vram() == 0) {
      continue;
    }

    uint32_t segment_vram_start = segment.vram();
    uint32_t segment_rom_size =
        (segment.rom_end() > segment.rom_start()) ? (segment.rom_end() - segment.rom_start()) : 0;
    uint32_t segment_vram_end = segment_vram_start + segment_rom_size + segment.bss_size();
    for (const auto& subsegment : segment.subsegments()) {
      if (subsegment.vram() != 0 && subsegment.bss_size() > 0) {
        segment_vram_end = std::max(segment_vram_end, subsegment.vram() + subsegment.bss_size());
      } else if (subsegment.bss_size() > 0) {
        segment_vram_end = std::max(segment_vram_end,
                                    segment_vram_start + segment_rom_size + subsegment.bss_size());
      }
    }

    if (vram_address < segment_vram_start || vram_address >= segment_vram_end) {
      continue;
    }

    // Check if the address falls into a specific subsegment
    if (segment.subsegments_size() > 0) {
      for (int i = 0; i < segment.subsegments_size(); ++i) {
        const auto& subsegment = segment.subsegments(i);
        uint32_t subsegment_vram_start =
            subsegment.vram() != 0
                ? subsegment.vram()
                : (segment_vram_start + (subsegment.rom_start() - segment.rom_start()));

        uint32_t subsegment_vram_end = segment_vram_end;
        if (subsegment.bss_size() > 0) {
          subsegment_vram_end = subsegment_vram_start + subsegment.bss_size();
        } else if (i + 1 < segment.subsegments_size()) {
          const auto& next_subsegment = segment.subsegments(i + 1);
          subsegment_vram_end =
              next_subsegment.vram() != 0
                  ? next_subsegment.vram()
                  : (segment_vram_start + (next_subsegment.rom_start() - segment.rom_start()));
        }

        if (vram_address >= subsegment_vram_start && vram_address < subsegment_vram_end) {
          switch (subsegment.type()) {
            case SUBSEGMENT_DATA:
            case SUBSEGMENT_RODATA:
            case SUBSEGMENT_BSS:
            case SUBSEGMENT_BIN:
              return SectionVramClassification::kData;
            case SUBSEGMENT_C:
            case SUBSEGMENT_ASM:
            case SUBSEGMENT_HASM:
              return SectionVramClassification::kCode;
            default:
              break;
          }
        }
      }
    }

    // Fall back to segment-level classification
    switch (segment.type()) {
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

}  // namespace rom_nom_nom
