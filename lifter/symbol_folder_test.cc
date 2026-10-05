#include "lifter/symbol_folder.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ::testing::Eq;
using ::testing::NotNull;
using ::testing::SizeIs;

TEST(SymbolFolderTest, AddressLoadPair) {
  // lui $at, 0x801F
  // addiu $a0, $at, 0x2340
  std::vector<uint32_t> words = {
      0x3C01801F,  // 0: lui $at, 0x801F
      0x24242340,  // 1: addiu $a0, $at, 0x2340
  };

  auto insts = *DecodeSequence(words);
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

  auto insts = *DecodeSequence(words);
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

  auto insts = *DecodeSequence(words);
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

  auto insts = *DecodeSequence(words);
  SymbolFolder folder;
  folder.Fold(insts, &(*index_or));

  ASSERT_THAT(folder.AllFolded(), SizeIs(1));
  EXPECT_EQ(folder.AllFolded()[0].symbol_name, "g_audio_status");
}

TEST(SymbolFolderTest, GlobalLoadAndStoreSynthesizeDataSymbolsWhenUnregistered) {
  // 0: lui $at, 0x801F
  // 1: lw  $v0, 0x2340($at)  -> 0x801F2340
  // 2: lui $t0, 0x8005
  // 3: sw  $a0, 0x1000($t0)  -> 0x80051000
  std::vector<uint32_t> words = {
      0x3C01801F,  // 0: lui $at, 0x801F
      0x8C222340,  // 1: lw  $v0, 0x2340($at)
      0x3C088005,  // 2: lui $t0, 0x8005
      0xAD041000,  // 3: sw  $a0, 0x1000($t0)
  };

  auto instructions = *DecodeSequence(words);
  SymbolIndex empty_symbol_index;
  SymbolFolder folder;
  folder.Fold(instructions, &empty_symbol_index);

  ASSERT_THAT(folder.AllFolded(), SizeIs(2));
  EXPECT_EQ(folder.AllFolded()[0].symbol_name, "D_801F2340");
  EXPECT_EQ(folder.AllFolded()[1].symbol_name, "D_80051000");
}

TEST(SymbolFolderTest, AddressLoadDefaultsToDataWhenSplitConfigNotProvided) {
  // 0: lui $at, 0x8008
  // 1: addiu $a0, $at, 0x1000  -> 0x80081000
  // 2: lui $at, 0x8015
  // 3: addiu $a1, $at, 0x2000  -> 0x80152000
  std::vector<uint32_t> words = {
      0x3C018008,  // 0: lui $at, 0x8008
      0x24241000,  // 1: addiu $a0, $at, 0x1000
      0x3C018015,  // 2: lui $at, 0x8015
      0x24252000,  // 3: addiu $a1, $at, 0x2000
  };

  auto instructions = *DecodeSequence(words);
  SymbolIndex empty_symbol_index;
  SymbolFolder folder;
  folder.Fold(instructions, &empty_symbol_index);

  ASSERT_THAT(folder.AllFolded(), SizeIs(2));
  EXPECT_EQ(folder.AllFolded()[0].symbol_name, "D_80081000");
  EXPECT_EQ(folder.AllFolded()[1].symbol_name, "D_80152000");
}

TEST(SymbolFolderTest, AddressLoadClassifiesCodeAndDataViaSplitConfig) {
  constexpr std::string_view kConfigText = R"pb(
    game_name: "Test Game"
    sha1: "dummy_sha1"
    basename: "test_game"
    segments {
      name: "main"
      type: SEGMENT_CODE
      rom_start: 0x1000
      rom_end: 0x5000
      vram: 0x80001000
      subsegments { rom_start: 0x1000 type: SUBSEGMENT_ASM name: "code_subseg" }
      subsegments { rom_start: 0x3000 type: SUBSEGMENT_DATA name: "data_subseg" }
    }
  )pb";
  auto config_or = ParseSplitConfig(kConfigText);
  ASSERT_TRUE(config_or.ok()) << config_or.status();
  const SplitConfig& config = *config_or;

  // Address in code subsegment: 0x80001500 (code -> func_80001500)
  // Address in data subsegment: 0x80003500 (data -> D_80003500)
  std::vector<uint32_t> words = {
      0x3C018000,  // 0: lui $at, 0x8000
      0x24241500,  // 1: addiu $a0, $at, 0x1500  -> 0x80001500
      0x3C018000,  // 2: lui $at, 0x8000
      0x24253500,  // 3: addiu $a1, $at, 0x3500  -> 0x80003500
  };

  auto instructions = *DecodeSequence(words);
  SymbolIndex empty_symbol_index;
  SymbolFolder folder;
  folder.Fold(instructions, &empty_symbol_index, &config);

  ASSERT_THAT(folder.AllFolded(), SizeIs(2));
  EXPECT_EQ(folder.AllFolded()[0].symbol_name, "func_80001500");
  EXPECT_EQ(folder.AllFolded()[1].symbol_name, "D_80003500");
}

TEST(SymbolFolderTest, SizedGlobalAccess) {
  // lui $at, 0x8020
  // sb  $v0, 0x4B38($at)
  // lui $t0, 0x8018
  // sh  $a0, 0x2BA0($t0)
  // lui $t1, 0x8025
  // lbu $v1, 0x0100($t1)
  std::vector<uint32_t> words = {
      0x3C018020,  // 0: lui $at, 0x8020
      0xA0224B38,  // 1: sb  $v0, 0x4B38($at)
      0x3C088018,  // 2: lui $t0, 0x8018
      0xA5042BA0,  // 3: sh  $a0, 0x2BA0($t0)
      0x3C098025,  // 4: lui $t1, 0x8025
      0x91230100,  // 5: lbu $v1, 0x0100($t1)
  };

  auto instructions = *DecodeSequence(words);
  SymbolFolder folder;
  folder.Fold(instructions);

  ASSERT_THAT(folder.AllFolded(), SizeIs(3));
  EXPECT_EQ(folder.AllFolded()[0].type, FoldedPatternType::kGlobalStore);
  EXPECT_EQ(folder.AllFolded()[0].access_type, "u8");
  EXPECT_EQ(folder.AllFolded()[0].hi_inst_index, 0u);
  EXPECT_EQ(folder.AllFolded()[0].lo_inst_index, 1u);

  EXPECT_EQ(folder.AllFolded()[1].type, FoldedPatternType::kGlobalStore);
  EXPECT_EQ(folder.AllFolded()[1].access_type, "u16");
  EXPECT_EQ(folder.AllFolded()[1].hi_inst_index, 2u);
  EXPECT_EQ(folder.AllFolded()[1].lo_inst_index, 3u);

  EXPECT_EQ(folder.AllFolded()[2].type, FoldedPatternType::kGlobalLoad);
  EXPECT_EQ(folder.AllFolded()[2].access_type, "u8");
  EXPECT_EQ(folder.AllFolded()[2].hi_inst_index, 4u);
  EXPECT_EQ(folder.AllFolded()[2].lo_inst_index, 5u);
}

}  // namespace
}  // namespace rom_nom_nom
