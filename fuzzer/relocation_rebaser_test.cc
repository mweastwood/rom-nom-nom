#include "fuzzer/relocation_rebaser.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/endian.h"
#include "core/game_config.h"
#include "core/symbol_registry.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::fuzzer {
namespace {

constexpr uint32_t kRMips32 = 2;
constexpr uint32_t kRMips26 = 4;
constexpr uint32_t kRMipsHi16 = 5;
constexpr uint32_t kRMipsLo16 = 6;

struct TestSymbol {
  std::string name;
  uint32_t value = 0;
  uint8_t type = 0;    // STT_NOTYPE unless set (3 = STT_SECTION).
  uint16_t shndx = 1;  // Defined in .main by default.
};

struct TestReloc {
  uint32_t offset = 0;
  uint32_t sym = 0;  // Index into the symbol list (index 0 is the null symbol).
  uint32_t type = 0;
};

void Put16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v));
}

void Put32(std::vector<uint8_t>* out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out->push_back(static_cast<uint8_t>(v >> shift));
}

// Builds a minimal linked big-endian MIPS ELF32 with sections:
// [0] null, [1] .main (PROGBITS), [2] .main_bss (NOBITS), [3] .rel.main, [4] .symtab,
// [5] .strtab, [6] .shstrtab.
std::vector<uint8_t> BuildElf(uint32_t main_addr, const std::vector<uint32_t>& main_words,
                              uint32_t bss_addr, uint32_t bss_size,
                              const std::vector<TestSymbol>& symbols,
                              const std::vector<TestReloc>& relocs) {
  std::vector<uint8_t> main_data;
  for (uint32_t w : main_words) Put32(&main_data, w);

  std::vector<uint8_t> strtab = {0};
  std::vector<uint8_t> symtab(16, 0);  // Null symbol.
  for (const auto& sym : symbols) {
    uint32_t name_off = static_cast<uint32_t>(strtab.size());
    strtab.insert(strtab.end(), sym.name.begin(), sym.name.end());
    strtab.push_back(0);
    Put32(&symtab, name_off);
    Put32(&symtab, sym.value);
    Put32(&symtab, 0);
    symtab.push_back(static_cast<uint8_t>(0x10 | sym.type));  // STB_GLOBAL.
    symtab.push_back(0);
    Put16(&symtab, sym.shndx);
  }

  std::vector<uint8_t> rel;
  for (const auto& r : relocs) {
    Put32(&rel, r.offset);
    Put32(&rel, (r.sym << 8) | r.type);
  }

  std::vector<std::string> names = {"",        ".main",   ".main_bss", ".rel.main",
                                    ".symtab", ".strtab", ".shstrtab"};
  std::vector<uint8_t> shstrtab;
  std::vector<uint32_t> name_offsets;
  for (const auto& n : names) {
    name_offsets.push_back(static_cast<uint32_t>(shstrtab.size()));
    shstrtab.insert(shstrtab.end(), n.begin(), n.end());
    shstrtab.push_back(0);
  }

  std::vector<uint8_t> elf(52, 0);
  auto append = [&](const std::vector<uint8_t>& data) {
    while (elf.size() % 4 != 0) elf.push_back(0);
    uint32_t off = static_cast<uint32_t>(elf.size());
    elf.insert(elf.end(), data.begin(), data.end());
    return off;
  };
  uint32_t main_off = append(main_data);
  uint32_t rel_off = append(rel);
  uint32_t symtab_off = append(symtab);
  uint32_t strtab_off = append(strtab);
  uint32_t shstrtab_off = append(shstrtab);
  while (elf.size() % 4 != 0) elf.push_back(0);
  uint32_t shoff = static_cast<uint32_t>(elf.size());

  struct Header {
    uint32_t type, flags, addr, offset, size, link, info, entsize;
  };
  std::vector<Header> headers = {
      {0, 0, 0, 0, 0, 0, 0, 0},
      {1, 0x7, main_addr, main_off, static_cast<uint32_t>(main_data.size()), 0, 0, 0},
      {8, 0x3, bss_addr, main_off, bss_size, 0, 0, 0},
      {9, 0x40, 0, rel_off, static_cast<uint32_t>(rel.size()), 4, 1, 8},
      {2, 0, 0, symtab_off, static_cast<uint32_t>(symtab.size()), 5, 1, 16},
      {3, 0, 0, strtab_off, static_cast<uint32_t>(strtab.size()), 0, 0, 0},
      {3, 0, 0, shstrtab_off, static_cast<uint32_t>(shstrtab.size()), 0, 0, 0},
  };
  for (size_t i = 0; i < headers.size(); ++i) {
    const Header& h = headers[i];
    Put32(&elf, name_offsets[i]);
    Put32(&elf, h.type);
    Put32(&elf, h.flags);
    Put32(&elf, h.addr);
    Put32(&elf, h.offset);
    Put32(&elf, h.size);
    Put32(&elf, h.link);
    Put32(&elf, h.info);
    Put32(&elf, 4);
    Put32(&elf, h.entsize);
  }

  elf[0] = 0x7F;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = 1;  // ELFCLASS32
  elf[5] = 2;  // ELFDATA2MSB
  elf[6] = 1;
  WriteBigEndian16(elf.data() + 16, 2);  // ET_EXEC
  WriteBigEndian16(elf.data() + 18, 8);  // EM_MIPS
  WriteBigEndian32(elf.data() + 32, shoff);
  WriteBigEndian16(elf.data() + 40, 52);
  WriteBigEndian16(elf.data() + 46, 40);
  WriteBigEndian16(elf.data() + 48, static_cast<uint16_t>(headers.size()));
  WriteBigEndian16(elf.data() + 50, 6);
  return elf;
}

