#include "differ/sequence_aligner.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "differ/normalizer.h"

namespace rom_nom_nom {
namespace {

using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::Property;
using ::testing::SizeIs;

NormalizedInstruction MakeNorm(uint32_t raw_word, uint32_t vram, const Normalizer& normalizer,
                               bool is_target) {
  auto inst = DecodeInstruction(raw_word, vram);
  EXPECT_TRUE(inst.ok());
  if (is_target) {
    return normalizer.NormalizeTarget(*inst);
  }
  return normalizer.NormalizeCompiled(*inst);
}

TEST(SequenceAlignerTest, HandlesEmptySequences) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);
  SequenceAligner aligner(&normalizer);

  std::vector<NormalizedInstruction> empty;
  auto result = aligner.Align(empty, empty);

  EXPECT_THAT(result.rows, SizeIs(0));
  EXPECT_EQ(result.stats.total_rows, 0u);
  EXPECT_EQ(result.stats.matching_instructions, 0u);
  EXPECT_THAT(result.stats.MatchPercentage(), DoubleEq(100.0));
  EXPECT_FALSE(result.stats.IsBitExactMatch());
}

TEST(SequenceAlignerTest, AlignsIdenticalSequencesAsBitExactMatch) {
  FunctionDiffContext ctx;
  ctx.target_vram = 0x80025C00;
  ctx.target_size = 0xC;
  ctx.compiled_vram = 0x0;
  ctx.compiled_size = 0xC;

  Normalizer normalizer(ctx);
  SequenceAligner aligner(&normalizer);

  // 1. addiu $sp, $sp, -32 (0x27BDFFE0)
  // 2. sw $ra, 28($sp)     (0xAFBF001C)
  // 3. addu $t0, $a0, $a1  (0x00854021)
  std::vector<uint32_t> words = {0x27BDFFE0, 0xAFBF001C, 0x00854021};

  std::vector<NormalizedInstruction> target;
  std::vector<NormalizedInstruction> compiled;
  for (size_t i = 0; i < words.size(); ++i) {
    target.push_back(MakeNorm(words[i], 0x80025C00 + i * 4, normalizer, /*is_target=*/true));
    compiled.push_back(MakeNorm(words[i], i * 4, normalizer, /*is_target=*/false));
  }

  auto result = aligner.Align(target, compiled);

  EXPECT_THAT(result.rows, SizeIs(3));
  EXPECT_EQ(result.stats.total_rows, 3u);
  EXPECT_EQ(result.stats.matching_instructions, 3u);
  EXPECT_EQ(result.stats.differing_instructions, 0u);
  EXPECT_EQ(result.stats.target_only_instructions, 0u);
  EXPECT_EQ(result.stats.compiled_only_instructions, 0u);
  EXPECT_TRUE(result.stats.IsBitExactMatch());
  EXPECT_THAT(result.stats.MatchPercentage(), DoubleEq(100.0));

  for (const auto& row : result.rows) {
    EXPECT_EQ(row.kind, DiffRowKind::kMatch);
    EXPECT_EQ(row.quality, MatchQuality::kBitExact);
    EXPECT_TRUE(row.target.has_value());
    EXPECT_TRUE(row.compiled.has_value());
  }
}

TEST(SequenceAlignerTest, DetectsCompiledInsertion) {
  FunctionDiffContext ctx;
  ctx.target_vram = 0x80025C00;
  ctx.target_size = 0x8;
  ctx.compiled_vram = 0x0;
  ctx.compiled_size = 0xC;

  Normalizer normalizer(ctx);
  SequenceAligner aligner(&normalizer);

  uint32_t addiu = 0x27BDFFE0;
  uint32_t nop = 0x00000000;
  uint32_t sw = 0xAFBF001C;

  // Target has: addiu, sw
  std::vector<NormalizedInstruction> target = {
      MakeNorm(addiu, 0x80025C00, normalizer, /*is_target=*/true),
      MakeNorm(sw, 0x80025C04, normalizer, /*is_target=*/true),
  };

  // Compiled has: addiu, nop (inserted), sw
  std::vector<NormalizedInstruction> compiled = {
      MakeNorm(addiu, 0x0, normalizer, /*is_target=*/false),
      MakeNorm(nop, 0x4, normalizer, /*is_target=*/false),
      MakeNorm(sw, 0x8, normalizer, /*is_target=*/false),
  };

  auto result = aligner.Align(target, compiled);

  ASSERT_THAT(result.rows, SizeIs(3));
  EXPECT_EQ(result.rows[0].kind, DiffRowKind::kMatch);
  EXPECT_EQ(result.rows[1].kind, DiffRowKind::kCompiledOnly);
  EXPECT_FALSE(result.rows[1].target.has_value());
  ASSERT_TRUE(result.rows[1].compiled.has_value());
  EXPECT_EQ(result.rows[1].compiled->original.raw_word, nop);
  EXPECT_EQ(result.rows[2].kind, DiffRowKind::kMatch);

  EXPECT_EQ(result.stats.matching_instructions, 2u);
  EXPECT_EQ(result.stats.compiled_only_instructions, 1u);
  EXPECT_EQ(result.stats.target_only_instructions, 0u);
  EXPECT_FALSE(result.stats.IsBitExactMatch());
  EXPECT_THAT(result.stats.MatchPercentage(), DoubleEq(100.0));  // 2 of 2 target matched
}

