#include "core/sha1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "absl/strings/str_format.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

namespace {

inline uint32_t LeftRotate(uint32_t value, size_t bits) {
  return (value << bits) | (value >> (32 - bits));
}

}  // namespace

Sha1::Sha1() {
  state_ = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  count_ = 0;
  finalized_ = false;
  buffer_.fill(0);
}

void Sha1::Transform(absl::Span<const uint8_t> block) {
  std::array<uint32_t, 80> w{};

  for (size_t t = 0; t < 16; ++t) {
    w[t] = (static_cast<uint32_t>(block[t * 4 + 0]) << 24) |
           (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[t * 4 + 2]) << 8) | static_cast<uint32_t>(block[t * 4 + 3]);
  }

  for (size_t t = 16; t < 80; ++t) {
    w[t] = LeftRotate(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
  }

  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];

  for (size_t t = 0; t < 80; ++t) {
    uint32_t f = 0;
    uint32_t k = 0;

    if (t < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999;
    } else if (t < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (t < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }

    const uint32_t temp = LeftRotate(a, 5) + f + e + k + w[t];
    e = d;
    d = c;
    c = LeftRotate(b, 30);
    b = a;
    a = temp;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
}

void Sha1::Update(absl::Span<const uint8_t> data) {
  if (finalized_ || data.empty()) {
    return;
  }

  size_t buffer_index = static_cast<size_t>(count_ % 64);
  count_ += data.size();

  size_t data_index = 0;
  size_t remaining = data.size();

  if (buffer_index > 0) {
    const size_t needed = 64 - buffer_index;
    if (remaining < needed) {
      std::memcpy(&buffer_[buffer_index], data.data(), remaining);
      return;
    }

    std::memcpy(&buffer_[buffer_index], data.data(), needed);
    Transform(absl::MakeConstSpan(buffer_));
    data_index += needed;
    remaining -= needed;
  }

  while (remaining >= 64) {
    Transform(data.subspan(data_index, 64));
    data_index += 64;
    remaining -= 64;
  }

  if (remaining > 0) {
    std::memcpy(buffer_.data(), &data[data_index], remaining);
  }
}

void Sha1::Update(std::string_view data) {
  Update(absl::MakeConstSpan(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

Sha1Digest Sha1::Finalize() {
  if (finalized_) {
    return digest_;
  }

  const uint64_t total_bits = count_ * 8;
  size_t buffer_index = static_cast<size_t>(count_ % 64);

  buffer_[buffer_index++] = 0x80;

  if (buffer_index > 56) {
    std::memset(&buffer_[buffer_index], 0, 64 - buffer_index);
    Transform(absl::MakeConstSpan(buffer_));
    buffer_index = 0;
  }

  std::memset(&buffer_[buffer_index], 0, 56 - buffer_index);

  for (int i = 7; i >= 0; --i) {
    buffer_[56 + (7 - i)] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
  }

  Transform(absl::MakeConstSpan(buffer_));

  for (size_t i = 0; i < 5; ++i) {
    digest_[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
    digest_[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
    digest_[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
    digest_[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFF);
  }

  finalized_ = true;
  return digest_;
}

std::string Sha1::FinalizeHex() {
  return Sha1DigestToHex(Finalize());
}

std::string Sha1DigestToHex(const Sha1Digest& digest) {
  std::string hex;
  hex.reserve(40);
  for (uint8_t byte : digest) {
    absl::StrAppendFormat(&hex, "%02x", byte);
  }
  return hex;
}

std::string ComputeSha1Hex(absl::Span<const uint8_t> data) {
  Sha1 hasher;
  hasher.Update(data);
  return hasher.FinalizeHex();
}

std::string ComputeSha1Hex(std::string_view data) {
  Sha1 hasher;
  hasher.Update(data);
  return hasher.FinalizeHex();
}

Sha1Digest ComputeSha1Digest(absl::Span<const uint8_t> data) {
  Sha1 hasher;
  hasher.Update(data);
  return hasher.Finalize();
}

}  // namespace rom_nom_nom
