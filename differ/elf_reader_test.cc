#include "differ/elf_reader.h"

#include <elf.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/endian.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::SizeIs;

std::vector<uint8_t> BuildSyntheticMipsElf() {
  std::vector<uint8_t> elf;

  // 1. ELF Header (52 bytes)
  elf.push_back(0x7F);
  elf.push_back('E');
  elf.push_back('L');
  elf.push_back('F');
  elf.push_back(ELFCLASS32);
  elf.push_back(ELFDATA2MSB);
  elf.push_back(EV_CURRENT);
  elf.push_back(0);  // ABI
  for (int i = 0; i < 8; ++i) elf.push_back(0);

  AppendBigEndian16(elf, ET_REL);   // e_type
  AppendBigEndian16(elf, EM_MIPS);  // e_machine
  AppendBigEndian32(elf, 1);        // e_version
  AppendBigEndian32(elf, 0);        // e_entry
  AppendBigEndian32(elf, 0);        // e_phoff
  // e_shoff will be patched later
  size_t shoff_pos = elf.size();
  AppendBigEndian32(elf, 0);   // placeholder for e_shoff
  AppendBigEndian32(elf, 0);   // e_flags
  AppendBigEndian16(elf, 52);  // e_ehsize
  AppendBigEndian16(elf, 0);   // e_phentsize
  AppendBigEndian16(elf, 0);   // e_phnum
  AppendBigEndian16(elf, 40);  // e_shentsize
  AppendBigEndian16(elf, 6);   // e_shnum (NULL, .text, .shstrtab, .symtab, .strtab, .rel.text)
  AppendBigEndian16(elf, 2);   // e_shstrndx (index 2 is .shstrtab)

  // 2. .text section content (offset 0x40 = 64)
  while (elf.size() < 64) elf.push_back(0);
  uint32_t text_offset = static_cast<uint32_t>(elf.size());
  // Function "TestFunc": 2 instructions (addiu $a0, $a0, 1; jr $ra)
  AppendBigEndian32(elf, 0x24840001);  // addiu $a0, $a0, 1
  AppendBigEndian32(elf, 0x03E00008);  // jr $ra
  uint32_t text_size = 8;

  // 3. .shstrtab section content
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

  // 4. .strtab section content
  uint32_t strtab_offset = static_cast<uint32_t>(elf.size());
  std::string strtab_data;
  strtab_data.push_back('\0');
  size_t str_func_name = strtab_data.size();
  strtab_data.append("TestFunc\0", 9);
  size_t str_target_sym = strtab_data.size();
  strtab_data.append("g_target_symbol\0", 16);
  for (char c : strtab_data) elf.push_back(static_cast<uint8_t>(c));
  uint32_t strtab_size = static_cast<uint32_t>(strtab_data.size());

  // 5. .symtab section content (each symbol is 16 bytes)
  uint32_t symtab_offset = static_cast<uint32_t>(elf.size());
  // Symbol 0: NULL symbol
  for (int i = 0; i < 16; ++i) elf.push_back(0);
  // Symbol 1: TestFunc (STT_FUNC, STB_GLOBAL, section 1 (.text), value 0, size 8)
  AppendBigEndian32(elf, static_cast<uint32_t>(str_func_name));
  AppendBigEndian32(elf, 0);  // st_value
  AppendBigEndian32(elf, 8);  // st_size
  elf.push_back((STB_GLOBAL << 4) | (STT_FUNC & 0xF));
  elf.push_back(0);
  AppendBigEndian16(elf, 1);  // st_shndx = 1 (.text)

  // Symbol 2: g_target_symbol (STT_NOTYPE, STB_GLOBAL, SHN_UNDEF)
  AppendBigEndian32(elf, static_cast<uint32_t>(str_target_sym));
  AppendBigEndian32(elf, 0);  // st_value
  AppendBigEndian32(elf, 0);  // st_size
  elf.push_back((STB_GLOBAL << 4) | (STT_NOTYPE & 0xF));
  elf.push_back(0);
  AppendBigEndian16(elf, SHN_UNDEF);  // undefined
  uint32_t symtab_size = 3 * 16;

  // 6. .rel.text section content (each relocation is 8 bytes)
  uint32_t rel_offset = static_cast<uint32_t>(elf.size());
  // Relocation 0: offset 0x0, type R_MIPS_HI16 (5), sym 2 (g_target_symbol)
  AppendBigEndian32(elf, 0x0);  // r_offset
  AppendBigEndian32(elf, (2 << 8) | 5);
  uint32_t rel_size = 8;

  // 7. Section Header Table (6 sections * 40 bytes)
  uint32_t sh_table_offset = static_cast<uint32_t>(elf.size());
  // Patch e_shoff in header
  elf[shoff_pos] = static_cast<uint8_t>((sh_table_offset >> 24) & 0xFF);
  elf[shoff_pos + 1] = static_cast<uint8_t>((sh_table_offset >> 16) & 0xFF);
  elf[shoff_pos + 2] = static_cast<uint8_t>((sh_table_offset >> 8) & 0xFF);
  elf[shoff_pos + 3] = static_cast<uint8_t>(sh_table_offset & 0xFF);

  auto append_shdr = [&](uint32_t name, uint32_t type, uint32_t flags, uint32_t offset,
                         uint32_t size, uint32_t link, uint32_t info, uint32_t entsize) {
    AppendBigEndian32(elf, name);
    AppendBigEndian32(elf, type);
    AppendBigEndian32(elf, flags);
    AppendBigEndian32(elf, 0);  // sh_addr
    AppendBigEndian32(elf, offset);
    AppendBigEndian32(elf, size);
    AppendBigEndian32(elf, link);
    AppendBigEndian32(elf, info);
    AppendBigEndian32(elf, 4);  // sh_addralign
    AppendBigEndian32(elf, entsize);
  };

  // Section 0: NULL
  append_shdr(0, SHT_NULL, 0, 0, 0, 0, 0, 0);
  // Section 1: .text
  append_shdr(static_cast<uint32_t>(sh_text_name), SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
              text_offset, text_size, 0, 0, 0);
  // Section 2: .shstrtab
  append_shdr(static_cast<uint32_t>(sh_shstrtab_name), SHT_STRTAB, 0, shstrtab_offset,
              shstrtab_size, 0, 0, 0);
  // Section 3: .symtab
  append_shdr(static_cast<uint32_t>(sh_symtab_name), SHT_SYMTAB, 0, symtab_offset, symtab_size, 4,
              2, 16);  // link to .strtab (index 4)
  // Section 4: .strtab
  append_shdr(static_cast<uint32_t>(sh_strtab_name), SHT_STRTAB, 0, strtab_offset, strtab_size, 0,
              0, 0);
  // Section 5: .rel.text
  append_shdr(static_cast<uint32_t>(sh_reltext_name), SHT_REL, 0, rel_offset, rel_size, 3, 1,
              8);  // link to .symtab (3), info .text (1)

  return elf;
}

