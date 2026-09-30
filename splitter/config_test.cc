#include "splitter/config.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

constexpr std::string_view kValidConfigText = R"pb(

  game_name: "Harvest Moon 64 (USA)"
  sha1: "90631460f1876a14849df0541d534012b410a34c"
  basename: "harvest-moon-64"
  symbol_files: "symbols/harvest-moon-64.txt"

  segments { name: "header" type: SEGMENT_HEADER rom_start: 0x0 rom_end: 0x40 }

  segments { name: "ipl3" type: SEGMENT_BIN rom_start: 0x40 rom_end: 0x1000 }

  segments {
    name: "main"
    type: SEGMENT_CODE
    rom_start: 0x1050
    rom_end: 0x100000
    vram: 0x80025C50
    bss_size: 0x113870

    subsegments { rom_start: 0x1050 type: SUBSEGMENT_C name: "boot" vram: 0x80025C50 }
    subsegments { rom_start: 0x1190 type: SUBSEGMENT_C name: "main" vram: 0x80025D90 }
    subsegments { rom_start: 0x1CF0 type: SUBSEGMENT_ASM name: "graphic_1" }
  }

  c_flags {
    key: "default"
    value { flags: [ "-O2", "-mips2", "-mcpu=r4000", "-Wa,-g" ] }
  }
  c_flags {
    key: "boot"
    value { flags: [ "-O0" ] }
  }
)pb";

TEST(ConfigTest, ParseValidTextproto) {
  auto config_or = ParseSplitConfig(kValidConfigText);
  ASSERT_TRUE(config_or.ok()) << config_or.status();

  const SplitConfig& config = *config_or;
  EXPECT_EQ(config.game_name(), "Harvest Moon 64 (USA)");
  EXPECT_EQ(config.sha1(), "90631460f1876a14849df0541d534012b410a34c");
  EXPECT_EQ(config.basename(), "harvest-moon-64");
  ASSERT_EQ(config.symbol_files_size(), 1);
  EXPECT_EQ(config.symbol_files(0), "symbols/harvest-moon-64.txt");

  ASSERT_EQ(config.segments_size(), 3);
  EXPECT_EQ(config.segments(0).name(), "header");
  EXPECT_EQ(config.segments(0).type(), SEGMENT_HEADER);
  EXPECT_EQ(config.segments(0).rom_start(), 0x0u);

  EXPECT_EQ(config.segments(1).name(), "ipl3");
  EXPECT_EQ(config.segments(1).type(), SEGMENT_BIN);
  EXPECT_EQ(config.segments(1).rom_start(), 0x40u);

  const Segment& main_seg = config.segments(2);
  EXPECT_EQ(main_seg.name(), "main");
  EXPECT_EQ(main_seg.type(), SEGMENT_CODE);
  EXPECT_EQ(main_seg.rom_start(), 0x1050u);
  EXPECT_EQ(main_seg.vram(), 0x80025C50u);
  EXPECT_EQ(main_seg.bss_size(), 0x113870u);

  ASSERT_EQ(main_seg.subsegments_size(), 3);
  EXPECT_EQ(main_seg.subsegments(0).name(), "boot");
  EXPECT_EQ(main_seg.subsegments(0).type(), SUBSEGMENT_C);
  EXPECT_EQ(main_seg.subsegments(0).rom_start(), 0x1050u);
  EXPECT_EQ(main_seg.subsegments(1).name(), "main");
  EXPECT_EQ(main_seg.subsegments(1).type(), SUBSEGMENT_C);
  EXPECT_EQ(main_seg.subsegments(2).name(), "graphic_1");
  EXPECT_EQ(main_seg.subsegments(2).type(), SUBSEGMENT_ASM);

  EXPECT_TRUE(config.c_flags().contains("default"));
  EXPECT_TRUE(config.c_flags().contains("boot"));
  EXPECT_EQ(config.c_flags().at("boot").flags(0), "-O0");
}

