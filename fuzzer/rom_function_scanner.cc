#include "fuzzer/rom_function_scanner.h"

#include <algorithm>
#include <cstdint>
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

}  // namespace rom_nom_nom::fuzzer
