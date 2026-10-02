#include "lifter/symbol_folder.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ::testing::Eq;
using ::testing::NotNull;
using ::testing::SizeIs;

std::vector<Instruction> DecodeSequence(const std::vector<uint32_t>& words,
                                        uint32_t base_vram = 0x80000000) {
  std::vector<Instruction> instructions;
  for (size_t i = 0; i < words.size(); ++i) {
    auto inst_or = DecodeInstruction(words[i], base_vram + static_cast<uint32_t>(i * 4));
    EXPECT_TRUE(inst_or.ok());
    if (inst_or.ok()) {
      instructions.push_back(*inst_or);
    }
  }
  return instructions;
}

TEST(SymbolFolderTest, AddressLoadPair) {
  // lui $at, 0x801F
  // addiu $a0, $at, 0x2340
  std::vector<uint32_t> words = {
      0x3C01801F,  // 0: lui $at, 0x801F
      0x24242340,  // 1: addiu $a0, $at, 0x2340
  };

  auto insts = DecodeSequence(words);
  SymbolFolder folder;
  folder.Fold(insts);

  ASSERT_THAT(folder.AllFolded(), SizeIs(1));
  const auto& folded = folder.AllFolded()[0];
  EXPECT_EQ(folded.type, FoldedPatternType::kAddressLoad);
  EXPECT_EQ(folded.address, 0x801F2340u);
  EXPECT_EQ(folded.dest_reg, Register::kA0);
  EXPECT_EQ(folded.hi_inst_index, 0u);
  EXPECT_EQ(folded.lo_inst_index, 1u);

  EXPECT_TRUE(folder.IsFoldedHi(0));
  EXPECT_FALSE(folder.IsFoldedHi(1));
  EXPECT_THAT(folder.GetFoldedLo(1), NotNull());
  EXPECT_EQ(folder.GetFoldedLo(0), nullptr);
}

TEST(SymbolFolderTest, ConstantLiteralPair) {
  // lui $t0, 0x1234
  // ori $t0, $t0, 0x5678
  std::vector<uint32_t> words = {
      0x3C081234,  // 0: lui $t0, 0x1234
      0x35085678,  // 1: ori $t0, $t0, 0x5678
  };

  auto insts = DecodeSequence(words);
  SymbolFolder folder;
  folder.Fold(insts);

  ASSERT_THAT(folder.AllFolded(), SizeIs(1));
  const auto& folded = folder.AllFolded()[0];
  EXPECT_EQ(folded.type, FoldedPatternType::kConstantLiteral);
  EXPECT_EQ(folded.address, 0x12345678u);
  EXPECT_EQ(folded.dest_reg, Register::kT0);
}

TEST(SymbolFolderTest, GlobalLoadAndStoreWithSignedOffset) {
  // 0: lui $at, 0x8020
  // 1: lw  $v0, -0x7FE0($at)  (-32736 -> 0x80200000 - 0x7FE0 = 0x801F8020)
  // 2: lui $t0, 0x8005
  // 3: sw  $a0, 0x1000($t0)
  std::vector<uint32_t> words = {
      0x3C018020,  // 0: lui $at, 0x8020
      0x8C228020,  // 1: lw  $v0, 0x8020($at) (imm = -0x7FE0)
      0x3C088005,  // 2: lui $t0, 0x8005
      0xAD041000,  // 3: sw  $a0, 0x1000($t0)
  };

  auto insts = DecodeSequence(words);
  SymbolFolder folder;
  folder.Fold(insts);

  ASSERT_THAT(folder.AllFolded(), SizeIs(2));

  // Global load
  const auto& load_folded = folder.AllFolded()[0];
  EXPECT_EQ(load_folded.type, FoldedPatternType::kGlobalLoad);
  EXPECT_EQ(load_folded.address, 0x801F8020u);
  EXPECT_EQ(load_folded.dest_reg, Register::kV0);

  // Global store
  const auto& store_folded = folder.AllFolded()[1];
  EXPECT_EQ(store_folded.type, FoldedPatternType::kGlobalStore);
  EXPECT_EQ(store_folded.address, 0x80051000u);
  EXPECT_EQ(store_folded.src_reg, Register::kA0);
}

TEST(SymbolFolderTest, SymbolNameResolutionWithIndex) {
  std::string textproto = R"pb(
    entries { name: "g_audio_status" address: 0x801F2340 type: SYMBOL_DATA size: 64 }
  )pb";

  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x3C01801F,  // 0: lui $at, 0x801F
      0x24242340,  // 1: addiu $a0, $at, 0x2340
  };

  auto insts = DecodeSequence(words);
  SymbolFolder folder;
  folder.Fold(insts, &(*index_or));

  ASSERT_THAT(folder.AllFolded(), SizeIs(1));
  EXPECT_EQ(folder.AllFolded()[0].symbol_name, "g_audio_status");
}

}  // namespace
}  // namespace rom_nom_nom
