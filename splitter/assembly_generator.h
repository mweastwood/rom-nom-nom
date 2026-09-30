#ifndef ROM_NOM_NOM_SPLITTER_ASSEMBLY_GENERATOR_H_
#define ROM_NOM_NOM_SPLITTER_ASSEMBLY_GENERATOR_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "splitter/disassembler.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

// Configuration options for assembly generation and file output.
struct AssemblyGeneratorOptions {
  std::filesystem::path asm_dir = "asm";
  const SymbolIndex* symbols = nullptr;
};

// Generates MIPS assembly source files (.s), data sections (.data.s),
// BSS sections (.bss.s), header assembly (header.s), and standard macro includes (macro.inc).
class AssemblyGenerator {
 public:
  explicit AssemblyGenerator(AssemblyGeneratorOptions options);

  // Writes standard N64 macro.inc and copies game c_macro.inc if available.
  absl::Status EmitMacroIncludes(const std::filesystem::path& splat_macro_src = {});

  // Formats and writes header.s for SEGMENT_HEADER from the 64-byte ROM header slice.
  absl::Status WriteHeaderAssembly(absl::Span<const uint8_t> header_bytes,
                                   const std::filesystem::path& out_path);

  // Formats and writes .data.s containing .word and .byte definitions under a dlabel.
  absl::Status WriteDataAssembly(absl::Span<const uint8_t> data_bytes, uint32_t vram,
                                 const std::filesystem::path& out_path);

  // Formats and writes .bss.s containing .space definitions under a dlabel.
  absl::Status WriteBssAssembly(size_t bss_size, uint32_t vram,
                                const std::filesystem::path& out_path);

  // Writes disassembled C subsegment functions to nonmatchings/<sub_name>/<func_name>.s.
  // Returns the count of function files written.
  absl::StatusOr<size_t> WriteNonmatchingFunctions(
      std::string_view sub_name, const std::vector<DisassembledFunction>& functions);

  // Writes standalone assembly (.set noat, .set noreorder, .set gp=64) for SUBSEGMENT_ASM.
  absl::Status WriteStandaloneAssembly(std::string_view sub_name, uint32_t rom_start,
                                       uint32_t rom_end, uint32_t vram,
                                       const std::vector<DisassembledFunction>& functions,
                                       const std::filesystem::path& out_path);

  // Returns the resolved output path for a subsegment of type SUBSEGMENT_DATA.
  std::filesystem::path ResolveDataPath(std::string_view sub_name) const;

  // Returns the resolved output path for a subsegment of type SUBSEGMENT_BSS.
  std::filesystem::path ResolveBssPath(std::string_view sub_name) const;

  // Resolves the destination label name for a given VRAM address.
  std::string ResolveDataLabelName(uint32_t vram) const;

 private:
  AssemblyGeneratorOptions options_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_SPLITTER_ASSEMBLY_GENERATOR_H_
