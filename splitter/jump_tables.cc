#include "splitter/jump_tables.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

JumpTableResolver::JumpTableResolver(const SymbolIndex* symbols, JumpTableOptions options)
    : symbols_(symbols), options_(options) {}

std::optional<JumpTable> JumpTableResolver::DetectTableAt(absl::Span<const uint8_t> rodata,
                                                          size_t offset_in_rodata,
                                                          uint32_t rodata_vram_start,
                                                          const FunctionRange& func) const {
  if (offset_in_rodata % 4 != 0 || offset_in_rodata + 4 > rodata.size()) {
    return std::nullopt;
  }

  std::vector<uint32_t> targets;
  for (size_t cur = offset_in_rodata; cur + 4 <= rodata.size(); cur += 4) {
    uint32_t target = ReadBe32(rodata.data() + cur);
    if (target >= func.vram_start && target < func.vram_end && (target % 4 == 0)) {
      targets.push_back(target);
    } else {
      break;
    }
  }

  if (targets.size() < options_.min_entries) {
    return std::nullopt;
  }

  JumpTable table;
  table.vram_start = rodata_vram_start + static_cast<uint32_t>(offset_in_rodata);
  if (symbols_ != nullptr) {
    const auto* entry = symbols_->FindByAddress(table.vram_start);
    if (entry != nullptr) {
      table.name = entry->name();
    } else {
      table.name = absl::StrFormat("jtbl_%08X", table.vram_start);
    }
  } else {
    table.name = absl::StrFormat("jtbl_%08X", table.vram_start);
  }
  table.entry_targets = std::move(targets);
  return table;
}

std::vector<JumpTable> JumpTableResolver::ScanRodata(
    absl::Span<const uint8_t> rodata, uint32_t rodata_vram_start,
    absl::Span<const FunctionRange> functions) const {
  std::vector<JumpTable> tables;
  size_t offset = 0;
  size_t min_bytes = options_.min_entries * 4;

  while (offset + min_bytes <= rodata.size()) {
    std::optional<JumpTable> found_table;
    for (const auto& func : functions) {
      auto table = DetectTableAt(rodata, offset, rodata_vram_start, func);
      if (table.has_value()) {
        found_table = std::move(table);
        break;
      }
    }

    if (found_table.has_value()) {
      offset += found_table->SizeInBytes();
      tables.push_back(std::move(*found_table));
    } else {
      offset += 4;
    }
  }

  return tables;
}

std::string JumpTableResolver::FormatJumpTableAsm(const JumpTable& jtbl) const {
  std::string asm_text;
  if (options_.emit_section) {
    absl::StrAppend(&asm_text, ".section .rodata\n\n");
  }
  if (options_.emit_globl) {
    absl::StrAppend(&asm_text, absl::StrFormat(".globl %s\n", jtbl.name));
  }
  absl::StrAppend(&asm_text, absl::StrFormat("%s:\n", jtbl.name));

  for (uint32_t target : jtbl.entry_targets) {
    std::string label = symbols_ ? symbols_->LookupOrSynthesizeName(target, SYMBOL_LABEL)
                                 : absl::StrFormat(".L%08X", target);
    absl::StrAppend(&asm_text, absl::StrFormat("    .word %s\n", label));
  }

  return asm_text;
}

std::vector<uint32_t> JumpTableResolver::ExtractLocalLabels(absl::Span<const JumpTable> tables,
                                                            const FunctionRange& func) const {
  std::vector<uint32_t> labels;
  absl::flat_hash_set<uint32_t> seen;

  for (const auto& table : tables) {
    for (uint32_t target : table.entry_targets) {
      if (target >= func.vram_start && target < func.vram_end) {
        if (seen.insert(target).second) {
          labels.push_back(target);
        }
      }
    }
  }

  std::sort(labels.begin(), labels.end());
  return labels;
}

}  // namespace rom_nom_nom
