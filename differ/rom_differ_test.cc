#include "differ/rom_differ.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(RomDifferTest, ComputeWordLcsEmptySequences) {
  std::vector<uint32_t> empty;
  std::vector<uint32_t> words = {1, 2, 3};
  EXPECT_EQ(RomDiffer::ComputeWordLcs(empty, empty), 0u);
  EXPECT_EQ(RomDiffer::ComputeWordLcs(empty, words), 0u);
  EXPECT_EQ(RomDiffer::ComputeWordLcs(words, empty), 0u);
}

TEST(RomDifferTest, ComputeWordLcsIdenticalSequences) {
  std::vector<uint32_t> words = {0x27BDFFE0, 0xAFBF001C, 0xAFBE0018, 0x03A0F021};
  EXPECT_EQ(RomDiffer::ComputeWordLcs(words, words), 4u);
}

TEST(RomDifferTest, ComputeWordLcsDisjointSequences) {
  std::vector<uint32_t> target = {1, 2, 3};
  std::vector<uint32_t> built = {4, 5, 6};
  EXPECT_EQ(RomDiffer::ComputeWordLcs(target, built), 0u);
}

TEST(RomDifferTest, ComputeWordLcsWithPrefixAndSuffix) {
  // Common prefix: {10, 20}
  // Target middle: {1, 2, 3}
  // Built middle:  {1, 9, 3}  (LCS of middle is {1, 3} = 2)
  // Common suffix: {80, 90}
  std::vector<uint32_t> target = {10, 20, 1, 2, 3, 80, 90};
  std::vector<uint32_t> built = {10, 20, 1, 9, 3, 80, 90};
  EXPECT_EQ(RomDiffer::ComputeWordLcs(target, built), 6u);
}

TEST(RomDifferTest, ComputeWordLcsInsertionsAndDeletions) {
  // Target: A, B, C, D, E, F
  // Built:     B, X, D, Y, F  (LCS is B, D, F = 3)
  std::vector<uint32_t> target = {1, 2, 3, 4, 5, 6};
  std::vector<uint32_t> built = {2, 99, 4, 88, 6};
  EXPECT_EQ(RomDiffer::ComputeWordLcs(target, built), 3u);
}

class RomDifferEndToEndTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "rom_differ_test";
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override {
    std::error_code error_code;
    std::filesystem::remove_all(test_dir_, error_code);
  }

  std::filesystem::path test_dir_;
};

TEST_F(RomDifferEndToEndTest, StructuralValidationFailsOnSizeMismatch) {
  std::filesystem::path target_path = test_dir_ / "target.z64";
  std::filesystem::path built_path = test_dir_ / "built.z64";

  {
    std::ofstream target_file(target_path, std::ios::binary);
    std::vector<char> buffer(100, 0);
    target_file.write(buffer.data(), buffer.size());
  }
  {
    std::ofstream built_file(built_path, std::ios::binary);
    std::vector<char> buffer(80, 0);
    built_file.write(buffer.data(), buffer.size());
  }

  RomDiffer differ(RomDiffOptions{
      .base_rom_path = target_path,
      .candidate_rom_path = built_path,
  });

  auto result_or = differ.Diff();
  ASSERT_TRUE(result_or.ok());
  EXPECT_FALSE(result_or->success);
  EXPECT_FALSE(result_or->structural.is_valid);
  EXPECT_FALSE(result_or->structural.size_matches);
}

TEST_F(RomDifferEndToEndTest, StructuralValidationFailsOnMagicMismatch) {
  std::filesystem::path target_path = test_dir_ / "target.z64";
  std::filesystem::path built_path = test_dir_ / "built.z64";

  std::vector<uint8_t> target_bytes(1024, 0);
  target_bytes[0] = 0x80;
  target_bytes[1] = 0x37;
  target_bytes[2] = 0x12;
  target_bytes[3] = 0x40;

  std::vector<uint8_t> built_bytes = target_bytes;
  built_bytes[0] = 0x00;  // Corrupt magic

  {
    std::ofstream target_file(target_path, std::ios::binary);
    target_file.write(reinterpret_cast<const char*>(target_bytes.data()), target_bytes.size());
  }
  {
    std::ofstream built_file(built_path, std::ios::binary);
    built_file.write(reinterpret_cast<const char*>(built_bytes.data()), built_bytes.size());
  }

  RomDiffer differ(RomDiffOptions{
      .base_rom_path = target_path,
      .candidate_rom_path = built_path,
  });

  auto result_or = differ.Diff();
  ASSERT_TRUE(result_or.ok());
  EXPECT_FALSE(result_or->success);
  EXPECT_FALSE(result_or->structural.header_magic_matches);
}

