#include "fuzzer/rom_function_scanner.h"

#include <cstdint>
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

TEST(RomFunctionScannerTest, FailsOnTooSmallRom) {
  std::vector<uint8_t> small_rom(100, 0);
  auto res_or = RomFunctionScanner::Scan(small_rom);
  EXPECT_FALSE(res_or.ok());
}

TEST(RomFunctionScannerTest, ScansFunctionsFromRawRomWithoutConfig) {
  std::vector<uint8_t> rom(0x2000, 0);
  // Set entrypoint at offset 8: 0x80025C00 -> base VRAM for code at 0x1050 is 0x80025C50
  rom[8] = 0x80;
  rom[9] = 0x02;
  rom[10] = 0x5C;
  rom[11] = 0x00;

  // Build Function 1 at 0x1050 (VRAM 0x80025C50):
  // addiu $sp, $sp, -32
  // sw    $ra, 28($sp)
  // jal   0x80025C70 (Function 2)
  // nop
  // lw    $ra, 28($sp)
  // jr    $ra
  // addiu $sp, $sp, 32
  // nop (alignment padding)
  std::vector<uint8_t> f1;
  AppendWord(f1, 0x27BDFFE0);  // addiu $sp, $sp, -32
  AppendWord(f1, 0xAFBF001C);  // sw $ra, 28($sp)
  // jal target: (0x80025C70 & 0x0FFFFFFF) >> 2 = 0x000971C
  AppendWord(f1, 0x0C000000 | 0x000971C);
  AppendWord(f1, 0x00000000);  // nop
  AppendWord(f1, 0x8FBF001C);  // lw $ra, 28($sp)
  AppendWord(f1, 0x03E00008);  // jr $ra
  AppendWord(f1, 0x27BD0020);  // addiu $sp, $sp, 32
  AppendWord(f1, 0x00000000);  // nop padding

  // Build Function 2 at 0x1070 (VRAM 0x80025C70):
  // addiu $v0, $zero, 42
  // jr    $ra
  // nop
  std::vector<uint8_t> f2;
  AppendWord(f2, 0x2402002A);  // addiu $v0, $zero, 42
  AppendWord(f2, 0x03E00008);  // jr $ra
  AppendWord(f2, 0x00000000);  // nop

  std::copy(f1.begin(), f1.end(), rom.begin() + 0x1050);
  std::copy(f2.begin(), f2.end(), rom.begin() + 0x1070);

  auto funcs_or = RomFunctionScanner::Scan(rom);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;

  ASSERT_GE(funcs.size(), 2u);
  EXPECT_EQ(funcs[0].vram, 0x80025C50u);
  EXPECT_EQ(funcs[0].rom_offset, 0x1050u);
  EXPECT_EQ(funcs[0].size, 32u);
  EXPECT_EQ(funcs[0].raw_words.size(), 8u);

  EXPECT_EQ(funcs[1].vram, 0x80025C70u);
  EXPECT_EQ(funcs[1].rom_offset, 0x1070u);
  EXPECT_EQ(funcs[1].size, 16u);
  EXPECT_EQ(funcs[1].raw_words.size(), 4u);
}

TEST(RomFunctionScannerTest, ScansFunctionsWithSplitConfig) {
  std::vector<uint8_t> rom(0x3000, 0);

  SplitConfig config;
  auto* seg = config.add_segments();
  seg->set_name("code");
  seg->set_type(SEGMENT_CODE);
  seg->set_rom_start(0x1050);
  seg->set_rom_end(0x2000);
  seg->set_vram(0x80100000);

  // Single function at 0x1050:
  // jr $ra
  // nop
  std::vector<uint8_t> f1;
  AppendWord(f1, 0x03E00008);  // jr $ra
  AppendWord(f1, 0x00000000);  // nop

  std::copy(f1.begin(), f1.end(), rom.begin() + 0x1050);

  auto funcs_or = RomFunctionScanner::Scan(rom, &config);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;

  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_EQ(funcs[0].vram, 0x80100000u);
  EXPECT_EQ(funcs[0].rom_offset, 0x1050u);
  EXPECT_EQ(funcs[0].size, 16u);
}

