#include "differ/diff_formatter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "core/mips.h"
#include "differ/normalizer.h"
#include "differ/sequence_aligner.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;

NormalizedInstruction MakeNorm(uint32_t raw_word, uint32_t vram, const Normalizer& normalizer,
                               bool is_target) {
  auto inst = DecodeInstruction(raw_word, vram);
  EXPECT_TRUE(inst.ok());
  if (is_target) {
    return normalizer.NormalizeTarget(*inst);
  }
  return normalizer.NormalizeCompiled(*inst);
}

TEST(DiffFormatterTest, FormatsPlainMatchRow) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  uint32_t addiu = 0x27BDFFE0;  // addiu $sp, $sp, -32
  auto target_norm = MakeNorm(addiu, 0x80025C00, normalizer, /*is_target=*/true);
  auto compiled_norm = MakeNorm(addiu, 0x0, normalizer, /*is_target=*/false);

  AlignedDiffRow row;
  row.kind = DiffRowKind::kMatch;
  row.quality = MatchQuality::kBitExact;
  row.target = target_norm;
  row.compiled = compiled_norm;

  DiffFormatterOptions options;
  options.use_color = false;
  options.column_width = 40;

  DiffFormatter formatter(options);
  std::string formatted = formatter.FormatRow(row);

  EXPECT_THAT(formatted, HasSubstr("+0x000: 27BDFFE0  addiu $sp, $sp, -32"));
  EXPECT_THAT(formatted, HasSubstr(" | "));
  EXPECT_THAT(formatted, HasSubstr("[OK]"));
}

TEST(DiffFormatterTest, FormatsColoredMatchRow) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  uint32_t addiu = 0x27BDFFE0;
  auto target_norm = MakeNorm(addiu, 0x80025C00, normalizer, /*is_target=*/true);
  auto compiled_norm = MakeNorm(addiu, 0x0, normalizer, /*is_target=*/false);

  AlignedDiffRow row;
  row.kind = DiffRowKind::kMatch;
  row.quality = MatchQuality::kBitExact;
  row.target = target_norm;
  row.compiled = compiled_norm;

  DiffFormatterOptions options;
  options.use_color = true;

  DiffFormatter formatter(options);
  std::string formatted = formatter.FormatRow(row);

  // Expect ANSI green escape sequence: \033[32m
  EXPECT_THAT(formatted, HasSubstr("\033[32m"));
  EXPECT_THAT(formatted, HasSubstr("\033[0m"));
  EXPECT_THAT(formatted, HasSubstr("[OK]"));
}

TEST(DiffFormatterTest, FormatsDifferenceRow) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  uint32_t addu_t0 = (0 << 26) | (4 << 21) | (5 << 16) | (8 << 11) | 0x21;
  uint32_t addu_t1 = (0 << 26) | (4 << 21) | (5 << 16) | (9 << 11) | 0x21;

  auto target_norm = MakeNorm(addu_t0, 0x80025C00, normalizer, /*is_target=*/true);
  auto compiled_norm = MakeNorm(addu_t1, 0x0, normalizer, /*is_target=*/false);

  AlignedDiffRow row;
  row.kind = DiffRowKind::kDifference;
  row.quality = MatchQuality::kSimilarOpcode;
  row.target = target_norm;
  row.compiled = compiled_norm;

  DiffFormatterOptions options;
  options.use_color = false;

  DiffFormatter formatter(options);
  std::string formatted = formatter.FormatRow(row);

  EXPECT_THAT(formatted, HasSubstr("[DIFF]"));
}

TEST(DiffFormatterTest, FormatsTargetOnlyRow) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  uint32_t nop = 0x00000000;
  auto target_norm = MakeNorm(nop, 0x80025C00, normalizer, /*is_target=*/true);

  AlignedDiffRow row;
  row.kind = DiffRowKind::kTargetOnly;
  row.quality = MatchQuality::kDifferent;
  row.target = target_norm;
  row.compiled = std::nullopt;

  DiffFormatterOptions options;
  options.use_color = false;

  DiffFormatter formatter(options);
  std::string formatted = formatter.FormatRow(row);

  EXPECT_THAT(formatted, HasSubstr("nop"));
  EXPECT_THAT(formatted, HasSubstr("[MISSING]"));
}