constexpr char kConfig[] = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "main"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x2040
  vram: 0x80001000
  subsegments { name: "code" type: SUBSEGMENT_ASM rom_start: 0x1000 }
  subsegments { name: "data" type: SUBSEGMENT_DATA rom_start: 0x2000 }
  subsegments { name: "bss" type: SUBSEGMENT_BSS vram: 0x80003000 }
  bss_size: 0x100
}
)";

constexpr char kSymbols[] = R"(
entries { name: "Func" address: 0x80001000 type: SYMBOL_FUNC }
entries { name: "g_table" address: 0x80002000 type: SYMBOL_DATA }
entries { name: "g_counter" address: 0x80002010 type: SYMBOL_DATA }
entries { name: "g_bss" address: 0x80003000 type: SYMBOL_DATA }
entries { name: "g_bss_value" address: 0x80003010 type: SYMBOL_DATA }
)";

TEST(RelocationRebaserTest, CollectDataBlobsFindsDataAndBssSubsegments) {
  auto cfg = ParseGameConfig(kConfig);
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  auto syms = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(syms.ok()) << syms.status();

  auto blobs = CollectDataBlobs(&*cfg, &*syms);
  ASSERT_EQ(blobs.size(), 2u);
  EXPECT_EQ(blobs[0].label, "g_table");
  EXPECT_EQ(blobs[0].target_start, 0x80002000u);
  EXPECT_EQ(blobs[0].size, 0x40u);
  EXPECT_FALSE(blobs[0].is_bss);
  EXPECT_EQ(blobs[1].label, "g_bss");
  EXPECT_EQ(blobs[1].target_start, 0x80003000u);
  EXPECT_EQ(blobs[1].size, 0x100u);
  EXPECT_TRUE(blobs[1].is_bss);
}

