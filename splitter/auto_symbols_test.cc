#include "splitter/auto_symbols.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

TEST(AutoSymbolsTest, DiscoversUndefinedFunctionCall) {
  AutoSymbolFinder finder;

  // jal 0x80099990 at PC 0x80025C00 -> 0x0C026664
  std::vector<Instruction> instructions;
  auto inst_or = DecodeInstruction(0x0C026664, 0x80025C00);
  ASSERT_TRUE(inst_or.ok());
  instructions.push_back(*inst_or);

  // nop delay slot
  auto nop_or = DecodeInstruction(0x00000000, 0x80025C04);
  ASSERT_TRUE(nop_or.ok());
  instructions.push_back(*nop_or);

  auto status = finder.ScanInstructions(instructions);
  ASSERT_TRUE(status.ok());

  auto funcs = finder.DiscoveredFuncSymbols();
  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_EQ(funcs[0].address, 0x80099990u);
  EXPECT_EQ(funcs[0].name, "func_80099990");
  EXPECT_EQ(funcs[0].type, SYMBOL_FUNC);
  EXPECT_EQ(funcs[0].reference_count, 1u);

  std::string script = finder.GenerateUndefinedFuncsScript();
  EXPECT_NE(script.find("func_80099990 = 0x80099990;"), std::string::npos);
}

TEST(AutoSymbolsTest, DiscoversUndefinedDataRelocation) {
  AutoSymbolFinder finder;

  std::vector<Instruction> instructions;
  // lui $v0, 0x801F -> 0x3C02801F at PC 0x80025C00
  auto lui = DecodeInstruction(0x3C02801F, 0x80025C00);
  ASSERT_TRUE(lui.ok());
  instructions.push_back(*lui);

  // lw $a0, 0x7004($v0) -> 0x8C447004 at PC 0x80025C04
  auto lw = DecodeInstruction(0x8C447004, 0x80025C04);
  ASSERT_TRUE(lw.ok());
  instructions.push_back(*lw);

  auto status = finder.ScanInstructions(instructions);
  ASSERT_TRUE(status.ok());

  auto data_syms = finder.DiscoveredDataSymbols();
  ASSERT_EQ(data_syms.size(), 1u);
  EXPECT_EQ(data_syms[0].address, 0x801F7004u);
  EXPECT_EQ(data_syms[0].name, "D_801F7004");
  EXPECT_EQ(data_syms[0].type, SYMBOL_DATA);
  EXPECT_EQ(data_syms[0].reference_count, 2u);

  std::string script = finder.GenerateUndefinedSymsScript();
  EXPECT_NE(script.find("D_801F7004 = 0x801F7004;"), std::string::npos);
}

TEST(AutoSymbolsTest, ExcludesKnownSymbols) {
  constexpr std::string_view kSymbols = R"pb(
    entries { name: "AudioUpdate" address: 0x8003CF38 type: SYMBOL_FUNC }
    entries { name: "g_audio_voices" address: 0x801F7004 type: SYMBOL_DATA }
  )pb";

  auto index_or = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(index_or.ok());

  AutoSymbolFinder finder(&*index_or);

  std::vector<Instruction> instructions;
  // jal 0x8003CF38
  instructions.push_back(*DecodeInstruction(0x0C00F3CE, 0x80025C00));
  // lui $v0, 0x801F
  instructions.push_back(*DecodeInstruction(0x3C02801F, 0x80025C04));
  // lw $a0, 0x7004($v0)
  instructions.push_back(*DecodeInstruction(0x8C447004, 0x80025C08));

  auto status = finder.ScanInstructions(instructions);
  ASSERT_TRUE(status.ok());

  EXPECT_EQ(finder.TotalDiscovered(), 0u);
  EXPECT_TRUE(finder.DiscoveredFuncSymbols().empty());
  EXPECT_TRUE(finder.DiscoveredDataSymbols().empty());
}