TEST_F(RomDifferEndToEndTest, MatchesIdenticalRoms) {
  std::filesystem::path target_path = test_dir_ / "target.z64";
  std::filesystem::path built_path = test_dir_ / "built.z64";

  std::vector<uint8_t> bytes(0x2000, 0);
  // Valid header magic & entrypoint
  bytes[0] = 0x80;
  bytes[1] = 0x37;
  bytes[2] = 0x12;
  bytes[3] = 0x40;
  bytes[8] = 0x80;
  bytes[9] = 0x02;
  bytes[10] = 0x5C;
  bytes[11] = 0x00;

  for (size_t i = 0x1050; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }

  {
    std::ofstream target_file(target_path, std::ios::binary);
    target_file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  {
    std::ofstream built_file(built_path, std::ios::binary);
    built_file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  RomDiffer differ(RomDiffOptions{
      .base_rom_path = target_path,
      .candidate_rom_path = built_path,
      .use_color = false,
  });

  auto result_or = differ.Diff();
  ASSERT_TRUE(result_or.ok());
  EXPECT_TRUE(result_or->success);
  EXPECT_TRUE(result_or->structural.is_valid);
  EXPECT_EQ(result_or->structural.target_sha1, result_or->structural.built_sha1);
  EXPECT_DOUBLE_EQ(result_or->overall_match_percentage, 100.0);
}

TEST_F(RomDifferEndToEndTest, StructuralValidationSucceedsWithShiftedAssetSegments) {
  std::filesystem::path target_path = test_dir_ / "target.z64";
  std::filesystem::path built_path = test_dir_ / "built.z64";
  std::filesystem::path config_path = test_dir_ / "config.textproto";

  const std::string config_content = R"(
game_name: "test"
sha1: "0000000000000000000000000000000000000000"
basename: "test"
segments {
  name: "main"
  rom_start: 0x1050
  rom_end: 0x1200
  type: SEGMENT_CODE
  subsegments {
    name: "main"
    rom_start: 0x1050
    type: SUBSEGMENT_C
  }
}
segments {
  name: "assets"
  rom_start: 0x1200
  rom_end: 0x1400
  type: SEGMENT_DATA
}
)";
  {
    std::ofstream config_file(config_path);
    config_file << config_content;
  }

  std::vector<uint8_t> target_bytes(0x1400, 0);
  target_bytes[0] = 0x80;
  target_bytes[1] = 0x37;
  target_bytes[2] = 0x12;
  target_bytes[3] = 0x40;
  target_bytes[8] = 0x80;
  target_bytes[9] = 0x02;
  target_bytes[10] = 0x5C;
  target_bytes[11] = 0x00;

  for (size_t i = 0x1050; i < 0x1200; ++i) {
    target_bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }
  for (size_t i = 0x1200; i < 0x1400; ++i) {
    target_bytes[i] = static_cast<uint8_t>(0x5A);
  }

  // Built ROM has 0x20 additional bytes in the code segment (+0x20 shift)
  std::vector<uint8_t> built_bytes(0x1420, 0);
  built_bytes[0] = 0x80;
  built_bytes[1] = 0x37;
  built_bytes[2] = 0x12;
  built_bytes[3] = 0x40;
  built_bytes[8] = 0x80;
  built_bytes[9] = 0x02;
  built_bytes[10] = 0x5C;
  built_bytes[11] = 0x00;

  for (size_t i = 0x1050; i < 0x1220; ++i) {
    built_bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }
  // Shifted assets start at 0x1220
  for (size_t i = 0x1220; i < 0x1420; ++i) {
    built_bytes[i] = static_cast<uint8_t>(0x5A);
  }

  {
    std::ofstream target_file(target_path, std::ios::binary);
    target_file.write(reinterpret_cast<const char*>(target_bytes.data()), target_bytes.size());
  }
  {
    std::ofstream built_file(built_path, std::ios::binary);
    built_file.write(reinterpret_cast<const char*>(built_bytes.data()), built_bytes.size());
  }

  // 1. With allow_code_shift = true (default)
  {
    RomDiffer differ(RomDiffOptions{
        .base_rom_path = target_path,
        .candidate_rom_path = built_path,
        .config_path = config_path,
        .use_color = false,
        .allow_code_shift = true,
    });

    auto result_or = differ.Diff();
    ASSERT_TRUE(result_or.ok());
    EXPECT_TRUE(result_or->success);
    EXPECT_TRUE(result_or->structural.is_valid);
    EXPECT_TRUE(result_or->structural.code_shift_matches);
    EXPECT_FALSE(result_or->structural.size_matches);
    ASSERT_EQ(result_or->asset_segments.size(), 1u);
    EXPECT_EQ(result_or->asset_segments[0].shift_offset, 0x20);
    EXPECT_DOUBLE_EQ(result_or->asset_segments[0].MatchPercentage(), 100.0);
  }

  // 2. With allow_code_shift = false
  {
    RomDiffer differ(RomDiffOptions{
        .base_rom_path = target_path,
        .candidate_rom_path = built_path,
        .config_path = config_path,
        .use_color = false,
        .allow_code_shift = false,
    });

    auto result_or = differ.Diff();
    ASSERT_TRUE(result_or.ok());
    EXPECT_FALSE(result_or->success);
    EXPECT_FALSE(result_or->structural.is_valid);
    EXPECT_FALSE(result_or->structural.code_shift_matches);
    EXPECT_FALSE(result_or->structural.size_matches);
  }

  // 3. With require_bit_exact = true (structurally valid, but fails bit-exactness)
  {
    RomDiffer differ(RomDiffOptions{
        .base_rom_path = target_path,
        .candidate_rom_path = built_path,
        .config_path = config_path,
        .use_color = false,
        .allow_code_shift = true,
        .require_bit_exact = true,
    });

    auto result_or = differ.Diff();
    ASSERT_TRUE(result_or.ok());
    EXPECT_FALSE(result_or->success);
    EXPECT_TRUE(result_or->structural.is_valid);
    EXPECT_FALSE(result_or->structural.exact_sha1_match);
  }
}

TEST_F(RomDifferEndToEndTest, TargetRomSmallerThanFirstAssetStartDoesNotUnderflow) {
  std::filesystem::path target_path = test_dir_ / "target_short.z64";
  std::filesystem::path built_path = test_dir_ / "built_short.z64";
  std::filesystem::path config_path = test_dir_ / "config_short.textproto";

  const std::string config_content = R"(
game_name: "test"
sha1: "0000000000000000000000000000000000000000"
basename: "test"
segments {
  name: "main"
  rom_start: 0x1050
  rom_end: 0x1200
  type: SEGMENT_CODE
}
segments {
  name: "assets"
  rom_start: 0x2000
  rom_end: 0x2400
  type: SEGMENT_DATA
}
)";
  {
    std::ofstream config_file(config_path);
    config_file << config_content;
  }

  // Target ROM size (0x1500) is smaller than first_retail_asset_start (0x2000)
  std::vector<uint8_t> target_bytes(0x1500, 0);
  target_bytes[0] = 0x80;
  target_bytes[1] = 0x37;
  target_bytes[2] = 0x12;
  target_bytes[3] = 0x40;
  target_bytes[8] = 0x80;
  target_bytes[9] = 0x02;
  target_bytes[10] = 0x5C;
  target_bytes[11] = 0x00;

  {
    std::ofstream target_file(target_path, std::ios::binary);
    target_file.write(reinterpret_cast<const char*>(target_bytes.data()), target_bytes.size());
  }
  {
    std::ofstream built_file(built_path, std::ios::binary);
    built_file.write(reinterpret_cast<const char*>(target_bytes.data()), target_bytes.size());
  }

  RomDiffer differ(RomDiffOptions{
      .base_rom_path = target_path,
      .candidate_rom_path = built_path,
      .config_path = config_path,
      .use_color = false,
  });

  auto result_or = differ.Diff();
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->success);
  ASSERT_EQ(result_or->asset_segments.size(), 1u);
  EXPECT_EQ(result_or->asset_segments[0].matching_bytes, 0u);
}

