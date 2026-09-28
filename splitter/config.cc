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
    if (i > 0 && seg.rom_start() <= prev_rom_start) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Segment '%s' rom_start (0x%X) must be strictly greater than "
                          "previous segment start (0x%X)",
                          seg.name(), seg.rom_start(), prev_rom_start));
    }
    prev_rom_start = seg.rom_start();

    uint32_t prev_sub_start = 0;
    for (int j = 0; j < seg.subsegments_size(); ++j) {
      const Subsegment& sub = seg.subsegments(j);
      if (sub.type() == SUBSEGMENT_TYPE_UNSPECIFIED) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Subsegment '%s' in segment '%s' has unspecified type", sub.name(), seg.name()));
      }
      if (j > 0 && sub.rom_start() <= prev_sub_start) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Subsegment '%s' rom_start (0x%X) in segment '%s' must be strictly "
                            "greater than previous subsegment start (0x%X)",
                            sub.name(), sub.rom_start(), seg.name(), prev_sub_start));
      }
      prev_sub_start = sub.rom_start();
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

}  // namespace rom_nom_nom