TEST(SequenceAlignerTest, DetectsTargetDeletion) {
  FunctionDiffContext ctx;
  ctx.target_vram = 0x80025C00;
  ctx.target_size = 0xC;
  ctx.compiled_vram = 0x0;
  ctx.compiled_size = 0x8;

  Normalizer normalizer(ctx);
  SequenceAligner aligner(&normalizer);

  uint32_t addiu = 0x27BDFFE0;
  uint32_t nop = 0x00000000;
  uint32_t sw = 0xAFBF001C;

  // Target has: addiu, nop, sw
  std::vector<NormalizedInstruction> target = {
      MakeNorm(addiu, 0x80025C00, normalizer, /*is_target=*/true),
      MakeNorm(nop, 0x80025C04, normalizer, /*is_target=*/true),
      MakeNorm(sw, 0x80025C08, normalizer, /*is_target=*/true),
  };

  // Compiled has: addiu, sw (nop deleted)
  std::vector<NormalizedInstruction> compiled = {
      MakeNorm(addiu, 0x0, normalizer, /*is_target=*/false),
      MakeNorm(sw, 0x4, normalizer, /*is_target=*/false),
  };

  auto result = aligner.Align(target, compiled);

  ASSERT_THAT(result.rows, SizeIs(3));
  EXPECT_EQ(result.rows[0].kind, DiffRowKind::kMatch);
  EXPECT_EQ(result.rows[1].kind, DiffRowKind::kTargetOnly);
  ASSERT_TRUE(result.rows[1].target.has_value());
  EXPECT_EQ(result.rows[1].target->original.raw_word, nop);
  EXPECT_FALSE(result.rows[1].compiled.has_value());
  EXPECT_EQ(result.rows[2].kind, DiffRowKind::kMatch);

  EXPECT_EQ(result.stats.matching_instructions, 2u);
  EXPECT_EQ(result.stats.target_only_instructions, 1u);
  EXPECT_EQ(result.stats.compiled_only_instructions, 0u);
  EXPECT_FALSE(result.stats.IsBitExactMatch());
  // 2 of 3 target matched = 66.666...%
  EXPECT_NEAR(result.stats.MatchPercentage(), 66.666, 0.01);
}

TEST(SequenceAlignerTest, PairsSimilarOpcodesOnSameRow) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);
  SequenceAligner aligner(&normalizer);

  // Target:   addu $t0, $a0, $a1 (rd=8)
  uint32_t addu_t0 = (0 << 26) | (4 << 21) | (5 << 16) | (8 << 11) | 0x21;
  // Compiled: addu $t1, $a0, $a1 (rd=9)
  uint32_t addu_t1 = (0 << 26) | (4 << 21) | (5 << 16) | (9 << 11) | 0x21;

  std::vector<NormalizedInstruction> target = {
      MakeNorm(addu_t0, 0x80025C00, normalizer, /*is_target=*/true),
  };
  std::vector<NormalizedInstruction> compiled = {
      MakeNorm(addu_t1, 0x0, normalizer, /*is_target=*/false),
  };

  auto result = aligner.Align(target, compiled);

  ASSERT_THAT(result.rows, SizeIs(1));
  EXPECT_EQ(result.rows[0].kind, DiffRowKind::kDifference);
  EXPECT_EQ(result.rows[0].quality, MatchQuality::kSimilarOpcode);
  ASSERT_TRUE(result.rows[0].target.has_value());
  ASSERT_TRUE(result.rows[0].compiled.has_value());
  EXPECT_EQ(result.stats.differing_instructions, 1u);
  EXPECT_EQ(result.stats.matching_instructions, 0u);
  EXPECT_THAT(result.stats.MatchPercentage(), DoubleEq(0.0));
}

}  // namespace
}  // namespace rom_nom_nom
