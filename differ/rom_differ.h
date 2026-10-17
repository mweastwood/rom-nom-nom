#ifndef DIFFER_ROM_DIFFER_H_
#define DIFFER_ROM_DIFFER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "differ/diff_formatter.h"

namespace rom_nom_nom {

// Options configuring the ROM difference analysis.
struct RomDiffOptions {
  std::filesystem::path base_rom_path;
  std::filesystem::path candidate_rom_path;
  std::filesystem::path config_path;
  bool use_color = true;
  bool allow_code_shift = true;
  bool require_bit_exact = false;
};

// RomDiffer coordinates structural verification, segment-aware partitioning,
// 32-bit word LCS sequence alignment on code segments, and shift-aligned
// block verification on data/asset segments.
class RomDiffer {
 public:
  explicit RomDiffer(RomDiffOptions options);

  // Executes difference analysis comparing the target ROM and built ROM.
  absl::StatusOr<RomDiffResult> Diff() const;

  // Computes the size of the Longest Common Subsequence (LCS) between two
  // sequences of 32-bit words using prefix/suffix trimming and Hunt-Szymanski.
  static size_t ComputeWordLcs(absl::Span<const uint32_t> target_words,
                               absl::Span<const uint32_t> built_words);

 private:
  RomDiffOptions options_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_ROM_DIFFER_H_
