#ifndef CORE_SHA1_H_
#define CORE_SHA1_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/types/span.h"

namespace rom_nom_nom {

// 20-byte SHA-1 raw digest array.
using Sha1Digest = std::array<uint8_t, 20>;

// Self-contained, streaming SHA-1 hash calculator (FIPS PUB 180-1).
class Sha1 {
 public:
  Sha1();

  // Feeds bytes into the running SHA-1 state.
  void Update(absl::Span<const uint8_t> data);
  void Update(std::string_view data);

  // Finalizes the calculation and returns the 20-byte raw digest.
  Sha1Digest Finalize();

  // Finalizes the calculation and returns lowercase 40-character hex string.
  std::string FinalizeHex();

 private:
  void Transform(absl::Span<const uint8_t> block);

  std::array<uint32_t, 5> state_{};
  uint64_t count_ = 0;
  std::array<uint8_t, 64> buffer_{};
  bool finalized_ = false;
  Sha1Digest digest_{};
};

// One-shot helper functions:
// Computes 40-character lowercase hex string for the given byte span.
std::string ComputeSha1Hex(absl::Span<const uint8_t> data);
std::string ComputeSha1Hex(std::string_view data);

// Computes 20-byte raw digest for the given byte span.
Sha1Digest ComputeSha1Digest(absl::Span<const uint8_t> data);

// Converts raw 20-byte digest to 40-character lowercase hex string.
std::string Sha1DigestToHex(const Sha1Digest& digest);

}  // namespace rom_nom_nom

#endif  // CORE_SHA1_H_
