#include "core/candidate_compiler.h"

#include <elf.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/endian.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::SizeIs;

std::vector<uint8_t> BuildCandidateMipsElfWithJumps() {
  std::vector<uint8_t> elf;

  // 1. ELF Header (52 bytes)
  elf.push_back(0x7F);
  elf.push_back('E');
  elf.push_back('L');
  elf.push_back('F');
  elf.push_back(ELFCLASS32);
  elf.push_back(ELFDATA2MSB);
  elf.push_back(EV_CURRENT);
  elf.push_back(0);
  for (int i = 0; i < 8; ++i) elf.push_back(0);

  AppendBigEndian16(elf, ET_REL);
  AppendBigEndian16(elf, EM_MIPS);
  AppendBigEndian32(elf, 1);
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, 0);
  size_t shoff_pos = elf.size();
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, 0);
  AppendBigEndian16(elf, 52);
  AppendBigEndian16(elf, 0);
  AppendBigEndian16(elf, 0);
  AppendBigEndian16(elf, 40);
  AppendBigEndian16(elf, 6);
  AppendBigEndian16(elf, 2);

  // 2. .text section content (offset 64)
  while (elf.size() < 64) elf.push_back(0);
  uint32_t text_offset = static_cast<uint32_t>(elf.size());
  // TargetFunc (4 instructions = 16 bytes):
  //   0x00: jal NextFunc (relocated by R_MIPS_26 to NextFunc = 0x8002100C)
  //   0x04: nop
  //   0x08: j   0x80021000 (0x08008400)
  //   0x0C: nop
  AppendBigEndian32(elf, 0x0C000000);  // jal placeholder
  AppendBigEndian32(elf, 0x00000000);  // nop
  AppendBigEndian32(elf, 0x08008400);  // j 0x80021000
  AppendBigEndian32(elf, 0x00000000);  // nop
  uint32_t text_size = 16;

  // 3. .shstrtab
  uint32_t shstrtab_offset = static_cast<uint32_t>(elf.size());
  std::string shstrtab_data;
  shstrtab_data.push_back('\0');
  size_t sh_text_name = shstrtab_data.size();
  shstrtab_data.append(".text\0", 6);
  size_t sh_shstrtab_name = shstrtab_data.size();
  shstrtab_data.append(".shstrtab\0", 10);
  size_t sh_symtab_name = shstrtab_data.size();
  shstrtab_data.append(".symtab\0", 8);
  size_t sh_strtab_name = shstrtab_data.size();
  shstrtab_data.append(".strtab\0", 8);
  size_t sh_reltext_name = shstrtab_data.size();
  shstrtab_data.append(".rel.text\0", 10);
  for (char c : shstrtab_data) elf.push_back(static_cast<uint8_t>(c));
  uint32_t shstrtab_size = static_cast<uint32_t>(shstrtab_data.size());

  // 4. .strtab
  uint32_t strtab_offset = static_cast<uint32_t>(elf.size());
  std::string strtab_data;
  strtab_data.push_back('\0');
  size_t str_func_name = strtab_data.size();
  strtab_data.append("TargetFunc\0", 11);
  size_t str_callee_name = strtab_data.size();
  strtab_data.append("NextFunc\0", 9);
  for (char c : strtab_data) elf.push_back(static_cast<uint8_t>(c));
  uint32_t strtab_size = static_cast<uint32_t>(strtab_data.size());

  // 5. .symtab
  uint32_t symtab_offset = static_cast<uint32_t>(elf.size());
  for (int i = 0; i < 16; ++i) elf.push_back(0);
  // Symbol 1: TargetFunc
  AppendBigEndian32(elf, static_cast<uint32_t>(str_func_name));
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, text_size);
  elf.push_back((STB_GLOBAL << 4) | (STT_FUNC & 0xF));
  elf.push_back(0);
  AppendBigEndian16(elf, 1);
  // Symbol 2: NextFunc (undefined external symbol resolved via SymbolIndex)
  AppendBigEndian32(elf, static_cast<uint32_t>(str_callee_name));
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, 0);
  elf.push_back((STB_GLOBAL << 4) | (STT_NOTYPE & 0xF));
  elf.push_back(0);
  AppendBigEndian16(elf, SHN_UNDEF);
  uint32_t symtab_size = 3 * 16;

  // 6. .rel.text: R_MIPS_26 (type 4) at offset 0x0 for symbol 2 (NextFunc)
  uint32_t rel_offset = static_cast<uint32_t>(elf.size());
  AppendBigEndian32(elf, 0x0);
  AppendBigEndian32(elf, (2 << 8) | 4);
  uint32_t rel_size = 8;

  // 7. Section Header Table
  uint32_t sh_table_offset = static_cast<uint32_t>(elf.size());
  WriteBigEndian32(elf.data() + shoff_pos, sh_table_offset);

  auto append_shdr = [&](uint32_t name, uint32_t type, uint32_t flags, uint32_t offset,
                         uint32_t size, uint32_t link, uint32_t info, uint32_t entsize) {
    AppendBigEndian32(elf, name);
    AppendBigEndian32(elf, type);
    AppendBigEndian32(elf, flags);
    AppendBigEndian32(elf, 0);
    AppendBigEndian32(elf, offset);
    AppendBigEndian32(elf, size);
    AppendBigEndian32(elf, link);
    AppendBigEndian32(elf, info);
    AppendBigEndian32(elf, 4);
    AppendBigEndian32(elf, entsize);
  };

  append_shdr(0, SHT_NULL, 0, 0, 0, 0, 0, 0);
  append_shdr(static_cast<uint32_t>(sh_text_name), SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
              text_offset, text_size, 0, 0, 0);
  append_shdr(static_cast<uint32_t>(sh_shstrtab_name), SHT_STRTAB, 0, shstrtab_offset,
              shstrtab_size, 0, 0, 0);
  append_shdr(static_cast<uint32_t>(sh_symtab_name), SHT_SYMTAB, 0, symtab_offset, symtab_size, 4,
              1, 16);
  append_shdr(static_cast<uint32_t>(sh_strtab_name), SHT_STRTAB, 0, strtab_offset, strtab_size, 0,
              0, 0);
  append_shdr(static_cast<uint32_t>(sh_reltext_name), SHT_REL, 0, rel_offset, rel_size, 3, 1, 8);

  return elf;
}

class CandidateCompilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "candidate_compiler_test";
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);

    std::string proto_syms = R"(
entries {
  name: "TargetFunc"
  address: 0x80021000
  type: SYMBOL_FUNC
}
entries {
  name: "NextFunc"
  address: 0x8002100C
  type: SYMBOL_FUNC
}
)";
    auto syms_or = SymbolIndex::ParseFromTextproto(proto_syms);
    ASSERT_TRUE(syms_or.ok()) << syms_or.status();
    symbols_ = std::move(*syms_or);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
  SymbolIndex symbols_;
};

TEST_F(CandidateCompilerTest, CompileCandidateFunctionPreservesUpperNibbleOnJumpTargets) {
  // Set up mock toolchain executables inside test_dir_ so Toolchain::Discover succeeds
  // and kmc-as writes a prebuilt relocatable MIPS ELF object to the requested -o path.
  std::filesystem::create_directories(test_dir_ / "bin");
  std::filesystem::create_directories(test_dir_ / "toolchain" / "gcc-2.7.2");
  std::filesystem::create_directories(test_dir_ / "toolchain" / "binutils-2.6");
  std::filesystem::create_directories(test_dir_ / "src");

  std::filesystem::path prebuilt_elf_path = test_dir_ / "prebuilt_candidate.o";
  {
    auto elf_bytes = BuildCandidateMipsElfWithJumps();
    std::ofstream elf_out(prebuilt_elf_path, std::ios::binary);
    elf_out.write(reinterpret_cast<const char*>(elf_bytes.data()), elf_bytes.size());
  }

  auto write_executable = [](const std::filesystem::path& path, const std::string& script) {
    std::ofstream out(path);
    out << script;
    out.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace);
  };

  const std::string noop_script = "#!/bin/sh\nexit 0\n";
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-as", noop_script);
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-ld", noop_script);
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-objcopy", noop_script);
  write_executable(test_dir_ / "toolchain" / "gcc-2.7.2" / "gcc", noop_script);

  std::string kmc_as_script =
      "#!/bin/sh\n"
      "while [ $# -gt 0 ]; do\n"
      "  if [ \"$1\" = \"-o\" ]; then\n"
      "    cp \"" +
      prebuilt_elf_path.string() +
      "\" \"$2\"\n"
      "    exit 0\n"
      "  fi\n"
      "  shift\n"
      "done\n"
      "exit 1\n";
  write_executable(test_dir_ / "toolchain" / "binutils-2.6" / "as", kmc_as_script);

  std::filesystem::path src_file = test_dir_ / "src" / "candidate.c";
  {
    std::ofstream src_out(src_file);
    src_out << "void TargetFunc(void) {}\n";
  }

  CandidateCompiler compiler(
      {.repo_root = test_dir_, .game_name = "test-game", .symbols = &symbols_});
  auto compiled_or = compiler.CompileCandidateFunction(src_file, "TargetFunc", 0x80021000);
  ASSERT_TRUE(compiled_or.ok()) << compiled_or.status();

  ASSERT_THAT(compiled_or->instructions, SizeIs(4));
  // Instruction 0: jal NextFunc (0x8002100C) at vram 0x80021000
  EXPECT_EQ(compiled_or->instructions[0].vram, 0x80021000u);
  EXPECT_EQ(compiled_or->instructions[0].opcode, Opcode::kJal);
  EXPECT_EQ(compiled_or->instructions[0].JumpTarget() & 0xF0000000u, 0x80000000u);
  EXPECT_EQ(compiled_or->instructions[0].JumpTarget(), 0x8002100Cu);

  // Instruction 2: j 0x80021000 at vram 0x80021008
  EXPECT_EQ(compiled_or->instructions[2].vram, 0x80021008u);
  EXPECT_EQ(compiled_or->instructions[2].opcode, Opcode::kJ);
  EXPECT_EQ(compiled_or->instructions[2].JumpTarget() & 0xF0000000u, 0x80000000u);
  EXPECT_EQ(compiled_or->instructions[2].JumpTarget(), 0x80021000u);
}

