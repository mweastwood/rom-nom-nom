#include "splitter/splitter.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"
#include "core/rom.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

std::vector<uint8_t> CreateMinimalN64Rom() {
  // 64-byte cartridge header + 64 bytes of code
  std::vector<uint8_t> rom(128, 0);

  // Big-endian initial PI reg magic: 0x80371240
  rom[0] = 0x80;
  rom[1] = 0x37;
  rom[2] = 0x12;
  rom[3] = 0x40;

  // Entry point: 0x80025C00
  rom[8] = 0x80;
  rom[9] = 0x02;
  rom[10] = 0x5C;
  rom[11] = 0x00;

  // Code at offset 0x40 (PC: 0x80025C00):
  // addiu $v0, $zero, 42 -> 0x2402002A
  rom[0x40] = 0x24;
  rom[0x41] = 0x02;
  rom[0x42] = 0x00;
  rom[0x43] = 0x2A;

  // jr $ra -> 0x03E00008
  rom[0x44] = 0x03;
  rom[0x45] = 0xE0;
  rom[0x46] = 0x00;
  rom[0x47] = 0x08;

  // nop -> 0x00000000 (delay slot)
  rom[0x48] = 0x00;
  rom[0x49] = 0x00;
  rom[0x4A] = 0x00;
  rom[0x4B] = 0x00;

  return rom;
}

std::string BuildTestConfig(std::string_view sha1) {
  return absl::StrFormat(
      R"pb(
        game_name: "Test Game"
        sha1: "%s"
        basename: "test_game"

        segments { name: "header" type: SEGMENT_HEADER rom_start: 0x0 rom_end: 0x40 }

        segments {
          name: "code"
          type: SEGMENT_CODE
          rom_start: 0x40
          rom_end: 0x80
          vram: 0x80025C00

          subsegments { rom_start: 0x40 type: SUBSEGMENT_ASM name: "entrypoint" vram: 0x80025C00 }
        }
      )pb",
      sha1);
}

TEST(SplitterTest, EmptyConfigReturnsError) {
  SplitterOptions options;
  auto result_or = RunSplitter(options);
  EXPECT_FALSE(result_or.ok());
}

TEST(SplitterTest, MissingRomReturnsError) {
  SplitterOptions options;
  options.config_text_override = BuildTestConfig("dummy_sha1");
  options.rom_path = "non_existent_rom.z64";

  auto result_or = RunSplitter(options);
  EXPECT_FALSE(result_or.ok());
}

TEST(SplitterTest, SuccessfulPipelineExecution) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "splitter_test_output";
  std::filesystem::remove_all(temp_dir);

  auto rom_bytes = CreateMinimalN64Rom();
  auto rom_or = Rom::FromBuffer(rom_bytes);
  ASSERT_TRUE(rom_or.ok());
  std::string sha1 = rom_or->Sha1();

  SplitterOptions options;
  options.config_text_override = BuildTestConfig(sha1);
  options.symbols_text_override = R"pb(
    entries { name: "entrypoint" address: 0x80025C00 type: SYMBOL_FUNC }
  )pb";
  options.rom_buffer_override = std::move(rom_bytes);
  options.out_dir = temp_dir / "build";
  options.asm_out_dir = temp_dir / "asm" / "test_game";
  options.verify_sha1 = false;
  options.slice_data = true;
  options.write_ld_script = true;
  options.disassemble_code = true;

  auto result_or = RunSplitter(options);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  const auto& result = *result_or;
  EXPECT_GT(result.bytes_carved, 0u);
  EXPECT_EQ(result.files_written, 1u);  // header.bin
  EXPECT_TRUE(result.ld_script_written);
  EXPECT_EQ(result.asm_files_written, 1u);
  EXPECT_EQ(result.functions_disassembled, 1u);

  // Check generated header slice
  std::filesystem::path header_bin = options.out_dir / "test_game" / "header.bin";
  EXPECT_TRUE(std::filesystem::exists(header_bin));
  EXPECT_EQ(std::filesystem::file_size(header_bin), 0x40u);

  // Check generated linker script and symbols script
  std::filesystem::path ld_script = options.out_dir / "test_game" / "test_game.ld";
  EXPECT_TRUE(std::filesystem::exists(ld_script));

  std::filesystem::path symbols_ld = options.out_dir / "test_game" / "symbols.ld";
  EXPECT_TRUE(std::filesystem::exists(symbols_ld));

  // Check generated assembly file
  std::filesystem::path asm_file = options.asm_out_dir / "entrypoint.s";
  EXPECT_TRUE(std::filesystem::exists(asm_file));

  std::ifstream s_in(asm_file);
  std::string s_content((std::istreambuf_iterator<char>(s_in)), std::istreambuf_iterator<char>());
  EXPECT_NE(s_content.find("entrypoint:"), std::string::npos);
  EXPECT_NE(s_content.find("addiu $v0, $zero, 42"), std::string::npos);
  EXPECT_NE(s_content.find("jr    $ra"), std::string::npos);
  EXPECT_NE(s_content.find("nop"), std::string::npos);

  // Check generated undefined symbol files
  std::filesystem::path undef_syms = options.out_dir / "test_game" / "undefined_syms_auto.txt";
  EXPECT_TRUE(std::filesystem::exists(undef_syms));

  std::filesystem::path undef_funcs = options.out_dir / "test_game" / "undefined_funcs_auto.txt";
  EXPECT_TRUE(std::filesystem::exists(undef_funcs));

  std::filesystem::remove_all(temp_dir);
}

TEST(SplitterTest, Sha1MismatchFails) {
  std::string config_with_sha1 = R"pb(
    game_name: "Test Game"
    sha1: "0000000000000000000000000000000000000000"
    basename: "test_game"

    segments { name: "header" type: SEGMENT_HEADER rom_start: 0x0 rom_end: 0x40 }
  )pb";

  SplitterOptions options;
  options.config_text_override = config_with_sha1;
  options.rom_buffer_override = CreateMinimalN64Rom();
  options.verify_sha1 = true;

  auto result_or = RunSplitter(options);
  EXPECT_FALSE(result_or.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(result_or.status()));
}

}  // namespace
}  // namespace rom_nom_nom
