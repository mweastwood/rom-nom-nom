#include "fuzzer/relocation_rebaser.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/endian.h"
#include "core/game_config.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom::fuzzer {
namespace {

constexpr uint32_t kShtProgbits = 1;
constexpr uint32_t kShtRela = 4;
constexpr uint32_t kShtNobits = 8;
constexpr uint32_t kShtRel = 9;
constexpr uint32_t kShfAlloc = 0x2;
constexpr uint8_t kSttSection = 3;

constexpr uint32_t kRMipsNone = 0;
constexpr uint32_t kRMips32 = 2;
constexpr uint32_t kRMips26 = 4;
constexpr uint32_t kRMipsHi16 = 5;
constexpr uint32_t kRMipsLo16 = 6;
constexpr uint32_t kRMipsPc16 = 10;

constexpr size_t kMaxWarnings = 16;
// Largest function the code scratch window can host.
constexpr uint32_t kCodeScratchSize = 0x10000;

struct Section {
  std::string name;
  uint32_t type = 0;
  uint32_t flags = 0;
  uint32_t addr = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t link = 0;
  uint32_t info = 0;
};

struct Symbol {
  std::string name;
  uint32_t value = 0;
  uint32_t size = 0;
  uint8_t type = 0;
  uint16_t shndx = 0;
};

struct Relocation {
  uint32_t offset = 0;
  uint32_t sym = 0;
  uint32_t type = 0;
};

struct Range {
  uint32_t start = 0;
  uint32_t end = 0;  // Exclusive.
};

bool Overlaps(const Range& a, const Range& b) {
  return a.start < b.end && b.start < a.end;
}

// Minimal big-endian ELF32 view over a fully linked executable.
class LinkedElf {
 public:
  static absl::StatusOr<LinkedElf> Parse(absl::Span<const uint8_t> bytes) {
    LinkedElf elf;
    elf.bytes_ = bytes;
    if (bytes.size() < 52 || bytes[0] != 0x7F || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F') {
      return absl::InvalidArgumentError("candidate is not an ELF file");
    }
    if (bytes[4] != 1 || bytes[5] != 2) {
      return absl::InvalidArgumentError("candidate ELF is not big-endian ELF32");
    }
    uint32_t shoff = ReadBigEndian32(bytes.data() + 32);
    uint16_t shentsize = ReadBigEndian16(bytes.data() + 46);
    uint16_t shnum = ReadBigEndian16(bytes.data() + 48);
    uint16_t shstrndx = ReadBigEndian16(bytes.data() + 50);
    if (shoff == 0 || shentsize < 40 ||
        static_cast<size_t>(shoff) + static_cast<size_t>(shnum) * shentsize > bytes.size()) {
      return absl::InvalidArgumentError("candidate ELF has invalid section headers");
    }
    std::vector<uint32_t> name_offsets;
    for (uint16_t i = 0; i < shnum; ++i) {
      const uint8_t* h = bytes.data() + shoff + static_cast<size_t>(i) * shentsize;
      Section s;
      name_offsets.push_back(ReadBigEndian32(h));
      s.type = ReadBigEndian32(h + 4);
      s.flags = ReadBigEndian32(h + 8);
      s.addr = ReadBigEndian32(h + 12);
      s.offset = ReadBigEndian32(h + 16);
      s.size = ReadBigEndian32(h + 20);
      s.link = ReadBigEndian32(h + 24);
      s.info = ReadBigEndian32(h + 28);
      if (s.type != kShtNobits && static_cast<size_t>(s.offset) + s.size > bytes.size()) {
        return absl::InvalidArgumentError("candidate ELF section exceeds file size");
      }
      elf.sections_.push_back(std::move(s));
    }
    if (shstrndx < elf.sections_.size()) {
      const Section& strs = elf.sections_[shstrndx];
      for (size_t i = 0; i < elf.sections_.size(); ++i) {
        elf.sections_[i].name = elf.ReadString(strs, name_offsets[i]);
      }
    }

    for (const Section& s : elf.sections_) {
      if (s.type != 2 /* SHT_SYMTAB */) continue;
      if (s.link >= elf.sections_.size()) break;
      const Section& strtab = elf.sections_[s.link];
      for (uint32_t off = 0; off + 16 <= s.size; off += 16) {
        const uint8_t* e = bytes.data() + s.offset + off;
        Symbol sym;
        sym.name = elf.ReadString(strtab, ReadBigEndian32(e));
        sym.value = ReadBigEndian32(e + 4);
        sym.size = ReadBigEndian32(e + 8);
        sym.type = e[12] & 0xF;
        sym.shndx = ReadBigEndian16(e + 14);
        elf.symbols_.push_back(std::move(sym));
      }
      break;
    }
    return elf;
  }

