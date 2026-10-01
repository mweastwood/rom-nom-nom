#include "core/rom.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_format.h"
#include "core/endian.h"
#include "core/sha1.h"

namespace rom_nom_nom {

namespace {

RomEndianness DetectEndianness(absl::Span<const uint8_t> data) {
  if (data.size() < 4) {
    return RomEndianness::kUnknown;
  }
  const uint32_t magic = ReadBigEndian32(data.data());
  switch (magic) {
    case 0x80371240:
      return RomEndianness::kBigEndian;
    case 0x37804012:
      return RomEndianness::kByteSwapped;
    case 0x40123780:
      return RomEndianness::kLittleEndian;
    default:
      return RomEndianness::kUnknown;
  }
}

absl::StatusOr<RomHeader> ParseHeader(absl::Span<const uint8_t> data) {
  if (data.size() < 64) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "ROM size (%d bytes) is smaller than 64-byte cartridge header", data.size()));
  }

  RomHeader header;
  header.endianness = DetectEndianness(data);

  if (header.endianness != RomEndianness::kBigEndian) {
    // TODO(rom-nom-nom): Implement in-memory byte-swapping to support .v64 and .n64 formats.
    return absl::InvalidArgumentError(absl::StrFormat(
        "ROM format '%s' is not supported; only native big-endian .z64 files are supported",
        EndiannessExtension(header.endianness)));
  }

  const uint8_t* p = data.data();
  header.initial_pi_reg = ReadBigEndian32(p + 0x00);
  header.clock_rate = ReadBigEndian32(p + 0x04);
  header.entry_point_pc = ReadBigEndian32(p + 0x08);
  header.release_offset = ReadBigEndian32(p + 0x0C);
  header.crc1 = ReadBigEndian32(p + 0x10);
  header.crc2 = ReadBigEndian32(p + 0x14);

  std::string raw_title(reinterpret_cast<const char*>(p + 0x20), 20);
  while (!raw_title.empty() &&
         (raw_title.back() == '\0' || absl::ascii_isspace(raw_title.back()))) {
    raw_title.pop_back();
  }
  header.game_title = raw_title;

  header.media_format = static_cast<char>(p[0x3B]);
  header.game_code = std::string(reinterpret_cast<const char*>(p + 0x3C), 2);
  header.country_code = static_cast<char>(p[0x3E]);
  header.version = p[0x3F];

  return header;
}

}  // namespace

std::string_view EndiannessExtension(RomEndianness endian) {
  switch (endian) {
    case RomEndianness::kBigEndian:
      return ".z64";
    case RomEndianness::kByteSwapped:
      return ".v64";
    case RomEndianness::kLittleEndian:
      return ".n64";
    case RomEndianness::kUnknown:
      return ".bin";
  }
  return ".bin";
}

absl::StatusOr<Rom> Rom::OpenFile(const std::filesystem::path& path) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return absl::NotFoundError(absl::StrFormat("Failed to open ROM file: %s", path.string()));
  }

  struct stat sb;
  if (fstat(fd, &sb) < 0) {
    close(fd);
    return absl::InternalError(absl::StrFormat("Failed to stat ROM file: %s", path.string()));
  }

  const size_t file_size = static_cast<size_t>(sb.st_size);
  if (file_size < 64) {
    close(fd);
    return absl::InvalidArgumentError(
        absl::StrFormat("ROM file '%s' is too small (%d bytes)", path.string(), file_size));
  }

  void* addr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return absl::InternalError(absl::StrFormat("Failed to mmap ROM file: %s", path.string()));
  }

  const uint8_t* byte_ptr = static_cast<const uint8_t*>(addr);
  auto header_or = ParseHeader(absl::MakeConstSpan(byte_ptr, file_size));
  if (!header_or.ok()) {
    munmap(addr, file_size);
    close(fd);
    return header_or.status();
  }

  Rom rom;
  rom.data_ = byte_ptr;
  rom.size_ = file_size;
  rom.fd_ = fd;
  rom.header_ = std::move(*header_or);
  return rom;
}

