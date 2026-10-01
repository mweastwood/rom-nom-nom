#ifndef CORE_ENDIAN_H_
#define CORE_ENDIAN_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/types/span.h"

namespace rom_nom_nom {

inline constexpr uint16_t ReadBigEndian16(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline constexpr uint16_t ReadBigEndian16(absl::Span<const uint8_t> bytes, size_t offset = 0) {
  return ReadBigEndian16(bytes.data() + offset);
}

inline constexpr int16_t ReadBigEndian16Signed(const uint8_t* p) {
  return static_cast<int16_t>(ReadBigEndian16(p));
}

inline constexpr int16_t ReadBigEndian16Signed(absl::Span<const uint8_t> bytes, size_t offset = 0) {
  return static_cast<int16_t>(ReadBigEndian16(bytes, offset));
}

inline constexpr uint32_t ReadBigEndian32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline constexpr uint32_t ReadBigEndian32(absl::Span<const uint8_t> bytes, size_t offset = 0) {
  return ReadBigEndian32(bytes.data() + offset);
}

inline constexpr int32_t ReadBigEndian32Signed(const uint8_t* p) {
  return static_cast<int32_t>(ReadBigEndian32(p));
}

inline constexpr int32_t ReadBigEndian32Signed(absl::Span<const uint8_t> bytes, size_t offset = 0) {
  return static_cast<int32_t>(ReadBigEndian32(bytes, offset));
}

inline constexpr uint64_t ReadBigEndian64(const uint8_t* p) {
  return (static_cast<uint64_t>(p[0]) << 56) | (static_cast<uint64_t>(p[1]) << 48) |
         (static_cast<uint64_t>(p[2]) << 40) | (static_cast<uint64_t>(p[3]) << 32) |
         (static_cast<uint64_t>(p[4]) << 24) | (static_cast<uint64_t>(p[5]) << 16) |
         (static_cast<uint64_t>(p[6]) << 8) | static_cast<uint64_t>(p[7]);
}

inline constexpr uint64_t ReadBigEndian64(absl::Span<const uint8_t> bytes, size_t offset = 0) {
  return ReadBigEndian64(bytes.data() + offset);
}

inline void WriteBigEndian16(uint8_t* p, uint16_t val) {
  p[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
  p[1] = static_cast<uint8_t>(val & 0xFF);
}

inline void WriteBigEndian32(uint8_t* p, uint32_t val) {
  p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
  p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
  p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
  p[3] = static_cast<uint8_t>(val & 0xFF);
}

inline void WriteBigEndian64(uint8_t* p, uint64_t val) {
  p[0] = static_cast<uint8_t>((val >> 56) & 0xFF);
  p[1] = static_cast<uint8_t>((val >> 48) & 0xFF);
  p[2] = static_cast<uint8_t>((val >> 40) & 0xFF);
  p[3] = static_cast<uint8_t>((val >> 32) & 0xFF);
  p[4] = static_cast<uint8_t>((val >> 24) & 0xFF);
  p[5] = static_cast<uint8_t>((val >> 16) & 0xFF);
  p[6] = static_cast<uint8_t>((val >> 8) & 0xFF);
  p[7] = static_cast<uint8_t>(val & 0xFF);
}

inline void AppendBigEndian16(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

inline void AppendBigEndian32(std::vector<uint8_t>& buf, uint32_t val) {
  buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

inline void AppendBigEndian64(std::vector<uint8_t>& buf, uint64_t val) {
  buf.push_back(static_cast<uint8_t>((val >> 56) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 48) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 40) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 32) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

}  // namespace rom_nom_nom

#endif  // CORE_ENDIAN_H_
