#include "fuzzer/candidate_symbol_map.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/endian.h"
#include "core/game_config.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom::fuzzer {

bool CandidateSymbolMap::IsCandidateData(uint32_t addr) const {
  for (const auto& [start, end] : data_ranges) {
    if (addr >= start && addr < end) return true;
  }
  return false;
}

bool CandidateSymbolMap::IsDataSymbol(uint32_t cand_addr) const {
  if (entries.empty()) return false;
  auto it = std::upper_bound(entries.begin(), entries.end(), cand_addr,
                             [](uint32_t addr, const Entry& e) { return addr < e.cand_addr; });
  if (it != entries.begin()) {
    --it;
    if (!it->is_data) return false;
    uint32_t offset = cand_addr - it->cand_addr;
    auto next_it = it + 1;
    while (next_it != entries.end() && next_it->cand_addr == it->cand_addr) {
      ++next_it;
    }
    uint32_t max_offset =
        (next_it != entries.end()) ? (next_it->cand_addr - it->cand_addr) : 0x100000;
    if (it->size > 0) {
      max_offset = std::max(it->size, max_offset);
    }
    uint32_t sym_limit = (it->size > 0) ? it->size : std::min(max_offset, 256u);
    return offset < sym_limit;
  }
  return false;
}

bool CandidateSymbolMap::IsCandidateRodata(uint32_t addr, bool is_fp) const {
  if (is_fp && IsDataSymbol(addr)) return false;
  for (const auto& [start, end] : rodata_ranges) {
    if (addr >= start && addr < end) return true;
  }
  return false;
}

uint32_t CandidateSymbolMap::Translate(uint32_t cand_addr) const {
  if (!entries.empty()) {
    auto it = std::upper_bound(entries.begin(), entries.end(), cand_addr,
                               [](uint32_t addr, const Entry& e) { return addr < e.cand_addr; });
    if (it != entries.begin()) {
      --it;
      uint32_t offset = cand_addr - it->cand_addr;
      auto next_it = it + 1;
      while (next_it != entries.end() && next_it->cand_addr == it->cand_addr) {
        ++next_it;
      }
      uint32_t max_offset =
          (next_it != entries.end()) ? (next_it->cand_addr - it->cand_addr) : 0x100000;
      if (it->size > 0) {
        max_offset = std::max(it->size, max_offset);
      }
      uint32_t sym_limit = (it->size > 0) ? it->size : std::min(max_offset, 256u);
      if (offset < sym_limit) {
        return it->target_addr + offset;
      }
    }
  }
  for (const auto& sr : section_ranges) {
    if (cand_addr >= sr.cand_start && cand_addr < sr.cand_end) {
      return static_cast<uint32_t>(cand_addr + sr.shift);
    }
    if (cand_addr >= 0xA0000000 && cand_addr < 0xC0000000) {
      uint32_t kseg0 = cand_addr - 0x20000000;
      if (kseg0 >= sr.cand_start && kseg0 < sr.cand_end) {
        return static_cast<uint32_t>(cand_addr + sr.shift);
      }
    }
  }
  return cand_addr;
}

std::vector<MemoryPayload> ExtractTargetDataPayloads(absl::Span<const uint8_t> base_rom,
                                                     const GameConfig* config) {
  std::vector<MemoryPayload> payloads;
  if (config == nullptr || base_rom.empty()) return payloads;

  for (const auto& seg : ResolveSegments(*config, base_rom.size())) {
    if (seg.type != SEGMENT_CODE) continue;
    for (const auto& sub : seg.subsegments) {
      if (sub.type == SUBSEGMENT_DATA || sub.type == SUBSEGMENT_RODATA) {
        if (sub.rom_start < sub.rom_end && sub.rom_end <= base_rom.size()) {
          MemoryPayload payload;
          payload.address = sub.vram_start;
          payload.data.assign(base_rom.begin() + sub.rom_start, base_rom.begin() + sub.rom_end);
          payloads.push_back(std::move(payload));
        }
      }
    }
  }
  return payloads;
}

