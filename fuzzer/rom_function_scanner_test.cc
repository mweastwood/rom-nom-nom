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

}  // namespace
}  // namespace rom_nom_nom::fuzzer
