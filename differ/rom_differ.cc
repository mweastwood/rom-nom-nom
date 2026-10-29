#include "differ/rom_differ.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/disassembler.h"
#include "core/endian.h"
#include "core/game_config.h"
#include "core/game_config.pb.h"
#include "core/register_tracker.h"
#include "core/rom.h"
#include "core/sha1.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"
#include "differ/diff_formatter.h"

namespace rom_nom_nom {

namespace {

struct ScannedRomFunction {
  std::string name;
  uint32_t vram = 0;
  std::vector<uint32_t> raw_words;
};

std::vector<ScannedRomFunction> ScanFunctionsFromRom(absl::Span<const uint8_t> rom_bytes,
                                                     const GameConfig* config,
                                                     const SymbolIndex* symbols) {
  if (rom_bytes.size() < 0x1050) {
    return {};
  }
  struct CodeRegion {
    size_t rom_start;
    size_t rom_end;
    uint32_t vram;
  };
  std::vector<CodeRegion> regions;
  if (config != nullptr) {
    for (const auto& seg : ResolveSegments(*config, rom_bytes.size())) {
      if (seg.type != SEGMENT_CODE) {
        continue;
      }
      if (!seg.subsegments.empty()) {
        for (const auto& sub : seg.subsegments) {
          if (sub.type != SUBSEGMENT_C && sub.type != SUBSEGMENT_ASM) {
            continue;
          }
          if (sub.rom_start < sub.rom_end && sub.rom_end <= rom_bytes.size()) {
            regions.push_back({sub.rom_start, sub.rom_end, sub.vram_start});
          }
        }
      } else if (seg.rom_start < seg.rom_end && seg.rom_end <= rom_bytes.size()) {
        regions.push_back({seg.rom_start, seg.rom_end, seg.vram_start});
      }
    }
  }
  if (regions.empty()) {
    uint32_t entrypoint = ReadBigEndian32(rom_bytes.data() + 8);
    if (entrypoint != 0) {
      regions.push_back({0x1050, rom_bytes.size(), entrypoint + 0x50});
    }
  }

  DisassemblerOptions disasm_opts;
  disasm_opts.emit_line_comments = false;
  disasm_opts.emit_function_framing = false;
  disasm_opts.emit_relocations = false;
  Disassembler disasm(symbols, disasm_opts);

  if (config != nullptr) {
    for (const auto& seg : ResolveSegments(*config, rom_bytes.size())) {
      if (seg.type != SEGMENT_CODE || seg.vram_start == 0) {
        continue;
      }
      if (seg.rom_start < seg.rom_end && seg.rom_end <= rom_bytes.size()) {
        disasm.PreScanCode(rom_bytes.subspan(seg.rom_start, seg.rom_end - seg.rom_start),
                           seg.vram_start);
      }
    }
  }

  std::vector<ScannedRomFunction> result;
  for (const auto& reg : regions) {
    absl::Span<const uint8_t> code_slice =
        rom_bytes.subspan(reg.rom_start, reg.rom_end - reg.rom_start);
    auto funcs_or = disasm.DisassembleAllFunctions(code_slice, reg.vram);
    if (!funcs_or.ok()) {
      continue;
    }
    for (const auto& dfn : *funcs_or) {
      if (!dfn.is_function || dfn.instructions.empty()) {
        continue;
      }
      ScannedRomFunction sf;
      sf.name = dfn.name;
      sf.vram = dfn.vram_start;
      sf.raw_words.reserve(dfn.instructions.size());
      for (const auto& inst : dfn.instructions) {
        sf.raw_words.push_back(inst.raw_word);
      }
      result.push_back(std::move(sf));
    }
  }
  return result;
}

std::vector<ScannedRomFunction> ScanFunctionsFromElfAndRom(absl::Span<const uint8_t> rom_bytes,
                                                           absl::Span<const uint8_t> elf_bytes,
                                                           const GameConfig* config) {
  if (elf_bytes.size() < 52 || elf_bytes[0] != 0x7F || elf_bytes[1] != 'E' || elf_bytes[2] != 'L' ||
      elf_bytes[3] != 'F') {
    return {};
  }
  uint32_t e_phoff = ReadBigEndian32(elf_bytes.data() + 28);
  uint16_t e_phentsize = ReadBigEndian16(elf_bytes.data() + 42);
  uint16_t e_phnum = ReadBigEndian16(elf_bytes.data() + 44);

  struct LoadSegment {
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
  };
  std::vector<LoadSegment> load_segments;
  if (e_phoff > 0 && e_phentsize >= 32 &&
      e_phoff + static_cast<size_t>(e_phnum) * e_phentsize <= elf_bytes.size()) {
    for (uint16_t i = 0; i < e_phnum; ++i) {
      size_t ph_offset = e_phoff + i * e_phentsize;
      uint32_t p_type = ReadBigEndian32(elf_bytes.data() + ph_offset);
      if (p_type == 1 /* PT_LOAD */) {
        load_segments.push_back({
            ReadBigEndian32(elf_bytes.data() + ph_offset + 8),
            ReadBigEndian32(elf_bytes.data() + ph_offset + 12),
            ReadBigEndian32(elf_bytes.data() + ph_offset + 16),
        });
      }
    }
  }

  uint32_t e_shoff = ReadBigEndian32(elf_bytes.data() + 32);
  uint16_t e_shentsize = ReadBigEndian16(elf_bytes.data() + 46);
  uint16_t e_shnum = ReadBigEndian16(elf_bytes.data() + 48);
  if (e_shoff == 0 || e_shentsize < 40 ||
      e_shoff + static_cast<size_t>(e_shnum) * e_shentsize > elf_bytes.size()) {
    return {};
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
    return {};
  }

  auto vram_to_rom = [&](uint32_t vram) -> std::optional<uint32_t> {
    for (const auto& seg : load_segments) {
      if (vram >= seg.vaddr && vram < seg.vaddr + seg.filesz) {
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

  std::vector<ScannedRomFunction> functions;
  for (size_t off = 0; off + symtab_entsize <= symtab_size; off += symtab_entsize) {
    size_t entry = symtab_offset + off;
    uint32_t st_name = ReadBigEndian32(elf_bytes.data() + entry);
    uint32_t st_value = ReadBigEndian32(elf_bytes.data() + entry + 4);
    uint32_t st_size = ReadBigEndian32(elf_bytes.data() + entry + 8);
    uint8_t st_type = elf_bytes[entry + 12] & 0x0F;
    if (st_type != 2 /* STT_FUNC */ || st_name >= strtab_size) {
      continue;
    }
    const char* name_ptr =
        reinterpret_cast<const char*>(elf_bytes.data() + strtab_offset + st_name);
    size_t max_len = strtab_size - st_name;
    std::string name(name_ptr, strnlen(name_ptr, max_len));
    if (name.empty()) {
      continue;
    }
    auto rom_off_opt = vram_to_rom(st_value);
    if (!rom_off_opt.has_value() || *rom_off_opt + st_size > rom_bytes.size()) {
      continue;
    }
    uint32_t rom_off = *rom_off_opt;
    ScannedRomFunction sf;
    sf.name = std::move(name);
    sf.vram = st_value;
    sf.raw_words.reserve(st_size / 4);
    for (uint32_t w = 0; w < st_size; w += 4) {
      if (rom_off + w + 4 <= rom_bytes.size()) {
        sf.raw_words.push_back(ReadBigEndian32(rom_bytes.data() + rom_off + w));
      }
    }
    functions.push_back(std::move(sf));
  }
  return functions;
}

struct LoadedRom {
  std::optional<Rom> rom;
  std::vector<uint8_t> fallback_bytes;

  absl::Span<const uint8_t> Data() const {
    return rom.has_value() ? rom->Data() : absl::MakeConstSpan(fallback_bytes);
  }

  std::string Sha1() const {
    return rom.has_value() ? rom->Sha1() : ComputeSha1Hex(fallback_bytes);
  }
};

absl::StatusOr<LoadedRom> LoadRomOrFallback(const std::filesystem::path& file_path) {
  auto rom_or = Rom::OpenFile(file_path);
  if (rom_or.ok()) {
    LoadedRom loaded;
    loaded.rom = std::move(*rom_or);
    return loaded;
  }
  if (!absl::IsInvalidArgument(rom_or.status())) {
    return rom_or.status();
  }
  std::ifstream file_stream(file_path, std::ios::binary);
  if (!file_stream.is_open()) {
    return absl::NotFoundError(absl::StrFormat("Could not open file '%s'", file_path.string()));
  }
  LoadedRom loaded;
  loaded.fallback_bytes = std::vector<uint8_t>((std::istreambuf_iterator<char>(file_stream)),
                                               std::istreambuf_iterator<char>());
  return loaded;
}

std::vector<uint32_t> ExtractBigEndianWords(absl::Span<const uint8_t> bytes) {
  std::vector<uint32_t> words;
  words.reserve(bytes.size() / 4);
  for (size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
    words.push_back(ReadBigEndian32(bytes.data() + offset));
  }
  return words;
}

}  // namespace

RomDiffer::RomDiffer(RomDiffOptions options) : options_(std::move(options)) {}

size_t RomDiffer::ComputeWordLcs(absl::Span<const uint32_t> target_words,
                                 absl::Span<const uint32_t> built_words) {
  const size_t target_length = target_words.size();
  const size_t built_length = built_words.size();
  if (target_length == 0 || built_length == 0) {
    return 0;
  }

  // 1. Trim common prefix
  size_t prefix_length = 0;
  const size_t minimum_length = std::min(target_length, built_length);
  while (prefix_length < minimum_length &&
         target_words[prefix_length] == built_words[prefix_length]) {
    ++prefix_length;
  }
  if (prefix_length == target_length || prefix_length == built_length) {
    return prefix_length;
  }

  // 2. Trim common suffix
  size_t suffix_length = 0;
  while (suffix_length < (target_length - prefix_length) &&
         suffix_length < (built_length - prefix_length) &&
         target_words[target_length - 1 - suffix_length] ==
             built_words[built_length - 1 - suffix_length]) {
    ++suffix_length;
  }

  absl::Span<const uint32_t> middle_target =
      target_words.subspan(prefix_length, target_length - prefix_length - suffix_length);
  absl::Span<const uint32_t> middle_built =
      built_words.subspan(prefix_length, built_length - prefix_length - suffix_length);

  // 3. Hunt-Szymanski algorithm on trimmed middle sequences
  std::unordered_map<uint32_t, std::vector<int>> target_word_positions;
  target_word_positions.reserve(middle_target.size());
  for (int index = 0; index < static_cast<int>(middle_target.size()); ++index) {
    target_word_positions[middle_target[index]].push_back(index);
  }

  std::vector<int> longest_increasing_subsequence;
  longest_increasing_subsequence.reserve(std::min(middle_target.size(), middle_built.size()));

  for (int built_index = 0; built_index < static_cast<int>(middle_built.size()); ++built_index) {
    auto positions_iterator = target_word_positions.find(middle_built[built_index]);
    if (positions_iterator == target_word_positions.end()) {
      continue;
    }
    const std::vector<int>& positions = positions_iterator->second;
    // Iterate in reverse order so each position in middle_target is consumed at most once
    for (auto reverse_position_iterator = positions.rbegin();
         reverse_position_iterator != positions.rend(); ++reverse_position_iterator) {
      int position = *reverse_position_iterator;
      auto lower_bound_iterator = std::lower_bound(longest_increasing_subsequence.begin(),
                                                   longest_increasing_subsequence.end(), position);
      if (lower_bound_iterator == longest_increasing_subsequence.end()) {
        longest_increasing_subsequence.push_back(position);
      } else {
        *lower_bound_iterator = position;
      }
    }
  }

  return prefix_length + longest_increasing_subsequence.size() + suffix_length;
}

absl::StatusOr<RomDiffResult> RomDiffer::Diff() const {
  auto target_loaded_or = LoadRomOrFallback(options_.base_rom_path);
  if (!target_loaded_or.ok()) {
    return target_loaded_or.status();
  }
  absl::Span<const uint8_t> target_bytes = target_loaded_or->Data();

  auto built_loaded_or = LoadRomOrFallback(options_.candidate_rom_path);
  if (!built_loaded_or.ok()) {
    return built_loaded_or.status();
  }
  absl::Span<const uint8_t> built_bytes = built_loaded_or->Data();

  RomDiffResult result;
  result.total_rom_bytes = target_bytes.size();

  // 1. Structural Validation
  result.structural.target_size = target_bytes.size();
  result.structural.built_size = built_bytes.size();
  result.structural.target_sha1 = target_loaded_or->Sha1();
  result.structural.built_sha1 = built_loaded_or->Sha1();
  result.structural.size_matches = (result.structural.target_size == result.structural.built_size);

  if (target_bytes.size() >= 4 && built_bytes.size() >= 4) {
    result.structural.header_magic_matches =
        (ReadBigEndian32(target_bytes.data()) == ReadBigEndian32(built_bytes.data()));
  }
  if (target_bytes.size() >= 12 && built_bytes.size() >= 12) {
    result.structural.entrypoint_matches =
        (ReadBigEndian32(target_bytes.data() + 8) == ReadBigEndian32(built_bytes.data() + 8));
  }

  // 2. Header and IPL3 Verification (0x0 - 0x1050)
  const size_t header_end_offset = std::min<size_t>(0x1050, target_bytes.size());
  result.header_total_bytes = header_end_offset;
  size_t header_matches = 0;
  for (size_t index = 0; index < header_end_offset && index < built_bytes.size(); ++index) {
    if (target_bytes[index] == built_bytes[index]) {
      ++header_matches;
    }
  }
  result.header_matching_bytes = header_matches;

  // 3. Segment Resolution
  uint64_t retail_code_start = 0x1050;
  uint64_t retail_code_end = target_bytes.size();
  std::string code_segment_name = "code";
  struct AssetSegmentRange {
    std::string name;
    uint64_t rom_start = 0;
    uint64_t rom_end = 0;
  };
  std::vector<AssetSegmentRange> asset_segment_ranges;

  std::optional<GameConfig> loaded_config;
  std::error_code error_code;
  if (!options_.config_path.empty() && std::filesystem::exists(options_.config_path, error_code)) {
    auto config_or = LoadGameConfig(options_.config_path);
    if (config_or.ok()) {
      loaded_config = std::move(*config_or);
      auto resolved_segments = ResolveSegments(*loaded_config);
      bool found_code_segment = false;
      for (const auto& segment : resolved_segments) {
        bool has_code_subsegments = false;
        for (const auto& subsegment : segment.subsegments) {
          if (subsegment.type == SUBSEGMENT_C || subsegment.type == SUBSEGMENT_ASM) {
            has_code_subsegments = true;
            break;
          }
        }
        if (!found_code_segment && (segment.type == SEGMENT_CODE || has_code_subsegments ||
                                    segment.name == "main" || segment.name == "code")) {
          found_code_segment = true;
          code_segment_name = segment.name;
          retail_code_start = segment.rom_start;
          retail_code_end = segment.rom_end > 0 ? segment.rom_end : target_bytes.size();
        } else if (found_code_segment && (segment.type == SEGMENT_CODE || has_code_subsegments) &&
                   asset_segment_ranges.empty()) {
          uint64_t seg_end = segment.rom_end > 0 ? segment.rom_end : target_bytes.size();
          if (seg_end - segment.rom_start > retail_code_end - retail_code_start) {
            code_segment_name = segment.name;
          }
          retail_code_end = std::max(retail_code_end, seg_end);
        } else if (found_code_segment && segment.type != SEGMENT_CODE &&
                   segment.type != SEGMENT_BSS) {
          uint64_t seg_end = segment.rom_end > 0 ? segment.rom_end : target_bytes.size();
          asset_segment_ranges.push_back(AssetSegmentRange{
              .name = segment.name, .rom_start = segment.rom_start, .rom_end = seg_end});
        }
      }
    }
  }

  // 4. Locate start of Asset Segments in built ROM
  uint64_t built_code_end = built_bytes.size();
  uint64_t first_asset_built_offset = built_bytes.size();

  if (!asset_segment_ranges.empty()) {
    const uint64_t first_retail_asset_start = asset_segment_ranges.front().rom_start;
    if (first_retail_asset_start < target_bytes.size()) {
      // Extract a 32-byte non-zero signature from the start of the first asset segment
      size_t signature_length =
          std::min<size_t>(32, target_bytes.size() - first_retail_asset_start);
      std::string_view asset_signature(
          reinterpret_cast<const char*>(target_bytes.data() + first_retail_asset_start),
          signature_length);

      std::string_view built_rom_view(reinterpret_cast<const char*>(built_bytes.data()),
                                      built_bytes.size());
      size_t found_position = built_rom_view.find(asset_signature, retail_code_start);
      if (found_position != std::string_view::npos) {
        first_asset_built_offset = found_position;
        built_code_end = found_position;
      }
    }
  }

  // 5. Code Segment Word-Level LCS
  CodeSegmentDiffResult code_diff;
  code_diff.segment_name = code_segment_name;
  code_diff.retail_rom_start = retail_code_start;
  code_diff.retail_rom_end = retail_code_end;
  code_diff.built_rom_start = retail_code_start;
  code_diff.built_rom_end = built_code_end;

  if (retail_code_end > retail_code_start && target_bytes.size() >= retail_code_end &&
      built_code_end > retail_code_start && built_bytes.size() >= built_code_end) {
    absl::Span<const uint8_t> target_code_bytes(target_bytes.data() + retail_code_start,
                                                retail_code_end - retail_code_start);
    absl::Span<const uint8_t> built_code_bytes(built_bytes.data() + retail_code_start,
                                               built_code_end - retail_code_start);

    std::vector<uint32_t> target_words = ExtractBigEndianWords(target_code_bytes);
    std::vector<uint32_t> built_words = ExtractBigEndianWords(built_code_bytes);

    code_diff.retail_words_count = target_words.size();
    code_diff.built_words_count = built_words.size();
    code_diff.matched_words_count = ComputeWordLcs(target_words, built_words);
    code_diff.target_only_words_count =
        code_diff.retail_words_count - code_diff.matched_words_count;
    code_diff.built_only_words_count = code_diff.built_words_count - code_diff.matched_words_count;
  }
  result.code_segments.push_back(code_diff);

  // 6. Asset Segments Shift-Aligned Matching
  size_t total_asset_matches = 0;
  size_t total_asset_bytes = 0;
  for (const auto& asset_range : asset_segment_ranges) {
    uint64_t retail_segment_start = asset_range.rom_start;
    uint64_t retail_segment_end = asset_range.rom_end;
    uint64_t segment_length = retail_segment_end - retail_segment_start;
    total_asset_bytes += segment_length;

    int64_t shift_offset = 0;
    if (first_asset_built_offset < built_bytes.size() && !asset_segment_ranges.empty()) {
      shift_offset = static_cast<int64_t>(first_asset_built_offset) -
                     static_cast<int64_t>(asset_segment_ranges.front().rom_start);
    }
    int64_t built_segment_start = static_cast<int64_t>(retail_segment_start) + shift_offset;

    AssetSegmentDiffResult asset_diff;
    asset_diff.segment_name = asset_range.name;
    asset_diff.retail_rom_start = retail_segment_start;
    asset_diff.retail_rom_end = retail_segment_end;
    asset_diff.built_rom_start = (built_segment_start >= 0) ? built_segment_start : 0;
    asset_diff.shift_offset = shift_offset;
    asset_diff.total_bytes = segment_length;

    size_t matching_bytes = 0;
    if (built_segment_start >= 0 &&
        static_cast<size_t>(built_segment_start + segment_length) <= built_bytes.size() &&
        retail_segment_end <= target_bytes.size()) {
      for (size_t offset = 0; offset < segment_length; ++offset) {
        if (target_bytes[retail_segment_start + offset] ==
            built_bytes[built_segment_start + offset]) {
          ++matching_bytes;
        }
      }
    }
    asset_diff.matching_bytes = matching_bytes;
    total_asset_matches += matching_bytes;
    result.asset_segments.push_back(asset_diff);
  }

  int64_t total_shift_offset = 0;
  if (first_asset_built_offset < built_bytes.size() && !asset_segment_ranges.empty()) {
    total_shift_offset = static_cast<int64_t>(first_asset_built_offset) -
                         static_cast<int64_t>(asset_segment_ranges.front().rom_start);
  }

  if (!result.structural.size_matches && options_.allow_code_shift &&
      !asset_segment_ranges.empty() && total_asset_bytes > 0 &&
      total_asset_matches == total_asset_bytes &&
      static_cast<int64_t>(built_bytes.size()) ==
          static_cast<int64_t>(target_bytes.size()) + total_shift_offset) {
    result.structural.code_shift_matches = true;
  }

  result.structural.exact_sha1_match =
      (result.structural.target_sha1 == result.structural.built_sha1 &&
       target_bytes == built_bytes);

  result.structural.is_valid =
      ((result.structural.size_matches || result.structural.code_shift_matches) &&
       result.structural.header_magic_matches && result.structural.entrypoint_matches);

  if (!result.structural.header_magic_matches) {
    result.structural.failure_reason = "N64 header magic mismatch";
  } else if (!result.structural.entrypoint_matches) {
    result.structural.failure_reason = "N64 entrypoint address mismatch";
  } else if (!result.structural.size_matches && !result.structural.code_shift_matches) {
    result.structural.failure_reason =
        absl::StrFormat("Size mismatch: target has %d bytes, built has %d bytes",
                        result.structural.target_size, result.structural.built_size);
  }

  // 7. Per-Function Structural Readiness Analysis
  if (target_bytes.size() >= 0x1050 && built_bytes.size() >= 0x1050) {
    const GameConfig* cfg_ptr = loaded_config.has_value() ? &(*loaded_config) : nullptr;
    SymbolIndex symbols;
    if (cfg_ptr != nullptr) {
      auto syms_or = LoadSymbolIndex({
          .config = cfg_ptr,
          .repo_root = FindRepoRoot(),
      });
      if (syms_or.ok()) {
        symbols = std::move(*syms_or);
      }
    }
    const SymbolIndex* syms_ptr = symbols.Empty() ? nullptr : &symbols;
    std::vector<ScannedRomFunction> base_funcs =
        ScanFunctionsFromRom(target_bytes, cfg_ptr, syms_ptr);

    std::vector<ScannedRomFunction> cand_funcs;
    if (!options_.candidate_rom_path.empty() && options_.candidate_rom_path.extension() == ".z64") {
      std::filesystem::path potential_elf = options_.candidate_rom_path;
      potential_elf.replace_extension(".elf");
      if (std::filesystem::exists(potential_elf, error_code)) {
        std::ifstream elf_stream(potential_elf, std::ios::binary);
        if (elf_stream.is_open()) {
          std::vector<uint8_t> elf_bytes((std::istreambuf_iterator<char>(elf_stream)),
                                         std::istreambuf_iterator<char>());
          auto elf_funcs = ScanFunctionsFromElfAndRom(built_bytes, elf_bytes, cfg_ptr);
          if (!elf_funcs.empty()) {
            absl::flat_hash_map<std::string, const ScannedRomFunction*> cand_map;
            for (const auto& fn : elf_funcs) {
              cand_map[fn.name] = &fn;
            }
            cand_funcs.reserve(base_funcs.size());
            for (const auto& base_fn : base_funcs) {
              auto it = cand_map.find(base_fn.name);
              if (it != cand_map.end()) {
                cand_funcs.push_back(*it->second);
              }
            }
          }
        }
      }
    }

    if (cand_funcs.size() != base_funcs.size()) {
      cand_funcs = ScanFunctionsFromRom(built_bytes, cfg_ptr, syms_ptr);
    }

    if (!base_funcs.empty() && cand_funcs.size() == base_funcs.size()) {
      result.readiness.total_functions = base_funcs.size();
      for (size_t i = 0; i < base_funcs.size(); ++i) {
        FunctionStructuralComparison comp =
            CompareFunctionStructure(base_funcs[i].raw_words, base_funcs[i].vram,
                                     cand_funcs[i].raw_words, cand_funcs[i].vram);
        if (comp.frame_size_matches) {
          ++result.readiness.frame_size_matches;
        }
        if (comp.saved_registers_match) {
          ++result.readiness.saved_registers_matches;
        }
        if (comp.cfg_topology_matches) {
          ++result.readiness.cfg_topology_matches;
        }
        if (comp.IsStructurallyIsomorphic()) {
          ++result.readiness.structurally_isomorphic_functions;
        }
        result.readiness.total_target_instructions += comp.target.instruction_count;
        result.readiness.total_normalized_lcs_instructions += comp.normalized_lcs_instructions;
      }
    }
  }

  // 8. Aggregate Total Logical Matches
  size_t total_code_matched_bytes = code_diff.matched_words_count * 4;
  result.total_logical_matching_bytes =
      result.header_matching_bytes + total_code_matched_bytes + total_asset_matches;
  if (result.total_rom_bytes > 0) {
    result.overall_match_percentage = (static_cast<double>(result.total_logical_matching_bytes) /
                                       static_cast<double>(result.total_rom_bytes)) *
                                      100.0;
  }
  result.success =
      options_.require_bit_exact ? result.structural.exact_sha1_match : result.structural.is_valid;

  // 9. Generate Formatted Output Report
  DiffFormatter formatter(DiffFormatterOptions{.use_color = options_.use_color});
  result.formatted_report = formatter.FormatRomDiff(result);
  return result;
}

}  // namespace rom_nom_nom
