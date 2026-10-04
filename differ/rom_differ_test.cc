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

}  // namespace
}  // namespace rom_nom_nom