TEST(AutoSymbolsTest, ExcludesRegisteredDefinedSymbolsAndRanges) {
  AutoSymbolFinder finder;
  finder.RegisterDefinedSymbol("LocalFunc", 0x8003CF38);
  finder.RegisterDefinedAddressRange(0x801F7000, 0x801F7100);

  std::vector<Instruction> instructions;
  // jal 0x8003CF38 (registered defined symbol)
  instructions.push_back(*DecodeInstruction(0x0C00F3CE, 0x80025C00));
  // lui $v0, 0x801F
  instructions.push_back(*DecodeInstruction(0x3C02801F, 0x80025C04));
  // lw $a0, 0x7004($v0) -> targets 0x801F7004 (inside registered range)
  instructions.push_back(*DecodeInstruction(0x8C447004, 0x80025C08));

  auto status = finder.ScanInstructions(instructions);
  ASSERT_TRUE(status.ok());

  EXPECT_EQ(finder.TotalDiscovered(), 0u);
}

TEST(AutoSymbolsTest, ExcludesHardwareRegisters) {
  AutoSymbolFinder finder;

  std::vector<Instruction> instructions;
  // lui $v0, 0x0404 (SP status register)
  instructions.push_back(*DecodeInstruction(0x3C020404, 0x80025C00));
  // sw $zero, 0x0010($v0)
  instructions.push_back(*DecodeInstruction(0xAC400010, 0x80025C04));

  auto status = finder.ScanInstructions(instructions);
  ASSERT_TRUE(status.ok());

  // Hardware MMIO should be excluded
  EXPECT_EQ(finder.TotalDiscovered(), 0u);
}

TEST(AutoSymbolsTest, ExportToRegistry) {
  AutoSymbolFinder finder;

  // jal 0x80099990
  std::vector<Instruction> instructions;
  instructions.push_back(*DecodeInstruction(0x0C026664, 0x80025C00));
  // lui $v0, 0x801F
  instructions.push_back(*DecodeInstruction(0x3C02801F, 0x80025C04));
  // lw $a0, 0x7004($v0)
  instructions.push_back(*DecodeInstruction(0x8C447004, 0x80025C08));

  ASSERT_TRUE(finder.ScanInstructions(instructions).ok());

  SymbolRegistry proto = finder.ExportToRegistry();
  ASSERT_EQ(proto.entries_size(), 2);

  // Sorted by address: D_801F7004 < func_80099990 (0x80099990 < 0x801F7004)
  EXPECT_EQ(proto.entries(0).name(), "func_80099990");
  EXPECT_EQ(proto.entries(0).address(), 0x80099990u);
  EXPECT_EQ(proto.entries(0).type(), SYMBOL_FUNC);

  EXPECT_EQ(proto.entries(1).name(), "D_801F7004");
  EXPECT_EQ(proto.entries(1).address(), 0x801F7004u);
  EXPECT_EQ(proto.entries(1).type(), SYMBOL_DATA);
}

TEST(AutoSymbolsTest, PreservesFuncTypeWhenTargetHasDataRelocations) {
  AutoSymbolFinder finder;

  std::vector<Instruction> instructions;
  // jal 0x800E9C20 -> 0x0C03A708 at PC 0x80025C00
  instructions.push_back(*DecodeInstruction(0x0C03A708, 0x80025C00));
  // lui $t0, 0x800F (%hi of 0x800E9C20)
  instructions.push_back(*DecodeInstruction(0x3C08800F, 0x80025C04));
  // addiu $t0, $t0, -25568 (0x9C20, %lo of 0x800E9C20)
  instructions.push_back(*DecodeInstruction(0x25089C20, 0x80025C08));

  ASSERT_TRUE(finder.ScanInstructions(instructions).ok());

  auto funcs = finder.DiscoveredFuncSymbols();
  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_EQ(funcs[0].address, 0x800E9C20u);
  EXPECT_EQ(funcs[0].name, "func_800E9C20");
  EXPECT_EQ(funcs[0].type, SYMBOL_FUNC);

  // Should NOT be in data symbols
  auto data_syms = finder.DiscoveredDataSymbols();
  EXPECT_TRUE(data_syms.empty());
}

}  // namespace
}  // namespace rom_nom_nom
