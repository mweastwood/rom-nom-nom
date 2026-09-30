#include "splitter/slicer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/rom.h"
#include "splitter/config.pb.h"

namespace rom_nom_nom {

absl::StatusOr<std::vector<CarvedSlice>> Slicer::PlanSlices(const Rom& rom,
                                                            const SplitConfig& config) const {
  if (options_.verify_sha1 && !config.sha1().empty()) {
    std::string actual_sha1 = rom.Sha1();
    if (actual_sha1 != config.sha1()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("ROM SHA-1 mismatch: expected %s, got %s", config.sha1(), actual_sha1));
    }
  }

  std::vector<CarvedSlice> slices;
  const std::filesystem::path game_out_dir =
      options_.output_dir / (config.basename().empty() ? "game" : config.basename());

  for (int i = 0; i < config.segments_size(); ++i) {
    const Segment& seg = config.segments(i);

    // BSS occupies 0 bytes in ROM
    if (seg.type() == SEGMENT_BSS) {
      continue;
    }

    // Compute segment ROM end boundary (resolving implicit ends)
    size_t seg_start = seg.rom_start();
    size_t seg_end = seg.rom_end();
    if (seg_end == 0) {
      seg_end = rom.Size();
      for (int k = i + 1; k < config.segments_size(); ++k) {
        if (config.segments(k).type() != SEGMENT_BSS &&
            config.segments(k).rom_start() > seg_start) {
          seg_end = std::min<size_t>(config.segments(k).rom_start(), rom.Size());
          break;
        }
      }
    }

    if (seg_start > seg_end || seg_end > rom.Size()) {
      return absl::OutOfRangeError(
          absl::StrFormat("Segment '%s' range [0x%X, 0x%X) exceeds ROM bounds (size: 0x%X)",
                          seg.name(), seg_start, seg_end, rom.Size()));
    }

    if (seg.type() == SEGMENT_CODE) {
      // Code segments may contain embedded data/rodata/bin subsegments
      for (int j = 0; j < seg.subsegments_size(); ++j) {
        const Subsegment& sub = seg.subsegments(j);
        if (sub.type() == SUBSEGMENT_DATA || sub.type() == SUBSEGMENT_RODATA ||
            sub.type() == SUBSEGMENT_BIN) {
          size_t sub_start = sub.rom_start();
          size_t sub_end = seg_end;
          for (int k = j + 1; k < seg.subsegments_size(); ++k) {
            if (seg.subsegments(k).type() != SUBSEGMENT_BSS && seg.subsegments(k).rom_start() > 0) {
              sub_end = seg.subsegments(k).rom_start();
              break;
            }
          }

          if (sub_start > sub_end || sub_end > seg_end) {
            return absl::OutOfRangeError(absl::StrFormat(
                "Subsegment '%s' range [0x%X, 0x%X) exceeds parent segment bounds [0x%X, 0x%X)",
                sub.name(), sub_start, sub_end, seg_start, seg_end));
          }

          if (sub_start < sub_end) {
            std::string sub_name = sub.name().empty()
                                       ? absl::StrFormat("%s_0x%X", seg.name(), sub_start)
                                       : std::string(sub.name());
            CarvedSlice slice;
            slice.name = sub_name;
            slice.rom_start = sub_start;
            slice.rom_end = sub_end;
            slice.output_path = game_out_dir / absl::StrCat(sub_name, ".bin");
            slices.push_back(std::move(slice));
          }
        }
      }
      continue;
    }

    // Non-code segment (HEADER, BIN, DATA, RODATA)
    if (seg_start < seg_end) {
      CarvedSlice slice;
      slice.name = std::string(seg.name());
      slice.rom_start = seg_start;
      slice.rom_end = seg_end;
      slice.output_path = game_out_dir / absl::StrCat(seg.name(), ".bin");
      slices.push_back(std::move(slice));
    }
  }

  return slices;
}

absl::StatusOr<absl::Span<const uint8_t>> Slicer::SliceData(const Rom& rom,
                                                            const CarvedSlice& slice) const {
  return rom.SliceRange(slice.rom_start, slice.rom_end);
}

absl::Status Slicer::SliceToDisk(const Rom& rom, const CarvedSlice& slice) const {
  auto data_or = SliceData(rom, slice);
  if (!data_or.ok()) {
    return data_or.status();
  }

  std::filesystem::create_directories(slice.output_path.parent_path());

  std::ofstream out(slice.output_path, std::ios::binary);
  if (!out.is_open()) {
    return absl::InternalError(
        absl::StrFormat("Failed to open output file: %s", slice.output_path.string()));
  }

  out.write(reinterpret_cast<const char*>(data_or->data()), data_or->size());
  if (!out.good()) {
    return absl::InternalError(
        absl::StrFormat("Failed to write slice data to: %s", slice.output_path.string()));
  }

  if (options_.write_incbin_asm) {
    std::filesystem::path asm_path = slice.output_path;
    asm_path.replace_extension(".s");
    std::ofstream asm_out(asm_path);
    if (asm_out.is_open()) {
      asm_out << ".section .data\n";
      asm_out << ".incbin \"" << slice.output_path.filename().string() << "\"\n";
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<SlicingReport> Slicer::SliceAll(const Rom& rom, const SplitConfig& config) const {
  auto slices_or = PlanSlices(rom, config);
  if (!slices_or.ok()) {
    return slices_or.status();
  }

  SlicingReport report;
  report.slices = std::move(*slices_or);

  for (const auto& slice : report.slices) {
    auto status = SliceToDisk(rom, slice);
    if (!status.ok()) {
      return status;
    }
    report.total_bytes_carved += slice.Size();
    report.total_files_written++;
  }

  return report;
}

}  // namespace rom_nom_nom
