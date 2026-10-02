#include "differ/relocation_applier.h"

#include <elf.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/numbers.h"
#include "absl/types/span.h"
#include "differ/elf_reader.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

RelocationApplyResult RelocationApplier::Apply(const ElfFunction& function,
                                               const SymbolLookupFn& symbol_lookup) {
  return ApplyRelocations(function.raw_words, function.relocations, symbol_lookup);
}

RelocationApplyResult RelocationApplier::Apply(
    const ElfFunction& function, const absl::flat_hash_map<std::string, uint32_t>& symbol_map) {
  auto lookup = [&](std::string_view name) -> std::optional<uint32_t> {
    auto it = symbol_map.find(name);
    if (it != symbol_map.end()) {
      return it->second;
    }
    // Fallback: If symbol name ends with 8-character hex address (e.g. func_XXXXXXXX, D_XXXXXXXX)
    size_t last_underscore = name.rfind('_');
    if (last_underscore != std::string_view::npos && last_underscore + 1 < name.size() &&
        name.size() - (last_underscore + 1) == 8) {
      uint32_t addr = 0;
      if (absl::SimpleHexAtoi(name.substr(last_underscore + 1), &addr)) {
        return addr;
      }
    }
    return std::nullopt;
  };
  return ApplyRelocations(function.raw_words, function.relocations, lookup);
}

RelocationApplyResult RelocationApplier::Apply(const ElfFunction& function,
                                               const SymbolIndex& symbol_index) {
  auto lookup = [&](std::string_view name) -> std::optional<uint32_t> {
    const auto* entry = symbol_index.FindByName(name);
    if (entry != nullptr) {
      return entry->address();
    }
    // Fallback: If symbol name ends with 8-character hex address (e.g. func_XXXXXXXX, D_XXXXXXXX)
    size_t last_underscore = name.rfind('_');
    if (last_underscore != std::string_view::npos && last_underscore + 1 < name.size() &&
        name.size() - (last_underscore + 1) == 8) {
      uint32_t addr = 0;
      if (absl::SimpleHexAtoi(name.substr(last_underscore + 1), &addr)) {
        return addr;
      }
    }
    return std::nullopt;
  };
  return ApplyRelocations(function.raw_words, function.relocations, lookup);
}

RelocationApplyResult RelocationApplier::ApplyRelocations(
    absl::Span<const uint32_t> words, absl::Span<const ElfRelocation> relocations,
    const SymbolLookupFn& symbol_lookup) {
  RelocationApplyResult result;
  result.patched_words.assign(words.begin(), words.end());
  absl::flat_hash_set<std::string> missing_symbols;

  for (size_t i = 0; i < relocations.size(); ++i) {
    const auto& rel = relocations[i];
    uint32_t word_idx = rel.function_offset / 4;
    if (word_idx >= result.patched_words.size()) {
      continue;
    }

    if (rel.symbol_name.empty()) {
      continue;
    }

    auto sym_addr_opt = symbol_lookup(rel.symbol_name);
    if (!sym_addr_opt.has_value()) {
      missing_symbols.insert(rel.symbol_name);
      continue;
    }
    uint32_t sym_addr = *sym_addr_opt;
    uint32_t cur_word = result.patched_words[word_idx];

    switch (rel.type) {
      case R_MIPS_26: {
        // Jump / Call target (26-bit field, target shifted right by 2)
        uint32_t orig_target = cur_word & 0x03FFFFFF;
        uint32_t new_target = ((orig_target << 2) + sym_addr) >> 2;
        result.patched_words[word_idx] = (cur_word & 0xFC000000) | (new_target & 0x03FFFFFF);
        break;
      }

      case R_MIPS_HI16: {
        // Find matching LO16 relocation for the same symbol to extract the low addend
        int16_t lo_addend = 0;
        for (size_t j = i + 1; j < relocations.size(); ++j) {
          if (relocations[j].type == R_MIPS_LO16 && relocations[j].symbol_name == rel.symbol_name) {
            uint32_t lo_word_idx = relocations[j].function_offset / 4;
            if (lo_word_idx < words.size()) {
              lo_addend = static_cast<int16_t>(words[lo_word_idx] & 0xFFFF);
            }
            break;
          }
        }

        uint32_t full_addr = sym_addr + lo_addend;
        uint32_t hi = (full_addr + 0x8000) >> 16;
        result.patched_words[word_idx] = (cur_word & 0xFFFF0000) | (hi & 0xFFFF);
        break;
      }

      case R_MIPS_LO16: {
        int16_t orig_addend = static_cast<int16_t>(cur_word & 0xFFFF);
        uint32_t full_addr = sym_addr + orig_addend;
        uint16_t lo = full_addr & 0xFFFF;
        result.patched_words[word_idx] = (cur_word & 0xFFFF0000) | lo;
        break;
      }

      case R_MIPS_32: {
        result.patched_words[word_idx] = cur_word + sym_addr;
        break;
      }

      default:
        break;
    }
  }

  result.unresolved_symbols.assign(missing_symbols.begin(), missing_symbols.end());
  return result;
}

}  // namespace rom_nom_nom
