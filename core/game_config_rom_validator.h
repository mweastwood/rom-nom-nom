#ifndef CORE_GAME_CONFIG_ROM_VALIDATOR_H_
#define CORE_GAME_CONFIG_ROM_VALIDATOR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "core/game_config.pb.h"
#include "core/rom.h"

namespace rom_nom_nom {

struct ConfigValidationViolation {
  std::string segment_name;
  std::string subsegment_name;
  uint32_t rom_offset = 0;
  uint32_t vram = 0;
  std::string message;
};

struct ConfigValidationReport {
  size_t code_segments_checked = 0;
  size_t code_subsegments_checked = 0;
  size_t functions_checked = 0;
  std::vector<ConfigValidationViolation> violations;

  bool Ok() const { return violations.empty(); }
};

// Inspects `config` together with `rom` and returns a detailed validation report verifying:
// 1. Structural config invariants (`ValidateGameConfig`) and ROM SHA-1 / segment bounds.
// 2. 16-byte (`0x10`) section alignment of code/data/rodata subsegments in `SEGMENT_CODE`.
// 3. Absence of hand-written assembly (`SUBSEGMENT_ASM`) instructions/idioms inside C-lifted
//    subsegments (`SUBSEGMENT_C`), and presence of hand-written assembly signatures in
//    `SUBSEGMENT_ASM` subsegments.
// 4. Consistency between resolved compiler optimization flags (`-O0` vs `-O1`/`-O2`/`-O3`) and
//    binary frame-pointer prologues (`sw $fp, N($sp)` + `move $fp, $sp`).
ConfigValidationReport AnalyzeGameConfigAgainstRom(const GameConfig& config, const Rom& rom);

// Returns `absl::OkStatus()` if `AnalyzeGameConfigAgainstRom(config, rom)` finds no violations,
// or an `absl::InvalidArgumentError` listing the violations.
absl::Status ValidateGameConfigAgainstRom(const GameConfig& config, const Rom& rom);

}  // namespace rom_nom_nom

#endif  // CORE_GAME_CONFIG_ROM_VALIDATOR_H_
