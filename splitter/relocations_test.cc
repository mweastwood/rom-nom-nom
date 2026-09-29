#include "splitter/relocations.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "gtest/gtest.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

void AppendWord(std::vector<uint8_t>& buf, uint32_t word) {
  buf.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(word & 0xFF));
}

constexpr std::string_view kSymbolsProtoText = R"pb(
  entries { name: "g_my_symbol" address: 0x80701000 type: SYMBOL_DATA }
  entries { name: "g_config" address: 0x80702000 size: 0x20 type: SYMBOL_DATA }
)pb";

TEST(RelocationsTest, PairLuiAddiuExactSymbol) {
  auto symbols_or = SymbolIndex::ParseFromTextproto(kSymbolsProtoText);
  ASSERT_TRUE(symbols_or.ok());

  std::vector<uint8_t> code;
  // lui   $a0, 0x8070 (0x3C048070)
  AppendWord(code, 0x3C048070);
  // addiu $a0, $a0, 0x1000 (0x24841000)
  AppendWord(code, 0x24841000);

  RelocationTracker tracker(&*symbols_or);
  ASSERT_TRUE(tracker.AnalyzeCode(code, 0x80700000).ok());

  ASSERT_EQ(tracker.Size(), 2u);

  const auto* hi_reloc = tracker.FindRelocation(0x80700000);
  ASSERT_NE(hi_reloc, nullptr);
  EXPECT_EQ(hi_reloc->type, RelocType::kHi16);
  EXPECT_EQ(hi_reloc->target_vram, 0x80701000u);
  EXPECT_EQ(hi_reloc->symbol_name, "g_my_symbol");
  EXPECT_EQ(hi_reloc->addend, 0);

  const auto* lo_reloc = tracker.FindRelocation(0x80700004);
  ASSERT_NE(lo_reloc, nullptr);
  EXPECT_EQ(lo_reloc->type, RelocType::kLo16);
  EXPECT_EQ(lo_reloc->target_vram, 0x80701000u);
  EXPECT_EQ(lo_reloc->symbol_name, "g_my_symbol");
  EXPECT_EQ(lo_reloc->addend, 0);

  auto inst_hi = *DecodeInstruction(absl::MakeConstSpan(code).subspan(0, 4), 0x80700000);
  auto inst_lo = *DecodeInstruction(absl::MakeConstSpan(code).subspan(4, 4), 0x80700004);

  EXPECT_EQ(tracker.FormatInstruction(inst_hi), R"(lui   $a0, %hi(g_my_symbol))");
  EXPECT_EQ(tracker.FormatInstruction(inst_lo), R"(addiu $a0, $a0, %lo(g_my_symbol))");
}

TEST(RelocationsTest, PairLuiSignedNegativeLo) {
  // Target: 0x80708BD4 (unregistered, in KSEG0)
  // %hi(0x80708BD4): (0x80708BD4 + 0x8000) >> 16 = 0x8071
  // %lo(0x80708BD4): 0x8BD4 (sign-extended is negative: -29740)
  std::vector<uint8_t> code;
  // lui   $s3, 0x8071 (0x3C138071)
  AppendWord(code, 0x3C138071);
  // addiu $s3, $s3, 0x8BD4 (0x26738BD4)
  AppendWord(code, 0x26738BD4);

  RelocationTracker tracker;
  ASSERT_TRUE(tracker.AnalyzeCode(code, 0x80700000).ok());

  ASSERT_EQ(tracker.Size(), 2u);

  const auto* hi_reloc = tracker.FindRelocation(0x80700000);
  ASSERT_NE(hi_reloc, nullptr);
  EXPECT_EQ(hi_reloc->target_vram, 0x80708BD4u);
  EXPECT_EQ(hi_reloc->symbol_name, "D_80708BD4");

  const auto* lo_reloc = tracker.FindRelocation(0x80700004);
  ASSERT_NE(lo_reloc, nullptr);
  EXPECT_EQ(lo_reloc->target_vram, 0x80708BD4u);
  EXPECT_EQ(lo_reloc->symbol_name, "D_80708BD4");

  auto inst_hi = *DecodeInstruction(absl::MakeConstSpan(code).subspan(0, 4), 0x80700000);
  auto inst_lo = *DecodeInstruction(absl::MakeConstSpan(code).subspan(4, 4), 0x80700004);

  EXPECT_EQ(tracker.FormatInstruction(inst_hi), R"(lui   $s3, %hi(D_80708BD4))");
  EXPECT_EQ(tracker.FormatInstruction(inst_lo), R"(addiu $s3, $s3, %lo(D_80708BD4))");
}

TEST(RelocationsTest, PairLuiMemoryStoreMultipleWithAddend) {
  auto symbols_or = SymbolIndex::ParseFromTextproto(kSymbolsProtoText);
  ASSERT_TRUE(symbols_or.ok());

  std::vector<uint8_t> code;
  // lui $at, 0x8070 (0x3C018070)
  AppendWord(code, 0x3C018070);
  // sw  $zero, 0x2000($at) (0xAC202000) -> base g_config
  AppendWord(code, 0xAC202000);
  // sb  $zero, 0x2004($at) (0xA0202004) -> g_config + 0x4
  AppendWord(code, 0xA0202004);

  RelocationTracker tracker(&*symbols_or);
  ASSERT_TRUE(tracker.AnalyzeCode(code, 0x80700000).ok());

  ASSERT_EQ(tracker.Size(), 3u);

  const auto* lo1 = tracker.FindRelocation(0x80700004);
  ASSERT_NE(lo1, nullptr);
  EXPECT_EQ(lo1->symbol_name, "g_config");
  EXPECT_EQ(lo1->addend, 0);

  const auto* lo2 = tracker.FindRelocation(0x80700008);
  ASSERT_NE(lo2, nullptr);
  EXPECT_EQ(lo2->symbol_name, "g_config");
  EXPECT_EQ(lo2->addend, 0x4);

  auto inst_sw = *DecodeInstruction(absl::MakeConstSpan(code).subspan(4, 4), 0x80700004);
  auto inst_sb = *DecodeInstruction(absl::MakeConstSpan(code).subspan(8, 4), 0x80700008);

  EXPECT_EQ(tracker.FormatInstruction(inst_sw), R"(sw    $zero, %lo(g_config)($at))");
  EXPECT_EQ(tracker.FormatInstruction(inst_sb), R"(sb    $zero, %lo(g_config + 0x4)($at))");
}

TEST(RelocationsTest, RejectNonAddressConstants) {
  std::vector<uint8_t> code;
  // lui  $at, 0x437F (0x3C01437F) -> 0x437F0000 (float constant, not address)
  AppendWord(code, 0x3C01437F);
  // ori  $at, $at, 0x1234
  AppendWord(code, 0x34211234);

  RelocationTracker tracker;
  ASSERT_TRUE(tracker.AnalyzeCode(code, 0x80700000).ok());

  EXPECT_EQ(tracker.Size(), 0u);
}

TEST(RelocationsTest, ClobberedRegisterNotPaired) {
  std::vector<uint8_t> code;
  // lui   $t0, 0x8070
  AppendWord(code, 0x3C088070);
  // addu  $t0, $zero, $zero (clobbers $t0)
  AppendWord(code, 0x00004021);
  // addiu $t1, $t0, 0x1000
  AppendWord(code, 0x25091000);

  RelocationTracker tracker;
  ASSERT_TRUE(tracker.AnalyzeCode(code, 0x80700000).ok());

  EXPECT_EQ(tracker.Size(), 0u);
}

}  // namespace
}  // namespace rom_nom_nom
