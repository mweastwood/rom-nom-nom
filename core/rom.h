#ifndef CORE_ROM_H_
#define CORE_ROM_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

// N64 ROM file format / byte order.
enum class RomEndianness {
  kBigEndian,     // .z64: Native N64 format (magic: 0x80371240)
  kByteSwapped,   // .v64: Byte-swapped 16-bit (magic: 0x37804012)
  kLittleEndian,  // .n64: Little-endian 32-bit (magic: 0x40123780)
  kUnknown,
};

// Returns standard format extension (e.g. ".z64", ".v64", ".n64").
std::string_view EndiannessExtension(RomEndianness endian);

// Parsed 64-byte N64 cartridge header (0x0000 - 0x0040).
struct RomHeader {
  uint32_t initial_pi_reg = 0;  // 0x00: Usually 0x80371240
  uint32_t clock_rate = 0;      // 0x04
  uint32_t entry_point_pc = 0;  // 0x08: RDRAM bootstrap execution PC (e.g. 0x80025C00)
  uint32_t release_offset = 0;  // 0x0C
  uint32_t crc1 = 0;            // 0x10: Checksum 1
  uint32_t crc2 = 0;            // 0x14: Checksum 2
  std::string game_title;       // 0x20: 20-byte ASCII internal game title (trimmed)
  char media_format = 0;        // 0x3B: Usually 'N' for cartridge
  std::string game_code;        // 0x3C: 2-character ASCII cartridge ID (e.g. "3Y")
  char country_code = 0;        // 0x3E: 'E'=USA, 'J'=Japan, 'P'=Europe
  uint8_t version = 0;          // 0x3F: ROM revision / version
  RomEndianness endianness = RomEndianness::kUnknown;
};

// RAII wrapper around a memory-mapped or buffered N64 ROM.
class Rom {
 public:
  // Opens and memory-maps a .z64 ROM file from disk (read-only, zero-copy).
  // Returns an error if the ROM is in .v64 (byte-swapped) or .n64 (little-endian) format.
  // (NOTE: In-memory byte-swapping can optionally be implemented in the future).
  static absl::StatusOr<Rom> OpenFile(const std::filesystem::path& path);

  // Creates an in-memory ROM from a buffer (ideal for unit testing).
  static absl::StatusOr<Rom> FromBuffer(std::vector<uint8_t> buffer);

  // Move-only semantics for safe mmap ownership.
  Rom(Rom&& other) noexcept;
  Rom& operator=(Rom&& other) noexcept;
  Rom(const Rom&) = delete;
  Rom& operator=(const Rom&) = delete;
  ~Rom();

  // Total size of the ROM in bytes (e.g. 16,777,216 for 16MB HM64).
  size_t Size() const { return size_; }

  // Returns a view over the entire ROM buffer.
  absl::Span<const uint8_t> Data() const;

  // Returns the parsed 64-byte cartridge header.
  const RomHeader& Header() const { return header_; }

  // Computes the SHA-1 checksum hex string using core/sha1.h.
  std::string Sha1() const;

  // --- Safe Slicing Operations ---

  // Returns a sub-slice view [offset, offset + length).
  // Returns absl::OutOfRangeError if offset + length exceeds ROM bounds.
  absl::StatusOr<absl::Span<const uint8_t>> Slice(size_t offset, size_t length) const;

  // Returns a sub-slice view [start_offset, end_offset).
  absl::StatusOr<absl::Span<const uint8_t>> SliceRange(size_t start_offset,
                                                       size_t end_offset) const;

  // --- Big-Endian Word Readers (Zero-Copy) ---

  // Reads a 32-bit big-endian word at the specified ROM offset.
  absl::StatusOr<uint32_t> ReadWord(size_t offset) const;

  // Reads a 16-bit big-endian halfword at the specified ROM offset.
  absl::StatusOr<uint16_t> ReadHalf(size_t offset) const;

  // Reads a single byte at the specified ROM offset.
  absl::StatusOr<uint8_t> ReadByte(size_t offset) const;

 private:
  Rom() = default;

  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  int fd_ = -1;                        // File descriptor for mmap (-1 if in-memory)
  std::vector<uint8_t> owned_buffer_;  // Used only for FromBuffer
  RomHeader header_;
};

}  // namespace rom_nom_nom

#endif  // CORE_ROM_H_
