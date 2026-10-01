#include "differ/elf_reader.h"

#include <elf.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/endian.h"

namespace rom_nom_nom {

namespace {

std::string_view ExtractString(absl::Span<const uint8_t> strtab, uint32_t offset) {
  if (offset >= strtab.size()) {
    return "";
  }
  const char* start = reinterpret_cast<const char*>(strtab.data() + offset);
  size_t max_len = strtab.size() - offset;
  size_t len = 0;
  while (len < max_len && start[len] != '\0') {
    len++;
  }
  return std::string_view(start, len);
}

}  // namespace

absl::StatusOr<ElfReader> ElfReader::LoadFromFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return absl::NotFoundError(
        absl::StrFormat("ElfReader: Could not open file '%s'", path.string()));
  }
  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(size);
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return absl::InternalError(
        absl::StrFormat("ElfReader: Failed to read %zu bytes from '%s'", size, path.string()));
  }
  return LoadFromBytes(buffer);
}

absl::StatusOr<ElfReader> ElfReader::LoadFromBytes(absl::Span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(Elf32_Ehdr)) {
    return absl::InvalidArgumentError(
        absl::StrFormat("ElfReader: Data too small for ELF header (%zu bytes)", bytes.size()));
  }

  // Validate ELF magic: \x7f E L F
  if (bytes[EI_MAG0] != ELFMAG0 || bytes[EI_MAG1] != ELFMAG1 || bytes[EI_MAG2] != ELFMAG2 ||
      bytes[EI_MAG3] != ELFMAG3) {
    return absl::InvalidArgumentError("ElfReader: Invalid ELF magic number.");
  }

  if (bytes[EI_CLASS] != ELFCLASS32) {
    return absl::InvalidArgumentError("ElfReader: Only ELFCLASS32 (32-bit) is supported.");
  }

  if (bytes[EI_DATA] != ELFDATA2MSB) {
    return absl::InvalidArgumentError("ElfReader: Only ELFDATA2MSB (big-endian) is supported.");
  }

  uint16_t e_type = ReadBigEndian16(&bytes[16]);
  uint16_t e_machine = ReadBigEndian16(&bytes[18]);
  uint32_t e_shoff = ReadBigEndian32(&bytes[32]);
  uint16_t e_shentsize = ReadBigEndian16(&bytes[46]);
  uint16_t e_shnum = ReadBigEndian16(&bytes[48]);
  uint16_t e_shstrndx = ReadBigEndian16(&bytes[50]);

  if (e_type != ET_REL) {
    return absl::InvalidArgumentError(
        absl::StrFormat("ElfReader: Expected ET_REL (1), got %d", e_type));
  }
  if (e_machine != EM_MIPS) {
    return absl::InvalidArgumentError(
        absl::StrFormat("ElfReader: Expected EM_MIPS (8), got %d", e_machine));
  }
  if (e_shentsize < sizeof(Elf32_Shdr)) {
    return absl::InvalidArgumentError(
        absl::StrFormat("ElfReader: Invalid section header size %d", e_shentsize));
  }

  uint64_t sh_table_end =
      static_cast<uint64_t>(e_shoff) + static_cast<uint64_t>(e_shnum) * e_shentsize;
  if (sh_table_end > bytes.size()) {
    return absl::InvalidArgumentError("ElfReader: Section header table out of bounds.");
  }

  ElfReader reader;
  reader.sections_.reserve(e_shnum);

  struct RawShdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
  };
  std::vector<RawShdr> raw_shdrs(e_shnum);

  for (uint16_t i = 0; i < e_shnum; ++i) {
    const uint8_t* sh_ptr = &bytes[e_shoff + i * e_shentsize];
    RawShdr raw;
    raw.sh_name = ReadBigEndian32(&sh_ptr[0]);
    raw.sh_type = ReadBigEndian32(&sh_ptr[4]);
    raw.sh_flags = ReadBigEndian32(&sh_ptr[8]);
    raw.sh_offset = ReadBigEndian32(&sh_ptr[16]);
    raw.sh_size = ReadBigEndian32(&sh_ptr[20]);
    raw.sh_link = ReadBigEndian32(&sh_ptr[24]);
    raw.sh_info = ReadBigEndian32(&sh_ptr[28]);
    raw_shdrs[i] = raw;

    ElfSection sec;
    sec.type = raw.sh_type;
    sec.flags = raw.sh_flags;
    sec.offset = raw.sh_offset;
    sec.size = raw.sh_size;
    if (raw.sh_type != SHT_NOBITS && raw.sh_size > 0) {
      if (raw.sh_offset + raw.sh_size <= bytes.size()) {
        sec.data.assign(&bytes[raw.sh_offset], &bytes[raw.sh_offset + raw.sh_size]);
      }
    }
    reader.sections_.push_back(std::move(sec));
  }

  // Populate section names from .shstrtab
  if (e_shstrndx < e_shnum) {
    absl::Span<const uint8_t> shstrtab = reader.sections_[e_shstrndx].data;
    for (uint16_t i = 0; i < e_shnum; ++i) {
      reader.sections_[i].name = std::string(ExtractString(shstrtab, raw_shdrs[i].sh_name));
      if (reader.sections_[i].name == ".text") {
        reader.text_section_index_ = i;
      }
    }
  }

  // Locate symbol table and parse symbols
  int symtab_idx = -1;
  for (uint16_t i = 0; i < e_shnum; ++i) {
    if (raw_shdrs[i].sh_type == SHT_SYMTAB) {
      symtab_idx = i;
      break;
    }
  }

  if (symtab_idx >= 0) {
    const RawShdr& sym_shdr = raw_shdrs[symtab_idx];
    uint32_t strtab_idx = sym_shdr.sh_link;
    absl::Span<const uint8_t> strtab =
        (strtab_idx < e_shnum) ? reader.sections_[strtab_idx].data : absl::Span<const uint8_t>();
    absl::Span<const uint8_t> sym_bytes = reader.sections_[symtab_idx].data;

    size_t num_symbols = sym_bytes.size() / sizeof(Elf32_Sym);
    reader.symbols_.reserve(num_symbols);

    for (size_t i = 0; i < num_symbols; ++i) {
      const uint8_t* sym_ptr = &sym_bytes[i * sizeof(Elf32_Sym)];
      uint32_t st_name = ReadBigEndian32(&sym_ptr[0]);
      uint32_t st_value = ReadBigEndian32(&sym_ptr[4]);
      uint32_t st_size = ReadBigEndian32(&sym_ptr[8]);
      uint8_t st_info = sym_ptr[12];
      uint16_t st_shndx = ReadBigEndian16(&sym_ptr[14]);

      ElfSymbol sym;
      sym.value = st_value;
      sym.size = st_size;
      sym.bind = ELF32_ST_BIND(st_info);
      sym.type = ELF32_ST_TYPE(st_info);
      sym.shndx = st_shndx;
      sym.is_defined = (st_shndx != SHN_UNDEF && st_shndx < SHN_LORESERVE);

      if (st_name != 0) {
        sym.name = std::string(ExtractString(strtab, st_name));
      } else if (sym.type == STT_SECTION && st_shndx < e_shnum) {
        sym.name = reader.sections_[st_shndx].name;
      }
      reader.symbols_.push_back(std::move(sym));
    }
  }

  // Locate relocation section for .text
  if (reader.text_section_index_ >= 0) {
    for (uint16_t i = 0; i < e_shnum; ++i) {
      if (raw_shdrs[i].sh_type == SHT_REL &&
          raw_shdrs[i].sh_info == static_cast<uint32_t>(reader.text_section_index_)) {
        absl::Span<const uint8_t> rel_bytes = reader.sections_[i].data;
        size_t num_relocs = rel_bytes.size() / sizeof(Elf32_Rel);
        reader.text_relocations_.reserve(num_relocs);

        for (size_t j = 0; j < num_relocs; ++j) {
          const uint8_t* rel_ptr = &rel_bytes[j * sizeof(Elf32_Rel)];
          uint32_t r_offset = ReadBigEndian32(&rel_ptr[0]);
          uint32_t r_info = ReadBigEndian32(&rel_ptr[4]);
          uint32_t sym_idx = ELF32_R_SYM(r_info);
          uint32_t r_type = ELF32_R_TYPE(r_info);

          ElfRelocation reloc;
          reloc.section_offset = r_offset;
          reloc.type = r_type;
          reloc.sym_index = sym_idx;
          if (sym_idx < reader.symbols_.size()) {
            reloc.symbol_name = reader.symbols_[sym_idx].name;
          }
          reader.text_relocations_.push_back(std::move(reloc));
        }
      }
    }
  }

  return reader;
}