TEST(ElfReaderTest, RejectsInvalidMagicOrTooSmall) {
  std::vector<uint8_t> too_small = {0x7F, 'E', 'L'};
  auto reader_or = ElfReader::LoadFromBytes(too_small);
  EXPECT_FALSE(reader_or.ok());

  std::vector<uint8_t> bad_magic(64, 0);
  reader_or = ElfReader::LoadFromBytes(bad_magic);
  EXPECT_FALSE(reader_or.ok());
}

TEST(ElfReaderTest, RejectsNonMipsOrNonRelocatable) {
  auto elf = BuildSyntheticMipsElf();
  // Patch e_type to ET_EXEC (2)
  elf[16] = 0;
  elf[17] = ET_EXEC;
  EXPECT_FALSE(ElfReader::LoadFromBytes(elf).ok());

  elf = BuildSyntheticMipsElf();
  // Patch e_machine to EM_386 (3)
  elf[18] = 0;
  elf[19] = EM_386;
  EXPECT_FALSE(ElfReader::LoadFromBytes(elf).ok());
}

TEST(ElfReaderTest, ParsesSectionsAndSymbolsCorrectly) {
  auto elf = BuildSyntheticMipsElf();
  auto reader_or = ElfReader::LoadFromBytes(elf);
  ASSERT_TRUE(reader_or.ok()) << reader_or.status();

  const auto& reader = *reader_or;
  EXPECT_THAT(reader.Sections(), SizeIs(6));

  const ElfSection* text_sec = reader.FindSection(".text");
  ASSERT_NE(text_sec, nullptr);
  EXPECT_EQ(text_sec->size, 8u);
  EXPECT_THAT(text_sec->data, ElementsAre(0x24, 0x84, 0x00, 0x01, 0x03, 0xE0, 0x00, 0x08));

  const ElfSymbol* func_sym = reader.FindSymbol("TestFunc");
  ASSERT_NE(func_sym, nullptr);
  EXPECT_EQ(func_sym->value, 0u);
  EXPECT_EQ(func_sym->size, 8u);
  EXPECT_EQ(func_sym->type, static_cast<uint8_t>(STT_FUNC));
  EXPECT_TRUE(func_sym->is_defined);

  const ElfSymbol* target_sym = reader.FindSymbol("g_target_symbol");
  ASSERT_NE(target_sym, nullptr);
  EXPECT_FALSE(target_sym->is_defined);
}

TEST(ElfReaderTest, ParsesRelocationsAndExtractsFunctions) {
  auto elf = BuildSyntheticMipsElf();
  auto reader_or = ElfReader::LoadFromBytes(elf);
  ASSERT_TRUE(reader_or.ok()) << reader_or.status();

  const auto& reader = *reader_or;
  EXPECT_THAT(reader.TextRelocations(),
              ElementsAre(AllOf(Field(&ElfRelocation::section_offset, 0u),
                                Field(&ElfRelocation::type, 5u),  // R_MIPS_HI16
                                Field(&ElfRelocation::symbol_name, "g_target_symbol"))));

  auto func_or = reader.ExtractFunction("TestFunc");
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  const auto& func = *func_or;
  EXPECT_EQ(func.name, "TestFunc");
  EXPECT_EQ(func.section_offset, 0u);
  EXPECT_EQ(func.size, 8u);
  EXPECT_THAT(func.raw_words, ElementsAre(0x24840001, 0x03E00008));
  EXPECT_THAT(func.relocations,
              ElementsAre(AllOf(Field(&ElfRelocation::function_offset, 0u),
                                Field(&ElfRelocation::symbol_name, "g_target_symbol"))));

  // Non-existent function returns NotFound
  EXPECT_FALSE(reader.ExtractFunction("NonExistent").ok());
}

}  // namespace
}  // namespace rom_nom_nom
