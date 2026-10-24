#include "core/target_extractor.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/endian.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

class TargetExtractorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "target_extractor_test";
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_ / "config");
    std::filesystem::create_directories(test_dir_ / "symbols");
    std::filesystem::create_directories(test_dir_ / "roms");

    std::string proto_cfg = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "code"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x5000
  vram: 0x80020000
}
)";
    auto cfg_or = ParseGameConfig(proto_cfg);
    ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();
    config_ = *cfg_or;

    std::string proto_syms = R"(
entries {
  name: "TargetFunc"
  address: 0x80021000
  type: SYMBOL_FUNC
}
entries {
  name: "NextFunc"
  address: 0x8002100C
  type: SYMBOL_FUNC
}
)";
    auto syms_or = SymbolIndex::ParseFromTextproto(proto_syms);
    ASSERT_TRUE(syms_or.ok()) << syms_or.status();
    symbols_ = std::move(*syms_or);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
  GameConfig config_;
  SymbolIndex symbols_;
};

TEST_F(TargetExtractorTest, ResolvesVramFromVariousRepresentations) {
  TargetExtractor extractor({.repo_root = test_dir_, .game_name = "test-game"}, &config_,
                            &symbols_);

  // Symbol name
  EXPECT_EQ(extractor.ResolveVram("TargetFunc").value_or(0), 0x80021000u);

  // func_XXXXXXXX
  EXPECT_EQ(extractor.ResolveVram("func_80021000").value_or(0), 0x80021000u);

  // 0xXXXXXXXX
  EXPECT_EQ(extractor.ResolveVram("0x80021000").value_or(0), 0x80021000u);

  // Hex directly
  EXPECT_EQ(extractor.ResolveVram("80021000").value_or(0), 0x80021000u);

  // Unknown returns error
  EXPECT_FALSE(extractor.ResolveVram("UnknownSymbol").ok());
}

TEST_F(TargetExtractorTest, ConvertsVramToRomOffset) {
  TargetExtractor extractor({.repo_root = test_dir_, .game_name = "test-game"}, &config_,
                            &symbols_);

  // 0x80021000 is 0x1000 past segment vram 0x80020000.
  // segment rom_start is 0x1000 -> 0x1000 + 0x1000 = 0x2000.
  EXPECT_EQ(extractor.VramToRomOffset(0x80021000).value_or(0), 0x2000u);

  // Address outside segment returns error
  EXPECT_FALSE(extractor.VramToRomOffset(0x80010000).ok());
}

TEST_F(TargetExtractorTest, ExtractsTargetFromRomBuffer) {
  TargetExtractor extractor({.repo_root = test_dir_, .game_name = "test-game"}, &config_,
                            &symbols_);

  // Prepare ROM buffer: TargetFunc at offset 0x2000 (vram 0x80021000)
  // TargetFunc has 3 instructions:
  // 1. addiu $sp, $sp, -32 (0x27BDFFE0)
  // 2. jr $ra (0x03E00008)
  // 3. nop (0x00000000)
  std::vector<uint8_t> rom(0x5000, 0);
  std::vector<uint8_t> code;
  AppendBigEndian32(code, 0x27BDFFE0);
  AppendBigEndian32(code, 0x03E00008);
  AppendBigEndian32(code, 0x00000000);
  std::copy(code.begin(), code.end(), rom.begin() + 0x2000);

  auto target_or = extractor.ExtractFromRom(rom, "TargetFunc");
  ASSERT_TRUE(target_or.ok()) << target_or.status();

  const auto& target = *target_or;
  EXPECT_EQ(target.name, "TargetFunc");
  EXPECT_EQ(target.vram, 0x80021000u);
  EXPECT_EQ(target.rom_offset, 0x2000u);
  EXPECT_EQ(target.size, 12u);
  EXPECT_THAT(target.raw_words, ElementsAre(0x27BDFFE0u, 0x03E00008u, 0x00000000u));
  EXPECT_THAT(target.instructions, SizeIs(3));
}

