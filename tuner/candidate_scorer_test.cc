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

TEST(CandidateScorerTest, ScoreInstructionsStripsTrailingSectionPaddingNops) {
  std::vector<uint32_t> target_words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop (delay slot)
  };
  TargetFunction target;
  target.name = "UnalignedTarget";
  target.vram = 0x80025C08;  // Not 16-byte aligned in retail ROM
  target.raw_words = target_words;
  auto target_insts_or = DecodeSequence(target_words, target.vram);
  ASSERT_TRUE(target_insts_or.ok());
  target.instructions = *std::move(target_insts_or);

  // Standalone compiled .text section is padded to 16-byte boundary (4 words = 16 bytes)
  std::vector<uint32_t> compiled_words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop (delay slot)
      0x00000000,  // nop (16-byte section alignment padding)
  };
  auto compiled_insts_or = DecodeSequence(compiled_words, target.vram);
  ASSERT_TRUE(compiled_insts_or.ok());
  std::vector<Instruction> compiled_insts = *std::move(compiled_insts_or);

  CandidateScorer scorer;
  CandidateScore score = scorer.ScoreInstructions(target, compiled_insts, compiled_words);
  EXPECT_TRUE(score.raw_bit_exact);
  EXPECT_TRUE(score.normalized_exact);
  EXPECT_EQ(score.bit_exact_instructions, 3u);
  EXPECT_EQ(score.compiled_instruction_count, 3u);
  EXPECT_EQ(score.target_instruction_count, 3u);
}

TEST(CandidateScorerTest, VerifyEquivalenceVerifiesPointerDereferencingFunctions) {
  // Target: lw $v0, 0($a0); jr $ra; nop
  std::vector<uint32_t> target_words = {
      0x8C820000,  // lw $v0, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  TargetFunction target;
  target.name = "LoadWord";
  target.vram = 0x80025C00;
  target.raw_words = target_words;
  auto target_insts_or = DecodeSequence(target_words, target.vram);
  ASSERT_TRUE(target_insts_or.ok());
  target.instructions = *std::move(target_insts_or);

  CandidateScorer scorer;

  // 1. Functionally equivalent candidate: loads word in the delay slot of jr $ra
  std::vector<uint32_t> equivalent_words = {
      0x03E00008,  // jr $ra
      0x8C820000,  // lw $v0, 0($a0) (delay slot)
  };
  EXPECT_TRUE(scorer.VerifyEquivalence(target, equivalent_words));

  // 2. Non-equivalent candidate: loads from offset +4 instead of 0
  std::vector<uint32_t> differing_offset_words = {
      0x8C820004,  // lw $v0, 4($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  EXPECT_FALSE(scorer.VerifyEquivalence(target, differing_offset_words));
}

TEST(CandidateScorerTest, IsBetterThanPenalizesMassiveCodeSizeBloat) {
  CandidateScore compact_o2;
  compact_o2.target_instruction_count = 100;
  compact_o2.compiled_instruction_count = 100;
  compact_o2.bit_exact_instructions = 48;
  compact_o2.matching_instructions = 60;

  CandidateScore bloated_o0;
  bloated_o0.target_instruction_count = 100;
  bloated_o0.compiled_instruction_count = 260;  // +160 instructions of -O0 stack spill bloat
  bloated_o0.bit_exact_instructions = 52;       // +4 accidental matches in bloated sequence
  bloated_o0.matching_instructions = 65;

  EXPECT_TRUE(compact_o2.IsBetterThan(bloated_o0));
  EXPECT_FALSE(bloated_o0.IsBetterThan(compact_o2));
}

TEST(CandidateScorerTest, IsBetterThanPreservesStructuralIsomorphismOverAccidentalInstructionHits) {
  CandidateScore isomorphic_baseline;
  isomorphic_baseline.target_instruction_count = 30;
  isomorphic_baseline.compiled_instruction_count = 30;
  isomorphic_baseline.bit_exact_instructions = 20;
  isomorphic_baseline.matching_instructions = 22;
  isomorphic_baseline.frame_size_matches = true;
  isomorphic_baseline.saved_registers_match = true;
  isomorphic_baseline.cfg_topology_matches = true;
  EXPECT_TRUE(isomorphic_baseline.IsStructurallyIsomorphic());

  CandidateScore regressed_structure;
  regressed_structure.target_instruction_count = 30;
  regressed_structure.compiled_instruction_count = 30;
  regressed_structure.bit_exact_instructions = 24;  // +4 accidental raw instruction matches
  regressed_structure.matching_instructions = 25;
  regressed_structure.frame_size_matches = false;  // Broke stack frame size!
  regressed_structure.saved_registers_match = true;
  regressed_structure.cfg_topology_matches = true;
  EXPECT_FALSE(regressed_structure.IsStructurallyIsomorphic());

  EXPECT_TRUE(isomorphic_baseline.IsBetterThan(regressed_structure));
  EXPECT_FALSE(regressed_structure.IsBetterThan(isomorphic_baseline));

  // Only a genuinely bit-exact candidate may supersede a structurally isomorphic baseline
  CandidateScore bit_exact_candidate = regressed_structure;
  bit_exact_candidate.raw_bit_exact = true;
  EXPECT_TRUE(bit_exact_candidate.IsBetterThan(isomorphic_baseline));
}

TEST(CandidateScorerTest, IsBetterThanIncorporatesNormalizedLcsInstructions) {
  CandidateScore baseline;
  baseline.target_instruction_count = 24;
  baseline.compiled_instruction_count = 24;
  baseline.bit_exact_instructions = 14;
  baseline.matching_instructions = 16;
  baseline.normalized_lcs_instructions = 18;
  baseline.frame_size_matches = true;
  baseline.saved_registers_match = true;
  baseline.cfg_topology_matches = true;
  EXPECT_FALSE(baseline.IsOpcodeSequenceExact());

  // Candidate A has the same structural isomorphism and bit_exact_instructions, slightly fewer
  // positional `matching_instructions` due to a shifted register move, but 100% opcode sequence
  // alignment (`normalized_lcs_instructions == 24`).
  CandidateScore lcs_aligned = baseline;
  lcs_aligned.matching_instructions = 15;
  lcs_aligned.normalized_lcs_instructions = 24;
  EXPECT_TRUE(lcs_aligned.IsOpcodeSequenceExact());

  EXPECT_TRUE(lcs_aligned.IsBetterThan(baseline));
  EXPECT_FALSE(baseline.IsBetterThan(lcs_aligned));
}

}  // namespace
}  // namespace rom_nom_nom
