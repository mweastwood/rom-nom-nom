#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "splitter/disassembler.h"
#include "splitter/jump_tables.h"
#include "splitter/relocations.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

std::filesystem::path FindWorkspaceFile(std::string_view relative_path) {
  // Check direct path from current working directory
  if (std::filesystem::exists(relative_path)) {
    return std::filesystem::absolute(relative_path);
  }

  // Check Bazel test runfiles environment
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  if (test_srcdir != nullptr) {
    std::filesystem::path p(test_srcdir);
    if (test_workspace != nullptr) {
      p /= test_workspace;
    } else {
      p /= "_main";
    }
    p /= relative_path;
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  // Fallback to relative path
  return relative_path;
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return {};
  }
  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(size);
  if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return buffer;
  }
  return {};
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream file(path, std::ios::out | std::ios::trunc);
  file.write(text.data(), text.size());
}

TEST(RoundtripIntegrationTest, CustomCCodeCompilesDisassemblesAndReassemblesBitExact) {
  std::filesystem::path gcc_bin = FindWorkspaceFile("toolchain/gcc-2.7.2/gcc");
  std::filesystem::path as_bin = FindWorkspaceFile("toolchain/binutils-2.6/as");
  std::filesystem::path sample_c = FindWorkspaceFile("splitter/testdata/custom_sample.c");

  ASSERT_TRUE(std::filesystem::exists(gcc_bin)) << "GCC binary not found: " << gcc_bin;
  ASSERT_TRUE(std::filesystem::exists(as_bin)) << "AS binary not found: " << as_bin;
  ASSERT_TRUE(std::filesystem::exists(sample_c)) << "Sample C file not found: " << sample_c;

  // Use Bazel test temp directory or system temp directory
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  std::filesystem::path tmp_dir =
      (test_tmpdir != nullptr) ? test_tmpdir : std::filesystem::temp_directory_path();
  std::filesystem::path work_dir = tmp_dir / "roundtrip_test";
  std::filesystem::create_directories(work_dir);

  std::filesystem::path orig_s = work_dir / "orig.s";
  std::filesystem::path orig_o = work_dir / "orig.o";
  std::filesystem::path orig_text_bin = work_dir / "orig_text.bin";
  std::filesystem::path reasm_s = work_dir / "reasm.s";
  std::filesystem::path reasm_o = work_dir / "reasm.o";
  std::filesystem::path reasm_text_bin = work_dir / "reasm_text.bin";

  // 1. Compile custom_sample.c with GCC 2.7.2
  std::string gcc_cmd =
      absl::StrFormat("%s -B%s/ -x c -S -O2 -mips2 -G0 %s -o %s", gcc_bin.string(),
                      gcc_bin.parent_path().string(), sample_c.string(), orig_s.string());
  int gcc_ret = std::system(gcc_cmd.c_str());
  ASSERT_EQ(gcc_ret, 0) << "Failed command: " << gcc_cmd;

  // 2. Assemble orig.s with KMC binutils-2.6 as
  std::string as_cmd = absl::StrFormat("%s -mcpu=vr4300 -mips2 -EB -G0 -N %s -o %s",
                                       as_bin.string(), orig_s.string(), orig_o.string());
  int as_ret = std::system(as_cmd.c_str());
  ASSERT_EQ(as_ret, 0) << "Failed command: " << as_cmd;

  // 3. Extract .text section payload from orig.o
  std::string objcopy_cmd = absl::StrFormat("mips-linux-gnu-objcopy -O binary -j .text %s %s",
                                            orig_o.string(), orig_text_bin.string());
  int objcopy_ret = std::system(objcopy_cmd.c_str());
  ASSERT_EQ(objcopy_ret, 0) << "Failed command: " << objcopy_cmd;

  std::vector<uint8_t> orig_text = ReadBinaryFile(orig_text_bin);
  ASSERT_FALSE(orig_text.empty());
  ASSERT_EQ(orig_text.size() % 4, 0u);

  // 4. Disassemble all functions from orig_text with Disassembler
  DisassemblerOptions disasm_options;
  disasm_options.emit_line_comments = false;
  disasm_options.emit_function_framing = true;
  Disassembler disassembler(nullptr, disasm_options);

  auto funcs_or = disassembler.DisassembleAllFunctions(orig_text, 0x00000000);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  const auto& funcs = *funcs_or;
  ASSERT_FALSE(funcs.empty());

  std::string combined_asm;
  for (const auto& func : funcs) {
    combined_asm += func.emitted_assembly;
    combined_asm += "\n";
  }

  WriteTextFile(reasm_s, combined_asm);

  // 5. Reassemble the disassembled assembly
  std::string reasm_cmd = absl::StrFormat("%s -mcpu=vr4300 -mips2 -EB -G0 -N %s -o %s",
                                          as_bin.string(), reasm_s.string(), reasm_o.string());
  int reasm_ret = std::system(reasm_cmd.c_str());
  ASSERT_EQ(reasm_ret, 0) << "Failed reassembly: " << reasm_cmd << "\nAssembly:\n" << combined_asm;

  // 6. Extract .text section payload from reasm.o
  std::string reasm_objcopy_cmd = absl::StrFormat("mips-linux-gnu-objcopy -O binary -j .text %s %s",
                                                  reasm_o.string(), reasm_text_bin.string());
  int reasm_objcopy_ret = std::system(reasm_objcopy_cmd.c_str());
  ASSERT_EQ(reasm_objcopy_ret, 0) << "Failed command: " << reasm_objcopy_cmd;

  std::vector<uint8_t> reasm_text = ReadBinaryFile(reasm_text_bin);
  ASSERT_FALSE(reasm_text.empty());

  // 7. Verify byte-for-byte exact equality between original and reassembled binary!
  EXPECT_EQ(reasm_text.size(), orig_text.size());
  EXPECT_EQ(reasm_text, orig_text);
}

}  // namespace
}  // namespace rom_nom_nom