std::vector<MemoryPayload> ExtractCandidateDataPayloads(absl::Span<const uint8_t> candidate_rom,
                                                        absl::Span<const uint8_t> candidate_elf,
                                                        const GameConfig* config) {
  std::vector<MemoryPayload> payloads;
  if (candidate_rom.empty() || candidate_elf.size() < 52) return payloads;

  absl::flat_hash_set<std::string> valid_prefixes;
  if (config != nullptr) {
    for (const auto& seg : config->segments()) {
      if (seg.type() == SEGMENT_CODE) {
        valid_prefixes.insert(std::string(seg.name()));
      }
    }
  }

  uint32_t e_phoff = ReadBigEndian32(candidate_elf.data() + 28);
  uint16_t e_phentsize = ReadBigEndian16(candidate_elf.data() + 42);
  uint16_t e_phnum = ReadBigEndian16(candidate_elf.data() + 44);

  struct SegmentMapping {
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
  };
  std::vector<SegmentMapping> load_segments;
  if (e_phoff != 0 && e_phentsize >= 32) {
    for (uint16_t i = 0; i < e_phnum; ++i) {
      size_t ph_offset = e_phoff + i * e_phentsize;
      if (ph_offset + 32 > candidate_elf.size()) break;
      uint32_t p_type = ReadBigEndian32(candidate_elf.data() + ph_offset);
      if (p_type == 1 /* PT_LOAD */) {
        uint32_t p_vaddr = ReadBigEndian32(candidate_elf.data() + ph_offset + 8);
        uint32_t p_paddr = ReadBigEndian32(candidate_elf.data() + ph_offset + 12);
        uint32_t p_filesz = ReadBigEndian32(candidate_elf.data() + ph_offset + 16);
        uint32_t p_memsz = ReadBigEndian32(candidate_elf.data() + ph_offset + 20);
        load_segments.push_back({p_vaddr, p_paddr, p_filesz, p_memsz});
      }
    }
  }

  uint32_t e_shoff = ReadBigEndian32(candidate_elf.data() + 32);
  uint16_t e_shentsize = ReadBigEndian16(candidate_elf.data() + 46);
  uint16_t e_shnum = ReadBigEndian16(candidate_elf.data() + 48);
  if (e_shoff == 0 || e_shentsize < 40 ||
      e_shoff + static_cast<size_t>(e_shnum) * e_shentsize > candidate_elf.size()) {
    return payloads;
  }

  size_t symtab_offset = 0;
  size_t symtab_size = 0;
  size_t symtab_entsize = 16;
  size_t strtab_offset = 0;
  size_t strtab_size = 0;

  for (uint16_t i = 0; i < e_shnum; ++i) {
    size_t sh_off = e_shoff + i * e_shentsize;
    uint32_t sh_type = ReadBigEndian32(candidate_elf.data() + sh_off + 4);
    if (sh_type == 2 /* SHT_SYMTAB */) {
      symtab_offset = ReadBigEndian32(candidate_elf.data() + sh_off + 16);
      symtab_size = ReadBigEndian32(candidate_elf.data() + sh_off + 20);
      symtab_entsize = ReadBigEndian32(candidate_elf.data() + sh_off + 36);
      if (symtab_entsize == 0) symtab_entsize = 16;
      uint32_t sh_link = ReadBigEndian32(candidate_elf.data() + sh_off + 24);
      if (sh_link < e_shnum) {
        size_t str_hdr = e_shoff + sh_link * e_shentsize;
        strtab_offset = ReadBigEndian32(candidate_elf.data() + str_hdr + 16);
        strtab_size = ReadBigEndian32(candidate_elf.data() + str_hdr + 20);
      }
      break;
    }
  }

  if (symtab_offset == 0 || strtab_offset == 0 ||
      symtab_offset + symtab_size > candidate_elf.size() ||
      strtab_offset + strtab_size > candidate_elf.size()) {
    return payloads;
  }

  absl::flat_hash_map<std::string, uint32_t> symbols;
  for (size_t off = 0; off + symtab_entsize <= symtab_size; off += symtab_entsize) {
    size_t entry = symtab_offset + off;
    uint32_t st_name = ReadBigEndian32(candidate_elf.data() + entry);
    uint32_t st_value = ReadBigEndian32(candidate_elf.data() + entry + 4);
    if (st_name < strtab_size) {
      const char* sym_name =
          reinterpret_cast<const char*>(candidate_elf.data() + strtab_offset + st_name);
      size_t max_len = strtab_size - st_name;
      size_t len = strnlen(sym_name, max_len);
      symbols[std::string(sym_name, len)] = st_value;
    }
  }

  auto vram_to_rom = [&](uint32_t vram) -> std::optional<uint32_t> {
    for (const auto& seg : load_segments) {
      if (vram >= seg.vaddr && vram <= seg.vaddr + seg.filesz) {
        return seg.paddr + (vram - seg.vaddr);
      }
    }
    return std::nullopt;
  };

  for (const auto& [name, start_vram] : symbols) {
    if (absl::EndsWith(name, "_DATA_START") || absl::EndsWith(name, "_RODATA_START")) {
      std::string prefix;
      std::string end_suffix;
      if (absl::EndsWith(name, "_DATA_START")) {
        prefix = name.substr(0, name.size() - std::string("_DATA_START").size());
        end_suffix = "_DATA_END";
      } else {
        prefix = name.substr(0, name.size() - std::string("_RODATA_START").size());
        end_suffix = "_RODATA_END";
      }
      if (!valid_prefixes.empty() && !valid_prefixes.contains(prefix)) {
        continue;
      }
      auto end_it = symbols.find(absl::StrCat(prefix, end_suffix));
      if (end_it != symbols.end()) {
        uint32_t end_vram = end_it->second;
        if (start_vram < end_vram) {
          auto start_rom_opt = vram_to_rom(start_vram);
          auto end_rom_opt = vram_to_rom(end_vram);
          if (start_rom_opt.has_value() && end_rom_opt.has_value() &&
              *start_rom_opt < *end_rom_opt && *end_rom_opt <= candidate_rom.size()) {
            MemoryPayload payload;
            payload.address = start_vram;
            payload.data.assign(candidate_rom.begin() + *start_rom_opt,
                                candidate_rom.begin() + *end_rom_opt);
            payloads.push_back(std::move(payload));
          }
        }
      }
    }
  }

  return payloads;
}

