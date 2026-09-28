#include "core/sha1.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(Sha1Test, EmptyString) {
  EXPECT_EQ(ComputeSha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(Sha1Test, Rfc3174Vector1) {
  // Test vector 1: "abc"
  EXPECT_EQ(ComputeSha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(Sha1Test, Rfc3174Vector2) {
  // Test vector 2: "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" (56 bytes)
  const std::string input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  EXPECT_EQ(ComputeSha1Hex(input), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha1Test, Rfc3174Vector3MillionAs) {
  // Test vector 3: 1,000,000 repetitions of 'a'
  Sha1 hasher;
  const std::string chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i) {
    hasher.Update(chunk);
  }
  EXPECT_EQ(hasher.FinalizeHex(), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(Sha1Test, StreamingEquivalence) {
  const std::string text = "The quick brown fox jumps over the lazy dog";
  const std::string expected = "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12";

  EXPECT_EQ(ComputeSha1Hex(text), expected);

  // Stream byte-by-byte
  Sha1 streamer;
  for (char c : text) {
    streamer.Update(std::string_view(&c, 1));
  }
  EXPECT_EQ(streamer.FinalizeHex(), expected);
}

TEST(Sha1Test, ByteSpanOverload) {
  const std::vector<uint8_t> bytes = {'a', 'b', 'c'};
  EXPECT_EQ(ComputeSha1Hex(absl::MakeConstSpan(bytes)), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(Sha1Test, RawDigest) {
  const std::string input = "abc";
  Sha1Digest digest = ComputeSha1Digest(
      absl::MakeConstSpan(reinterpret_cast<const uint8_t*>(input.data()), input.size()));
  EXPECT_EQ(Sha1DigestToHex(digest), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(digest[0], 0xa9);
  EXPECT_EQ(digest[1], 0x99);
  EXPECT_EQ(digest[19], 0x9d);
}

}  // namespace
}  // namespace rom_nom_nom
