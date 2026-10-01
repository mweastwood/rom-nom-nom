#include "core/endian.h"

#include <cstdint>
#include <vector>

#include "absl/types/span.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;

TEST(EndianTest, ReadsBigEndian16UnsignedAndSigned) {
  const std::vector<uint8_t> data = {0x12, 0x34, 0x80, 0x01, 0xFF, 0xFE};

  // Pointer overloads
  EXPECT_EQ(ReadBigEndian16(data.data()), 0x1234u);
  EXPECT_EQ(ReadBigEndian16(data.data() + 2), 0x8001u);
  EXPECT_EQ(ReadBigEndian16Signed(data.data()), 0x1234);
  EXPECT_EQ(ReadBigEndian16Signed(data.data() + 2), static_cast<int16_t>(0x8001));
  EXPECT_EQ(ReadBigEndian16Signed(data.data() + 4), -2);

  // Span overloads
  absl::Span<const uint8_t> span(data);
  EXPECT_EQ(ReadBigEndian16(span, 0), 0x1234u);
  EXPECT_EQ(ReadBigEndian16(span, 2), 0x8001u);
  EXPECT_EQ(ReadBigEndian16Signed(span, 0), 0x1234);
  EXPECT_EQ(ReadBigEndian16Signed(span, 2), static_cast<int16_t>(0x8001));
  EXPECT_EQ(ReadBigEndian16Signed(span, 4), -2);
}

TEST(EndianTest, ReadsBigEndian32UnsignedAndSigned) {
  const std::vector<uint8_t> data = {
      0x12, 0x34, 0x56, 0x78, 0x80, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFC,
  };

  // Pointer overloads
  EXPECT_EQ(ReadBigEndian32(data.data()), 0x12345678u);
  EXPECT_EQ(ReadBigEndian32(data.data() + 4), 0x80000001u);
  EXPECT_EQ(ReadBigEndian32Signed(data.data()), 0x12345678);
  EXPECT_EQ(ReadBigEndian32Signed(data.data() + 4), static_cast<int32_t>(0x80000001u));
  EXPECT_EQ(ReadBigEndian32Signed(data.data() + 8), -4);

  // Span overloads
  absl::Span<const uint8_t> span(data);
  EXPECT_EQ(ReadBigEndian32(span, 0), 0x12345678u);
  EXPECT_EQ(ReadBigEndian32(span, 4), 0x80000001u);
  EXPECT_EQ(ReadBigEndian32Signed(span, 0), 0x12345678);
  EXPECT_EQ(ReadBigEndian32Signed(span, 4), static_cast<int32_t>(0x80000001u));
  EXPECT_EQ(ReadBigEndian32Signed(span, 8), -4);
}

TEST(EndianTest, ReadsBigEndian64) {
  const std::vector<uint8_t> data = {
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
  };

  EXPECT_EQ(ReadBigEndian64(data.data()), 0x0123456789ABCDEFULL);

  absl::Span<const uint8_t> span(data);
  EXPECT_EQ(ReadBigEndian64(span, 0), 0x0123456789ABCDEFULL);
}

TEST(EndianTest, WritesBigEndian16BigEndian32BigEndian64) {
  std::vector<uint8_t> buffer(14, 0);

  WriteBigEndian16(buffer.data(), 0xA1B2);
  EXPECT_EQ(buffer[0], 0xA1);
  EXPECT_EQ(buffer[1], 0xB2);

  WriteBigEndian32(buffer.data() + 2, 0x12345678);
  EXPECT_EQ(buffer[2], 0x12);
  EXPECT_EQ(buffer[3], 0x34);
  EXPECT_EQ(buffer[4], 0x56);
  EXPECT_EQ(buffer[5], 0x78);

  WriteBigEndian64(buffer.data() + 6, 0xFEDCBA9876543210ULL);
  EXPECT_EQ(buffer[6], 0xFE);
  EXPECT_EQ(buffer[7], 0xDC);
  EXPECT_EQ(buffer[8], 0xBA);
  EXPECT_EQ(buffer[9], 0x98);
  EXPECT_EQ(buffer[10], 0x76);
  EXPECT_EQ(buffer[11], 0x54);
  EXPECT_EQ(buffer[12], 0x32);
  EXPECT_EQ(buffer[13], 0x10);
}

TEST(EndianTest, AppendsBigEndian16BigEndian32BigEndian64) {
  std::vector<uint8_t> buf;

  AppendBigEndian16(buf, 0x1234);
  EXPECT_THAT(buf, ElementsAre(0x12, 0x34));

  AppendBigEndian32(buf, 0x56789ABC);
  EXPECT_THAT(buf, ElementsAre(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC));

  AppendBigEndian64(buf, 0x0102030405060708ULL);
  EXPECT_THAT(buf, ElementsAre(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0x01, 0x02, 0x03, 0x04, 0x05,
                               0x06, 0x07, 0x08));
}

}  // namespace
}  // namespace rom_nom_nom
