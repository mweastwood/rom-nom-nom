#ifndef DIFFER_ELF_READER_H_
#define DIFFER_ELF_READER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

// Represents an ELF symbol extracted from .symtab.
struct ElfSymbol {
  std::string name;
  uint32_t value = 0;  // Symbol offset in section or absolute address
  uint32_t size = 0;   // Symbol size in bytes
  uint8_t bind = 0;    // STB_LOCAL, STB_GLOBAL, etc.
  uint8_t type = 0;    // STT_NOTYPE, STT_OBJECT, STT_FUNC, etc.
  uint16_t shndx = 0;  // Section index (SHN_UNDEF if undefined)
  bool is_defined = false;
};

// Represents an ELF relocation entry extracted from .rel.text.
struct ElfRelocation {
  uint32_t section_offset = 0;   // Byte offset within .text
  uint32_t function_offset = 0;  // Byte offset relative to function start
  uint32_t type = 0;             // R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16, etc.
  uint32_t sym_index = 0;        // Symbol table index
  std::string symbol_name;       // Name of referenced symbol
};

// Represents a function extracted from the .text section.
struct ElfFunction {
  std::string name;
  uint32_t section_offset = 0;             // Byte offset in .text
  uint32_t size = 0;                       // Size in bytes
  std::vector<uint32_t> raw_words;         // Big-endian 32-bit instruction words
  std::vector<ElfRelocation> relocations;  // Relocations within this function
};

// Represents an ELF section.
struct ElfSection {
  std::string name;
  uint32_t type = 0;
  uint32_t flags = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  std::vector<uint8_t> data;
};

// Lightweight parser for MIPS ELF32 relocatable object files (.o).
class ElfReader {
 public:
  // Loads and parses an ELF32 object from raw bytes.
  static absl::StatusOr<ElfReader> LoadFromBytes(absl::Span<const uint8_t> bytes);

  // Loads and parses an ELF32 object from a file on disk.
  static absl::StatusOr<ElfReader> LoadFromFile(const std::filesystem::path& path);

  const std::vector<ElfSection>& Sections() const { return sections_; }
  const std::vector<ElfSymbol>& Symbols() const { return symbols_; }
  const std::vector<ElfRelocation>& TextRelocations() const { return text_relocations_; }

  // Finds a section by name (e.g. ".text", ".data").
  const ElfSection* FindSection(std::string_view name) const;

  // Finds a symbol by name in .symtab.
  const ElfSymbol* FindSymbol(std::string_view name) const;

  // Extracts all functions defined in .text.
  std::vector<ElfFunction> ExtractFunctions() const;

  // Extracts a specific function by name from .text.
  absl::StatusOr<ElfFunction> ExtractFunction(std::string_view func_name) const;

 private:
  ElfReader() = default;

  std::vector<ElfSection> sections_;
  std::vector<ElfSymbol> symbols_;
  std::vector<ElfRelocation> text_relocations_;
  int text_section_index_ = -1;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_ELF_READER_H_