  const std::vector<Section>& Sections() const { return sections_; }
  const std::vector<Symbol>& Symbols() const { return symbols_; }

  std::vector<Relocation> ReadRelocations(const Section& rel) const {
    std::vector<Relocation> relocs;
    for (uint32_t off = 0; off + 8 <= rel.size; off += 8) {
      const uint8_t* e = bytes_.data() + rel.offset + off;
      uint32_t info = ReadBigEndian32(e + 4);
      relocs.push_back({ReadBigEndian32(e), info >> 8, info & 0xFF});
    }
    return relocs;
  }

  // Reads the loaded word at `vaddr`, if a PROGBITS ALLOC section covers it.
  std::optional<uint32_t> ReadWord(uint32_t vaddr) const {
    for (const Section& s : sections_) {
      if (s.type != kShtProgbits || (s.flags & kShfAlloc) == 0) continue;
      if (vaddr >= s.addr &&
          static_cast<uint64_t>(vaddr) + 4 <= static_cast<uint64_t>(s.addr) + s.size) {
        return ReadBigEndian32(bytes_.data() + s.offset + (vaddr - s.addr));
      }
    }
    return std::nullopt;
  }

  // Reads `size` loaded bytes starting at `vaddr`; bytes not backed by PROGBITS read as zero.
  std::vector<uint8_t> ReadRange(uint32_t vaddr, uint32_t size) const {
    std::vector<uint8_t> out(size, 0);
    for (const Section& s : sections_) {
      if (s.type != kShtProgbits || (s.flags & kShfAlloc) == 0 || s.size == 0) continue;
      uint64_t lo = std::max<uint64_t>(vaddr, s.addr);
      uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(vaddr) + size,
                                       static_cast<uint64_t>(s.addr) + s.size);
      for (uint64_t a = lo; a < hi; ++a) {
        out[a - vaddr] = bytes_[s.offset + (a - s.addr)];
      }
    }
    return out;
  }

 private:
  std::string ReadString(const Section& strtab, uint32_t off) const {
    if (off >= strtab.size) return "";
    const char* begin = reinterpret_cast<const char*>(bytes_.data() + strtab.offset + off);
    size_t max_len = strtab.size - off;
    size_t len = 0;
    while (len < max_len && begin[len] != '\0') ++len;
    return std::string(begin, len);
  }

  absl::Span<const uint8_t> bytes_;
  std::vector<Section> sections_;
  std::vector<Symbol> symbols_;
};

// Returns `ranges` minus every range in `holes`.
std::vector<Range> SubtractRanges(const std::vector<Range>& ranges,
                                  const std::vector<Range>& holes) {
  std::vector<Range> result = ranges;
  for (const Range& hole : holes) {
    std::vector<Range> next;
    for (const Range& r : result) {
      if (!Overlaps(r, hole)) {
        next.push_back(r);
        continue;
      }
      if (r.start < hole.start) next.push_back({r.start, hole.start});
      if (hole.end < r.end) next.push_back({hole.end, r.end});
    }
    result = std::move(next);
  }
  return result;
}

std::optional<uint32_t> TargetAddressForName(std::string_view name,
                                             const SymbolIndex* symbol_index) {
  if (absl::StartsWith(name, "0x") || absl::StartsWith(name, "0X") ||
      absl::StartsWith(name, ".L")) {
    return std::nullopt;
  }
  return ResolveSymbolOrSynthesizedAddress(name, symbol_index);
}

}  // namespace

