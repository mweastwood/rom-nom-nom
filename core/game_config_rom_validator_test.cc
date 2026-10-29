#include "core/game_config_rom_validator.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/endian.h"
#include "core/game_config.pb.h"
#include "core/rom.h"
#include "core/sha1.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

void WriteWordsAt(std::vector<uint8_t>* rom_bytes, size_t offset,
                  const std::vector<uint32_t>& words) {
  for (size_t i = 0; i < words.size(); ++i) {
    WriteBigEndian32(rom_bytes->data() + offset + i * 4, words[i]);
  }
}

GameConfig MakeBaseTestConfig(const std::vector<uint8_t>& rom_bytes) {
  GameConfig config;
  config.set_game_name("Test Game");
  config.set_basename("test-game");
  config.set_sha1(ComputeSha1Hex(rom_bytes));
  (*config.mutable_c_flags())["default"].add_flags("-O2");
  (*config.mutable_c_flags())["default"].add_flags("-mips2");

  auto* header = config.add_segments();
  header->set_name("header");
  header->set_type(SEGMENT_HEADER);
  header->set_rom_start(0x0);
  header->set_rom_end(0x40);

  auto* boot = config.add_segments();
  boot->set_name("boot");
  boot->set_type(SEGMENT_BIN);
  boot->set_rom_start(0x40);
  boot->set_rom_end(0x1050);

  return config;
}

TEST(GameConfigRomValidatorTest, AcceptsValidO2O0AndHasmSubsegments) {
  std::vector<uint8_t> rom_bytes(0x2000, 0);
  // Set valid N64 big-endian magic at offset 0.
  WriteBigEndian32(rom_bytes.data(), 0x80371240);

  // Subsegment 1 (0x1050..0x1070): -O0 function with frame pointer ($fp)
  // addiu $sp, $sp, -24
  // sw    $ra, 20($sp)
  // sw    $fp, 16($sp)
  // addu  $fp, $sp, $zero
  // addu  $sp, $fp, $zero
  // lw    $fp, 16($sp)
  // jr    $ra
  // addiu $sp, $sp, 24
  WriteWordsAt(&rom_bytes, 0x1050,
               {0x27BDFFE8, 0xAFBF0014, 0xAFBE0010, 0x03A0F021, 0x03C0E821, 0x8FBE0010, 0x03E00008,
                0x27BD0018});

  // Subsegment 2 (0x1070..0x1090): -O2 function without frame pointer
  // addiu $sp, $sp, -24
  // sw    $ra, 16($sp)
  // jal   0x80025C50
  // nop
  // lw    $ra, 16($sp)
  // jr    $ra
  // addiu $sp, $sp, 24
  // nop
  WriteWordsAt(&rom_bytes, 0x1070,
               {0x27BDFFE8, 0xAFBF0010, 0x0C009714, 0x00000000, 0x8FBF0010, 0x03E00008, 0x27BD0018,
                0x00000000});

  // Subsegment 3 (0x1090..0x10A0): SUBSEGMENT_HASM function with mfc0 $v0, $12
  // mfc0  $v0, $12
  // jr    $ra
  // nop
  // nop
  WriteWordsAt(&rom_bytes, 0x1090, {0x40026000, 0x03E00008, 0x00000000, 0x00000000});

  GameConfig config = MakeBaseTestConfig(rom_bytes);
  (*config.mutable_c_flags())["boot_o0"].add_flags("-O0");

  auto* main_seg = config.add_segments();
  main_seg->set_name("main");
  main_seg->set_type(SEGMENT_CODE);
  main_seg->set_rom_start(0x1050);
  main_seg->set_rom_end(0x10A0);
  main_seg->set_vram(0x80025C50);

  auto* sub1 = main_seg->add_subsegments();
  sub1->set_name("boot_o0");
  sub1->set_type(SUBSEGMENT_ASM);
  sub1->set_rom_start(0x1050);

  auto* sub2 = main_seg->add_subsegments();
  sub2->set_name("game_o2");
  sub2->set_type(SUBSEGMENT_ASM);
  sub2->set_rom_start(0x1070);

  auto* sub3 = main_seg->add_subsegments();
  sub3->set_name("os_hasm");
  sub3->set_type(SUBSEGMENT_HASM);
  sub3->set_rom_start(0x1090);

  auto rom_or = Rom::FromBuffer(rom_bytes);
  ASSERT_TRUE(rom_or.ok());

  EXPECT_TRUE(ValidateGameConfigAgainstRom(config, *rom_or).ok());
}

