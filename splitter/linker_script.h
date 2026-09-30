#ifndef SPLITTER_LINKER_SCRIPT_H_
#define SPLITTER_LINKER_SCRIPT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "splitter/config.pb.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

// Options controlling GNU linker script formatting and object path resolution.
struct LinkerScriptOptions {
  std::string base_build_dir = "build";  // e.g. "build" -> "build/<game_name>"
  size_t subalign = 16;                  // Section SUBALIGN (16 bytes for N64 DMA)
  uint32_t fill_value = 0x00000000;      // Word fill value
  bool emit_discard = true;              // Emit /DISCARD/ section
};

// Generates GNU ld scripts (.ld) and symbol definition scripts (symbols.ld).
class LinkerScriptGenerator {
 public:
  explicit LinkerScriptGenerator(LinkerScriptOptions options = {});

  // Generates the primary GNU ld script (SECTIONS, memory tracking, .text/.data/.rodata/.bss).
  absl::StatusOr<std::string> GenerateMainScript(const SplitConfig& config) const;

  // Generates a symbols.ld script from a SymbolIndex (symbol = 0xXXXXXXXX;).
  std::string GenerateSymbolsScript(const SymbolIndex& symbols) const;

  // Generates standard N64 hardware and OS memory-mapped register definitions.
  std::string GenerateHardwareRegsScript() const;

  // Resolves the destination object file path (.o) for a given segment and optional subsegment.
  std::string ResolveObjectPath(std::string_view game_name, const Segment& segment,
                                const Subsegment* subsegment = nullptr) const;

 private:
  LinkerScriptOptions options_;
};

}  // namespace rom_nom_nom

#endif  // SPLITTER_LINKER_SCRIPT_H_