TEST_F(CandidateCompilerTest, InferTargetRodataVramExtractsJumpTableAddress) {
  // Sequence:
  //   lui  $at, 0x8012        (0x3C018012)
  //   addu $at, $at, $v0      (0x00220821)
  //   lw   $v0, 0x22B0($at)   (0x8C2222B0)
  //   jr   $v0                (0x00400008)
  //   nop                     (0x00000000)
  std::vector<uint32_t> words = {
      0x3C018012u, 0x00220821u, 0x8C2222B0u, 0x00400008u, 0x00000000u,
  };
  auto instructions_or = DecodeSequence(words, 0x800CF888u);
  ASSERT_TRUE(instructions_or.ok());
  auto rodata_vram = InferTargetRodataVram(*instructions_or);
  ASSERT_TRUE(rodata_vram.has_value());
  EXPECT_EQ(*rodata_vram, 0x801222B0u);

  // Sequence without a jump table returns std::nullopt
  std::vector<uint32_t> non_jump_table_words = {
      0x27BDFFE0u,  // addiu $sp, $sp, -32
      0x03E00008u,  // jr $ra
      0x00000000u,  // nop
  };
  auto non_jump_table_instructions_or = DecodeSequence(non_jump_table_words, 0x80021000u);
  ASSERT_TRUE(non_jump_table_instructions_or.ok());
  EXPECT_EQ(InferTargetRodataVram(*non_jump_table_instructions_or), std::nullopt);
}