TEST(RelocationRebaserTest, RebasesBlobReferencesAndRelocatesCollidingPrivateRodata) {
  auto cfg = ParseGameConfig(kConfig);
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  auto syms = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(syms.ok()) << syms.status();

  // Candidate layout: code grew so that compiler rodata now occupies the target's data address.
  //   code    0x80001000-0x80002000
  //   rodata  0x80002000-0x80002020 (candidate-private: jump table + literal)
  //   g_table 0x80002020-0x80002060 (data blob, target 0x80002000)
  //   g_bss   0x80003100-0x80003200 (BSS blob, target 0x80003000)
  std::vector<uint32_t> words(0x1060 / 4, 0);
  words[0] = 0x3C048000;           // lui  a0, %hi(g_counter)
  words[1] = 0x8C822030;           // lw   v0, %lo(g_counter)(a0)
  words[2] = 0x3C058000;           // lui  a1, %hi(.main + 0x1008)
  words[3] = 0x24A52008;           // addiu a1, a1, %lo(.main + 0x1008)
  words[4] = 0x3C068000;           // lui  a2, %hi(g_bss_value)
  words[5] = 0xACC03110;           // sw   zero, %lo(g_bss_value)(a2)
  words[6] = 0x0C000400;           // jal  Func
  words[0x1000 / 4] = 0x80001010;  // rodata: jump table entry into code
  words[0x1004 / 4] = 0x80002030;  // rodata: &g_counter
  words[0x1008 / 4] = 0x3F800000;  // rodata: 1.0f
  words[0x1020 / 4] = 0x11111111;  // g_table contents
  words[0x1030 / 4] = 0x80002030;  // pointer inside blob (target bytes win; must be ignored)

  std::vector<TestSymbol> symbols = {
      {".main", 0x80001000, 3},           // 1
      {"Func", 0x80001000},               // 2
      {"g_table", 0x80002020},            // 3
      {"g_counter", 0x80002030},          // 4
      {"g_bss", 0x80003100, 0, 2},        // 5
      {"g_bss_value", 0x80003110, 0, 2},  // 6
      {"main_RODATA_START", 0x80002000},
      {"main_RODATA_END", 0x80002020},
      {"main_DATA_START", 0x80002020},
      {"main_DATA_END", 0x80002060},
      {"main_BSS_START", 0x80003100, 0, 2},
      {"main_BSS_END", 0x80003200, 0, 2},
  };
  std::vector<TestReloc> relocs = {
      {0x80001000, 4, kRMipsHi16}, {0x80001004, 4, kRMipsLo16}, {0x80001008, 1, kRMipsHi16},
      {0x8000100C, 1, kRMipsLo16}, {0x80001010, 6, kRMipsHi16}, {0x80001014, 6, kRMipsLo16},
      {0x80001018, 2, kRMips26},   {0x80002000, 1, kRMips32},   {0x80002004, 4, kRMips32},
      {0x80002030, 4, kRMips32},
  };
  auto elf = BuildElf(0x80001000, words, 0x80003100, 0x100, symbols, relocs);

  auto blobs = CollectDataBlobs(&*cfg, &*syms);
  auto rebase = BuildCandidateRebase(elf, blobs, &*cfg, &*syms, RebaseOptions{});
  ASSERT_TRUE(rebase.ok()) << rebase.status();
  EXPECT_EQ(rebase->layout_mismatches, 0u);
  EXPECT_EQ(rebase->unsupported_relocations, 0u);

  std::vector<uint32_t> code(words.begin(), words.begin() + 7);
  auto rebased = rebase->RebaseFunctionWords(0x80001000, code);
  EXPECT_EQ(rebased[0], 0x3C048000u);
  EXPECT_EQ(rebased[1], 0x8C822010u);  // g_counter -> target 0x80002010
  EXPECT_EQ(rebased[2], 0x3C058000u);
  EXPECT_EQ(rebased[3], 0x24A53208u);  // rodata literal relocated to scratch 0x80003208
  EXPECT_EQ(rebased[4], 0x3C068000u);
  EXPECT_EQ(rebased[5], 0xACC03010u);  // g_bss_value -> target 0x80003010
  EXPECT_EQ(rebased[6], 0x0C000400u);  // calls are untouched

  ASSERT_EQ(rebase->private_payloads.size(), 1u);
  const auto& payload = rebase->private_payloads[0];
  EXPECT_EQ(payload.address, 0x80003200u);
  ASSERT_EQ(payload.data.size(), 0x20u);
  EXPECT_EQ(ReadBigEndian32(payload.data.data()), 0x80001010u);
  EXPECT_EQ(ReadBigEndian32(payload.data.data() + 4), 0x80002010u);
  EXPECT_EQ(ReadBigEndian32(payload.data.data() + 8), 0x3F800000u);

  EXPECT_EQ(rebase->MapAddress(0x80002030), 0x80002010u);
  EXPECT_EQ(rebase->MapAddress(0x80002008), 0x80003208u);
  EXPECT_EQ(rebase->MapAddress(0x80003110), 0x80003010u);
  EXPECT_EQ(rebase->MapAddress(0x80001000), 0x80001000u);
  EXPECT_FALSE(rebase->patched_words.contains(0x80002030));
}

TEST(RelocationRebaserTest, RecomputesHighHalfWhenLowHalfSignFlips) {
  constexpr char kCarryConfig[] = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "main"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x8020
  vram: 0x80001000
  subsegments { name: "code" type: SUBSEGMENT_ASM rom_start: 0x1000 }
  subsegments { name: "data" type: SUBSEGMENT_DATA rom_start: 0x8010 }
}
)";
  auto cfg = ParseGameConfig(kCarryConfig);
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  auto syms = SymbolIndex::ParseFromTextproto(
      R"(entries { name: "g_far" address: 0x80008010 type: SYMBOL_DATA })");
  ASSERT_TRUE(syms.ok()) << syms.status();

  std::vector<uint32_t> words(0x7000 / 4, 0);
  words[0] = 0x3C048000;  // lui a0, %hi(g_far)       (candidate 0x80007FF0)
  words[1] = 0x8C847FF0;  // lw  a0, %lo(g_far)(a0)
  std::vector<TestSymbol> symbols = {
      {"g_far", 0x80007FF0},
      {"main_DATA_START", 0x80007FF0},
      {"main_DATA_END", 0x80008000},
  };
  std::vector<TestReloc> relocs = {{0x80001000, 1, kRMipsHi16}, {0x80001004, 1, kRMipsLo16}};
  auto elf = BuildElf(0x80001000, words, 0x80008000, 0, symbols, relocs);

  auto rebase =
      BuildCandidateRebase(elf, CollectDataBlobs(&*cfg, &*syms), &*cfg, &*syms, RebaseOptions{});
  ASSERT_TRUE(rebase.ok()) << rebase.status();
  auto rebased = rebase->RebaseFunctionWords(0x80001000, {words[0], words[1]});
  EXPECT_EQ(rebased[0], 0x3C048001u);  // 0x80008010 = (0x8001 << 16) + (int16_t)0x8010
  EXPECT_EQ(rebased[1], 0x8C848010u);
}

