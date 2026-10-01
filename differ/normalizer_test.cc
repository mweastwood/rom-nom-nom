#include "differ/normalizer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>

#include "core/mips.h"

namespace rom_nom_nom {
namespace {

using ::testing::Eq;
using ::testing::Optional;

TEST(NormalizerTest, NormalizesIntraFunctionBranches) {
  FunctionDiffContext ctx;
  ctx.target_vram = 0x800266C0;
  ctx.target_size = 0x100;
  ctx.compiled_vram = 0x00000000;
  ctx.compiled_size = 0x100;

  Normalizer normalizer(ctx);

  // Target ROM: beqz $v0, 0x800266E0 (+0x20 bytes forward)
  // Displacement in words: (0x20 - 4) / 4 = 7
  uint32_t branch_word = (0x04 << 26) | (2 << 21) | (0 << 16) | 7;
  auto target_inst = DecodeInstruction(branch_word, 0x800266C0);
  ASSERT_TRUE(target_inst.ok());

  // Compiled object: beqz $v0, 0x00000020 (+0x20 bytes forward)
  auto compiled_inst = DecodeInstruction(branch_word, 0x00000000);
  ASSERT_TRUE(compiled_inst.ok());

  auto target_norm = normalizer.NormalizeTarget(*target_inst);
  auto compiled_norm = normalizer.NormalizeCompiled(*compiled_inst);

  EXPECT_EQ(target_norm.func_offset, 0x0u);
  EXPECT_EQ(compiled_norm.func_offset, 0x0u);
  EXPECT_THAT(target_norm.intra_function_target_offset, Optional(0x20u));
  EXPECT_THAT(compiled_norm.intra_function_target_offset, Optional(0x20u));

  // Raw machine code is bit-exact because MIPS PC-relative displacement is identical.
  EXPECT_EQ(normalizer.Compare(target_norm, compiled_norm), MatchQuality::kBitExact);
}

TEST(NormalizerTest, NormalizesIntraFunctionUnconditionalJumps) {
  FunctionDiffContext ctx;
  ctx.target_vram = 0x800266C0;
  ctx.target_size = 0x100;
  ctx.compiled_vram = 0x00000000;
  ctx.compiled_size = 0x100;

  Normalizer normalizer(ctx);

  // Target ROM: j 0x800266E0 (+0x20 into function)
  uint32_t target_rom_jump = (0x02 << 26) | ((0x800266E0 & 0x0FFFFFFF) >> 2);
  auto target_inst = DecodeInstruction(target_rom_jump, 0x800266D0);
  ASSERT_TRUE(target_inst.ok());

  // Compiled .o: j 0x00000020 (+0x20 into section)
  uint32_t compiled_jump = (0x02 << 26) | ((0x00000020 & 0x0FFFFFFF) >> 2);
  auto compiled_inst = DecodeInstruction(compiled_jump, 0x00000010);
  ASSERT_TRUE(compiled_inst.ok());

  // Raw words differ due to VRAM load address.
  EXPECT_NE(target_inst->raw_word, compiled_inst->raw_word);

  auto target_norm = normalizer.NormalizeTarget(*target_inst);
  auto compiled_norm = normalizer.NormalizeCompiled(*compiled_inst);

  EXPECT_EQ(target_norm.func_offset, 0x10u);
  EXPECT_EQ(compiled_norm.func_offset, 0x10u);
  EXPECT_THAT(target_norm.intra_function_target_offset, Optional(0x20u));
  EXPECT_THAT(compiled_norm.intra_function_target_offset, Optional(0x20u));

  // Normalized words match bit-exact once 26-bit targets are resolved to relative offsets.
  EXPECT_EQ(target_norm.normalized_word, compiled_norm.normalized_word);
  EXPECT_EQ(normalizer.Compare(target_norm, compiled_norm), MatchQuality::kNormalizedExact);
}

TEST(NormalizerTest, ClassifiesSimilarOpcodesDifferingInOperands) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  // addu $t0, $a0, $a1  (rd=8, rs=4, rt=5)
  uint32_t addu_t0 = (0 << 26) | (4 << 21) | (5 << 16) | (8 << 11) | 0x21;
  // addu $t1, $a0, $a1  (rd=9, rs=4, rt=5)
  uint32_t addu_t1 = (0 << 26) | (4 << 21) | (5 << 16) | (9 << 11) | 0x21;

  auto target = DecodeInstruction(addu_t0, 0x800266C0);
  auto compiled = DecodeInstruction(addu_t1, 0x00000000);
  ASSERT_TRUE(target.ok());
  ASSERT_TRUE(compiled.ok());

  auto target_norm = normalizer.NormalizeTarget(*target);
  auto compiled_norm = normalizer.NormalizeCompiled(*compiled);

  EXPECT_EQ(normalizer.Compare(target_norm, compiled_norm), MatchQuality::kSimilarOpcode);
}

TEST(NormalizerTest, ClassifiesDifferentOpcodes) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  // addu $t0, $a0, $a1
  uint32_t addu = (0 << 26) | (4 << 21) | (5 << 16) | (8 << 11) | 0x21;
  // subu $t0, $a0, $a1
  uint32_t subu = (0 << 26) | (4 << 21) | (5 << 16) | (8 << 11) | 0x23;

  auto target = DecodeInstruction(addu, 0x800266C0);
  auto compiled = DecodeInstruction(subu, 0x00000000);
  ASSERT_TRUE(target.ok());
  ASSERT_TRUE(compiled.ok());

  auto target_norm = normalizer.NormalizeTarget(*target);
  auto compiled_norm = normalizer.NormalizeCompiled(*compiled);

  EXPECT_EQ(normalizer.Compare(target_norm, compiled_norm), MatchQuality::kDifferent);
}

TEST(NormalizerTest, HandlesBranchesOutsideFunctionBoundary) {
  FunctionDiffContext ctx;
  ctx.target_vram = 0x800266C0;
  ctx.target_size = 0x40;  // Ends at 0x80026700

  Normalizer normalizer(ctx);

  // Branch jumping past function end to 0x80026720
  // Displacement in words: (0x80026720 - (0x800266C0 + 4)) / 4 = (0x60 - 4) / 4 = 23
  uint32_t branch_word = (0x04 << 26) | (2 << 21) | (0 << 16) | 23;
  auto target_inst = DecodeInstruction(branch_word, 0x800266C0);
  ASSERT_TRUE(target_inst.ok());

  auto target_norm = normalizer.NormalizeTarget(*target_inst);
  EXPECT_FALSE(target_norm.intra_function_target_offset.has_value());
}

}  // namespace
}  // namespace rom_nom_nom