TEST(DiffFormatterTest, FormatsCompiledOnlyRow) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  uint32_t nop = 0x00000000;
  auto compiled_norm = MakeNorm(nop, 0x0, normalizer, /*is_target=*/false);

  AlignedDiffRow row;
  row.kind = DiffRowKind::kCompiledOnly;
  row.quality = MatchQuality::kDifferent;
  row.target = std::nullopt;
  row.compiled = compiled_norm;

  DiffFormatterOptions options;
  options.use_color = false;

  DiffFormatter formatter(options);
  std::string formatted = formatter.FormatRow(row);

  EXPECT_THAT(formatted, HasSubstr("nop"));
  EXPECT_THAT(formatted, HasSubstr("[EXTRA]"));
}

TEST(DiffFormatterTest, FormatsBitExactSummaryBanner) {
  AlignmentStats stats;
  stats.target_instruction_count = 10;
  stats.compiled_instruction_count = 10;
  stats.matching_instructions = 10;
  stats.differing_instructions = 0;
  stats.target_only_instructions = 0;
  stats.compiled_only_instructions = 0;

  DiffFormatterOptions options;
  options.use_color = false;
  DiffFormatter formatter(options);

  std::string summary = formatter.FormatSummary(stats);
  EXPECT_THAT(summary, HasSubstr("*** [MATCH 100%] 10/10 instructions match bit-exact! ***"));
}

TEST(DiffFormatterTest, FormatsMismatchSummaryBanner) {
  AlignmentStats stats;
  stats.target_instruction_count = 10;
  stats.compiled_instruction_count = 9;
  stats.matching_instructions = 8;
  stats.differing_instructions = 1;
  stats.target_only_instructions = 1;
  stats.compiled_only_instructions = 0;

  DiffFormatterOptions options;
  options.use_color = false;
  DiffFormatter formatter(options);

  std::string summary = formatter.FormatSummary(stats);
  EXPECT_THAT(summary, HasSubstr("Match: 8/10 instructions (80.0%) [1 diff, 1 missing, 0 extra]"));
}

TEST(DiffFormatterTest, FormatsBranchWithRelativeLabels) {
  FunctionDiffContext ctx;
  ctx.target_vram = 0x800266C0;
  ctx.target_size = 0x100;

  Normalizer normalizer(ctx);

  // beqz $v0, 0x800266E0 (+0x20 bytes forward)
  uint32_t branch_word = (0x04 << 26) | (2 << 21) | (0 << 16) | 7;
  auto target_norm = MakeNorm(branch_word, 0x800266C0, normalizer, /*is_target=*/true);

  DiffFormatterOptions options;
  options.use_color = false;
  DiffFormatter formatter(options);

  std::string formatted = formatter.FormatInstructionSide(target_norm);
  EXPECT_THAT(formatted, HasSubstr("beqz   $v0, .L_0020"));
}

TEST(DiffFormatterTest, FormatsJalWithSymbolIndex) {
  FunctionDiffContext ctx;
  Normalizer normalizer(ctx);

  auto index_or = SymbolIndex::ParseFromTextproto(R"pb(
    entries { name: "AudioUpdate" address: 0x8003CF38 type: SYMBOL_FUNC }
  )pb");
  ASSERT_TRUE(index_or.ok()) << index_or.status();

  DiffFormatterOptions options;
  options.use_color = false;
  DiffFormatter formatter(options, &*index_or);

  // jal 0x8003CF38
  uint32_t jal_word = (0x03 << 26) | ((0x8003CF38 & 0x0FFFFFFF) >> 2);
  auto norm = MakeNorm(jal_word, 0x800266C4, normalizer, /*is_target=*/true);

  std::string formatted = formatter.FormatInstructionSide(norm);
  EXPECT_THAT(formatted, HasSubstr("jal    AudioUpdate"));
}

}  // namespace
}  // namespace rom_nom_nom