TEST(RelocationRebaserTest, MovesFunctionsWhoseCodeOverlapsRebasedData) {
  auto cfg = ParseGameConfig(kConfig);
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  auto syms = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(syms.ok()) << syms.status();

  // Candidate code grew so that function `Late` (0x80002000-0x80002010) sits where the target
  // keeps its data blob (0x80002000-0x80002040).
  std::vector<uint32_t> words(0x1160 / 4, 0);
  words[0x1000 / 4] = 0x08000802;  // j    0x80002008 (internal)
  words[0x1004 / 4] = 0x0C000400;  // jal  Func (external)
  words[0x1100 / 4] = 0x80002008;  // rodata jump table entry into Late
  std::vector<TestSymbol> symbols = {
      {".main", 0x80001000, 3},  // 1
      {"Func", 0x80001000},      // 2
      {"Late", 0x80002000},      // 3
      {"g_table", 0x80002120},
      {"main_RODATA_START", 0x80002100},
      {"main_RODATA_END", 0x80002120},
      {"main_DATA_START", 0x80002120},
      {"main_DATA_END", 0x80002160},
  };
  std::vector<TestReloc> relocs = {
      {0x80002000, 3, kRMips26}, {0x80002004, 2, kRMips26}, {0x80002100, 1, kRMips32}};
  auto elf = BuildElf(0x80001000, words, 0x80003100, 0x100, symbols, relocs);

  RebaseOptions options;
  options.code_scratch_base = 0x80100000;
  auto rebase = BuildCandidateRebase(elf, CollectDataBlobs(&*cfg, &*syms), &*cfg, &*syms, options);
  ASSERT_TRUE(rebase.ok()) << rebase.status();

  std::vector<uint32_t> late = {words[0x1000 / 4], words[0x1004 / 4], 0, 0};
  auto placed = rebase->PlaceFunction(0x80002000, late);
  EXPECT_EQ(placed.vram, 0x80100000u);
  EXPECT_EQ(placed.words[0], 0x08040002u);  // j 0x80100008
  EXPECT_EQ(placed.words[1], 0x0C000400u);  // external call unchanged
  ASSERT_EQ(placed.overlays.size(), 1u);
  EXPECT_EQ(placed.overlays[0].address, 0x80002100u);
  EXPECT_EQ(ReadBigEndian32(placed.overlays[0].data.data()), 0x80100008u);

  std::vector<uint32_t> func = {0, 0};
  auto in_place = rebase->PlaceFunction(0x80001000, func);
  EXPECT_EQ(in_place.vram, 0x80001000u);
  EXPECT_TRUE(in_place.overlays.empty());
}

TEST(RelocationRebaserTest, FailsWithoutRelocations) {
  auto cfg = ParseGameConfig(kConfig);
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  auto syms = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(syms.ok()) << syms.status();
  std::vector<uint32_t> words(0x1060 / 4, 0);
  // Retarget .rel.main (sh_info) at the non-ALLOC .symtab so the ELF carries no loadable
  // relocations, as if it had been linked without --emit-relocs.
  auto elf = BuildElf(0x80001000, words, 0x80003100, 0x100, {{"g_table", 0x80002020}}, {});
  elf[elf.size() - (7 * 40) + (3 * 40) + 31] = 4;
  auto rebase =
      BuildCandidateRebase(elf, CollectDataBlobs(&*cfg, &*syms), &*cfg, &*syms, RebaseOptions{});
  EXPECT_FALSE(rebase.ok());
}

TEST(RelocationRebaserTest, ReportsDataSymbolsThatMovedInsideTheirBlob) {
  auto cfg = ParseGameConfig(kConfig);
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  auto syms = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(syms.ok()) << syms.status();
  std::vector<uint32_t> words(0x1060 / 4, 0);
  std::vector<TestSymbol> symbols = {
      {"g_table", 0x80002020}, {"g_counter", 0x80002034},  // Should be g_table + 0x10 = 0x80002030.
  };
  auto elf = BuildElf(0x80001000, words, 0x80003100, 0x100, symbols, {{0x80001000, 2, kRMips26}});
  auto rebase =
      BuildCandidateRebase(elf, CollectDataBlobs(&*cfg, &*syms), &*cfg, &*syms, RebaseOptions{});
  ASSERT_TRUE(rebase.ok()) << rebase.status();
  EXPECT_EQ(rebase->layout_mismatches, 1u);
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
