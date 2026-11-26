#include "lifter/stage0_test_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

Stage0FunctionInput BakeLinkedRomStage0Input(const Stage0FunctionInput& reloc_input,
                                             const LinkedRomBakeOptions& options) {
  Stage0FunctionInput linked;
  linked.machine_words = reloc_input.machine_words;
  linked.base_vram = 0x80020000u;
  linked.gp_vram = 0x80060000u;
  linked.profile = reloc_input.profile;
  linked.profile.gp_symbol_sizes.clear();
  linked.jump_tables = reloc_input.jump_tables;
  linked.constant_pool = reloc_input.constant_pool;

  std::vector<std::pair<std::string, uint32_t>> sym_vrams;
  auto find_sym = [&](const std::string& s, uint32_t* out) -> bool {
    for (const auto& [k, val] : sym_vrams) {
      if (k == s) {
        *out = val;
        return true;
      }
    }
    return false;
  };

  uint32_t next_rodata = 0x80030000u;
  uint32_t next_func = 0x80040000u;
  uint32_t next_gp_data = 0x80059000u;
  uint32_t next_data = options.same_hi_page_for_globals ? 0x80031000u : 0x80070000u;

  for (Stage0JumpTable& jt : linked.jump_tables) {
    jt.table_vram = next_rodata;
    sym_vrams.emplace_back(jt.label_name, jt.table_vram);
    next_rodata += static_cast<uint32_t>(jt.target_word_indices.size() * 4u + 16u);
  }
  for (Stage0ConstantPoolEntry& cp : linked.constant_pool) {
    if (cp.section_name == ".lit4" || cp.section_name == ".lit8" || cp.section_name == ".sdata") {
      cp.entry_vram = next_gp_data;
      next_gp_data += 16u;
    } else {
      cp.entry_vram = next_rodata;
      next_rodata += 16u;
    }
    sym_vrams.emplace_back(cp.label_name, cp.entry_vram);
  }

  for (const Stage0Relocation& rel : reloc_input.relocations) {
    uint32_t existing = 0;
    if (find_sym(rel.symbol_name, &existing)) {
      continue;
    }
    int64_t max_add = 0;
    bool any_gp = false;
    bool any_call = false;
    for (const Stage0Relocation& r2 : reloc_input.relocations) {
      if (r2.symbol_name == rel.symbol_name) {
        max_add = std::max(max_add, r2.addend);
        if (r2.kind == Stage0RelocKind::kMipsGpRel16 || r2.kind == Stage0RelocKind::kMipsLiteral) {
          any_gp = true;
        }
        if (r2.kind == Stage0RelocKind::kMips26) {
          any_call = true;
        }
      }
    }
    const uint32_t span_bytes = static_cast<uint32_t>((max_add > 0 ? max_add : 0) + 16);
    uint32_t assigned = 0;
    if (any_call) {
      assigned = next_func;
      next_func += 64u;
    } else if (any_gp) {
      assigned = next_gp_data;
      next_gp_data += (span_bytes + 15u) & ~15u;
    } else {
      assigned = next_data;
      next_data += (span_bytes + 63u) & ~63u;
    }
    sym_vrams.emplace_back(rel.symbol_name, assigned);
    linked.vram_to_symbol[assigned] = rel.symbol_name;
    const auto gp_sz_it = reloc_input.profile.gp_symbol_sizes.find(rel.symbol_name);
    if (gp_sz_it != reloc_input.profile.gp_symbol_sizes.end() && gp_sz_it->second > 0) {
      linked.vram_to_symbol_size[assigned] = gp_sz_it->second;
    } else if (max_add > 0) {
      linked.vram_to_symbol_size[assigned] = span_bytes;
    }
  }

  std::vector<bool> relocated_word(linked.machine_words.size(), false);
  for (const Stage0Relocation& rel : reloc_input.relocations) {
    uint32_t sym_vram = 0;
    if (rel.word_index >= linked.machine_words.size() || !find_sym(rel.symbol_name, &sym_vram)) {
      continue;
    }
    relocated_word[rel.word_index] = true;
    const uint32_t full_addr = static_cast<uint32_t>(static_cast<int64_t>(sym_vram) + rel.addend);
    if (rel.kind == Stage0RelocKind::kMips26) {
      linked.machine_words[rel.word_index] =
          (linked.machine_words[rel.word_index] & 0xfc000000u) | ((full_addr >> 2) & 0x03ffffffu);
    } else if (rel.kind == Stage0RelocKind::kMipsHi16) {
      const uint32_t hi = ((full_addr + 0x8000u) >> 16) & 0xffffu;
      linked.machine_words[rel.word_index] =
          (linked.machine_words[rel.word_index] & 0xffff0000u) | hi;
    } else if (rel.kind == Stage0RelocKind::kMipsLo16) {
      linked.machine_words[rel.word_index] =
          (linked.machine_words[rel.word_index] & 0xffff0000u) | (full_addr & 0xffffu);
    } else if (rel.kind == Stage0RelocKind::kMipsGpRel16 ||
               rel.kind == Stage0RelocKind::kMipsLiteral) {
      const int32_t delta = static_cast<int32_t>(full_addr - linked.gp_vram);
      linked.machine_words[rel.word_index] = (linked.machine_words[rel.word_index] & 0xffff0000u) |
                                             (static_cast<uint32_t>(delta) & 0xffffu);
    }
  }

  for (size_t idx = 0; idx < linked.machine_words.size();) {
    const uint32_t w = linked.machine_words[idx];
    if (!relocated_word[idx] && ((w >> 26) & 0x3fu) == 0x02u) {
      const uint32_t local_idx = w & 0x03ffffffu;
      const uint32_t target_vram = linked.base_vram + local_idx * 4u;
      linked.machine_words[idx] = 0x08000000u | ((target_vram >> 2) & 0x03ffffffu);
    }
    const uint32_t op = (w >> 26) & 0x3fu;
    const uint32_t funct = w & 0x3fu;
    const bool has_delay =
        (op == 0x01u || op == 0x02u || op == 0x03u || (op >= 0x04u && op <= 0x07u) ||
         (op >= 0x14u && op <= 0x17u) || (op == 0x00u && (funct == 0x08u || funct == 0x09u)) ||
         (op == 0x11u && ((w >> 21) & 0x1fu) == 0x08u));
    idx += has_delay ? 2u : 1u;
  }

  if (options.reverse_jump_tables) {
    std::reverse(linked.jump_tables.begin(), linked.jump_tables.end());
  }
  if (options.strip_table_and_pool_labels) {
    for (Stage0JumpTable& jt : linked.jump_tables) {
      jt.label_name.clear();
    }
    for (Stage0ConstantPoolEntry& cp : linked.constant_pool) {
      cp.label_name.clear();
    }
  }
  return linked;
}

bool HasDistinctLinkedRomRepresentation(const Stage0FunctionInput& reloc_input,
                                        const Stage0FunctionInput& linked) {
  if (reloc_input.machine_words != linked.machine_words || !reloc_input.relocations.empty() ||
      !linked.relocations.empty() || !reloc_input.jump_tables.empty() ||
      !linked.jump_tables.empty() || !reloc_input.constant_pool.empty() ||
      !linked.constant_pool.empty() || !reloc_input.vram_to_symbol.empty() ||
      !linked.vram_to_symbol.empty() || !reloc_input.vram_to_symbol_size.empty() ||
      !linked.vram_to_symbol_size.empty() || reloc_input.profile != linked.profile ||
      reloc_input.profile.c_flags.empty()) {
    return true;
  }
  for (uint32_t w : reloc_input.machine_words) {
    const uint32_t op = (w >> 26) & 0x3fu;
    if (op == 0x02u || op == 0x03u) {
      return true;
    }
  }
  return false;
}

}  // namespace rom_nom_nom::lifter