absl::StatusOr<Rom> Rom::FromBuffer(std::vector<uint8_t> buffer) {
  if (buffer.size() < 64) {
    return absl::InvalidArgumentError(
        absl::StrFormat("ROM buffer is too small (%d bytes)", buffer.size()));
  }

  auto header_or = ParseHeader(absl::MakeConstSpan(buffer));
  if (!header_or.ok()) {
    return header_or.status();
  }

  Rom rom;
  rom.owned_buffer_ = std::move(buffer);
  rom.data_ = rom.owned_buffer_.data();
  rom.size_ = rom.owned_buffer_.size();
  rom.fd_ = -1;
  rom.header_ = std::move(*header_or);
  return rom;
}

Rom::Rom(Rom&& other) noexcept
    : data_(other.data_),
      size_(other.size_),
      fd_(other.fd_),
      owned_buffer_(std::move(other.owned_buffer_)),
      header_(std::move(other.header_)) {
  other.data_ = nullptr;
  other.size_ = 0;
  other.fd_ = -1;
}

Rom& Rom::operator=(Rom&& other) noexcept {
  if (this != &other) {
    if (fd_ != -1 && data_ != nullptr) {
      munmap(const_cast<uint8_t*>(data_), size_);
      close(fd_);
    }
    data_ = other.data_;
    size_ = other.size_;
    fd_ = other.fd_;
    owned_buffer_ = std::move(other.owned_buffer_);
    header_ = std::move(other.header_);

    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
  }
  return *this;
}

Rom::~Rom() {
  if (fd_ != -1 && data_ != nullptr) {
    munmap(const_cast<uint8_t*>(data_), size_);
    close(fd_);
  }
}

absl::Span<const uint8_t> Rom::Data() const {
  if (data_ == nullptr || size_ == 0) {
    return {};
  }
  return absl::MakeConstSpan(data_, size_);
}

std::string Rom::Sha1() const {
  return ComputeSha1Hex(Data());
}

absl::StatusOr<absl::Span<const uint8_t>> Rom::Slice(size_t offset, size_t length) const {
  if (offset > size_ || length > size_ - offset) {
    return absl::OutOfRangeError(absl::StrFormat(
        "ROM slice [0x%X, 0x%X) exceeds ROM bounds (size: 0x%X)", offset, offset + length, size_));
  }
  return absl::MakeConstSpan(data_ + offset, length);
}

absl::StatusOr<absl::Span<const uint8_t>> Rom::SliceRange(size_t start_offset,
                                                          size_t end_offset) const {
  if (start_offset > end_offset || end_offset > size_) {
    return absl::OutOfRangeError(
        absl::StrFormat("ROM slice range [0x%X, 0x%X) is invalid or exceeds bounds (size: 0x%X)",
                        start_offset, end_offset, size_));
  }
  return absl::MakeConstSpan(data_ + start_offset, end_offset - start_offset);
}

absl::StatusOr<uint32_t> Rom::ReadWord(size_t offset) const {
  if (offset + 4 > size_) {
    return absl::OutOfRangeError(
        absl::StrFormat("ReadWord at offset 0x%X exceeds ROM bounds (size: 0x%X)", offset, size_));
  }
  return ReadBigEndian32(data_ + offset);
}

absl::StatusOr<uint16_t> Rom::ReadHalf(size_t offset) const {
  if (offset + 2 > size_) {
    return absl::OutOfRangeError(
        absl::StrFormat("ReadHalf at offset 0x%X exceeds ROM bounds (size: 0x%X)", offset, size_));
  }
  return ReadBigEndian16(data_ + offset);
}

absl::StatusOr<uint8_t> Rom::ReadByte(size_t offset) const {
  if (offset >= size_) {
    return absl::OutOfRangeError(
        absl::StrFormat("ReadByte at offset 0x%X exceeds ROM bounds (size: 0x%X)", offset, size_));
  }
  return data_[offset];
}

}  // namespace rom_nom_nom