TEST_F(CandidateCompilerTest,
       CompileCandidateFunctionResolvesRdataSectionRelocationWhenRodataVramProvided) {
  std::filesystem::create_directories(test_dir_ / "bin");
  std::filesystem::create_directories(test_dir_ / "toolchain" / "gcc-2.7.2");
  std::filesystem::create_directories(test_dir_ / "toolchain" / "binutils-2.6");
  std::filesystem::create_directories(test_dir_ / "src");

  // Build a relocatable ELF with `.text` and `.rdata`, where `.text` has R_MIPS_HI16 and
  // R_MIPS_LO16 against STT_SECTION `.rdata` (section 2) with addend 0x00000008.
  std::vector<uint8_t> elf;
  elf.push_back(0x7F);
  elf.push_back('E');
  elf.push_back('L');
  elf.push_back('F');
  elf.push_back(ELFCLASS32);
  elf.push_back(ELFDATA2MSB);
  elf.push_back(EV_CURRENT);
  elf.push_back(0);
  for (int i = 0; i < 8; ++i) elf.push_back(0);
  AppendBigEndian16(elf, ET_REL);
  AppendBigEndian16(elf, EM_MIPS);
  AppendBigEndian32(elf, 1);
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, 0);
  size_t section_header_offset_position = elf.size();
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, 0);
  AppendBigEndian16(elf, 52);
  AppendBigEndian16(elf, 0);
  AppendBigEndian16(elf, 0);
  AppendBigEndian16(elf, 40);
  AppendBigEndian16(elf, 7);
  AppendBigEndian16(elf, 3);

  while (elf.size() < 64) elf.push_back(0);
  uint32_t text_offset = static_cast<uint32_t>(elf.size());
  // 0x00: lui $at, 0x0000
  // 0x04: lw  $v0, 0x0008($at)
  // 0x08: jr  $v0
  // 0x0C: nop
  AppendBigEndian32(elf, 0x3C010000);
  AppendBigEndian32(elf, 0x8C220008);
  AppendBigEndian32(elf, 0x00400008);
  AppendBigEndian32(elf, 0x00000000);
  uint32_t text_size = 16;

  uint32_t rdata_offset = static_cast<uint32_t>(elf.size());
  AppendBigEndian32(elf, 0x80021000);
  AppendBigEndian32(elf, 0x80021004);
  AppendBigEndian32(elf, 0x80021008);
  uint32_t rdata_size = 12;

  uint32_t shstrtab_offset = static_cast<uint32_t>(elf.size());
  std::string shstrtab_data;
  shstrtab_data.push_back('\0');
  size_t sh_text_name = shstrtab_data.size();
  shstrtab_data.append(".text\0", 6);
  size_t sh_rdata_name = shstrtab_data.size();
  shstrtab_data.append(".rdata\0", 7);
  size_t sh_shstrtab_name = shstrtab_data.size();
  shstrtab_data.append(".shstrtab\0", 10);
  size_t sh_symtab_name = shstrtab_data.size();
  shstrtab_data.append(".symtab\0", 8);
  size_t sh_strtab_name = shstrtab_data.size();
  shstrtab_data.append(".strtab\0", 8);
  size_t sh_reltext_name = shstrtab_data.size();
  shstrtab_data.append(".rel.text\0", 10);
  for (char character : shstrtab_data) elf.push_back(static_cast<uint8_t>(character));
  uint32_t shstrtab_size = static_cast<uint32_t>(shstrtab_data.size());

  uint32_t strtab_offset = static_cast<uint32_t>(elf.size());
  std::string strtab_data;
  strtab_data.push_back('\0');
  size_t str_function_name = strtab_data.size();
  strtab_data.append("TargetFunc\0", 11);
  for (char character : strtab_data) elf.push_back(static_cast<uint8_t>(character));
  uint32_t strtab_size = static_cast<uint32_t>(strtab_data.size());

  uint32_t symtab_offset = static_cast<uint32_t>(elf.size());
  for (int i = 0; i < 16; ++i) elf.push_back(0);
  // Symbol 1: TargetFunc in section 1 (.text)
  AppendBigEndian32(elf, static_cast<uint32_t>(str_function_name));
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, text_size);
  elf.push_back((STB_GLOBAL << 4) | (STT_FUNC & 0xF));
  elf.push_back(0);
  AppendBigEndian16(elf, 1);
  // Symbol 2: section symbol for section 2 (.rdata)
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, 0);
  AppendBigEndian32(elf, 0);
  elf.push_back((STB_LOCAL << 4) | (STT_SECTION & 0xF));
  elf.push_back(0);
  AppendBigEndian16(elf, 2);
  uint32_t symtab_size = 3 * 16;

  // .rel.text: R_MIPS_HI16 (5) at 0x0 and R_MIPS_LO16 (6) at 0x4 for symbol 2 (.rdata)
  uint32_t rel_offset = static_cast<uint32_t>(elf.size());
  AppendBigEndian32(elf, 0x0);
  AppendBigEndian32(elf, (2 << 8) | 5);
  AppendBigEndian32(elf, 0x4);
  AppendBigEndian32(elf, (2 << 8) | 6);
  uint32_t rel_size = 16;

  uint32_t section_header_table_offset = static_cast<uint32_t>(elf.size());
  WriteBigEndian32(elf.data() + section_header_offset_position, section_header_table_offset);

  auto append_section_header = [&](uint32_t name, uint32_t type, uint32_t flags, uint32_t offset,
                                   uint32_t size, uint32_t link, uint32_t info, uint32_t entsize) {
    AppendBigEndian32(elf, name);
    AppendBigEndian32(elf, type);
    AppendBigEndian32(elf, flags);
    AppendBigEndian32(elf, 0);
    AppendBigEndian32(elf, offset);
    AppendBigEndian32(elf, size);
    AppendBigEndian32(elf, link);
    AppendBigEndian32(elf, info);
    AppendBigEndian32(elf, 4);
    AppendBigEndian32(elf, entsize);
  };

  append_section_header(0, SHT_NULL, 0, 0, 0, 0, 0, 0);
  append_section_header(static_cast<uint32_t>(sh_text_name), SHT_PROGBITS,
                        SHF_ALLOC | SHF_EXECINSTR, text_offset, text_size, 0, 0, 0);
  append_section_header(static_cast<uint32_t>(sh_rdata_name), SHT_PROGBITS, SHF_ALLOC, rdata_offset,
                        rdata_size, 0, 0, 0);
  append_section_header(static_cast<uint32_t>(sh_shstrtab_name), SHT_STRTAB, 0, shstrtab_offset,
                        shstrtab_size, 0, 0, 0);
  append_section_header(static_cast<uint32_t>(sh_symtab_name), SHT_SYMTAB, 0, symtab_offset,
                        symtab_size, 5, 1, 16);
  append_section_header(static_cast<uint32_t>(sh_strtab_name), SHT_STRTAB, 0, strtab_offset,
                        strtab_size, 0, 0, 0);
  append_section_header(static_cast<uint32_t>(sh_reltext_name), SHT_REL, 0, rel_offset, rel_size, 4,
                        1, 8);

  std::filesystem::path prebuilt_elf_path = test_dir_ / "prebuilt_rdata.o";
  {
    std::ofstream elf_output(prebuilt_elf_path, std::ios::binary);
    elf_output.write(reinterpret_cast<const char*>(elf.data()), elf.size());
  }

  auto write_executable = [](const std::filesystem::path& path, const std::string& script) {
    std::ofstream output_stream(path);
    output_stream << script;
    output_stream.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace);
  };

  const std::string noop_script = "#!/bin/sh\nexit 0\n";
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-as", noop_script);
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-ld", noop_script);
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-objcopy", noop_script);
  write_executable(test_dir_ / "toolchain" / "gcc-2.7.2" / "gcc", noop_script);

  std::string kmc_as_script =
      "#!/bin/sh\n"
      "while [ $# -gt 0 ]; do\n"
      "  if [ \"$1\" = \"-o\" ]; then\n"
      "    cp \"" +
      prebuilt_elf_path.string() +
      "\" \"$2\"\n"
      "    exit 0\n"
      "  fi\n"
      "  shift\n"
      "done\n"
      "exit 1\n";
  write_executable(test_dir_ / "toolchain" / "binutils-2.6" / "as", kmc_as_script);

  std::filesystem::path source_file = test_dir_ / "src" / "candidate_rdata.c";
  {
    std::ofstream source_output(source_file);
    source_output << "void TargetFunc(void) {}\n";
  }

  CandidateCompiler compiler(
      {.repo_root = test_dir_, .game_name = "test-game", .symbols = &symbols_});
  // When target_rodata_vram is 0x801222B0 and the first .rdata LO16 addend is 0x8,
  // CandidateCompiler rebates .rdata base to 0x801222A8 so the jump table resolves to 0x801222B0
  // -> %hi(0x801222B0) = 0x8012, %lo(0x801222B0) = 0x22B0
  auto compiled_or =
      compiler.CompileCandidateFunction(source_file, "TargetFunc", 0x80021000, {}, {}, 0x801222B0u);
  ASSERT_TRUE(compiled_or.ok()) << compiled_or.status();
  ASSERT_THAT(compiled_or->patched_words, SizeIs(4));
  EXPECT_EQ(compiled_or->patched_words[0], 0x3C018012u);
  EXPECT_EQ(compiled_or->patched_words[1], 0x8C2222B0u);
}

