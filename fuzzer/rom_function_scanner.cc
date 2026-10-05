#include "fuzzer/rom_function_scanner.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/endian.h"
#include "splitter/disassembler.h"

namespace rom_nom_nom::fuzzer {

absl::StatusOr<std::vector<ScannedFunction>> RomFunctionScanner::Scan(
    absl::Span<const uint8_t> rom_bytes, const SplitConfig* config, const SymbolIndex* symbols) {
  if (rom_bytes.size() < 0x1050) {
    return absl::InvalidArgumentError(
        absl::StrFormat("RomFunctionScanner: ROM too small (%zu bytes)", rom_bytes.size()));
  }

  struct CodeRegion {
    size_t rom_start;
    size_t rom_end;
    uint32_t vram;
  };
  std::vector<CodeRegion> regions;

  if (config != nullptr) {
    for (int i = 0; i < config->segments_size(); ++i) {
      const auto& seg = config->segments(i);
      if (seg.type() != SEGMENT_CODE) {
        continue;
      }
      size_t seg_start = seg.rom_start();
      size_t seg_end = seg.rom_end();
      if (seg_end == 0) {
        seg_end = rom_bytes.size();
        for (int k = i + 1; k < config->segments_size(); ++k) {
          if (config->segments(k).type() != SEGMENT_BSS &&
              config->segments(k).rom_start() > seg_start) {
            seg_end = std::min<size_t>(config->segments(k).rom_start(), rom_bytes.size());
            break;
          }
        }
      }
      if (seg.subsegments_size() > 0) {
        for (int j = 0; j < seg.subsegments_size(); ++j) {
          const auto& sub = seg.subsegments(j);
          if (sub.type() != SUBSEGMENT_C && sub.type() != SUBSEGMENT_ASM &&
              sub.type() != SUBSEGMENT_HASM) {
            continue;
          }
          size_t sub_start = sub.rom_start();
          size_t sub_end = seg_end;
          for (int k = j + 1; k < seg.subsegments_size(); ++k) {
            if (seg.subsegments(k).type() != SUBSEGMENT_BSS && seg.subsegments(k).rom_start() > 0) {
              sub_end = seg.subsegments(k).rom_start();
              break;
            }
          }
          uint32_t sub_vram = sub.vram();
          if (sub_vram == 0 && seg.vram() != 0) {
            sub_vram = seg.vram() + static_cast<uint32_t>(sub_start - seg_start);
          }
          if (sub_start < sub_end && sub_end <= rom_bytes.size()) {
            regions.push_back({sub_start, sub_end, sub_vram});
          }
        }
      } else {
        regions.push_back({seg_start, seg_end, seg.vram()});
      }
    }
  }

  // Fallback if no code segments found in config
  if (regions.empty()) {
    uint32_t entrypoint = ReadBigEndian32(rom_bytes.data() + 8);
    size_t code_start = 0x1050;
    size_t code_end = rom_bytes.size();
    uint32_t base_vram = entrypoint != 0 ? (entrypoint + 0x50) : 0x80025C50;
    regions.push_back({code_start, code_end, base_vram});
  }

  DisassemblerOptions disasm_opts;
  disasm_opts.emit_line_comments = false;
  disasm_opts.emit_function_framing = false;
  disasm_opts.emit_relocations = false;
  Disassembler disasm(symbols, disasm_opts);

  if (config != nullptr) {
    for (int i = 0; i < config->segments_size(); ++i) {
      const auto& seg = config->segments(i);
      if (seg.type() != SEGMENT_CODE || seg.vram() == 0) {
        continue;
      }
      size_t seg_start = seg.rom_start();
      size_t seg_end = seg.rom_end() != 0 ? seg.rom_end() : rom_bytes.size();
      if (seg_start < seg_end && seg_end <= rom_bytes.size()) {
        disasm.PreScanCode(rom_bytes.subspan(seg_start, seg_end - seg_start), seg.vram());
      }
    }
  }

  std::vector<ScannedFunction> result;

  for (const auto& reg : regions) {
    absl::Span<const uint8_t> code_slice =
        rom_bytes.subspan(reg.rom_start, reg.rom_end - reg.rom_start);
    auto funcs_or = disasm.DisassembleAllFunctions(code_slice, reg.vram);
    if (!funcs_or.ok()) {
      return funcs_or.status();
    }

    for (const auto& dfn : *funcs_or) {
      if (dfn.instructions.empty()) {
        continue;
      }
      ScannedFunction sf;
      sf.name = dfn.name;
      sf.vram = dfn.vram_start;
      sf.rom_offset = reg.rom_start + (dfn.vram_start - reg.vram);
      sf.raw_words.reserve(dfn.instructions.size());
      for (const auto& inst : dfn.instructions) {
        sf.raw_words.push_back(inst.raw_word);
      }

      // Trim trailing zeros that exceed the 16-byte alignment boundary.
      // A function's logical exit (e.g. jr $ra + delay slot) only needs at most
      // 3 trailing alignment NOPs to reach a 16-byte boundary. Any zeros beyond
      // that are inter-segment or buffer padding.
      size_t last_nz = 0;
      bool has_nz = false;
      for (size_t i = sf.raw_words.size(); i > 0; --i) {
        if (sf.raw_words[i - 1] != 0) {
          last_nz = i - 1;
          has_nz = true;
          break;
        }
      }
      if (has_nz) {
        // Allow for delay slot (last_nz + 1)
        size_t min_words = last_nz + 2;
        // Align up to 16 bytes (4 words)
        size_t aligned_words = (min_words + 3) & ~size_t{3};
        if (aligned_words < sf.raw_words.size()) {
          bool all_zeros = true;
          for (size_t i = aligned_words; i < sf.raw_words.size(); ++i) {
            if (sf.raw_words[i] != 0) {
              all_zeros = false;
              break;
            }
          }
          if (all_zeros) {
            sf.raw_words.resize(aligned_words);
          }
        }
      }

      sf.size = static_cast<uint32_t>(sf.raw_words.size() * 4);
      result.push_back(std::move(sf));
    }
  }

  return result;
}

absl::StatusOr<std::vector<ScannedFunction>> RomFunctionScanner::ScanFromElfAndRom(
    absl::Span<const uint8_t> rom_bytes, absl::Span<const uint8_t> elf_bytes,
    const SplitConfig* config) {
  if (elf_bytes.size() < 52) {
    return absl::InvalidArgumentError("RomFunctionScanner: ELF binary too small");
  }
  if (elf_bytes[0] != 0x7F || elf_bytes[1] != 'E' || elf_bytes[2] != 'L' || elf_bytes[3] != 'F') {
    return absl::InvalidArgumentError("RomFunctionScanner: Not an ELF binary");
  }

  uint32_t e_phoff = ReadBigEndian32(elf_bytes.data() + 28);
  uint16_t e_phentsize = ReadBigEndian16(elf_bytes.data() + 42);
  uint16_t e_phnum = ReadBigEndian16(elf_bytes.data() + 44);

  struct LoadSegment {
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
  };
  std::vector<LoadSegment> load_segments;

  if (e_phoff > 0 && e_phentsize >= 32 &&
      e_phoff + static_cast<size_t>(e_phnum) * e_phentsize <= elf_bytes.size()) {
    for (uint16_t i = 0; i < e_phnum; ++i) {
      size_t ph_offset = e_phoff + i * e_phentsize;
      uint32_t p_type = ReadBigEndian32(elf_bytes.data() + ph_offset);
      if (p_type == 1 /* PT_LOAD */) {
        uint32_t p_vaddr = ReadBigEndian32(elf_bytes.data() + ph_offset + 8);
        uint32_t p_paddr = ReadBigEndian32(elf_bytes.data() + ph_offset + 12);
        uint32_t p_filesz = ReadBigEndian32(elf_bytes.data() + ph_offset + 16);
        uint32_t p_memsz = ReadBigEndian32(elf_bytes.data() + ph_offset + 20);
        load_segments.push_back({p_vaddr, p_paddr, p_filesz, p_memsz});
      }
    }
  }

  uint32_t e_shoff = ReadBigEndian32(elf_bytes.data() + 32);
  uint16_t e_shentsize = ReadBigEndian16(elf_bytes.data() + 46);
  uint16_t e_shnum = ReadBigEndian16(elf_bytes.data() + 48);

  if (e_shoff == 0 || e_shentsize < 40 ||
      e_shoff + static_cast<size_t>(e_shnum) * e_shentsize > elf_bytes.size()) {
    return absl::InvalidArgumentError("RomFunctionScanner: Invalid ELF section headers");
  }

  size_t symtab_offset = 0;
  size_t symtab_size = 0;
  size_t symtab_entsize = 16;
  size_t strtab_offset = 0;
  size_t strtab_size = 0;

  for (uint16_t i = 0; i < e_shnum; ++i) {
    size_t sh_off = e_shoff + i * e_shentsize;
    uint32_t sh_type = ReadBigEndian32(elf_bytes.data() + sh_off + 4);
    if (sh_type == 2 /* SHT_SYMTAB */) {
      symtab_offset = ReadBigEndian32(elf_bytes.data() + sh_off + 16);
      symtab_size = ReadBigEndian32(elf_bytes.data() + sh_off + 20);
      symtab_entsize = ReadBigEndian32(elf_bytes.data() + sh_off + 36);
      if (symtab_entsize == 0) symtab_entsize = 16;
      uint32_t sh_link = ReadBigEndian32(elf_bytes.data() + sh_off + 24);
      if (sh_link < e_shnum) {
        size_t str_hdr = e_shoff + sh_link * e_shentsize;
        strtab_offset = ReadBigEndian32(elf_bytes.data() + str_hdr + 16);
        strtab_size = ReadBigEndian32(elf_bytes.data() + str_hdr + 20);
      }
      break;
    }
  }

  if (symtab_offset == 0 || strtab_offset == 0 || symtab_offset + symtab_size > elf_bytes.size() ||
      strtab_offset + strtab_size > elf_bytes.size()) {
    return absl::InvalidArgumentError(
        "RomFunctionScanner: Symbol table or string table not found in ELF");
  }

  auto vram_to_rom = [&](uint32_t vram) -> std::optional<uint32_t> {
    for (const auto& seg : load_segments) {
      if (vram >= seg.vaddr && vram < seg.vaddr + seg.memsz) {
        return seg.paddr + (vram - seg.vaddr);
      }
    }
    if (config != nullptr) {
      for (const auto& seg : config->segments()) {
        if (seg.type() != SEGMENT_CODE || seg.vram() == 0) continue;
        size_t seg_start = seg.rom_start();
        size_t seg_end = seg.rom_end() != 0 ? seg.rom_end() : rom_bytes.size();
        uint32_t seg_size = static_cast<uint32_t>(seg_end - seg_start);
        if (vram >= seg.vram() && vram < seg.vram() + seg_size) {
          return static_cast<uint32_t>(seg_start + (vram - seg.vram()));
        }
      }
    }
    return std::nullopt;
  };

  std::vector<ScannedFunction> functions;
  for (size_t off = 0; off + symtab_entsize <= symtab_size; off += symtab_entsize) {
    size_t entry = symtab_offset + off;
    uint32_t st_name = ReadBigEndian32(elf_bytes.data() + entry);
    uint32_t st_value = ReadBigEndian32(elf_bytes.data() + entry + 4);
    uint32_t st_size = ReadBigEndian32(elf_bytes.data() + entry + 8);
    uint8_t st_info = elf_bytes[entry + 12];
    uint8_t st_type = st_info & 0x0F;

    if (st_type != 2 /* STT_FUNC */) {
      continue;
    }
    if (st_name >= strtab_size) {
      continue;
    }
    const char* name_ptr =
        reinterpret_cast<const char*>(elf_bytes.data() + strtab_offset + st_name);
    std::string name(name_ptr);
    if (name.empty()) {
      continue;
    }

    auto rom_off_opt = vram_to_rom(st_value);
    if (!rom_off_opt.has_value()) {
      continue;
    }
    uint32_t rom_off = *rom_off_opt;
    if (rom_off + st_size > rom_bytes.size()) {
      continue;
    }

    ScannedFunction sf;
    sf.name = std::move(name);
    sf.vram = st_value;
    sf.rom_offset = rom_off;
    sf.size = st_size;
    sf.raw_words.reserve(st_size / 4);
    for (uint32_t w = 0; w < st_size; w += 4) {
      if (rom_off + w + 4 <= rom_bytes.size()) {
        sf.raw_words.push_back(ReadBigEndian32(rom_bytes.data() + rom_off + w));
      }
    }
    functions.push_back(std::move(sf));
  }

  std::sort(functions.begin(), functions.end(),
            [](const ScannedFunction& a, const ScannedFunction& b) {
              if (a.vram != b.vram) return a.vram < b.vram;
              return a.rom_offset < b.rom_offset;
            });

  return functions;
}

}  // namespace rom_nom_nom::fuzzer
