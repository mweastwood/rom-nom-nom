#include "splitter/assembly_generator.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "splitter/disassembler.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

TEST(AssemblyGeneratorTest, EmitMacroIncludesCreatesMacroInc) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "asm_gen_macro_test";
  std::filesystem::remove_all(temp_dir);

  AssemblyGeneratorOptions options;
  options.asm_dir = temp_dir;
  AssemblyGenerator generator(options);

  ASSERT_TRUE(generator.EmitMacroIncludes().ok());

  std::filesystem::path macro_inc = temp_dir / "macro.inc";
  ASSERT_TRUE(std::filesystem::exists(macro_inc));

  std::ifstream in(macro_inc);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(content.find(".macro glabel"), std::string::npos);
  EXPECT_NE(content.find(".macro dlabel"), std::string::npos);
  EXPECT_NE(content.find(".set $fa0,"), std::string::npos);

  std::filesystem::remove_all(temp_dir);
}

TEST(AssemblyGeneratorTest, WriteHeaderAssemblyEmitsWords) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "asm_gen_header_test";
  std::filesystem::remove_all(temp_dir);

  AssemblyGeneratorOptions options;
  options.asm_dir = temp_dir;
  AssemblyGenerator generator(options);

  // 16 bytes: 4 words
  std::vector<uint8_t> header_bytes = {
      0x80, 0x37, 0x12, 0x40,  // 0x80371240
      0x00, 0x00, 0x00, 0x0F,  // 0x0000000F
      0x80, 0x02, 0x5C, 0x00,  // 0x80025C00
      0x00, 0x00, 0x14, 0x44,  // 0x00001444
  };

  std::filesystem::path out_path = temp_dir / "header.s";
  ASSERT_TRUE(generator.WriteHeaderAssembly(header_bytes, out_path).ok());

  ASSERT_TRUE(std::filesystem::exists(out_path));
  std::ifstream in(out_path);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(content.find(".section .data"), std::string::npos);
  EXPECT_NE(content.find(".word 0x80371240"), std::string::npos);
  EXPECT_NE(content.find(".word 0x80025C00"), std::string::npos);

  std::filesystem::remove_all(temp_dir);
}

TEST(AssemblyGeneratorTest, WriteDataAndBssAssembly) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "asm_gen_data_test";
  std::filesystem::remove_all(temp_dir);

  AssemblyGeneratorOptions options;
  options.asm_dir = temp_dir;
  AssemblyGenerator generator(options);

  // 5 bytes: 1 word + 1 trailing byte
  std::vector<uint8_t> data_bytes = {0x12, 0x34, 0x56, 0x78, 0x9A};
  std::filesystem::path data_path = generator.ResolveDataPath("sample");
  EXPECT_EQ(data_path, temp_dir / "data" / "sample.data.s");

  ASSERT_TRUE(generator.WriteDataAssembly(data_bytes, 0x80701000, data_path).ok());
  ASSERT_TRUE(std::filesystem::exists(data_path));

  std::ifstream data_in(data_path);
  std::string data_content((std::istreambuf_iterator<char>(data_in)),
                           std::istreambuf_iterator<char>());
  EXPECT_NE(data_content.find("dlabel D_80701000"), std::string::npos);
  EXPECT_NE(data_content.find(".word 0x12345678"), std::string::npos);
  EXPECT_NE(data_content.find(".byte 0x9A"), std::string::npos);
  EXPECT_NE(data_content.find("enddlabel D_80701000"), std::string::npos);

  // BSS
  std::filesystem::path bss_path = generator.ResolveBssPath("sample");
  EXPECT_EQ(bss_path, temp_dir / "data" / "sample.bss.s");

  ASSERT_TRUE(generator.WriteBssAssembly(0x100, 0x80702000, bss_path).ok());
  ASSERT_TRUE(std::filesystem::exists(bss_path));

  std::ifstream bss_in(bss_path);
  std::string bss_content((std::istreambuf_iterator<char>(bss_in)),
                          std::istreambuf_iterator<char>());
  EXPECT_NE(bss_content.find("dlabel D_80702000"), std::string::npos);
  EXPECT_NE(bss_content.find(".space 0x100"), std::string::npos);
  EXPECT_NE(bss_content.find("enddlabel D_80702000"), std::string::npos);

  std::filesystem::remove_all(temp_dir);
}

TEST(AssemblyGeneratorTest, WriteNonmatchingFunctionsAndStandaloneAssembly) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "asm_gen_funcs_test";
  std::filesystem::remove_all(temp_dir);

  AssemblyGeneratorOptions options;
  options.asm_dir = temp_dir;
  AssemblyGenerator generator(options);

  DisassembledFunction func;
  func.name = "MyTestFunc";
  func.vram_start = 0x80700000;
  func.vram_end = 0x80700008;
  func.emitted_assembly = "glabel MyTestFunc\n  jr    $ra\n   nop\nendlabel MyTestFunc";

  std::vector<DisassembledFunction> funcs = {func};

  // 1. Nonmatching functions
  auto count_or = generator.WriteNonmatchingFunctions("my_module", funcs);
  ASSERT_TRUE(count_or.ok());
  EXPECT_EQ(*count_or, 1u);

  std::filesystem::path func_file = temp_dir / "nonmatchings" / "my_module" / "MyTestFunc.s";
  ASSERT_TRUE(std::filesystem::exists(func_file));

  std::ifstream f_in(func_file);
  std::string f_content((std::istreambuf_iterator<char>(f_in)), std::istreambuf_iterator<char>());
  EXPECT_NE(f_content.find("glabel MyTestFunc"), std::string::npos);

  // 2. Standalone assembly
  std::filesystem::path standalone_file = temp_dir / "standalone.s";
  ASSERT_TRUE(
      generator
          .WriteStandaloneAssembly("standalone", 0x1000, 0x1008, 0x80700000, funcs, standalone_file)
          .ok());
  ASSERT_TRUE(std::filesystem::exists(standalone_file));

  std::ifstream s_in(standalone_file);
  std::string s_content((std::istreambuf_iterator<char>(s_in)), std::istreambuf_iterator<char>());
  EXPECT_NE(s_content.find(".set gp=64"), std::string::npos);
  EXPECT_NE(s_content.find(".set noat"), std::string::npos);
  EXPECT_NE(s_content.find(".set noreorder"), std::string::npos);
  EXPECT_NE(s_content.find("glabel MyTestFunc"), std::string::npos);

  std::filesystem::remove_all(temp_dir);
}

}  // namespace
}  // namespace rom_nom_nom
