#include "splitter/jump_tables.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

void AppendWord(std::vector<uint8_t>& buf, uint32_t word) {
  buf.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(word & 0xFF));
}

constexpr std::string_view kSymbolsProtoText = R"pb(
  entries { name: "g_my_jump_table" address: 0x80710000 type: SYMBOL_DATA }
)pb";

TEST(JumpTablesTest, DetectTableAtValidTable) {
  auto symbols_or = SymbolIndex::ParseFromTextproto(kSymbolsProtoText);
  ASSERT_TRUE(symbols_or.ok());

  FunctionRange func;
  func.vram_start = 0x80700000;
  func.vram_end = 0x80700100;
  func.name = "MyFunction";

  std::vector<uint8_t> rodata;
  AppendWord(rodata, 0x80700020);
  AppendWord(rodata, 0x80700040);
  AppendWord(rodata, 0x80700060);
  AppendWord(rodata, 0x80700080);

  JumpTableResolver resolver(&*symbols_or);
  auto table_opt = resolver.DetectTableAt(rodata, 0, 0x80710000, func);
  ASSERT_TRUE(table_opt.has_value());

  const auto& table = *table_opt;
  EXPECT_EQ(table.name, "g_my_jump_table");
  EXPECT_EQ(table.vram_start, 0x80710000u);
  EXPECT_EQ(table.SizeInBytes(), 16u);
  ASSERT_EQ(table.entry_targets.size(), 4u);
  EXPECT_EQ(table.entry_targets[0], 0x80700020u);
  EXPECT_EQ(table.entry_targets[1], 0x80700040u);
  EXPECT_EQ(table.entry_targets[2], 0x80700060u);
  EXPECT_EQ(table.entry_targets[3], 0x80700080u);
}

TEST(JumpTablesTest, DetectTableTooFewEntries) {
  FunctionRange func;
  func.vram_start = 0x80700000;
  func.vram_end = 0x80700100;
  func.name = "MyFunction";

  std::vector<uint8_t> rodata;
  AppendWord(rodata, 0x80700020);
  AppendWord(rodata, 0x80700040);
  // Third word points outside function
  AppendWord(rodata, 0x80720000);

  JumpTableResolver resolver(nullptr);
  auto table_opt = resolver.DetectTableAt(rodata, 0, 0x80710000, func);
  EXPECT_FALSE(table_opt.has_value());
}

TEST(JumpTablesTest, ScanRodataMultipleTables) {
  FunctionRange func1;
  func1.vram_start = 0x80700000;
  func1.vram_end = 0x80700100;
  func1.name = "Func1";

  FunctionRange func2;
  func2.vram_start = 0x80700200;
  func2.vram_end = 0x80700300;
  func2.name = "Func2";

  std::vector<FunctionRange> funcs = {func1, func2};

  std::vector<uint8_t> rodata;
  // Table 1 (12 bytes)
  AppendWord(rodata, 0x80700010);
  AppendWord(rodata, 0x80700020);
  AppendWord(rodata, 0x80700030);
  // Non-table padding word (e.g. 0x00000000)
  AppendWord(rodata, 0x00000000);
  // Table 2 (12 bytes)
  AppendWord(rodata, 0x80700210);
  AppendWord(rodata, 0x80700220);
  AppendWord(rodata, 0x80700230);

  JumpTableResolver resolver;
  auto tables = resolver.ScanRodata(rodata, 0x80710000, funcs);

  ASSERT_EQ(tables.size(), 2u);
  EXPECT_EQ(tables[0].vram_start, 0x80710000u);
  EXPECT_EQ(tables[0].name, "jtbl_80710000");
  EXPECT_EQ(tables[0].entry_targets.size(), 3u);

  EXPECT_EQ(tables[1].vram_start, 0x80710010u);
  EXPECT_EQ(tables[1].name, "jtbl_80710010");
  EXPECT_EQ(tables[1].entry_targets.size(), 3u);
}

TEST(JumpTablesTest, FormatJumpTableAsm) {
  JumpTable table;
  table.vram_start = 0x80710000;
  table.name = "jtbl_80710000";
  table.entry_targets = {0x80700020, 0x80700040, 0x80700060};

  JumpTableResolver resolver;
  std::string asm_text = resolver.FormatJumpTableAsm(table);

  EXPECT_EQ(asm_text, R"(.globl jtbl_80710000
jtbl_80710000:
    .word .L80700020
    .word .L80700040
    .word .L80700060
)");
}

TEST(JumpTablesTest, ExtractLocalLabels) {
  FunctionRange func;
  func.vram_start = 0x80700000;
  func.vram_end = 0x80700100;

  JumpTable table1;
  table1.entry_targets = {0x80700040, 0x80700020, 0x80700040};  // Duplicate 0x80700040

  JumpTable table2;
  table2.entry_targets = {0x80700080, 0x80720000};  // 0x80720000 is outside func

  JumpTableResolver resolver;
  auto labels = resolver.ExtractLocalLabels({table1, table2}, func);

  ASSERT_EQ(labels.size(), 3u);
  EXPECT_EQ(labels[0], 0x80700020u);
  EXPECT_EQ(labels[1], 0x80700040u);
  EXPECT_EQ(labels[2], 0x80700080u);
}

}  // namespace
}  // namespace rom_nom_nom
