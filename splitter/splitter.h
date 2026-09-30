#ifndef SPLITTER_SPLITTER_H_
#define SPLITTER_SPLITTER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace rom_nom_nom {

// Configuration options controlling a ROM splitting run.
struct SplitterOptions {
  std::filesystem::path config_path;
  std::filesystem::path rom_path;
  std::filesystem::path symbols_path;
  std::filesystem::path out_dir = "build";
  std::filesystem::path asm_out_dir;
  std::filesystem::path ld_script_path;
  std::filesystem::path symbols_ld_path;
  bool write_ld_script = true;
  bool slice_data = true;
  bool disassemble_code = true;
  bool verify_sha1 = true;
  bool emit_line_comments = true;
  bool verbose = false;

  // In-memory overrides for self-contained unit testing
  std::string config_text_override;
  std::string symbols_text_override;
  std::vector<uint8_t> rom_buffer_override;
};

// Summary metrics from a completed ROM splitting execution.
struct SplitterResult {
  size_t bytes_carved = 0;
  size_t files_written = 0;
  size_t functions_disassembled = 0;
  size_t asm_files_written = 0;
  bool ld_script_written = false;
};

// Executes the complete ROM splitting pipeline:
// 1. Loads and validates the split configuration (.textproto).
// 2. Opens and verifies the input ROM binary (.z64).
// 3. Loads symbol registries (if available).
// 4. Carves non-code binary assets (.bin, header, raw data).
// 5. Generates GNU ld linker scripts (.ld, symbols.ld, hardware_regs.ld).
// 6. Disassembles code subsegments into .s assembly files with symbol resolution.
absl::StatusOr<SplitterResult> RunSplitter(const SplitterOptions& options);

}  // namespace rom_nom_nom

#endif  // SPLITTER_SPLITTER_H_