const ElfSection* ElfReader::FindSection(std::string_view name) const {
  for (const auto& sec : sections_) {
    if (sec.name == name) {
      return &sec;
    }
  }
  return nullptr;
}

const ElfSymbol* ElfReader::FindSymbol(std::string_view name) const {
  for (const auto& sym : symbols_) {
    if (sym.name == name) {
      return &sym;
    }
  }
  return nullptr;
}

std::vector<ElfFunction> ElfReader::ExtractFunctions() const {
  std::vector<ElfFunction> functions;
  if (text_section_index_ < 0 || static_cast<size_t>(text_section_index_) >= sections_.size()) {
    return functions;
  }

  const ElfSection& text_sec = sections_[text_section_index_];
  const auto& text_data = text_sec.data;

  // Gather all function candidates in .text
  struct Candidate {
    const ElfSymbol* sym;
    uint32_t offset;
    uint32_t size;
  };
  std::vector<Candidate> candidates;

  for (const auto& sym : symbols_) {
    if (sym.is_defined && sym.shndx == text_section_index_ &&
        (sym.type == STT_FUNC || (sym.type == STT_NOTYPE && !sym.name.empty()))) {
      candidates.push_back({&sym, sym.value, sym.size});
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.offset < b.offset; });

  for (size_t i = 0; i < candidates.size(); ++i) {
    const auto& cand = candidates[i];
    uint32_t fn_offset = cand.offset;
    uint32_t fn_size = cand.size;

    // If size not recorded, determine from next symbol or end of .text
    if (fn_size == 0) {
      if (i + 1 < candidates.size()) {
        fn_size = candidates[i + 1].offset - fn_offset;
      } else {
        fn_size = text_sec.size - fn_offset;
      }
    }

    if (fn_offset + fn_size > text_data.size()) {
      fn_size = (fn_offset < text_data.size()) ? (text_data.size() - fn_offset) : 0;
    }

    ElfFunction fn;
    fn.name = cand.sym->name;
    fn.section_offset = fn_offset;
    fn.size = fn_size;

    // Read 32-bit big-endian words
    size_t num_words = fn_size / 4;
    fn.raw_words.reserve(num_words);
    for (size_t w = 0; w < num_words; ++w) {
      fn.raw_words.push_back(ReadBigEndian32(&text_data[fn_offset + w * 4]));
    }

    // Collect relocations within this function range
    for (const auto& rel : text_relocations_) {
      if (rel.section_offset >= fn_offset && rel.section_offset < fn_offset + fn_size) {
        ElfRelocation fn_rel = rel;
        fn_rel.function_offset = rel.section_offset - fn_offset;
        fn.relocations.push_back(std::move(fn_rel));
      }
    }

    functions.push_back(std::move(fn));
  }

  return functions;
}

absl::StatusOr<ElfFunction> ElfReader::ExtractFunction(std::string_view func_name) const {
  std::vector<ElfFunction> fns = ExtractFunctions();
  for (auto& fn : fns) {
    if (fn.name == func_name) {
      return fn;
    }
  }
  return absl::NotFoundError(
      absl::StrFormat("ElfReader: Function '%s' not found in .text section.", func_name));
}

}  // namespace rom_nom_nom
