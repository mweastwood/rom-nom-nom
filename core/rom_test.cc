#include "core/rom.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/sha1.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

// Helper to construct a minimal valid 64-byte .z64 header buffer.
std::vector<uint8_t> CreateTestRomBuffer(size_t size = 128) {
  std::vector<uint8_t> buffer(size, 0);

  // Initial PI register / magic: 0x80371240
  buffer[0x00] = 0x80;
  buffer[0x01] = 0x37;
  buffer[0x02] = 0x12;
  buffer[0x03] = 0x40;

  // Clock rate: 0x0000000F
  buffer[0x04] = 0x00;
  buffer[0x05] = 0x00;
  buffer[0x06] = 0x00;
  buffer[0x07] = 0x0F;

  // Entry point PC: 0x80025C00
  buffer[0x08] = 0x80;
  buffer[0x09] = 0x02;
  buffer[0x0A] = 0x5C;
  buffer[0x0B] = 0x00;

  // Release offset: 0x00001444
  buffer[0x0C] = 0x00;
  buffer[0x0D] = 0x00;
  buffer[0x0E] = 0x14;
  buffer[0x0F] = 0x44;

  // CRC1: 0xD678B781
  buffer[0x10] = 0xD6;
  buffer[0x11] = 0x78;
  buffer[0x12] = 0xB7;
  buffer[0x13] = 0x81;

  // CRC2: 0x69F8B500
  buffer[0x14] = 0x69;
  buffer[0x15] = 0xF8;
  buffer[0x16] = 0xB5;
  buffer[0x17] = 0x00;

  // Game title (0x20): "HARVESTMOON64       "
  const std::string title = "HARVESTMOON64       ";
  for (size_t i = 0; i < 20; ++i) {
    buffer[0x20 + i] = static_cast<uint8_t>(title[i]);
  }

  // Media format (0x3B): 'N'
  buffer[0x3B] = 'N';

  // Game code (0x3C): "3Y"
  buffer[0x3C] = '3';
  buffer[0x3D] = 'Y';

  // Country code (0x3E): 'E' (USA)
  buffer[0x3E] = 'E';

  // Version (0x3F): 0
  buffer[0x3F] = 0x00;

  // Fill remaining bytes with known test pattern
  for (size_t i = 64; i < size; ++i) {
    buffer[i] = static_cast<uint8_t>(i & 0xFF);
  }

  return buffer;
}

TEST(RomTest, ParseValidHeaderFromBuffer) {
  auto buffer = CreateTestRomBuffer(128);
  auto rom_or = Rom::FromBuffer(buffer);
  ASSERT_TRUE(rom_or.ok()) << rom_or.status();

  const Rom& rom = *rom_or;
  EXPECT_EQ(rom.Size(), 128u);

  const RomHeader& header = rom.Header();
  EXPECT_EQ(header.endianness, RomEndianness::kBigEndian);
  EXPECT_EQ(header.initial_pi_reg, 0x80371240u);
  EXPECT_EQ(header.clock_rate, 0x0000000Fu);
  EXPECT_EQ(header.entry_point_pc, 0x80025C00u);
  EXPECT_EQ(header.release_offset, 0x00001444u);
  EXPECT_EQ(header.crc1, 0xD678B781u);
  EXPECT_EQ(header.crc2, 0x69F8B500u);
  EXPECT_EQ(header.game_title, "HARVESTMOON64");
  EXPECT_EQ(header.media_format, 'N');
  EXPECT_EQ(header.game_code, "3Y");
  EXPECT_EQ(header.country_code, 'E');
  EXPECT_EQ(header.version, 0x00);
}