std::vector<DataBlob> CollectDataBlobs(const GameConfig* config, const SymbolIndex* symbols) {
  std::vector<DataBlob> blobs;
  if (config == nullptr) return blobs;
  auto label_for_vram = [&](uint32_t vram) {
    if (symbols != nullptr) {
      const auto* sym = symbols->FindByAddress(vram);
      if (sym != nullptr) return std::string(sym->name());
    }
    return absl::StrFormat("D_%08X", vram);
  };
  for (const auto& seg : ResolveSegments(*config)) {
    if (seg.type != SEGMENT_CODE) continue;
    for (const auto& sub : seg.subsegments) {
      if (sub.type == SUBSEGMENT_DATA || sub.type == SUBSEGMENT_RODATA) {
        if (sub.rom_end <= sub.rom_start) continue;
        blobs.push_back({sub.vram_start, sub.RomSize(), label_for_vram(sub.vram_start), false});
      } else if (sub.type == SUBSEGMENT_BSS && sub.vram_start != 0 && sub.bss_size != 0) {
        blobs.push_back({sub.vram_start, sub.bss_size, label_for_vram(sub.vram_start), true});
      }
    }
  }
  return blobs;
}

std::vector<uint32_t> CandidateRebase::RebaseFunctionWords(uint32_t vram,
                                                           absl::Span<const uint32_t> words) const {
  std::vector<uint32_t> out(words.begin(), words.end());
  for (size_t i = 0; i < out.size(); ++i) {
    auto it = patched_words.find(vram + static_cast<uint32_t>(i * 4));
    if (it != patched_words.end()) out[i] = it->second;
  }
  return out;
}