TEST_F(CandidateCompilerTest,
       CompileCandidateFunctionCompilesInTempDirectoryEvenIfSourceDirReadOnly) {
  std::filesystem::create_directories(test_dir_ / "bin");
  std::filesystem::create_directories(test_dir_ / "toolchain" / "gcc-2.7.2");
  std::filesystem::create_directories(test_dir_ / "toolchain" / "binutils-2.6");

  std::filesystem::path prebuilt_elf_path = test_dir_ / "prebuilt_candidate.o";
  {
    auto elf_bytes = BuildCandidateMipsElfWithJumps();
    std::ofstream elf_out(prebuilt_elf_path, std::ios::binary);
    elf_out.write(reinterpret_cast<const char*>(elf_bytes.data()), elf_bytes.size());
  }

  auto write_executable = [](const std::filesystem::path& path, const std::string& script) {
    std::ofstream out(path);
    out << script;
    out.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace);
  };

  const std::string noop_script = "#!/bin/sh\nexit 0\n";
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-as", noop_script);
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-ld", noop_script);
  write_executable(test_dir_ / "bin" / "mips-linux-gnu-objcopy", noop_script);
  write_executable(test_dir_ / "toolchain" / "gcc-2.7.2" / "gcc", noop_script);

  std::string kmc_as_script =
      "#!/bin/sh\n"
      "while [ $# -gt 0 ]; do\n"
      "  if [ \"$1\" = \"-o\" ]; then\n"
      "    cp \"" +
      prebuilt_elf_path.string() +
      "\" \"$2\"\n"
      "    exit 0\n"
      "  fi\n"
      "  shift\n"
      "done\n"
      "exit 1\n";
  write_executable(test_dir_ / "toolchain" / "binutils-2.6" / "as", kmc_as_script);

  std::filesystem::path readonly_src_dir = test_dir_ / "readonly_src";
  std::filesystem::create_directories(readonly_src_dir);
  std::filesystem::path source_file = readonly_src_dir / "candidate_readonly.c";
  {
    std::ofstream source_output(source_file);
    source_output << "void TargetFunc(void) {}\n";
  }

  // Make source directory read-only (0555)
  std::filesystem::permissions(
      readonly_src_dir,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
          std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
          std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
      std::filesystem::perm_options::replace);

  CandidateCompiler compiler(
      {.repo_root = test_dir_, .game_name = "test-game", .symbols = &symbols_});
  auto compiled_or = compiler.CompileCandidateFunction(source_file, "TargetFunc", 0x80021000);
  EXPECT_TRUE(compiled_or.ok()) << compiled_or.status();

  // Verify that no intermediate or object files were written into the source directory
  EXPECT_FALSE(std::filesystem::exists(readonly_src_dir / "candidate_readonly.o"));
  EXPECT_FALSE(std::filesystem::exists(readonly_src_dir / "diff_candidate_readonly_TargetFunc.o"));

  // Restore permissions for cleanup
  std::error_code ec;
  std::filesystem::permissions(readonly_src_dir, std::filesystem::perms::all, ec);
}

}  // namespace
}  // namespace rom_nom_nom