TEST(RomTest, RejectTooSmallBuffer) {
  std::vector<uint8_t> small_buffer(63, 0);
  auto rom_or = Rom::FromBuffer(small_buffer);
  EXPECT_FALSE(rom_or.ok());
  EXPECT_EQ(rom_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RomTest, RejectByteSwappedV64Format) {
  auto buffer = CreateTestRomBuffer(64);
  // Byte-swapped magic: 0x37804012
  buffer[0] = 0x37;
  buffer[1] = 0x80;
  buffer[2] = 0x40;
  buffer[3] = 0x12;

  auto rom_or = Rom::FromBuffer(buffer);
  EXPECT_FALSE(rom_or.ok());
  EXPECT_EQ(rom_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(rom_or.status().message(), ".v64"));
}

TEST(RomTest, RejectLittleEndianN64Format) {
  auto buffer = CreateTestRomBuffer(64);
  // Little-endian magic: 0x40123780
  buffer[0] = 0x40;
  buffer[1] = 0x12;
  buffer[2] = 0x37;
  buffer[3] = 0x80;

  auto rom_or = Rom::FromBuffer(buffer);
  EXPECT_FALSE(rom_or.ok());
  EXPECT_EQ(rom_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(rom_or.status().message(), ".n64"));
}

TEST(RomTest, RejectUnknownFormat) {
  auto buffer = CreateTestRomBuffer(64);
  buffer[0] = 0x00;
  buffer[1] = 0x00;
  buffer[2] = 0x00;
  buffer[3] = 0x00;

  auto rom_or = Rom::FromBuffer(buffer);
  EXPECT_FALSE(rom_or.ok());
  EXPECT_EQ(rom_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RomTest, SlicingAndReading) {
  auto buffer = CreateTestRomBuffer(128);
  auto rom_or = Rom::FromBuffer(buffer);
  ASSERT_TRUE(rom_or.ok());
  const Rom& rom = *rom_or;

  // Slice
  auto slice_or = rom.Slice(64, 4);
  ASSERT_TRUE(slice_or.ok());
  EXPECT_EQ(slice_or->size(), 4u);
  EXPECT_EQ((*slice_or)[0], 64);
  EXPECT_EQ((*slice_or)[1], 65);
  EXPECT_EQ((*slice_or)[2], 66);
  EXPECT_EQ((*slice_or)[3], 67);

  // Slice out of range
  auto bad_slice_or = rom.Slice(120, 10);
  EXPECT_FALSE(bad_slice_or.ok());
  EXPECT_EQ(bad_slice_or.status().code(), absl::StatusCode::kOutOfRange);

  // SliceRange
  auto range_or = rom.SliceRange(64, 68);
  ASSERT_TRUE(range_or.ok());
  EXPECT_EQ(range_or->size(), 4u);
  EXPECT_EQ((*range_or)[0], 64);

  // SliceRange invalid
  auto bad_range_or = rom.SliceRange(68, 64);
  EXPECT_FALSE(bad_range_or.ok());
  EXPECT_EQ(bad_range_or.status().code(), absl::StatusCode::kOutOfRange);

  // Word, Half, Byte reading
  auto word_or = rom.ReadWord(0x00);
  ASSERT_TRUE(word_or.ok());
  EXPECT_EQ(*word_or, 0x80371240u);

  auto half_or = rom.ReadHalf(0x02);
  ASSERT_TRUE(half_or.ok());
  EXPECT_EQ(*half_or, 0x1240);

  auto byte_or = rom.ReadByte(0x03);
  ASSERT_TRUE(byte_or.ok());
  EXPECT_EQ(*byte_or, 0x40);

  // Out of range readers
  EXPECT_FALSE(rom.ReadWord(125).ok());
  EXPECT_FALSE(rom.ReadHalf(127).ok());
  EXPECT_FALSE(rom.ReadByte(128).ok());
}

TEST(RomTest, Sha1Calculation) {
  auto buffer = CreateTestRomBuffer(64);
  auto rom_or = Rom::FromBuffer(buffer);
  ASSERT_TRUE(rom_or.ok());
  const Rom& rom = *rom_or;

  std::string sha1 = rom.Sha1();
  EXPECT_EQ(sha1.size(), 40u);
  // Verify with core/sha1.h
  EXPECT_EQ(sha1, ComputeSha1Hex(absl::MakeConstSpan(buffer)));
}

TEST(RomTest, MoveSemantics) {
  auto buffer = CreateTestRomBuffer(128);
  auto rom1_or = Rom::FromBuffer(buffer);
  ASSERT_TRUE(rom1_or.ok());

  Rom rom1 = std::move(*rom1_or);
  EXPECT_EQ(rom1.Size(), 128u);

  Rom rom2 = std::move(rom1);
  EXPECT_EQ(rom2.Size(), 128u);
  EXPECT_EQ(rom1.Size(), 0u);
  EXPECT_TRUE(rom1.Data().empty());
}

TEST(RomTest, OpenFile) {
  const std::filesystem::path temp_rom_path =
      std::filesystem::temp_directory_path() / "test_rom_mmap.z64";

  auto buffer = CreateTestRomBuffer(256);
  {
    std::ofstream out(temp_rom_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
  }

  auto rom_or = Rom::OpenFile(temp_rom_path);
  ASSERT_TRUE(rom_or.ok()) << rom_or.status();

  EXPECT_EQ(rom_or->Size(), 256u);
  EXPECT_EQ(rom_or->Header().game_title, "HARVESTMOON64");
  EXPECT_EQ(rom_or->Sha1(), ComputeSha1Hex(absl::MakeConstSpan(buffer)));

  // Clean up
  std::filesystem::remove(temp_rom_path);

  // Non-existent file
  auto bad_file_or = Rom::OpenFile("non_existent_file_12345.z64");
  EXPECT_FALSE(bad_file_or.ok());
  EXPECT_EQ(bad_file_or.status().code(), absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace rom_nom_nom