TEST_F(RomDifferEndToEndTest, BssSegmentPrecedingAssetSegmentIsIgnored) {
  std::filesystem::path target_path = test_dir_ / "target_bss.z64";
  std::filesystem::path built_path = test_dir_ / "built_bss.z64";
  std::filesystem::path config_path = test_dir_ / "config_bss.textproto";

  const std::string config_content = R"(
game_name: "test"
sha1: "0000000000000000000000000000000000000000"
basename: "test"
segments {
  name: "main"
  rom_start: 0x1050
  rom_end: 0x1200
  type: SEGMENT_CODE
}
segments {
  name: "main_bss"
  rom_start: 0x1200
  rom_end: 0x1200
  type: SEGMENT_BSS
  bss_size: 0x8000
}
segments {
  name: "assets"
  rom_start: 0x1200
  rom_end: 0x1400
  type: SEGMENT_DATA
}
)";
  {
    std::ofstream config_file(config_path);
    config_file << config_content;
  }

  std::vector<uint8_t> target_bytes(0x1400, 0);
  target_bytes[0] = 0x80;
  target_bytes[1] = 0x37;
  target_bytes[2] = 0x12;
  target_bytes[3] = 0x40;
  target_bytes[8] = 0x80;
  target_bytes[9] = 0x02;
  target_bytes[10] = 0x5C;
  target_bytes[11] = 0x00;

  for (size_t i = 0x1050; i < 0x1200; ++i) {
    target_bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }
  for (size_t i = 0x1200; i < 0x1400; ++i) {
    target_bytes[i] = static_cast<uint8_t>(0xA5);
  }

  std::vector<uint8_t> built_bytes(0x1420, 0);
  built_bytes[0] = 0x80;
  built_bytes[1] = 0x37;
  built_bytes[2] = 0x12;
  built_bytes[3] = 0x40;
  built_bytes[8] = 0x80;
  built_bytes[9] = 0x02;
  built_bytes[10] = 0x5C;
  built_bytes[11] = 0x00;

  for (size_t i = 0x1050; i < 0x1220; ++i) {
    built_bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }
  for (size_t i = 0x1220; i < 0x1420; ++i) {
    built_bytes[i] = static_cast<uint8_t>(0xA5);
  }

  {
    std::ofstream target_file(target_path, std::ios::binary);
    target_file.write(reinterpret_cast<const char*>(target_bytes.data()), target_bytes.size());
  }
  {
    std::ofstream built_file(built_path, std::ios::binary);
    built_file.write(reinterpret_cast<const char*>(built_bytes.data()), built_bytes.size());
  }

  RomDiffer differ(RomDiffOptions{
      .base_rom_path = target_path,
      .candidate_rom_path = built_path,
      .config_path = config_path,
      .use_color = false,
      .allow_code_shift = true,
  });

  auto result_or = differ.Diff();
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->success);
  EXPECT_TRUE(result_or->structural.code_shift_matches);
  ASSERT_EQ(result_or->asset_segments.size(), 1u);
  EXPECT_EQ(result_or->asset_segments[0].segment_name, "assets");
  EXPECT_EQ(result_or->asset_segments[0].shift_offset, 0x20);
  EXPECT_DOUBLE_EQ(result_or->asset_segments[0].MatchPercentage(), 100.0);
}