TEST(ConfigTest, RejectInvalidSyntax) {
  constexpr std::string_view kBadSyntax = "invalid_field_123: [";
  auto config_or = ParseSplitConfig(kBadSyntax);
  EXPECT_FALSE(config_or.ok());
  EXPECT_EQ(config_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ConfigTest, RejectMissingRequiredFields) {
  constexpr std::string_view kMissingName = R"pb(
    sha1: "90631460f1876a14849df0541d534012b410a34c"
    basename: "harvest-moon-64"
    segments { name: "header" type: SEGMENT_HEADER rom_start: 0x0 }
  )pb";
  EXPECT_FALSE(ParseSplitConfig(kMissingName).ok());

  constexpr std::string_view kMissingSha1 = R"pb(
    game_name: "Harvest Moon 64 (USA)"
    basename: "harvest-moon-64"
    segments { name: "header" type: SEGMENT_HEADER rom_start: 0x0 }
  )pb";
  EXPECT_FALSE(ParseSplitConfig(kMissingSha1).ok());

  constexpr std::string_view kMissingSegments = R"pb(
    game_name: "Harvest Moon 64 (USA)"
    sha1: "90631460f1876a14849df0541d534012b410a34c"
    basename: "harvest-moon-64"
  )pb";
  EXPECT_FALSE(ParseSplitConfig(kMissingSegments).ok());
}

TEST(ConfigTest, RejectNonMonotonicSegments) {
  constexpr std::string_view kOutOfOrder = R"pb(
    game_name: "Test"
    sha1: "1234567890123456789012345678901234567890"
    basename: "test"
    segments { name: "s2" type: SEGMENT_BIN rom_start: 0x1000 }
    segments { name: "s1" type: SEGMENT_BIN rom_start: 0x500 }
  )pb";
  EXPECT_FALSE(ParseSplitConfig(kOutOfOrder).ok());
}

TEST(ConfigTest, LoadFromFile) {
  const std::filesystem::path temp_path =
      std::filesystem::temp_directory_path() / "test_config.textproto";
  {
    std::ofstream out(temp_path);
    out << kValidConfigText;
  }

  auto config_or = LoadSplitConfig(temp_path);
  ASSERT_TRUE(config_or.ok()) << config_or.status();
  EXPECT_EQ(config_or->game_name(), "Harvest Moon 64 (USA)");

  std::filesystem::remove(temp_path);

  // Non-existent file
  auto bad_file_or = LoadSplitConfig("non_existent_config.textproto");
  EXPECT_FALSE(bad_file_or.ok());
  EXPECT_EQ(bad_file_or.status().code(), absl::StatusCode::kNotFound);
}

TEST(ConfigTest, AllowBssSubsegmentWithoutRomStart) {
  constexpr std::string_view kBssSubsegmentConfig = R"pb(
    game_name: "Test Game"
    sha1: "dummy_sha1"
    basename: "test_game"
    segments {
      name: "main"
      type: SEGMENT_CODE
      rom_start: 0x1000
      rom_end: 0x5000
      vram: 0x80001000
      subsegments { rom_start: 0x1000 type: SUBSEGMENT_ASM name: "code1" }
      subsegments { rom_start: 0x3000 type: SUBSEGMENT_DATA name: "data1" }
      subsegments { type: SUBSEGMENT_BSS name: "bss1" vram: 0x80005000 bss_size: 0x1000 }
    }
  )pb";
  auto config_or = ParseSplitConfig(kBssSubsegmentConfig);
  ASSERT_TRUE(config_or.ok()) << config_or.status();
  const auto& main_seg = config_or->segments(0);
  ASSERT_EQ(main_seg.subsegments_size(), 3);
  EXPECT_EQ(main_seg.subsegments(2).type(), SUBSEGMENT_BSS);
  EXPECT_EQ(main_seg.subsegments(2).vram(), 0x80005000u);
  EXPECT_EQ(main_seg.subsegments(2).bss_size(), 0x1000u);
}

TEST(ConfigTest, AllowDedicatedBssSegmentWithoutRomStart) {
  constexpr std::string_view kBssSegmentConfig = R"pb(
    game_name: "Test Game"
    sha1: "dummy_sha1"
    basename: "test_game"
    segments { name: "code" type: SEGMENT_CODE rom_start: 0x1000 rom_end: 0x5000 vram: 0x80001000 }
    segments { name: "bss" type: SEGMENT_BSS vram: 0x80005000 bss_size: 0x2000 }
  )pb";
  auto config_or = ParseSplitConfig(kBssSegmentConfig);
  ASSERT_TRUE(config_or.ok()) << config_or.status();
  ASSERT_EQ(config_or->segments_size(), 2);
  EXPECT_EQ(config_or->segments(1).type(), SEGMENT_BSS);
  EXPECT_EQ(config_or->segments(1).vram(), 0x80005000u);
}

}  // namespace
}  // namespace rom_nom_nom