CandidateSymbolMap BuildCandidateSymbolMap(absl::Span<const uint8_t> candidate_elf,
                                           const SymbolIndex* symbol_index) {
  CandidateSymbolMap map;
  if (candidate_elf.size() < 52) return map;

  size_t shoff = ReadBigEndian32(candidate_elf.data() + 32);
  size_t shentsize = ReadBigEndian16(candidate_elf.data() + 46);
  size_t shnum = ReadBigEndian16(candidate_elf.data() + 48);
  if (shoff == 0 || shentsize < 40 || shoff + shnum * shentsize > candidate_elf.size()) {
    return map;
  }

  size_t symtab_offset = 0;
  size_t symtab_size = 0;
  size_t symtab_entsize = 16;
  size_t strtab_offset = 0;
  size_t strtab_size = 0;

  for (size_t i = 0; i < shnum; ++i) {
    size_t hdr = shoff + i * shentsize;
    uint32_t sh_type = ReadBigEndian32(candidate_elf.data() + hdr + 4);
    if (sh_type == 2) {  // SHT_SYMTAB
      symtab_offset = ReadBigEndian32(candidate_elf.data() + hdr + 16);
      symtab_size = ReadBigEndian32(candidate_elf.data() + hdr + 20);
      symtab_entsize = ReadBigEndian32(candidate_elf.data() + hdr + 36);
      if (symtab_entsize == 0) symtab_entsize = 16;
      uint32_t sh_link = ReadBigEndian32(candidate_elf.data() + hdr + 24);
      if (sh_link < shnum) {
        size_t str_hdr = shoff + sh_link * shentsize;
        strtab_offset = ReadBigEndian32(candidate_elf.data() + str_hdr + 16);
        strtab_size = ReadBigEndian32(candidate_elf.data() + str_hdr + 20);
      }
      break;
    }
  }

  if (symtab_offset == 0 || strtab_offset == 0 ||
      symtab_offset + symtab_size > candidate_elf.size() ||
      strtab_offset + strtab_size > candidate_elf.size()) {
    return map;
  }

  struct ElfSection {
    std::string name;
    uint32_t addr;
    uint32_t size;
    uint32_t flags;
    uint32_t type;
  };
  std::vector<ElfSection> alloc_sections;
  if (shoff != 0 && shentsize >= 40 && candidate_elf.size() >= 52) {
    uint16_t e_shstrndx = ReadBigEndian16(candidate_elf.data() + 50);
    size_t shstr_offset = 0;
    size_t shstr_size = 0;
    if (e_shstrndx < shnum) {
      size_t shstr_hdr = shoff + e_shstrndx * shentsize;
      shstr_offset = ReadBigEndian32(candidate_elf.data() + shstr_hdr + 16);
      shstr_size = ReadBigEndian32(candidate_elf.data() + shstr_hdr + 20);
      if (shstr_offset + shstr_size > candidate_elf.size()) {
        shstr_offset = 0;
        shstr_size = 0;
      }
    }
    for (size_t i = 0; i < shnum; ++i) {
      size_t hdr = shoff + i * shentsize;
      uint32_t sh_name = ReadBigEndian32(candidate_elf.data() + hdr);
      uint32_t sh_type = ReadBigEndian32(candidate_elf.data() + hdr + 4);
      uint32_t sh_flags = ReadBigEndian32(candidate_elf.data() + hdr + 8);
      uint32_t sh_addr = ReadBigEndian32(candidate_elf.data() + hdr + 12);
      uint32_t sh_size = ReadBigEndian32(candidate_elf.data() + hdr + 20);
      if (sh_addr == 0 || sh_size == 0 || !(sh_flags & 2 /* SHF_ALLOC */)) continue;
      std::string sname;
      if (shstr_offset != 0 && sh_name < shstr_size) {
        const char* sname_ptr =
            reinterpret_cast<const char*>(candidate_elf.data() + shstr_offset + sh_name);
        size_t max_len = shstr_size - sh_name;
        sname.assign(sname_ptr, strnlen(sname_ptr, max_len));
      }
      alloc_sections.push_back({sname, sh_addr, sh_size, sh_flags, sh_type});
    }
  }

  std::vector<std::pair<uint32_t, uint32_t>> d_symbols;
  absl::flat_hash_map<std::string, uint32_t> elf_syms;

  for (size_t off = 0; off + symtab_entsize <= symtab_size; off += symtab_entsize) {
    size_t entry = symtab_offset + off;
    uint32_t st_name = ReadBigEndian32(candidate_elf.data() + entry);
    uint32_t st_value = ReadBigEndian32(candidate_elf.data() + entry + 4);
    uint32_t st_size = ReadBigEndian32(candidate_elf.data() + entry + 8);
    if (st_name >= strtab_size || st_value == 0) continue;

    const char* sym_name =
        reinterpret_cast<const char*>(candidate_elf.data() + strtab_offset + st_name);
    size_t max_len = strtab_size - st_name;
    std::string_view name(sym_name, strnlen(sym_name, max_len));

    bool is_data = false;
    std::optional<uint32_t> target_addr;
    if (absl::StartsWith(name, "D_") && name.size() == 10) {
      uint32_t parsed = 0;
      if (absl::SimpleHexAtoi(name.substr(2), &parsed)) {
        target_addr = parsed;
        is_data = true;
        d_symbols.push_back({st_value, *target_addr});
      }
    } else if (absl::StartsWith(name, "func_") && name.size() == 13) {
      uint32_t parsed = 0;
      if (absl::SimpleHexAtoi(name.substr(5), &parsed)) {
        target_addr = parsed;
      }
    } else if (symbol_index != nullptr) {
      const auto* sym_entry = symbol_index->FindByName(name);
      if (sym_entry != nullptr) {
        target_addr = sym_entry->address();
        if (sym_entry->size() > 0) {
          st_size = sym_entry->size();
        }
        if (sym_entry->type() == SYMBOL_DATA) {
          is_data = true;
        }
      }
    }

    if (absl::EndsWith(name, "_RODATA_START") || absl::EndsWith(name, "_RODATA_END") ||
        absl::EndsWith(name, "_DATA_START") || absl::EndsWith(name, "_DATA_END")) {
      elf_syms[std::string(name)] = st_value;
    }

    if (target_addr.has_value()) {
      map.entries.push_back({st_value, st_size, *target_addr, is_data});
    }
  }

  for (const auto& [name, start_vram] : elf_syms) {
    if (absl::EndsWith(name, "_RODATA_START")) {
      std::string prefix = name.substr(0, name.size() - std::string("_RODATA_START").size());
      auto end_it = elf_syms.find(absl::StrCat(prefix, "_RODATA_END"));
      if (end_it != elf_syms.end() && start_vram < end_it->second) {
        map.rodata_ranges.push_back({start_vram, end_it->second});
      }
    } else if (absl::EndsWith(name, "_DATA_START")) {
      std::string prefix = name.substr(0, name.size() - std::string("_DATA_START").size());
      auto end_it = elf_syms.find(absl::StrCat(prefix, "_DATA_END"));
      if (end_it != elf_syms.end() && start_vram < end_it->second) {
        map.data_ranges.push_back({start_vram, end_it->second});
      }
    }
  }

  for (const auto& sec : alloc_sections) {
    absl::flat_hash_map<int64_t, size_t> shift_counts;
    absl::flat_hash_map<int64_t, uint32_t> min_addrs;
    for (const auto& [cand_addr, target_addr] : d_symbols) {
      if (cand_addr >= sec.addr && cand_addr < sec.addr + sec.size) {
        int64_t diff = static_cast<int64_t>(target_addr) - static_cast<int64_t>(cand_addr);
        shift_counts[diff]++;
        auto it = min_addrs.find(diff);
        if (it == min_addrs.end() || cand_addr < it->second) {
          min_addrs[diff] = cand_addr;
        }
      }
    }
    int64_t chosen_shift = 0;
    size_t max_count = 0;
    for (const auto& [shift, count] : shift_counts) {
      if (count > max_count || (count == max_count && (std::abs(shift) < std::abs(chosen_shift) ||
                                                       (std::abs(shift) == std::abs(chosen_shift) &&
                                                        shift < chosen_shift)))) {
        max_count = count;
        chosen_shift = shift;
      }
    }
    if (max_count > 0 && chosen_shift != 0) {
      uint32_t start_addr = (sec.type == 8 /* SHT_NOBITS */) ? sec.addr : min_addrs[chosen_shift];
      uint32_t end_addr = sec.addr + sec.size;
      map.section_ranges.push_back({start_addr, end_addr, chosen_shift});
    }
  }

  std::sort(map.entries.begin(), map.entries.end(),
            [](const CandidateSymbolMap::Entry& a, const CandidateSymbolMap::Entry& b) {
              if (a.cand_addr != b.cand_addr) return a.cand_addr < b.cand_addr;
              bool a_shifted = (a.cand_addr != a.target_addr);
              bool b_shifted = (b.cand_addr != b.target_addr);
              if (a_shifted != b_shifted) return a_shifted > b_shifted;
              if (a.is_data != b.is_data) return a.is_data > b.is_data;
              return a.size > b.size;
            });
  auto unique_end =
      std::unique(map.entries.begin(), map.entries.end(),
                  [](const CandidateSymbolMap::Entry& a, const CandidateSymbolMap::Entry& b) {
                    return a.cand_addr == b.cand_addr;
                  });
  map.entries.erase(unique_end, map.entries.end());
  return map;
}

}  // namespace rom_nom_nom::fuzzer