CandidateRebase::PlacedFunction CandidateRebase::PlaceFunction(
    uint32_t vram, absl::Span<const uint32_t> words) const {
  PlacedFunction placed;
  placed.vram = vram;
  placed.words = RebaseFunctionWords(vram, words);
  uint64_t size = static_cast<uint64_t>(words.size()) * 4;
  auto overlaps_data = [&](uint64_t start) {
    return std::any_of(occupied_ranges.begin(), occupied_ranges.end(),
                       [&](const auto& r) { return start < r.second && r.first < start + size; });
  };
  if (code_scratch_base == 0 || size == 0 || size > kCodeScratchSize || !overlaps_data(vram)) {
    return placed;
  }
  uint32_t run_vram = code_scratch_base + (vram & 0xF);
  if (overlaps_data(run_vram) || ((run_vram ^ vram) & 0xF0000000u) != 0) return placed;

  uint32_t delta = run_vram - vram;
  uint32_t end = vram + static_cast<uint32_t>(size);
  for (size_t i = 0; i < placed.words.size(); ++i) {
    uint32_t site = vram + static_cast<uint32_t>(i * 4);
    if (!jump26_sites.contains(site)) continue;
    uint32_t word = placed.words[i];
    uint32_t dest = ((site + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
    if (dest >= vram && dest < end) {
      placed.words[i] = (word & 0xFC000000u) | (((dest + delta) >> 2) & 0x03FFFFFFu);
    }
  }
  for (const auto& [addr, value] : private_abs32) {
    if (value >= vram && value < end) {
      MemoryPayload overlay;
      overlay.address = addr;
      overlay.data.resize(4);
      WriteBigEndian32(overlay.data.data(), value + delta);
      placed.overlays.push_back(std::move(overlay));
    }
  }
  placed.vram = run_vram;
  return placed;
}

uint32_t CandidateRebase::MapAddress(uint32_t cand_addr) const {
  for (const auto& r : moved_ranges) {
    if (cand_addr >= r.cand_start && cand_addr - r.cand_start < r.size) {
      return static_cast<uint32_t>(cand_addr + r.shift);
    }
  }
  return cand_addr;
}

absl::StatusOr<CandidateRebase> BuildCandidateRebase(absl::Span<const uint8_t> candidate_elf,
                                                     absl::Span<const DataBlob> blobs,
                                                     const GameConfig* config,
                                                     const SymbolIndex* symbols,
                                                     const RebaseOptions& options) {
  auto elf_or = LinkedElf::Parse(candidate_elf);
  if (!elf_or.ok()) return elf_or.status();
  const LinkedElf& elf = *elf_or;

  CandidateRebase rebase;
  auto warn = [&](std::string message) {
    if (rebase.warnings.size() < kMaxWarnings) rebase.warnings.push_back(std::move(message));
  };

  std::vector<const Section*> rel_sections;
  for (const Section& s : elf.Sections()) {
    if (s.type == kShtRela) {
      return absl::UnimplementedError("candidate ELF uses RELA relocations (unsupported)");
    }
    if (s.type == kShtRel && s.info < elf.Sections().size() &&
        (elf.Sections()[s.info].flags & kShfAlloc) != 0) {
      rel_sections.push_back(&s);
    }
  }
  if (rel_sections.empty()) {
    return absl::FailedPreconditionError(
        "candidate ELF has no relocations; link it with --emit-relocs to enable rebasing");
  }

  absl::flat_hash_map<std::string_view, const Symbol*> defined_by_name;
  for (const Symbol& sym : elf.Symbols()) {
    if (sym.shndx != 0 && !sym.name.empty()) defined_by_name.emplace(sym.name, &sym);
  }

  // 1. Locate every blob in the candidate layout.
  struct PlacedBlob {
    const DataBlob* blob;
    uint32_t cand_start;
  };
  std::vector<PlacedBlob> placed;
  std::vector<Range> blob_cand_ranges;
  std::vector<Range> target_blob_ranges;
  for (const DataBlob& blob : blobs) {
    target_blob_ranges.push_back({blob.target_start, blob.target_start + blob.size});
    auto it = defined_by_name.find(blob.label);
    if (it == defined_by_name.end()) {
      warn(absl::StrFormat("blob label %s (target 0x%08X) not found in candidate ELF", blob.label,
                           blob.target_start));
      continue;
    }
    uint32_t cand_start = it->second->value;
    placed.push_back({&blob, cand_start});
    blob_cand_ranges.push_back({cand_start, cand_start + blob.size});
    rebase.moved_ranges.push_back(
        {cand_start, blob.size,
         static_cast<int64_t>(blob.target_start) - static_cast<int64_t>(cand_start)});
  }
  if (placed.empty()) {
    return absl::NotFoundError("no data blob labels found in candidate ELF");
  }

  // 2. Verify each registered data symbol keeps its original offset inside its blob.
  if (symbols != nullptr) {
    for (const Symbol& sym : elf.Symbols()) {
      if (sym.shndx == 0 || sym.name.empty()) continue;
      const auto* entry = symbols->FindByName(sym.name);
      if (entry == nullptr || entry->type() != SYMBOL_DATA) continue;
      for (const PlacedBlob& pb : placed) {
        uint32_t t = entry->address();
        if (t < pb.blob->target_start || t - pb.blob->target_start >= pb.blob->size) continue;
        uint32_t expected = pb.cand_start + (t - pb.blob->target_start);
        if (sym.value != expected) {
          ++rebase.layout_mismatches;
          warn(absl::StrFormat("data symbol %s is at 0x%08X in candidate, expected 0x%08X",
                               sym.name, sym.value, expected));
        }
        break;
      }
    }
  }

  // 3. Find candidate-private loaded data: every code segment's data/rodata/BSS range that is
  // not one of the blobs (e.g. compiler-generated rodata such as jump tables and literals).
  absl::flat_hash_set<std::string> code_segments;
  if (config != nullptr) {
    for (const auto& seg : config->segments()) {
      if (seg.type() == SEGMENT_CODE) code_segments.insert(std::string(seg.name()));
    }
  }
  std::vector<Range> content_ranges;
  uint32_t highest_end = 0;
  for (const Range& r : target_blob_ranges) highest_end = std::max(highest_end, r.end);
  for (const char* kind : {"_DATA", "_RODATA", "_BSS"}) {
    for (const std::string& seg : code_segments) {
      auto start_it = defined_by_name.find(absl::StrCat(seg, kind, "_START"));
      auto end_it = defined_by_name.find(absl::StrCat(seg, kind, "_END"));
      if (start_it == defined_by_name.end() || end_it == defined_by_name.end()) continue;
      uint32_t start = start_it->second->value;
      uint32_t end = end_it->second->value;
      if (start >= end) continue;
      content_ranges.push_back({start, end});
      highest_end = std::max(highest_end, end);
    }
  }
  for (const Range& r : blob_cand_ranges) highest_end = std::max(highest_end, r.end);
  std::vector<Range> private_ranges = SubtractRanges(content_ranges, blob_cand_ranges);
  std::sort(private_ranges.begin(), private_ranges.end(),
            [](const Range& a, const Range& b) { return a.start < b.start; });

  // 4. Relocate private ranges that collide with target blobs or reserved ranges into scratch
  // memory above every loaded range, preserving 16-byte alignment.
  std::vector<Range> avoid = target_blob_ranges;
  for (const auto& [lo, hi] : options.reserved_ranges) avoid.push_back({lo, hi});
  if (options.code_scratch_base != 0) {
    uint32_t base = options.code_scratch_base & ~0xFu;
    avoid.push_back({base, base + kCodeScratchSize});
  }
  uint32_t scratch = (std::max(highest_end, options.scratch_base) + 0xF) & ~0xFu;
  struct PlacedPrivate {
    Range cand;
    uint32_t final_start;
  };
  std::vector<PlacedPrivate> privates;
  for (const Range& r : private_ranges) {
    bool collides =
        options.scratch_base != 0 ||
        std::any_of(avoid.begin(), avoid.end(), [&](const Range& a) { return Overlaps(r, a); });
    uint32_t final_start = r.start;
    if (collides) {
      uint32_t size = r.end - r.start;
      Range dest{scratch + (r.start & 0xF), scratch + (r.start & 0xF) + size};
      for (bool moved = true; moved;) {
        moved = false;
        for (const Range& a : avoid) {
          if (Overlaps(dest, a)) {
            uint32_t base = (a.end + 0xF) & ~0xFu;
            dest = {base + (r.start & 0xF), base + (r.start & 0xF) + size};
            moved = true;
          }
        }
      }
      if (dest.end > options.ram_end || dest.end < dest.start) {
        return absl::ResourceExhaustedError(absl::StrFormat(
            "no scratch memory for candidate-private range 0x%08X-0x%08X", r.start, r.end));
      }
      final_start = dest.start;
      scratch = (dest.end + 0xF) & ~0xFu;
      avoid.push_back(dest);
      rebase.moved_ranges.push_back(
          {r.start, size, static_cast<int64_t>(final_start) - static_cast<int64_t>(r.start)});
    }
    privates.push_back({r, final_start});
  }
  rebase.code_scratch_base = options.code_scratch_base & ~0xFu;
  for (const Range& r : target_blob_ranges) rebase.occupied_ranges.emplace_back(r.start, r.end);
  for (const PlacedPrivate& p : privates) {
    rebase.occupied_ranges.emplace_back(p.final_start, p.final_start + (p.cand.end - p.cand.start));
  }
  if (rebase.code_scratch_base != 0) {
    Range code_window{rebase.code_scratch_base, rebase.code_scratch_base + kCodeScratchSize};
    for (const auto& [lo, hi] : rebase.occupied_ranges) {
      if (Overlaps(code_window, {lo, hi})) {
        return absl::InvalidArgumentError(
            absl::StrFormat("code scratch window 0x%08X overlaps rebased data at 0x%08X-0x%08X",
                            code_window.start, lo, hi));
      }
    }
  }

  auto shift_for = [&](uint32_t addr) -> std::optional<int64_t> {
    for (const auto& m : rebase.moved_ranges) {
      if (addr >= m.cand_start && addr - m.cand_start < m.size) return m.shift;
    }
    return std::nullopt;
  };
  auto in_blob = [&](uint32_t addr) {
    return std::any_of(blob_cand_ranges.begin(), blob_cand_ranges.end(),
                       [&](const Range& r) { return addr >= r.start && addr < r.end; });
  };
  auto in_content = [&](uint32_t addr) {
    return std::any_of(content_ranges.begin(), content_ranges.end(),
                       [&](const Range& r) { return addr >= r.start && addr < r.end; });
  };
  auto is_function_symbol = [&](const Symbol& sym) {
    if ((sym.type == 2 /* STT_FUNC */) || absl::StartsWith(sym.name, "func_")) {
      return true;
    }
    if (symbols != nullptr) {
      const auto* entry = symbols->FindByName(sym.name);
      if (entry != nullptr && entry->type() == SYMBOL_FUNC) {
        return true;
      }
    }
    return false;
  };
  struct ShiftedFuncRange {
    uint32_t cand_start = 0;
    uint32_t cand_end = 0;
    int64_t shift = 0;
  };
  std::vector<ShiftedFuncRange> shifted_funcs;
  for (const Symbol& s : elf.Symbols()) {
    if (s.shndx == 0 || s.type == kSttSection || s.name.empty()) continue;
    if (!is_function_symbol(s)) continue;
    std::optional<uint32_t> target = TargetAddressForName(s.name, symbols);
    if (!target.has_value() || *target == s.value) continue;
    if (in_blob(s.value) || in_content(s.value)) continue;
    uint32_t end = s.value + (s.size > 0 ? s.size : 64u);
    shifted_funcs.push_back(
        {s.value, end, static_cast<int64_t>(*target) - static_cast<int64_t>(s.value)});
  }
  std::sort(shifted_funcs.begin(), shifted_funcs.end(),
            [](const ShiftedFuncRange& a, const ShiftedFuncRange& b) {
              return a.cand_start < b.cand_start;
            });
  for (size_t i = 0; i + 1 < shifted_funcs.size(); ++i) {
    if (shifted_funcs[i].cand_end > shifted_funcs[i + 1].cand_start) {
      shifted_funcs[i].cand_end = shifted_funcs[i + 1].cand_start;
    }
  }
  auto func_shift_for = [&](uint32_t addr) -> std::optional<int64_t> {
    if (in_blob(addr) || in_content(addr)) return std::nullopt;
    for (const auto& f : shifted_funcs) {
      if (addr >= f.cand_start && addr < f.cand_end) return f.shift;
    }
    return std::nullopt;
  };
  // A named symbol that stands for a known target address (registered, or an auto-generated
  // D_/func_ label) moves exactly when that target address lies inside a blob. This keeps
  // absolute symbols pinned outside every blob (e.g. memory past BSS) in place even when their
  // value happens to fall inside a moved candidate range.
  auto symbol_shift = [&](const Symbol& sym) -> std::optional<int64_t> {
    std::optional<uint32_t> target = TargetAddressForName(sym.name, symbols);
    if (!target.has_value()) return shift_for(sym.value);
    for (const PlacedBlob& pb : placed) {
      if (*target >= pb.blob->target_start && *target - pb.blob->target_start < pb.blob->size) {
        return static_cast<int64_t>(pb.blob->target_start) - static_cast<int64_t>(pb.cand_start);
      }
    }
    if (is_function_symbol(sym) && sym.value != *target) {
      return static_cast<int64_t>(*target) - static_cast<int64_t>(sym.value);
    }
    return std::nullopt;
  };

  // 5. Apply relocations. Named symbols are classified by their own address (so `sym + addend`
  // follows `sym`); section symbols are classified by the resolved target address.
  const auto& syms = elf.Symbols();
  absl::flat_hash_set<uint32_t> warned_types;
  std::vector<uint32_t> abs32_sites;
  for (const Section* rel_section : rel_sections) {
    std::vector<Relocation> relocs = elf.ReadRelocations(*rel_section);
    absl::flat_hash_map<uint32_t, std::vector<uint32_t>> pending_hi;  // sym -> HI16 offsets
    absl::flat_hash_map<uint32_t, uint32_t> last_hi_imm;              // sym -> HI16 immediate
    for (const Relocation& rel : relocs) {
      if (rel.sym >= syms.size()) continue;
      const Symbol& sym = syms[rel.sym];
      bool is_section = sym.type == kSttSection;
      auto classify = [&](uint32_t resolved) {
        return is_section ? shift_for(resolved) : symbol_shift(sym);
      };
      auto classify_hi_lo = [&](uint32_t resolved) -> std::optional<int64_t> {
        if (!is_section) return symbol_shift(sym);
        if (auto s = shift_for(resolved); s.has_value()) return s;
        return func_shift_for(resolved);
      };
      // Bytes inside blobs come from the target ROM, so relocations located there are moot.
      if (in_blob(rel.offset)) continue;

      switch (rel.type) {
        case kRMipsNone:
        case kRMipsPc16:
          break;
        case kRMips26:
          rebase.jump26_sites.insert(rel.offset);
          break;
        case kRMips32: {
          auto word = elf.ReadWord(rel.offset);
          if (!word.has_value()) break;
          abs32_sites.push_back(rel.offset);
          auto shift = classify(*word);
          if (shift.has_value()) {
            rebase.patched_words[rel.offset] = static_cast<uint32_t>(*word + *shift);
            ++rebase.relocations_patched;
          }
          break;
        }
        case kRMipsHi16:
          pending_hi[rel.sym].push_back(rel.offset);
          break;
        case kRMipsLo16: {
          auto lo_word = elf.ReadWord(rel.offset);
          if (!lo_word.has_value()) break;
          int32_t lo_imm = static_cast<int16_t>(*lo_word & 0xFFFF);
          auto pending_it = pending_hi.find(rel.sym);
          if (pending_it != pending_hi.end()) {
            for (uint32_t hi_offset : pending_it->second) {
              auto hi_word = elf.ReadWord(hi_offset);
              if (!hi_word.has_value()) continue;
              uint32_t hi_imm = *hi_word & 0xFFFF;
              last_hi_imm[rel.sym] = hi_imm;
              uint32_t resolved = (hi_imm << 16) + static_cast<uint32_t>(lo_imm);
              auto shift = classify_hi_lo(resolved);
              if (!shift.has_value()) continue;
              uint32_t moved = static_cast<uint32_t>(resolved + *shift);
              uint32_t new_hi = ((moved + 0x8000) >> 16) & 0xFFFF;
              rebase.patched_words[hi_offset] = (*hi_word & 0xFFFF0000u) | new_hi;
              ++rebase.relocations_patched;
            }
            pending_hi.erase(pending_it);
          }
          std::optional<int64_t> shift;
          auto hi_it = last_hi_imm.find(rel.sym);
          if (hi_it != last_hi_imm.end()) {
            shift = classify_hi_lo((hi_it->second << 16) + static_cast<uint32_t>(lo_imm));
          } else if (!is_section) {
            shift = symbol_shift(sym);
          } else {
            ++rebase.unsupported_relocations;
            warn(absl::StrFormat("unpaired R_MIPS_LO16 at 0x%08X against a section symbol",
                                 rel.offset));
          }
          if (shift.has_value()) {
            uint32_t new_lo = static_cast<uint32_t>(lo_imm + *shift) & 0xFFFF;
            rebase.patched_words[rel.offset] = (*lo_word & 0xFFFF0000u) | new_lo;
            ++rebase.relocations_patched;
          }
          break;
        }
        default:
          if (is_section || symbol_shift(sym).has_value()) {
            ++rebase.unsupported_relocations;
            if (warned_types.insert(rel.type).second) {
              warn(absl::StrFormat("unsupported relocation type %u at 0x%08X", rel.type,
                                   rel.offset));
            }
          }
          break;
      }
    }
    for (const auto& [sym_index, offsets] : pending_hi) {
      if (syms[sym_index].type == kSttSection || symbol_shift(syms[sym_index]).has_value()) {
        rebase.unsupported_relocations += offsets.size();
        warn(absl::StrFormat("%zu R_MIPS_HI16 relocation(s) without a matching R_MIPS_LO16",
                             offsets.size()));
      }
    }
  }

  // 6. Materialize candidate-private payloads at their final addresses with patches applied.
  for (const PlacedPrivate& p : privates) {
    MemoryPayload payload;
    payload.address = p.final_start;
    payload.data = elf.ReadRange(p.cand.start, p.cand.end - p.cand.start);
    for (uint32_t a = (p.cand.start + 3) & ~3u; a + 4 <= p.cand.end; a += 4) {
      auto it = rebase.patched_words.find(a);
      if (it != rebase.patched_words.end()) {
        WriteBigEndian32(payload.data.data() + (a - p.cand.start), it->second);
      }
    }
    rebase.private_payloads.push_back(std::move(payload));
  }
  for (uint32_t site : abs32_sites) {
    for (const PlacedPrivate& p : privates) {
      if (site < p.cand.start || site + 4 > p.cand.end) continue;
      auto it = rebase.patched_words.find(site);
      uint32_t value = it != rebase.patched_words.end() ? it->second : *elf.ReadWord(site);
      rebase.private_abs32.emplace_back(p.final_start + (site - p.cand.start), value);
      break;
    }
  }
  return rebase;
}

}  // namespace rom_nom_nom::fuzzer
