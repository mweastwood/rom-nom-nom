#include "differ/relocation_applier.h"

#include <elf.h>

#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

TEST(RelocationApplierTest, AppliesJalRelocation26) {
  // jal 0x0 (opcode 0x0C000000)
  std::vector<uint32_t> words = {0x0C000000};
  std::vector<ElfRelocation> relocs = {
      ElfRelocation{
          .section_offset = 0,
          .function_offset = 0,
          .type = R_MIPS_26,
          .sym_index = 1,
          .symbol_name = "func_8003D970",
      },
  };

  absl::flat_hash_map<std::string, uint32_t> symbols = {
      {"func_8003D970", 0x8003D970},
  };

  auto result = RelocationApplier::ApplyRelocations(
      words, relocs, [&](std::string_view name) -> std::optional<uint32_t> {
        auto it = symbols.find(name);
        if (it != symbols.end()) return it->second;
        return std::nullopt;
      });

  EXPECT_THAT(result.unresolved_symbols, IsEmpty());
  // 0x8003D970: lower 28 bits are 0x0003D970 >> 2 = 0x0000F65C
  // 0x0C000000 | 0x0000F65C = 0x0C00F65C
  EXPECT_THAT(result.patched_words, ElementsAre(0x0C00F65C));
}

TEST(RelocationApplierTest, AppliesPairedHi16AndLo16Relocations) {
  // Test case matching MessageInit:
  // Offset 0x28 (index 10): lui at, 0x0 (0x3C010000)
  // Offset 0x2C (index 11): addu at, at, v1 (0x00230821)
  // Offset 0x30 (index 12): sh zero, 36(at) (0xA4200024)
  std::vector<uint32_t> words = {
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x3C010000,  // [10] 0x28: lui at, 0x0
      0x00230821,  // [11] 0x2C: addu at, at, v1
      0xA4200024,  // [12] 0x30: sh zero, 36(at) (orig addend = 0x0024 = 36)
  };

  std::vector<ElfRelocation> relocs = {
      ElfRelocation{
          .section_offset = 0x28,
          .function_offset = 0x28,
          .type = R_MIPS_HI16,
          .sym_index = 1,
          .symbol_name = "g_text_boxes",
      },
      ElfRelocation{
          .section_offset = 0x30,
          .function_offset = 0x30,
          .type = R_MIPS_LO16,
          .sym_index = 1,
          .symbol_name = "g_text_boxes",
      },
  };

  absl::flat_hash_map<std::string, uint32_t> symbols = {
      {"g_text_boxes", 0x801C3F00},
  };

  ElfFunction func{
      .name = "MessageInit",
      .section_offset = 0,
      .size = static_cast<uint32_t>(words.size() * 4),
      .raw_words = words,
      .relocations = relocs,
  };

  auto result = RelocationApplier::Apply(func, symbols);
  EXPECT_THAT(result.unresolved_symbols, IsEmpty());

  // full address = 0x801C3F00 + 0x0024 = 0x801C3F24
  // HI16: (0x801C3F24 + 0x8000) >> 16 = 0x801C
  // LO16: 0x801C3F24 & 0xFFFF = 0x3F24
  EXPECT_EQ(result.patched_words[10], 0x3C01801Cu);
  EXPECT_EQ(result.patched_words[11], 0x00230821u);
  EXPECT_EQ(result.patched_words[12], 0xA4203F24u);
}

TEST(RelocationApplierTest, HandlesSignExtensionCarryInHi16) {
  // If low 16 bits have bit 15 set (e.g. 0x8000), LO16 sign-extends negative,
  // requiring HI16 to be incremented by 1.
  std::vector<uint32_t> words = {
      0x3C040000,  // lui a0, 0x0
      0x24840000,  // addiu a0, a0, 0
  };

  std::vector<ElfRelocation> relocs = {
      ElfRelocation{
          .function_offset = 0x0,
          .type = R_MIPS_HI16,
          .symbol_name = "g_high_bit_symbol",
      },
      ElfRelocation{
          .function_offset = 0x4,
          .type = R_MIPS_LO16,
          .symbol_name = "g_high_bit_symbol",
      },
  };

  absl::flat_hash_map<std::string, uint32_t> symbols = {
      {"g_high_bit_symbol", 0x801C8F20},
  };

  ElfFunction func{
      .name = "SignExtTest",
      .section_offset = 0,
      .size = 8,
      .raw_words = words,
      .relocations = relocs,
  };

  auto result = RelocationApplier::Apply(func, symbols);
  EXPECT_THAT(result.unresolved_symbols, IsEmpty());

  // Address: 0x801C8F20.
  // HI16: (0x801C8F20 + 0x8000) >> 16 = 0x801D0F20 >> 16 = 0x801D.
  // LO16: 0x801C8F20 & 0xFFFF = 0x8F20 (as signed 16-bit: -28896).
  // 0x801D0000 + (-28896) = 0x801C8F20!
  EXPECT_EQ(result.patched_words[0], 0x3C04801Du);
  EXPECT_EQ(result.patched_words[1], 0x24848F20u);
}

TEST(RelocationApplierTest, RecordsUnresolvedSymbolsAndPreservesWords) {
  std::vector<uint32_t> words = {0x3C010000};
  std::vector<ElfRelocation> relocs = {
      ElfRelocation{
          .function_offset = 0,
          .type = R_MIPS_HI16,
          .symbol_name = "g_unknown_symbol",
      },
  };

  absl::flat_hash_map<std::string, uint32_t> symbols = {
      {"other_symbol", 0x80000000},
  };

  ElfFunction func{
      .name = "UnresolvedTest",
      .section_offset = 0,
      .size = 4,
      .raw_words = words,
      .relocations = relocs,
  };

  auto result = RelocationApplier::Apply(func, symbols);
  EXPECT_THAT(result.unresolved_symbols, ElementsAre("g_unknown_symbol"));
  EXPECT_THAT(result.patched_words, ElementsAre(0x3C010000u));
}

}  // namespace
}  // namespace rom_nom_nom
