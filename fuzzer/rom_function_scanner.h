#ifndef FUZZER_ROM_FUNCTION_SCANNER_H_
#define FUZZER_ROM_FUNCTION_SCANNER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom::fuzzer {

// Metadata and machine code for a function scanned directly from ROM bytes.
struct ScannedFunction {
  std::string name;
  uint32_t vram = 0;
  uint32_t rom_offset = 0;
  uint32_t size = 0;
  std::vector<uint32_t> raw_words;
};

// Discovers and extracts all executable functions directly from raw ROM bytes
// using MIPS control-flow analysis and entrypoint detection.
class RomFunctionScanner {
 public:
  // Scans raw ROM bytes and extracts all executable functions.
  // If config is provided, uses SEGMENT_CODE definitions for ROM ranges and VRAM bases.
  // If config is null, uses N64 header at 0x08 for entrypoint and starts code at 0x1000/0x1050.
  // If symbols is provided, uses semantic names; otherwise synthesizes func_%08X.
  static absl::StatusOr<std::vector<ScannedFunction>> Scan(absl::Span<const uint8_t> rom_bytes,
                                                           const SplitConfig* config = nullptr,
                                                           const SymbolIndex* symbols = nullptr);

  // Scans an executable candidate ELF and extracts all functions directly from
  // its symbol table (.symtab) and maps their VRAM addresses to ROM offsets
  // via ELF program headers (PT_LOAD segments).
  // If config is provided and program headers do not resolve an address,
  // falls back to SEGMENT_CODE definitions in config.
  static absl::StatusOr<std::vector<ScannedFunction>> ScanFromElfAndRom(
      absl::Span<const uint8_t> rom_bytes, absl::Span<const uint8_t> elf_bytes,
      const SplitConfig* config = nullptr);
};

}  // namespace rom_nom_nom::fuzzer

#endif  // FUZZER_ROM_FUNCTION_SCANNER_H_
