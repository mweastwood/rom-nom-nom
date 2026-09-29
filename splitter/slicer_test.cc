#include "splitter/slicer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "core/rom.h"
#include "gtest/gtest.h"
#include "splitter/config.h"
#include "splitter/config.pb.h"

namespace rom_nom_nom {
namespace {

// Helper to create a minimal 256-byte valid N64 test ROM buffer.
std::vector<uint8_t> CreateTestRom(size_t size = 256) {
  std::vector<uint8_t> buffer(size, 0);
  // Standard big-endian magic
  buffer[0x00] = 0x80;
  buffer[0x01] = 0x37;
  buffer[0x02] = 0x12;
  buffer[0x03] = 0x40;

  for (size_t i = 64; i < size; ++i) {
    buffer[i] = static_cast<uint8_t>(i & 0xFF);
  }
  return buffer;
}

constexpr std::string_view kTestConfigText = R"pb(
  game_name: "Test Game"
  sha1: "da39a3ee5e6b4b0d3255bfef95601890afd80709"
  basename: "test-game"

  segments {

    name: "header"
    type: SEGMENT_HEADER
    rom_start: 0x00
    rom_end: 0x40
  }

  segments { name: "ipl3" type: SEGMENT_BIN rom_start: 0x40 rom_end: 0x80 }

  segments {
    name: "main_code"
    type: SEGMENT_CODE
    rom_start: 0x80
    rom_end: 0xC0

    subsegments { rom_start: 0x80 type: SUBSEGMENT_ASM name: "engine_asm" }
    subsegments { rom_start: 0xA0 type: SUBSEGMENT_DATA name: "engine_data" }
  }

  segments {
    name: "audio_bank"
    type: SEGMENT_RODATA
    rom_start: 0xC0
    # rom_end is omitted (implicit to ROM end: 0x100)
  }

