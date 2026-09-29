#ifndef SPLITTER_SLICER_H_
#define SPLITTER_SLICER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/rom.h"
#include "splitter/config.pb.h"

namespace rom_nom_nom {

// Metadata for a carved binary slice.
struct CarvedSlice {
  std::string name;
  size_t rom_start = 0;
  size_t rom_end = 0;
  size_t Size() const { return rom_end - rom_start; }
  std::filesystem::path output_path;
};

// Summary report of a slicing run.
struct SlicingReport {
  size_t total_bytes_carved = 0;
  size_t total_files_written = 0;
  std::vector<CarvedSlice> slices;
};

// Slicing configuration options.
struct SlicerOptions {
  std::filesystem::path output_dir = "build";
  bool verify_sha1 = true;        // Asserts rom.Sha1() == config.sha1()
  bool write_incbin_asm = false;  // Emits .incbin .s files alongside binaries
};

class Slicer {
 public:
  explicit Slicer(SlicerOptions options = {}) : options_(std::move(options)) {}

  // Computes slice ranges for non-code segments, resolving implicit boundaries.
  // (If segment.rom_end is 0, it extends to the next segment's rom_start or ROM end).
  absl::StatusOr<std::vector<CarvedSlice>> PlanSlices(const Rom& rom,
                                                      const SplitConfig& config) const;

  // Extracts zero-copy ROM data for a specific slice range.
  absl::StatusOr<absl::Span<const uint8_t>> SliceData(const Rom& rom,
                                                      const CarvedSlice& slice) const;

  // Executes the complete slicing plan: verifies ROM checksum, computes slice bounds,
  // and writes all non-code binary assets (.bin, header, raw data) to output_dir.
  absl::StatusOr<SlicingReport> SliceAll(const Rom& rom, const SplitConfig& config) const;

  // Carves a single slice directly to disk.
  absl::Status SliceToDisk(const Rom& rom, const CarvedSlice& slice) const;

 private:
  SlicerOptions options_;
};

}  // namespace rom_nom_nom

#endif  // SPLITTER_SLICER_H_