TEST(RomFunctionScannerTest, DiscoversFunctionsViaJalTarget) {
  std::vector<uint8_t> rom(0x3000, 0);

  SplitConfig config;
  auto* seg = config.add_segments();
  seg->set_name("code");
  seg->set_type(SEGMENT_CODE);
  seg->set_rom_start(0x1000);
  seg->set_rom_end(0x2000);
  seg->set_vram(0x80001000);

  // Function 1 at 0x1000 (VRAM 0x80001000):
  // jal 0x80001040 (Function 2)
  // nop
  // jr $ra
  // nop
  std::vector<uint8_t> f1;
  // jal target: (0x80001040 & 0x0FFFFFFF) >> 2 = 0x0000410
  AppendWord(f1, 0x0C000000 | 0x0000410);
  AppendWord(f1, 0x00000000);  // nop
  AppendWord(f1, 0x03E00008);  // jr $ra
  AppendWord(f1, 0x00000000);  // nop

  // Function 2 at 0x1040 (VRAM 0x80001040):
  // addiu $v0, $zero, 1
  // jr $ra
  // nop
  std::vector<uint8_t> f2;
  AppendWord(f2, 0x24020001);  // addiu $v0, $zero, 1
  AppendWord(f2, 0x03E00008);  // jr $ra
  AppendWord(f2, 0x00000000);  // nop

  std::copy(f1.begin(), f1.end(), rom.begin() + 0x1000);
  std::copy(f2.begin(), f2.end(), rom.begin() + 0x1040);

  auto funcs_or = RomFunctionScanner::Scan(rom, &config);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;

  ASSERT_EQ(funcs.size(), 2u);
  EXPECT_EQ(funcs[0].vram, 0x80001000u);
  EXPECT_EQ(funcs[0].rom_offset, 0x1000u);
  EXPECT_EQ(funcs[1].vram, 0x80001040u);
  EXPECT_EQ(funcs[1].rom_offset, 0x1040u);
}

TEST(RomFunctionScannerTest, AppliesSemanticSymbolNames) {
  std::vector<uint8_t> rom(0x3000, 0);

  SplitConfig config;
  auto* seg = config.add_segments();
  seg->set_name("code");
  seg->set_type(SEGMENT_CODE);
  seg->set_rom_start(0x1000);
  seg->set_rom_end(0x2000);
  seg->set_vram(0x80001000);

  // Function at 0x1000 (VRAM 0x80001000):
  // jr $ra
  // nop
  std::vector<uint8_t> f1;
  AppendWord(f1, 0x03E00008);  // jr $ra
  AppendWord(f1, 0x00000000);  // nop
  std::copy(f1.begin(), f1.end(), rom.begin() + 0x1000);

  // Create SymbolIndex with a clean semantic name for 0x80001000
  SymbolRegistry registry;
  auto* entry = registry.add_entries();
  entry->set_name("MySemanticFunction");
  entry->set_address(0x80001000);
  entry->set_type(SYMBOL_FUNC);

  auto symbols_or = SymbolIndex::FromProto(std::move(registry));
  ASSERT_TRUE(symbols_or.ok()) << symbols_or.status();

  auto funcs_or = RomFunctionScanner::Scan(rom, &config, &*symbols_or);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;

  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_EQ(funcs[0].name, "MySemanticFunction");
  EXPECT_EQ(funcs[0].vram, 0x80001000u);
}

TEST(RomFunctionScannerTest, SkipsNonCodeSegments) {
  std::vector<uint8_t> rom(0x3000, 0);

  SplitConfig config;
  auto* seg_data = config.add_segments();
  seg_data->set_name("data");
  seg_data->set_type(SEGMENT_DATA);
  seg_data->set_rom_start(0x1000);
  seg_data->set_rom_end(0x1800);
  seg_data->set_vram(0x80001000);

  auto* seg_code = config.add_segments();
  seg_code->set_name("code");
  seg_code->set_type(SEGMENT_CODE);
  seg_code->set_rom_start(0x1800);
  seg_code->set_rom_end(0x2000);
  seg_code->set_vram(0x80001800);

  // Even if data segment has bytes looking like jr $ra, it must not be scanned
  std::vector<uint8_t> dummy;
  AppendWord(dummy, 0x03E00008);  // jr $ra
  AppendWord(dummy, 0x00000000);  // nop
  std::copy(dummy.begin(), dummy.end(), rom.begin() + 0x1000);

  // Function in code segment:
  std::vector<uint8_t> code_func;
  AppendWord(code_func, 0x24020005);  // addiu $v0, $zero, 5
  AppendWord(code_func, 0x03E00008);  // jr $ra
  AppendWord(code_func, 0x00000000);  // nop
  std::copy(code_func.begin(), code_func.end(), rom.begin() + 0x1800);

  auto funcs_or = RomFunctionScanner::Scan(rom, &config);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;

  // Only the function in the code segment should be returned
  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_EQ(funcs[0].vram, 0x80001800u);
  EXPECT_EQ(funcs[0].rom_offset, 0x1800u);
}

