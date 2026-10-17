#include "tuner/candidate_scorer.h"

#include <cstdint>
#include <vector>

#include "core/mips.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(CandidateScorerTest, ScoreInstructionsRecognizesBitExactAndPartialMatches) {
  std::vector<uint32_t> target_words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  TargetFunction target;
  target.name = "Add";
  target.vram = 0x80025C00;
  target.raw_words = target_words;
  auto target_insts_or = DecodeSequence(target_words, target.vram);
  ASSERT_TRUE(target_insts_or.ok());
  target.instructions = *std::move(target_insts_or);

  CandidateScorer scorer;

  // 1. Exact match
  CandidateScore exact_score = scorer.ScoreInstructions(target, target.instructions, target_words);
  EXPECT_TRUE(exact_score.raw_bit_exact);
  EXPECT_TRUE(exact_score.normalized_exact);
  EXPECT_EQ(exact_score.bit_exact_instructions, 3u);
  EXPECT_EQ(exact_score.matching_instructions, 3u);

  // 2. Commuted register order (addu $v0, $a1, $a0 = 0x00A41021)
  std::vector<uint32_t> commuted_words = {
      0x00A41021,  // addu $v0, $a1, $a0
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto commuted_insts_or = DecodeSequence(commuted_words, target.vram);
  ASSERT_TRUE(commuted_insts_or.ok());
  std::vector<Instruction> commuted_insts = *std::move(commuted_insts_or);
  CandidateScore partial_score = scorer.ScoreInstructions(target, commuted_insts, commuted_words);
  EXPECT_FALSE(partial_score.raw_bit_exact);
  EXPECT_FALSE(partial_score.normalized_exact);
  EXPECT_EQ(partial_score.bit_exact_instructions, 2u);
  EXPECT_TRUE(exact_score.IsBetterThan(partial_score));
  EXPECT_FALSE(partial_score.IsBetterThan(exact_score));
  EXPECT_TRUE(scorer.VerifyEquivalence(target, commuted_words));
}

}  // namespace
}  // namespace rom_nom_nom