TEST_F(TargetExtractorTest, ExtractsTargetFromAsmFiles) {
  std::filesystem::path asm_dir = test_dir_ / "asm" / "test-game";
  std::filesystem::create_directories(asm_dir);

  std::ofstream asm_file(asm_dir / "target.s");
  asm_file << R"(
glabel TargetFunc
/* 80021000 27BDFFE0 */  addiu      $sp, $sp, -32
/* 80021004 03E00008 */  jr         $ra
/* 80021008 00000000 */   nop
endlabel TargetFunc
)";
  asm_file.close();

  TargetExtractor extractor({.repo_root = test_dir_, .game_name = "test-game"}, &config_,
                            &symbols_);

  auto target_or = extractor.ExtractFromAsm("TargetFunc");
  ASSERT_TRUE(target_or.ok()) << target_or.status();

  const auto& target = *target_or;
  EXPECT_EQ(target.name, "TargetFunc");
  EXPECT_EQ(target.vram, 0x80021000u);
  EXPECT_EQ(target.size, 12u);
  EXPECT_THAT(target.formatted_instructions,
              ElementsAre("addiu      $sp, $sp, -32", "jr         $ra", "nop"));
}

TEST_F(TargetExtractorTest, ExtractsTargetFromCustomAsmDirWithGnuLabels) {
  std::filesystem::path custom_asm_dir = test_dir_ / "custom_asm";
  std::filesystem::create_directories(custom_asm_dir);

  std::ofstream asm_file(custom_asm_dir / "gnu_target.s");
  asm_file << R"(
.globl TargetFunc
.ent TargetFunc
TargetFunc:
/* 80021000 27BDFFE0 */  addiu      $sp, $sp, -32
/* 80021004 03E00008 */  jr         $ra
/* 80021008 00000000 */   nop
.end TargetFunc
)";
  asm_file.close();

  TargetExtractor extractor(
      {.repo_root = test_dir_, .game_name = "test-game", .asm_dir = custom_asm_dir}, &config_,
      &symbols_);

  auto target_or = extractor.ExtractFromAsm("TargetFunc");
  ASSERT_TRUE(target_or.ok()) << target_or.status();

  const auto& target = *target_or;
  EXPECT_EQ(target.name, "TargetFunc");
  EXPECT_EQ(target.vram, 0x80021000u);
  EXPECT_EQ(target.size, 12u);
  EXPECT_THAT(target.raw_words, ElementsAre(0x27BDFFE0u, 0x03E00008u, 0x00000000u));
  EXPECT_THAT(target.formatted_instructions,
              ElementsAre("addiu      $sp, $sp, -32", "jr         $ra", "nop"));
}

TEST_F(TargetExtractorTest, ReadRomWordReadsFromRomFile) {
  // Write a dummy ROM file with a known word at offset 0x2000 (vram 0x80021000)
  std::vector<uint8_t> rom(0x5000, 0);
  WriteBigEndian32(rom.data(), 0x80371240u);
  WriteBigEndian32(rom.data() + 0x2000, 0x80025D20u);

  std::filesystem::path rom_path = test_dir_ / "roms" / "test-game.z64";
  std::ofstream rom_file(rom_path, std::ios::binary);
  rom_file.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  rom_file.close();

  TargetExtractor extractor({.repo_root = test_dir_, .game_name = "test-game"}, &config_,
                            &symbols_);

  // Valid VRAM address mapped to 0x2000
  auto word = extractor.ReadRomWord(0x80021000);
  ASSERT_TRUE(word.has_value());
  EXPECT_EQ(*word, 0x80025D20u);

  // Unmapped VRAM address
  EXPECT_EQ(extractor.ReadRomWord(0x80010000), std::nullopt);

  // Address near end of file that cannot fit a 4-byte word (0x5000 is file size, offset 0x4FFE)
  // vram = 0x80020000 + (0x4FFE - 0x1000) = 0x80023FFE
  EXPECT_EQ(extractor.ReadRomWord(0x80023FFE), std::nullopt);
}

TEST_F(TargetExtractorTest, ReadRomWordReturnsNulloptWhenRomMissing) {
  // test_dir_ / "roms" / "test-game.z64" is not created
  TargetExtractor extractor({.repo_root = test_dir_, .game_name = "test-game"}, &config_,
                            &symbols_);
  EXPECT_EQ(extractor.ReadRomWord(0x80021000), std::nullopt);
}