TEST(RomFunctionScannerTest, ScansFunctionsAcrossSubsegments) {
  std::vector<uint8_t> rom(0x3000, 0);

  SplitConfig config;
  auto* seg = config.add_segments();
  seg->set_name("main");
  seg->set_type(SEGMENT_CODE);
  seg->set_rom_start(0x1000);
  seg->set_rom_end(0x2000);
  seg->set_vram(0x80025C00);

  // Subsegment 1: SUBSEGMENT_C from 0x1000 to 0x1100
  auto* sub1 = seg->add_subsegments();
  sub1->set_name("sub1");
  sub1->set_type(SUBSEGMENT_C);
  sub1->set_rom_start(0x1000);
  sub1->set_vram(0x80025C00);

  // Subsegment 2: SUBSEGMENT_ASM from 0x1100 to 0x1200
  auto* sub2 = seg->add_subsegments();
  sub2->set_name("sub2");
  sub2->set_type(SUBSEGMENT_ASM);
  sub2->set_rom_start(0x1100);
  sub2->set_vram(0x80025D00);

  // Subsegment 3: SUBSEGMENT_BSS from 0x1200
  auto* sub3 = seg->add_subsegments();
  sub3->set_name("sub3_bss");
  sub3->set_type(SUBSEGMENT_BSS);
  sub3->set_rom_start(0x1200);

  // Subsegment 1 contains Function 1 at 0x1000 (VRAM 0x80025C00):
  // jal 0x80025D00 (Function 2 in subsegment 2)
  // nop
  // jr $ra
  // nop
  std::vector<uint8_t> f1;
  // jal target: (0x80025D00 & 0x0FFFFFFF) >> 2 = 0x0009740
  AppendWord(f1, 0x0C000000 | 0x0009740);
  AppendWord(f1, 0x00000000);  // nop
  AppendWord(f1, 0x03E00008);  // jr $ra
  AppendWord(f1, 0x00000000);  // nop
  std::copy(f1.begin(), f1.end(), rom.begin() + 0x1000);

  // Subsegment 2 contains Function 2 at 0x1100 (VRAM 0x80025D00):
  // addiu $v0, $zero, 10
  // jr $ra
  // nop
  std::vector<uint8_t> f2;
  AppendWord(f2, 0x2402000A);  // addiu $v0, $zero, 10
  AppendWord(f2, 0x03E00008);  // jr $ra
  AppendWord(f2, 0x00000000);  // nop
  std::copy(f2.begin(), f2.end(), rom.begin() + 0x1100);

  auto funcs_or = RomFunctionScanner::Scan(rom, &config);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;

  ASSERT_EQ(funcs.size(), 2u);
  EXPECT_EQ(funcs[0].vram, 0x80025C00u);
  EXPECT_EQ(funcs[0].rom_offset, 0x1000u);
  EXPECT_EQ(funcs[1].vram, 0x80025D00u);
  EXPECT_EQ(funcs[1].rom_offset, 0x1100u);
}

