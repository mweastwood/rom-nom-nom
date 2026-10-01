#include "differ/target_extractor.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/endian.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

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
    auto cfg_or = ParseSplitConfig(proto_cfg);
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
  SplitConfig config_;
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
  EXPECT_THAT(target.raw_words, ElementsAre(0x27BDFFE0u, 0x03E00008u, 0x00000000u));
  EXPECT_THAT(target.formatted_instructions,
              ElementsAre("addiu      $sp, $sp, -32", "jr         $ra", "nop"));
}

}  // namespace
}  // namespace rom_nom_nom