TEST_F(RomDifferEndToEndTest, ComputesPerFunctionStructuralReadinessMetrics) {
  std::filesystem::path target_path = test_dir_ / "target_readiness.z64";
  std::filesystem::path built_path = test_dir_ / "built_readiness.z64";
  std::filesystem::path config_path = test_dir_ / "config_readiness.textproto";

  const std::string config_content = R"(
game_name: "test"
sha1: "0000000000000000000000000000000000000000"
basename: "test"
segments {
  name: "main"
  rom_start: 0x1050
  rom_end: 0x1060
  vram: 0x80025C00
  type: SEGMENT_CODE
  subsegments {
    name: "main"
    rom_start: 0x1050
    type: SUBSEGMENT_C
  }
}
)";
  {
    std::ofstream config_file(config_path);
    config_file << config_content;
  }

  auto write_word = [](std::vector<uint8_t>& buf, size_t offset, uint32_t word) {
    buf[offset + 0] = static_cast<uint8_t>((word >> 24) & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((word >> 16) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((word >> 8) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>(word & 0xFF);
  };

  std::vector<uint8_t> target_bytes(0x1060, 0);
  write_word(target_bytes, 0, 0x80371240);
  write_word(target_bytes, 8, 0x80025C00);
  // Function at 0x1050: addiu $v0, $a0, 1; jr $ra; nop; nop
  write_word(target_bytes, 0x1050, 0x24820001);
  write_word(target_bytes, 0x1054, 0x03E00008);
  write_word(target_bytes, 0x1058, 0x00000000);
  write_word(target_bytes, 0x105C, 0x00000000);

  std::vector<uint8_t> built_bytes = target_bytes;
  // Built function uses different immediate (addiu $v0, $a0, 2)
  write_word(built_bytes, 0x1050, 0x24820002);

  {
    std::ofstream target_file(target_path, std::ios::binary);
    target_file.write(reinterpret_cast<const char*>(target_bytes.data()), target_bytes.size());
  }
  {
    std::ofstream built_file(built_path, std::ios::binary);
    built_file.write(reinterpret_cast<const char*>(built_bytes.data()), built_bytes.size());
  }

  RomDiffer differ(RomDiffOptions{
      .base_rom_path = target_path,
      .candidate_rom_path = built_path,
      .config_path = config_path,
      .use_color = false,
  });

  auto result_or = differ.Diff();
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_EQ(result_or->readiness.total_functions, 1u);
  EXPECT_EQ(result_or->readiness.frame_size_matches, 1u);
  EXPECT_EQ(result_or->readiness.saved_registers_matches, 1u);
  EXPECT_EQ(result_or->readiness.cfg_topology_matches, 1u);
  EXPECT_EQ(result_or->readiness.structurally_isomorphic_functions, 1u);
  EXPECT_EQ(result_or->readiness.total_target_instructions, 3u);
  EXPECT_EQ(result_or->readiness.total_normalized_lcs_instructions, 3u);
}

}  // namespace
}  // namespace rom_nom_nom