TEST(RomFunctionScannerTest, ScansFunctionsFromElfAndRom) {
  std::vector<uint8_t> rom(0x3000, 0);
  // Place two instruction words at ROM 0x1050
  rom[0x1050] = 0x03;
  rom[0x1051] = 0xE0;
  rom[0x1052] = 0x00;
  rom[0x1053] = 0x08;  // jr $ra
  rom[0x1054] = 0x00;
  rom[0x1055] = 0x00;
  rom[0x1056] = 0x00;
  rom[0x1057] = 0x00;  // nop

  // Construct a minimal ELF binary
  std::vector<uint8_t> elf(52 + 32 + 3 * 40, 0);

  // ELF Header (52 bytes)
  elf[0] = 0x7F;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = 1;  // ELFCLASS32
  elf[5] = 2;  // ELFDATA2MSB (Big Endian)
  elf[6] = 1;  // EV_CURRENT
  elf[28] = 0;
  elf[29] = 0;
  elf[30] = 0;
  elf[31] = 52;  // e_phoff = 52
  elf[32] = 0;
  elf[33] = 0;
  elf[34] = 0;
  elf[35] = 84;  // e_shoff = 84 (52 + 32)
  elf[42] = 0;
  elf[43] = 32;  // e_phentsize = 32
  elf[44] = 0;
  elf[45] = 1;  // e_phnum = 1
  elf[46] = 0;
  elf[47] = 40;  // e_shentsize = 40
  elf[48] = 0;
  elf[49] = 3;  // e_shnum = 3
  elf[50] = 0;
  elf[51] = 0;  // e_shstrndx = 0

  // Program Header 0 (32 bytes at offset 52)
  // p_type = PT_LOAD (1)
  elf[52] = 0;
  elf[53] = 0;
  elf[54] = 0;
  elf[55] = 1;
  // p_vaddr = 0x80025C50
  elf[60] = 0x80;
  elf[61] = 0x02;
  elf[62] = 0x5C;
  elf[63] = 0x50;
  // p_paddr = 0x1050
  elf[64] = 0x00;
  elf[65] = 0x00;
  elf[66] = 0x10;
  elf[67] = 0x50;
  // p_filesz = 0x100
  elf[68] = 0x00;
  elf[69] = 0x00;
  elf[70] = 0x01;
  elf[71] = 0x00;
  // p_memsz = 0x100
  elf[72] = 0x00;
  elf[73] = 0x00;
  elf[74] = 0x01;
  elf[75] = 0x00;

  // Strtab data at offset = 204
  size_t strtab_offset = elf.size();
  std::vector<char> strtab_data = {'\0', 'T', 'e', 's', 't', 'F', 'u', 'n', 'c', '\0'};
  elf.insert(elf.end(), strtab_data.begin(), strtab_data.end());

  // Symtab data (2 entries: null symbol + TestFunc symbol)
  size_t symtab_offset = elf.size();
  // Null symbol (16 bytes)
  elf.insert(elf.end(), 16, 0);
  // TestFunc symbol (16 bytes)
  // st_name = 1
  AppendWord(elf, 1);
  // st_value = 0x80025C50
  AppendWord(elf, 0x80025C50);
  // st_size = 8
  AppendWord(elf, 8);
  // st_info = STT_FUNC (2), st_other = 0, st_shndx = 1
  elf.push_back(2);
  elf.push_back(0);
  elf.push_back(0);
  elf.push_back(1);

  // Section Header 2: .strtab (offset 84 + 80 = 164)
  // sh_type = SHT_STRTAB (3)
  elf[168] = 0;
  elf[169] = 0;
  elf[170] = 0;
  elf[171] = 3;
  // sh_offset
  elf[180] = static_cast<uint8_t>((strtab_offset >> 24) & 0xFF);
  elf[181] = static_cast<uint8_t>((strtab_offset >> 16) & 0xFF);
  elf[182] = static_cast<uint8_t>((strtab_offset >> 8) & 0xFF);
  elf[183] = static_cast<uint8_t>(strtab_offset & 0xFF);
  // sh_size
  elf[184] = static_cast<uint8_t>((strtab_data.size() >> 24) & 0xFF);
  elf[185] = static_cast<uint8_t>((strtab_data.size() >> 16) & 0xFF);
  elf[186] = static_cast<uint8_t>((strtab_data.size() >> 8) & 0xFF);
  elf[187] = static_cast<uint8_t>(strtab_data.size() & 0xFF);

  // Section Header 1: .symtab (offset 84 + 40 = 124)
  // sh_type = SHT_SYMTAB (2)
  elf[128] = 0;
  elf[129] = 0;
  elf[130] = 0;
  elf[131] = 2;
  // sh_offset
  elf[140] = static_cast<uint8_t>((symtab_offset >> 24) & 0xFF);
  elf[141] = static_cast<uint8_t>((symtab_offset >> 16) & 0xFF);
  elf[142] = static_cast<uint8_t>((symtab_offset >> 8) & 0xFF);
  elf[143] = static_cast<uint8_t>(symtab_offset & 0xFF);
  // sh_size = 32
  elf[144] = 0;
  elf[145] = 0;
  elf[146] = 0;
  elf[147] = 32;
  // sh_link = 2 (points to Section 2 .strtab)
  elf[148] = 0;
  elf[149] = 0;
  elf[150] = 0;
  elf[151] = 2;
  // sh_entsize = 16
  elf[160] = 0;
  elf[161] = 0;
  elf[162] = 0;
  elf[163] = 16;

  auto funcs_or = RomFunctionScanner::ScanFromElfAndRom(rom, elf);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;

  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_EQ(funcs[0].name, "TestFunc");
  EXPECT_EQ(funcs[0].vram, 0x80025C50u);
  EXPECT_EQ(funcs[0].rom_offset, 0x1050u);
  EXPECT_EQ(funcs[0].size, 8u);
  ASSERT_EQ(funcs[0].raw_words.size(), 2u);
  EXPECT_EQ(funcs[0].raw_words[0], 0x03E00008u);
  EXPECT_EQ(funcs[0].raw_words[1], 0x00000000u);
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
