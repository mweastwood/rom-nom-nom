#ifndef SPLITTER_AUTO_SYMBOLS_H_
#define SPLITTER_AUTO_SYMBOLS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "splitter/relocations.h"
#include "splitter/symbol_registry.h"
#include "splitter/symbol_registry.pb.h"

namespace rom_nom_nom {

// A memory symbol discovered from disassembly analysis.
struct DiscoveredSymbol {
  uint32_t address = 0;
  std::string name;
  SymbolType type = SYMBOL_DATA;
  size_t reference_count = 0;
};

// Configuration options for auto-symbol discovery filtering.
struct AutoSymbolOptions {
  bool include_kseg0 = true;         // 0x80000000 - 0x807FFFFF (N64 RDRAM cached)
  bool include_kseg1 = true;         // 0xA0000000 - 0xA07FFFFF (N64 RDRAM uncached)
  bool include_cartridge = false;    // 0xB0000000 - 0xBFFFFFFF (Direct ROM space)
  bool filter_hardware_regs = true;  // Exclude N64 hardware MMIO (0x04000000 - 0x04FFFFFF)
};

// Scans MIPS instructions and discovers unresolved external symbols
// (calls and data relocations) that need linker definitions.
class AutoSymbolFinder {
 public:
  explicit AutoSymbolFinder(const SymbolIndex* known_symbols = nullptr,
                            AutoSymbolOptions options = {});

  // Scans raw MIPS code bytes starting at vram_start, discovering unresolved symbols.
  absl::Status ScanCode(absl::Span<const uint8_t> code, uint32_t vram_start);

  // Scans pre-decoded instructions and discovers unresolved symbols.
  absl::Status ScanInstructions(absl::Span<const Instruction> instructions);

  // Registers a known internal or module-defined symbol so it is not emitted as undefined.
  void RegisterDefinedSymbol(std::string_view name, uint32_t address);

  // Registers a known code address range (e.g. current segment) so internal branches
  // are not mistakenly classified as undefined external functions.
  void RegisterDefinedAddressRange(uint32_t start_vram, uint32_t end_vram);

  // Returns all discovered undefined data symbols sorted ascending by address.
  std::vector<DiscoveredSymbol> DiscoveredDataSymbols() const;

  // Returns all discovered undefined function symbols sorted ascending by address.
  std::vector<DiscoveredSymbol> DiscoveredFuncSymbols() const;

  // Formats GNU ld script for undefined data symbols (undefined_syms_auto.txt).
  std::string GenerateUndefinedSymsScript() const;

  // Formats GNU ld script for undefined function symbols (undefined_funcs_auto.txt).
  std::string GenerateUndefinedFuncsScript() const;

  // Exports all discovered symbols into a SymbolRegistry protobuf message.
  SymbolRegistry ExportToRegistry() const;

  // Total count of distinct discovered undefined symbols.
  size_t TotalDiscovered() const { return discovered_.size(); }

 private:
  bool IsAllowedAddress(uint32_t address) const;
  bool IsDefinedAddress(uint32_t address) const;

  const SymbolIndex* known_symbols_ = nullptr;
  AutoSymbolOptions options_;
  absl::flat_hash_set<uint32_t> defined_addresses_;
  absl::flat_hash_set<std::string> defined_names_;
  std::vector<std::pair<uint32_t, uint32_t>> defined_ranges_;
  absl::flat_hash_map<uint32_t, DiscoveredSymbol> discovered_;
};

}  // namespace rom_nom_nom

#endif  // SPLITTER_AUTO_SYMBOLS_H_
