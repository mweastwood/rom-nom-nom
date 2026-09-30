#include "splitter/auto_symbols.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "splitter/relocations.h"
#include "splitter/symbol_registry.h"
#include "splitter/symbol_registry.pb.h"

namespace rom_nom_nom {

AutoSymbolFinder::AutoSymbolFinder(const SymbolIndex* known_symbols, AutoSymbolOptions options)
    : known_symbols_(known_symbols), options_(options) {}

bool AutoSymbolFinder::IsAllowedAddress(uint32_t address) const {
  // Exclude N64 MMIO hardware registers (0x04000000 - 0x04FFFFFF)
  if (options_.filter_hardware_regs && (address >= 0x04000000 && address <= 0x04FFFFFF)) {
    return false;
  }

  // KSEG0: 0x80000000 - 0x807FFFFF (N64 RDRAM cached)
  if (options_.include_kseg0 && (address >= 0x80000000 && address <= 0x807FFFFF)) {
    return true;
  }

  // KSEG1: 0xA0000000 - 0xA07FFFFF (N64 RDRAM uncached)
  if (options_.include_kseg1 && (address >= 0xA0000000 && address <= 0xA07FFFFF)) {
    return true;
  }

  // Cartridge domain: 0xB0000000 - 0xBFFFFFFF (Direct ROM space)
  if (options_.include_cartridge && (address >= 0xB0000000 && address <= 0xBFFFFFFF)) {
    return true;
  }

  return false;
}

bool AutoSymbolFinder::IsDefinedAddress(uint32_t address) const {
  if (defined_addresses_.contains(address)) {
    return true;
  }

  if (known_symbols_ != nullptr && known_symbols_->HasAddress(address)) {
    return true;
  }

  for (const auto& [start, end] : defined_ranges_) {
    if (address >= start && address < end) {
      return true;
    }
  }

  return false;
}

void AutoSymbolFinder::RegisterDefinedSymbol(std::string_view name, uint32_t address) {
  defined_addresses_.insert(address);
  defined_names_.insert(std::string(name));
}

void AutoSymbolFinder::RegisterDefinedAddressRange(uint32_t start_vram, uint32_t end_vram) {
  defined_ranges_.emplace_back(start_vram, end_vram);
}

absl::Status AutoSymbolFinder::ScanInstructions(absl::Span<const Instruction> instructions) {
  // 1. Scan for function calls (jal / j to external targets)
  for (const auto& inst : instructions) {
    if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJ) {
      uint32_t target = inst.JumpTarget();
      if (!IsDefinedAddress(target) && IsAllowedAddress(target)) {
        auto& entry = discovered_[target];
        entry.address = target;
        entry.name = absl::StrFormat("func_%08X", target);
        entry.type = SYMBOL_FUNC;
        entry.reference_count++;
      }
    }
  }

  // 2. Scan for paired data relocations (%hi / %lo)
  RelocationTracker tracker(known_symbols_);
  auto status = tracker.Analyze(instructions);
  if (!status.ok()) {
    return status;
  }

  for (const auto& reloc : tracker.Relocations()) {
    uint32_t target = reloc.target_vram;
    if (!IsDefinedAddress(target) && IsAllowedAddress(target)) {
      auto& entry = discovered_[target];
      entry.address = target;
      entry.name = absl::StrFormat("D_%08X", target);
      entry.type = SYMBOL_DATA;
      entry.reference_count++;
    }
  }

  return absl::OkStatus();
}

absl::Status AutoSymbolFinder::ScanCode(absl::Span<const uint8_t> code, uint32_t vram_start) {
  if (code.size() % 4 != 0) {
    return absl::InvalidArgumentError("Code buffer size must be a multiple of 4 bytes");
  }

  std::vector<Instruction> instructions;
  instructions.reserve(code.size() / 4);

  for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
    uint32_t pc = vram_start + static_cast<uint32_t>(offset);
    auto inst_or = DecodeInstruction(code.subspan(offset, 4), pc);
    if (!inst_or.ok()) {
      continue;
    }
    instructions.push_back(*inst_or);
  }

  return ScanInstructions(instructions);
}

std::vector<DiscoveredSymbol> AutoSymbolFinder::DiscoveredDataSymbols() const {
  std::vector<DiscoveredSymbol> data_syms;
  for (const auto& [_, sym] : discovered_) {
    if (sym.type == SYMBOL_DATA) {
      data_syms.push_back(sym);
    }
  }
  std::sort(
      data_syms.begin(), data_syms.end(),
      [](const DiscoveredSymbol& a, const DiscoveredSymbol& b) { return a.address < b.address; });
  return data_syms;
}

std::vector<DiscoveredSymbol> AutoSymbolFinder::DiscoveredFuncSymbols() const {
  std::vector<DiscoveredSymbol> func_syms;
  for (const auto& [_, sym] : discovered_) {
    if (sym.type == SYMBOL_FUNC) {
      func_syms.push_back(sym);
    }
  }
  std::sort(
      func_syms.begin(), func_syms.end(),
      [](const DiscoveredSymbol& a, const DiscoveredSymbol& b) { return a.address < b.address; });
  return func_syms;
}

std::string AutoSymbolFinder::GenerateUndefinedSymsScript() const {
  std::string output = "/* Auto-generated undefined data symbols */\n";
  for (const auto& sym : DiscoveredDataSymbols()) {
    absl::StrAppend(&output, absl::StrFormat("%s = 0x%08X;\n", sym.name, sym.address));
  }
  return output;
}

std::string AutoSymbolFinder::GenerateUndefinedFuncsScript() const {
  std::string output = "/* Auto-generated undefined function symbols */\n";
  for (const auto& sym : DiscoveredFuncSymbols()) {
    absl::StrAppend(&output, absl::StrFormat("%s = 0x%08X;\n", sym.name, sym.address));
  }
  return output;
}

SymbolRegistry AutoSymbolFinder::ExportToRegistry() const {
  SymbolRegistry registry;
  for (const auto& sym : DiscoveredFuncSymbols()) {
    auto* entry = registry.add_entries();
    entry->set_name(sym.name);
    entry->set_address(sym.address);
    entry->set_type(SYMBOL_FUNC);
  }
  for (const auto& sym : DiscoveredDataSymbols()) {
    auto* entry = registry.add_entries();
    entry->set_name(sym.name);
    entry->set_address(sym.address);
    entry->set_type(SYMBOL_DATA);
  }
  return registry;
}

}  // namespace rom_nom_nom