TEST_F(TargetExtractorTest, EnsureRomLoadedAndPrescannedDiscoversJalEntrypoints) {
  // Construct a ROM image containing two functions in the "code" segment (rom_start=0x1000,
  // vram=0x80020000) TargetFunc at offset 0x2000 (vram 0x80021000):
  //   80021000: jal 0x80021010 (target: Func2) -> target26 = 0x80021010 >> 2 = 0x02008404
  //   80021004: nop
  //   80021008: jr $ra
  //   8002100C: nop
  // Func2 at offset 0x2010 (vram 0x80021010):
  //   80021010: addiu $v0, $zero, 42
  //   80021014: jr $ra
  //   80021018: nop
  std::vector<uint8_t> rom(0x5000, 0);
  WriteBigEndian32(rom.data(), 0x80371240u);
  uint32_t jal_word = 0x0C000000 | ((0x80021010u >> 2) & 0x03FFFFFF);
  WriteBigEndian32(rom.data() + 0x2000, jal_word);
  WriteBigEndian32(rom.data() + 0x2004, 0x00000000);  // nop
  WriteBigEndian32(rom.data() + 0x2008, 0x03E00008);  // jr $ra
  WriteBigEndian32(rom.data() + 0x200C, 0x00000000);  // nop
  WriteBigEndian32(rom.data() + 0x2010, 0x2402002A);  // addiu $v0, $zero, 42
  WriteBigEndian32(rom.data() + 0x2014, 0x03E00008);  // jr $ra
  WriteBigEndian32(rom.data() + 0x2018, 0x00000000);  // nop
  // Non-zero instruction at offset 0x201C (vram 0x8002101C) to bound trailing alignment NOPs
  WriteBigEndian32(rom.data() + 0x201C, 0x24020001);  // addiu $v0, $zero, 1

  std::filesystem::path rom_path = test_dir_ / "roms" / "test-game.z64";
  std::ofstream rom_file(rom_path, std::ios::binary);
  rom_file.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  rom_file.close();

  TargetExtractor extractor({.repo_root = test_dir_, .game_name = "test-game"}, &config_,
                            &symbols_);

  // Extract TargetFunc; pre-scanning will run and discover 0x80021010 as a known entrypoint
  auto target_or = extractor.ExtractTarget("TargetFunc");
  ASSERT_TRUE(target_or.ok()) << target_or.status();

  const auto& target = *target_or;
  EXPECT_EQ(target.name, "TargetFunc");
  EXPECT_EQ(target.vram, 0x80021000u);
  EXPECT_EQ(target.size, 16u);
  EXPECT_THAT(target.raw_words, ElementsAre(jal_word, 0x00000000u, 0x03E00008u, 0x00000000u));

  // Extract Func2 by VRAM address (which was discovered via jal pre-scan)
  auto func2_or = extractor.ExtractTarget("0x80021010");
  ASSERT_TRUE(func2_or.ok()) << func2_or.status();
  EXPECT_EQ(func2_or->vram, 0x80021010u);
  EXPECT_EQ(func2_or->size, 12u);
  EXPECT_THAT(func2_or->raw_words, ElementsAre(0x2402002A, 0x03E00008, 0x00000000));

  // Verify cached ROM bytes are reused for subsequent read
  auto word = extractor.ReadRomWord(0x80021010);
  ASSERT_TRUE(word.has_value());
  EXPECT_EQ(*word, 0x2402002Au);
}

TEST_F(TargetExtractorTest, VramToRomOffsetRejectsOutOfSegmentAddresses) {
  TargetExtractor configured_extractor({.repo_root = test_dir_, .game_name = "ogre-battle-64"},
                                       &config_, &symbols_);
  EXPECT_TRUE(configured_extractor.VramToRomOffset(0x80021000).ok());
  EXPECT_FALSE(configured_extractor.VramToRomOffset(0x800EAF1C).ok());
  EXPECT_FALSE(configured_extractor.VramToRomOffset(0x801AB76C).ok());

  TargetExtractor unconfigured_ob64_extractor(
      {.repo_root = test_dir_, .game_name = "ogre-battle-64"}, nullptr, &symbols_);
  EXPECT_FALSE(unconfigured_ob64_extractor.VramToRomOffset(0x80070C60).ok());
  EXPECT_FALSE(unconfigured_ob64_extractor.VramToRomOffset(0x800EAF1C).ok());

  TargetExtractor unconfigured_hm64_extractor(
      {.repo_root = test_dir_, .game_name = "harvest-moon-64"}, nullptr, &symbols_);
  EXPECT_FALSE(unconfigured_hm64_extractor.VramToRomOffset(0x80025C50).ok());
  EXPECT_FALSE(unconfigured_hm64_extractor.VramToRomOffset(0x801241C0).ok());
}

}  // namespace
}  // namespace rom_nom_nom