  segments { name: "ram_bss" type: SEGMENT_BSS rom_start: 0x100 rom_end: 0x200 }
)pb";

TEST(SlicerTest, PlanSlicesWithImplicitEndsAndSubsegments) {
  auto rom_or = Rom::FromBuffer(CreateTestRom(256));
  ASSERT_TRUE(rom_or.ok());

  auto config_or = ParseSplitConfig(kTestConfigText);
  ASSERT_TRUE(config_or.ok()) << config_or.status();

  SlicerOptions options;
  options.verify_sha1 = false;
  Slicer slicer(options);
  auto slices_or = slicer.PlanSlices(*rom_or, *config_or);
  ASSERT_TRUE(slices_or.ok()) << slices_or.status();

  const auto& slices = *slices_or;
  // Expected slices:
  // 1. "header" (0x00 - 0x40)
  // 2. "ipl3" (0x40 - 0x80)
  // 3. "engine_data" (0xA0 - 0xC0) [from code subsegment]
  // 4. "audio_bank" (0xC0 - 0x100) [implicit end to ROM size]
  // Note: "engine_asm" and "ram_bss" are excluded.
  ASSERT_EQ(slices.size(), 4u);

  EXPECT_EQ(slices[0].name, "header");
  EXPECT_EQ(slices[0].rom_start, 0x00u);
  EXPECT_EQ(slices[0].rom_end, 0x40u);
  EXPECT_EQ(slices[0].Size(), 0x40u);

  EXPECT_EQ(slices[1].name, "ipl3");
  EXPECT_EQ(slices[1].rom_start, 0x40u);
  EXPECT_EQ(slices[1].rom_end, 0x80u);

  EXPECT_EQ(slices[2].name, "engine_data");
  EXPECT_EQ(slices[2].rom_start, 0xA0u);
  EXPECT_EQ(slices[2].rom_end, 0xC0u);

  EXPECT_EQ(slices[3].name, "audio_bank");
  EXPECT_EQ(slices[3].rom_start, 0xC0u);
  EXPECT_EQ(slices[3].rom_end, 0x100u);
}

TEST(SlicerTest, SliceDataSpan) {
  auto rom_buffer = CreateTestRom(256);
  auto rom_or = Rom::FromBuffer(rom_buffer);
  ASSERT_TRUE(rom_or.ok());

  CarvedSlice slice;
  slice.name = "ipl3";
  slice.rom_start = 0x40;
  slice.rom_end = 0x50;

  Slicer slicer;
  auto data_or = slicer.SliceData(*rom_or, slice);
  ASSERT_TRUE(data_or.ok());
  EXPECT_EQ(data_or->size(), 16u);
  EXPECT_EQ((*data_or)[0], rom_buffer[0x40]);
  EXPECT_EQ((*data_or)[15], rom_buffer[0x4F]);
}

TEST(SlicerTest, SliceAllWritesFilesToDisk) {
  auto rom_or = Rom::FromBuffer(CreateTestRom(256));
  ASSERT_TRUE(rom_or.ok());

  auto config_or = ParseSplitConfig(kTestConfigText);
  ASSERT_TRUE(config_or.ok());

  const std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() / "slicer_test_output";
  std::filesystem::remove_all(temp_dir);

  SlicerOptions options;
  options.output_dir = temp_dir;
  options.verify_sha1 = false;
  options.write_incbin_asm = true;
  Slicer slicer(options);

  auto report_or = slicer.SliceAll(*rom_or, *config_or);
  ASSERT_TRUE(report_or.ok()) << report_or.status();

  EXPECT_EQ(report_or->total_files_written, 4u);
  EXPECT_EQ(report_or->total_bytes_carved, static_cast<size_t>(0x40 + 0x40 + 0x20 + 0x40));

  // Check that files exist on disk
  const std::filesystem::path game_dir = temp_dir / "test-game";
  EXPECT_TRUE(std::filesystem::exists(game_dir / "header.bin"));
  EXPECT_TRUE(std::filesystem::exists(game_dir / "ipl3.bin"));
  EXPECT_TRUE(std::filesystem::exists(game_dir / "engine_data.bin"));
  EXPECT_TRUE(std::filesystem::exists(game_dir / "audio_bank.bin"));

  // Check that .incbin assembly wrappers exist
  EXPECT_TRUE(std::filesystem::exists(game_dir / "header.s"));
  EXPECT_TRUE(std::filesystem::exists(game_dir / "ipl3.s"));

  // Clean up
  std::filesystem::remove_all(temp_dir);
}

TEST(SlicerTest, VerifySha1Check) {
  auto rom_or = Rom::FromBuffer(CreateTestRom(256));
  ASSERT_TRUE(rom_or.ok());

  constexpr std::string_view kConfigWithSha1 = R"pb(
    game_name: "Test Game"
    sha1: "0000000000000000000000000000000000000000"
    basename: "test-game"
    segments { name: "header" type: SEGMENT_HEADER rom_start: 0x0 rom_end: 0x40 }
  )pb";
  auto config_or = ParseSplitConfig(kConfigWithSha1);
  ASSERT_TRUE(config_or.ok());

  // With verification enabled -> fails
  SlicerOptions strict_opts;
  strict_opts.verify_sha1 = true;
  Slicer strict_slicer(strict_opts);
  EXPECT_FALSE(strict_slicer.PlanSlices(*rom_or, *config_or).ok());

  // With verification disabled -> succeeds
  SlicerOptions lax_opts;
  lax_opts.verify_sha1 = false;
  Slicer lax_slicer(lax_opts);
  EXPECT_TRUE(lax_slicer.PlanSlices(*rom_or, *config_or).ok());
}

TEST(SlicerTest, OutOfBoundsSegmentError) {
  auto rom_or = Rom::FromBuffer(CreateTestRom(128));
  ASSERT_TRUE(rom_or.ok());

  constexpr std::string_view kOutOfBoundsConfig = R"pb(
    game_name: "Test Game"
    sha1: "da39a3ee5e6b4b0d3255bfef95601890afd80709"
    basename: "test-game"
    segments { name: "big" type: SEGMENT_BIN rom_start: 0x0 rom_end: 0x200 }
  )pb";
  auto config_or = ParseSplitConfig(kOutOfBoundsConfig);
  ASSERT_TRUE(config_or.ok()) << config_or.status();

  SlicerOptions options;
  options.verify_sha1 = false;
  Slicer slicer(options);
  auto plan_or = slicer.PlanSlices(*rom_or, *config_or);
  EXPECT_FALSE(plan_or.ok());
  EXPECT_EQ(plan_or.status().code(), absl::StatusCode::kOutOfRange);
}

}  // namespace
}  // namespace rom_nom_nom