TEST(GameConfigRomValidatorTest, RejectsHandWrittenAssemblyInsideSubsegmentAsm) {
  std::vector<uint8_t> rom_bytes(0x2000, 0);
  WriteBigEndian32(rom_bytes.data(), 0x80371240);

  // Function at 0x1050..0x1070 contains CACHE, mfc0, self-XOR, and unpaired swl:
  // cache 0x1, 0($a0)   (0xBD810000)
  // mfc0  $t0, $12      (0x40086000)
  // xor   $v1, $v1, $v1 (0x00631826)
  // swl   $zero, 0($a0) (0xA8800000)
  // jr    $ra           (0x03E00008)
  // nop
  WriteWordsAt(&rom_bytes, 0x1050,
               {0xBD810000, 0x40086000, 0x00631826, 0xA8800000, 0x03E00008, 0x00000000, 0x00000000,
                0x00000000});

  GameConfig config = MakeBaseTestConfig(rom_bytes);
  auto* main_seg = config.add_segments();
  main_seg->set_name("main");
  main_seg->set_type(SEGMENT_CODE);
  main_seg->set_rom_start(0x1050);
  main_seg->set_rom_end(0x1070);
  main_seg->set_vram(0x80025C50);

  auto* sub = main_seg->add_subsegments();
  sub->set_name("leaked_hasm");
  sub->set_type(SUBSEGMENT_ASM);
  sub->set_rom_start(0x1050);

  auto rom_or = Rom::FromBuffer(rom_bytes);
  ASSERT_TRUE(rom_or.ok());

  absl::Status status = ValidateGameConfigAgainstRom(config, *rom_or);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "CACHE instruction"));
  EXPECT_TRUE(absl::StrContains(status.message(), "COP0 register transfer"));
  EXPECT_TRUE(absl::StrContains(status.message(), "self-XOR"));
  EXPECT_TRUE(absl::StrContains(status.message(), "unpaired unaligned load/store"));
}

TEST(GameConfigRomValidatorTest, RejectsO0FramePointerInO2SubsegmentAndViceVersa) {
  std::vector<uint8_t> rom_bytes(0x2000, 0);
  WriteBigEndian32(rom_bytes.data(), 0x80371240);

  // Subsegment 1 (0x1050..0x1070): -O0 function placed in -O2 subsegment
  WriteWordsAt(&rom_bytes, 0x1050,
               {0x27BDFFE8, 0xAFBF0014, 0xAFBE0010, 0x03A0F021, 0x03C0E821, 0x8FBE0010, 0x03E00008,
                0x27BD0018});

  // Subsegment 2 (0x1070..0x1090): -O2 function placed in -O0 subsegment
  WriteWordsAt(&rom_bytes, 0x1070,
               {0x27BDFFE8, 0xAFBF0010, 0x0C009714, 0x00000000, 0x8FBF0010, 0x03E00008, 0x27BD0018,
                0x00000000});

  GameConfig config = MakeBaseTestConfig(rom_bytes);
  (*config.mutable_c_flags())["wrong_o0"].add_flags("-O0");

  auto* main_seg = config.add_segments();
  main_seg->set_name("main");
  main_seg->set_type(SEGMENT_CODE);
  main_seg->set_rom_start(0x1050);
  main_seg->set_rom_end(0x1090);
  main_seg->set_vram(0x80025C50);

  auto* sub1 = main_seg->add_subsegments();
  sub1->set_name("wrong_o2");
  sub1->set_type(SUBSEGMENT_ASM);
  sub1->set_rom_start(0x1050);

  auto* sub2 = main_seg->add_subsegments();
  sub2->set_name("wrong_o0");
  sub2->set_type(SUBSEGMENT_ASM);
  sub2->set_rom_start(0x1070);

  auto rom_or = Rom::FromBuffer(rom_bytes);
  ASSERT_TRUE(rom_or.ok());

  ConfigValidationReport report = AnalyzeGameConfigAgainstRom(config, *rom_or);
  ASSERT_EQ(report.violations.size(), 2u);
  EXPECT_TRUE(absl::StrContains(report.violations[0].message, "has an -O0 frame-pointer prologue"));
  EXPECT_TRUE(
      absl::StrContains(report.violations[1].message,
                        "allocates a stack frame without establishing an -O0 frame pointer"));
}

TEST(GameConfigRomValidatorTest, RejectsUnalignedSubsegmentAndFakeHasm) {
  std::vector<uint8_t> rom_bytes(0x2000, 0);
  WriteBigEndian32(rom_bytes.data(), 0x80371240);

  // Plain C leaf function at 0x1054..0x1060
  WriteWordsAt(&rom_bytes, 0x1054, {0x03E00008, 0x24020001, 0x00000000});

  GameConfig config = MakeBaseTestConfig(rom_bytes);
  auto* main_seg = config.add_segments();
  main_seg->set_name("main");
  main_seg->set_type(SEGMENT_CODE);
  main_seg->set_rom_start(0x1050);
  main_seg->set_rom_end(0x1060);
  main_seg->set_vram(0x80025C50);

  auto* sub = main_seg->add_subsegments();
  sub->set_name("unaligned_fake_hasm");
  sub->set_type(SUBSEGMENT_HASM);
  sub->set_rom_start(0x1054);

  auto rom_or = Rom::FromBuffer(rom_bytes);
  ASSERT_TRUE(rom_or.ok());

  ConfigValidationReport report = AnalyzeGameConfigAgainstRom(config, *rom_or);
  ASSERT_EQ(report.violations.size(), 2u);
  EXPECT_TRUE(absl::StrContains(report.violations[0].message, "not 16-byte (0x10) aligned"));
  EXPECT_TRUE(absl::StrContains(report.violations[1].message,
                                "none of its functions contain hand-written assembly signatures"));
}

}  // namespace
}  // namespace rom_nom_nom
