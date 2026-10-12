#include "fuzzer/candidate_symbol_map.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/endian.h"
#include "gtest/gtest.h"
#include "splitter/config.h"
#include "splitter/config.pb.h"
#include "splitter/symbol_registry.h"
#include "splitter/symbol_registry.pb.h"

namespace rom_nom_nom::fuzzer {
namespace {

void AppendWord(std::vector<uint8_t>& buf, uint32_t word) {
  buf.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(word & 0xFF));
}

TEST(CandidateSymbolMapTest, TranslateExactAndOffsetSymbols) {
  CandidateSymbolMap map;
  map.entries.push_back({
      .cand_addr = 0x80201000,
      .size = 0x40,
      .target_addr = 0x80200000,
      .is_data = true,
  });

  // Exact symbol address
  EXPECT_EQ(map.Translate(0x80201000), 0x80200000);
  // Offset within symbol size (0x40)
  EXPECT_EQ(map.Translate(0x80201010), 0x80200010);
  EXPECT_EQ(map.Translate(0x8020103C), 0x8020003C);
  // Beyond symbol size falls back
  EXPECT_EQ(map.Translate(0x80201050), 0x80201050);
}

TEST(CandidateSymbolMapTest, TranslateSectionRangesAndMirrors) {
  CandidateSymbolMap map;
  // Shift is -0x1000
  map.section_ranges.push_back({
      .cand_start = 0x80202000,
      .cand_end = 0x80205000,
      .shift = -0x1000,
  });

  // In range KSEG0
  EXPECT_EQ(map.Translate(0x80202100), 0x80201100);
  EXPECT_EQ(map.Translate(0x80204FFF), 0x80203FFF);

  // Uncached KSEG1 mirror (0xA0202100 -> KSEG0 0x80202100 -> shift -0x1000)
  EXPECT_EQ(map.Translate(0xA0202100), 0xA0201100);

  // Outside section range
  EXPECT_EQ(map.Translate(0x80206000), 0x80206000);
}

TEST(CandidateSymbolMapTest, IdentifiesDataAndRodataRanges) {
  CandidateSymbolMap map;
  map.data_ranges.push_back({0x80200000, 0x80204000});
  map.rodata_ranges.push_back({0x80100000, 0x80102000});
  map.entries.push_back({
      .cand_addr = 0x80100100,
      .size = 16,
      .target_addr = 0x80100100,
      .is_data = true,
  });

  EXPECT_TRUE(map.IsCandidateData(0x80201000));
  EXPECT_FALSE(map.IsCandidateData(0x80205000));

  // Non-FP checks range directly
  EXPECT_TRUE(map.IsCandidateRodata(0x80100050, /*is_fp=*/false));
  EXPECT_TRUE(map.IsCandidateRodata(0x80100104, /*is_fp=*/false));

  // FP checks data symbol override
  EXPECT_TRUE(map.IsCandidateRodata(0x80100050, /*is_fp=*/true));
  EXPECT_FALSE(map.IsCandidateRodata(0x80100104, /*is_fp=*/true));  // IsDataSymbol returns true
}

TEST(CandidateSymbolMapTest, ExtractsTargetDataPayloadsFromConfig) {
  std::vector<uint8_t> base_rom(0x4000, 0);
  for (size_t i = 0x1000; i < 0x1100; ++i) {
    base_rom[i] = static_cast<uint8_t>(i & 0xFF);
  }
  for (size_t i = 0x2000; i < 0x2200; ++i) {
    base_rom[i] = static_cast<uint8_t>((i + 5) & 0xFF);
  }

  SplitConfig config;
  auto* seg = config.add_segments();
  seg->set_name("code");
  seg->set_type(SEGMENT_CODE);
  seg->set_rom_start(0x1000);
  seg->set_rom_end(0x3000);
  seg->set_vram(0x80001000);

  auto* sub1 = seg->add_subsegments();
  sub1->set_type(SUBSEGMENT_RODATA);
  sub1->set_rom_start(0x1000);
  sub1->set_vram(0x80001000);

  auto* sub2 = seg->add_subsegments();
  sub2->set_type(SUBSEGMENT_DATA);
  sub2->set_rom_start(0x2000);
  sub2->set_vram(0x80002000);

  auto payloads = ExtractTargetDataPayloads(base_rom, &config);
  ASSERT_EQ(payloads.size(), 2u);

  EXPECT_EQ(payloads[0].address, 0x80001000);
  EXPECT_EQ(payloads[0].data.size(), 0x1000u);
  EXPECT_EQ(payloads[0].data[0], 0x00);

  EXPECT_EQ(payloads[1].address, 0x80002000);
  EXPECT_EQ(payloads[1].data.size(), 0x1000u);
  EXPECT_EQ(payloads[1].data[0], 0x05);
}

TEST(CandidateSymbolMapTest, BuildsCandidateSymbolMapFromElf) {
  // Construct minimal valid 32-bit big-endian ELF
  std::vector<uint8_t> elf(52, 0);
  // ELF Magic
  elf[0] = 0x7F;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = 1;  // 32-bit
  elf[5] = 2;  // Big-endian

  // Set e_shoff = 52
  uint32_t shoff = 52;
  elf[32] = (shoff >> 24) & 0xFF;
  elf[33] = (shoff >> 16) & 0xFF;
  elf[34] = (shoff >> 8) & 0xFF;
  elf[35] = shoff & 0xFF;

  // e_shentsize = 40
  elf[46] = 0;
  elf[47] = 40;

  // e_shnum = 4 (0: null, 1: .data, 2: .symtab, 3: .strtab)
  elf[48] = 0;
  elf[49] = 4;

  // e_shstrndx = 3 (.strtab)
  elf[50] = 0;
  elf[51] = 3;

  // String table
  using namespace std::string_literals;
  std::string strtab = "\0.data\0.symtab\0.strtab\0D_80200000\0func_80099990\0"s;
  size_t strtab_pos = 52 + 4 * 40;
  size_t d_sym_name_idx = strtab.find("D_80200000");
  size_t func_sym_name_idx = strtab.find("func_80099990");

  // Symbol table entries (16 bytes each: st_name(4), st_value(4), st_size(4), st_info(1),
  // st_other(1), st_shndx(2))
  std::vector<uint8_t> symtab;
  // Null symbol
  for (int i = 0; i < 16; ++i) symtab.push_back(0);

  // Symbol 1: D_80200000 at cand addr 0x80201000, size 0x20
  AppendWord(symtab, static_cast<uint32_t>(d_sym_name_idx));
  AppendWord(symtab, 0x80201000);
  AppendWord(symtab, 0x20);
  AppendWord(symtab, 0x00010001);

  // Symbol 2: func_80099990 at cand addr 0x80099B00, size 0x40
  AppendWord(symtab, static_cast<uint32_t>(func_sym_name_idx));
  AppendWord(symtab, 0x80099B00);
  AppendWord(symtab, 0x40);
  AppendWord(symtab, 0x00020001);

  size_t symtab_pos = strtab_pos + strtab.size();

  // Now create the 4 section headers (40 bytes each)
  std::vector<uint8_t> shdrs;
  // Section 0: Null section
  for (int i = 0; i < 40; ++i) shdrs.push_back(0);

  // Section 1: .data (addr=0x80201000, size=0x1000, flags=SHF_ALLOC(2)|SHF_WRITE(1))
  AppendWord(shdrs, 1);           // sh_name (".data")
  AppendWord(shdrs, 1);           // sh_type (SHT_PROGBITS)
  AppendWord(shdrs, 3);           // sh_flags (SHF_WRITE | SHF_ALLOC)
  AppendWord(shdrs, 0x80201000);  // sh_addr
  AppendWord(shdrs, 0);           // sh_offset
  AppendWord(shdrs, 0x1000);      // sh_size
  AppendWord(shdrs, 0);           // sh_link
  AppendWord(shdrs, 0);           // sh_info
  AppendWord(shdrs, 4);           // sh_addralign
  AppendWord(shdrs, 0);           // sh_entsize

  // Section 2: .symtab (sh_type=2, link=3 for strtab)
  AppendWord(shdrs, 7);                                     // sh_name (".symtab")
  AppendWord(shdrs, 2);                                     // sh_type (SHT_SYMTAB)
  AppendWord(shdrs, 0);                                     // sh_flags
  AppendWord(shdrs, 0);                                     // sh_addr
  AppendWord(shdrs, static_cast<uint32_t>(symtab_pos));     // sh_offset
  AppendWord(shdrs, static_cast<uint32_t>(symtab.size()));  // sh_size
  AppendWord(shdrs, 3);                                     // sh_link (index of .strtab)
  AppendWord(shdrs, 1);                                     // sh_info
  AppendWord(shdrs, 4);                                     // sh_addralign
  AppendWord(shdrs, 16);                                    // sh_entsize

  // Section 3: .strtab (sh_type=3)
  AppendWord(shdrs, 15);                                    // sh_name (".strtab")
  AppendWord(shdrs, 3);                                     // sh_type (SHT_STRTAB)
  AppendWord(shdrs, 0);                                     // sh_flags
  AppendWord(shdrs, 0);                                     // sh_addr
  AppendWord(shdrs, static_cast<uint32_t>(strtab_pos));     // sh_offset
  AppendWord(shdrs, static_cast<uint32_t>(strtab.size()));  // sh_size
  AppendWord(shdrs, 0);                                     // sh_link
  AppendWord(shdrs, 0);                                     // sh_info
  AppendWord(shdrs, 1);                                     // sh_addralign
  AppendWord(shdrs, 0);                                     // sh_entsize

  elf.insert(elf.end(), shdrs.begin(), shdrs.end());
  elf.insert(elf.end(), strtab.begin(), strtab.end());
  elf.insert(elf.end(), symtab.begin(), symtab.end());

  auto map = BuildCandidateSymbolMap(elf, /*symbol_index=*/nullptr);
  ASSERT_FALSE(map.entries.empty());

  // Check translation of D_80200000 (cand 0x80201000 -> target 0x80200000)
  EXPECT_EQ(map.Translate(0x80201000), 0x80200000);
  EXPECT_EQ(map.Translate(0x80201010), 0x80200010);

  // Check translation of func_80099990 (cand 0x80099B00 -> target 0x80099990)
  EXPECT_EQ(map.Translate(0x80099B00), 0x80099990);

  // Section shift detected for .data: target - cand = 0x80200000 - 0x80201000 = -0x1000
  EXPECT_EQ(map.Translate(0x80201800), 0x80200800);
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
