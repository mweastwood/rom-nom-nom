#include "splitter/assembly_generator.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"

namespace rom_nom_nom {
namespace {

inline uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

constexpr std::string_view kStandardMacroInc = R"(# Defines the expected assembly macros
.ifndef _MACRO_INC_GUARD
.set _MACRO_INC_GUARD, 1

.macro glabel label, visibility=global
    .\visibility \label
    .type \label, @function
    \label:
        .ent \label
.endm

.macro endlabel label
    .size \label, . - \label
    .end \label
.endm

.macro alabel label, visibility=global
    .\visibility \label
    .type \label, @function
    \label:
        .aent \label
.endm

.macro ehlabel label, visibility=global
    .\visibility \label
    \label:
.endm

.macro jlabel label, visibility=global
    .\visibility \label
    \label:
.endm

.macro dlabel label, visibility=global
    .\visibility \label
    .type \label, @object
    \label:
.endm

.macro enddlabel label
    .size \label, . - \label
.endm

.macro nonmatching label, size=1
    .global \label\().NON_MATCHING
    .type \label\().NON_MATCHING, @object
    .size \label\().NON_MATCHING, \size
    \label\().NON_MATCHING:
.endm

# COP0 register aliases
.set Index,         $0
.set Random,        $1
.set EntryLo0,      $2
.set EntryLo1,      $3
.set Context,       $4
.set PageMask,      $5
.set Wired,         $6
.set Reserved07,    $7
.set BadVaddr,      $8
.set Count,         $9
.set EntryHi,       $10
.set Compare,       $11
.set Status,        $12
.set Cause,         $13
.set EPC,           $14
.set PRevID,        $15
.set Config,        $16
.set LLAddr,        $17
.set WatchLo,       $18
.set WatchHi,       $19
.set XContext,      $20
.set Reserved21,    $21
.set Reserved22,    $22
.set Reserved23,    $23
.set Reserved24,    $24
.set Reserved25,    $25
.set PErr,          $26
.set CacheErr,      $27
.set TagLo,         $28
.set TagHi,         $29
.set ErrorEPC,      $30
.set Reserved31,    $31

# Float register aliases
.set $fv0,          $f0
.set $fv0f,         $f1
.set $fv1,          $f2
.set $fv1f,         $f3
.set $ft0,          $f4
.set $ft0f,         $f5
.set $ft1,          $f6
.set $ft1f,         $f7
.set $ft2,          $f8
.set $ft2f,         $f9
.set $ft3,          $f10
.set $ft3f,         $f11
.set $fa0,          $f12
.set $fa0f,         $f13
.set $fa1,          $f14
.set $fa1f,         $f15
.set $ft4,          $f16
.set $ft4f,         $f17
.set $ft5,          $f18
.set $ft5f,         $f19
.set $fs0,          $f20
.set $fs0f,         $f21
.set $fs1,          $f22
.set $fs1f,         $f23
.set $fs2,          $f24
.set $fs2f,         $f25
.set $fs3,          $f26
.set $fs3f,         $f27
.set $fs4,          $f28
.set $fs4f,         $f29
.set $fs5,          $f30
.set $fs5f,         $f31

.endif
)";

}  // namespace

AssemblyGenerator::AssemblyGenerator(AssemblyGeneratorOptions options)
    : options_(std::move(options)) {}

absl::Status AssemblyGenerator::EmitMacroIncludes(const std::filesystem::path& splat_macro_src) {
  std::filesystem::create_directories(options_.asm_dir);

  // Emit standard macro.inc if not present
  std::filesystem::path macro_inc_path = options_.asm_dir / "macro.inc";
  if (!std::filesystem::exists(macro_inc_path)) {
    std::ofstream macro_f(macro_inc_path, std::ios::out | std::ios::trunc);
    if (!macro_f.is_open()) {
      return absl::InternalError(absl::StrFormat(
          "AssemblyGenerator: Failed to open '%s' for writing", macro_inc_path.string()));
    }
    macro_f << kStandardMacroInc;
  }

  // Copy game-specific c_macro.inc if provided
  if (!splat_macro_src.empty() && std::filesystem::exists(splat_macro_src)) {
    std::filesystem::path c_macro_dest = options_.asm_dir / "c_macro.inc";
    std::error_code ec;
    std::filesystem::copy_file(splat_macro_src, c_macro_dest,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      return absl::InternalError(
          absl::StrFormat("AssemblyGenerator: Failed to copy c_macro.inc to '%s': %s",
                          c_macro_dest.string(), ec.message()));
    }
  }

  return absl::OkStatus();
}

absl::Status AssemblyGenerator::WriteHeaderAssembly(absl::Span<const uint8_t> header_bytes,
                                                    const std::filesystem::path& out_path) {
  std::string header_asm = ".section .data\n\n";
  for (size_t offset = 0; offset + 4 <= header_bytes.size(); offset += 4) {
    uint32_t w = ReadBe32(header_bytes.data() + offset);
    absl::StrAppend(&header_asm, absl::StrFormat(".word 0x%08X\n", w));
  }

  std::filesystem::create_directories(out_path.parent_path());
  std::ofstream f(out_path, std::ios::out | std::ios::trunc);
  if (!f.is_open()) {
    return absl::InternalError(
        absl::StrFormat("AssemblyGenerator: Failed to open '%s' for writing", out_path.string()));
  }
  f << header_asm;
  return absl::OkStatus();
}

std::string AssemblyGenerator::ResolveDataLabelName(uint32_t vram) const {
  if (options_.symbols == nullptr || options_.symbols->Empty()) {
    return absl::StrFormat("D_%08X", vram);
  }
  return options_.symbols->LookupOrSynthesizeName(vram, SYMBOL_DATA);
}

std::filesystem::path AssemblyGenerator::ResolveDataPath(std::string_view sub_name) const {
  if (sub_name.find('/') != std::string_view::npos ||
      sub_name.find(".data") != std::string_view::npos) {
    return options_.asm_dir / absl::StrCat(sub_name, ".s");
  }
  return options_.asm_dir / "data" / absl::StrCat(sub_name, ".data.s");
}

std::filesystem::path AssemblyGenerator::ResolveBssPath(std::string_view sub_name) const {
  if (sub_name.find('/') != std::string_view::npos ||
      sub_name.find(".bss") != std::string_view::npos) {
    return options_.asm_dir / absl::StrCat(sub_name, ".s");
  }
  return options_.asm_dir / "data" / absl::StrCat(sub_name, ".bss.s");
}

absl::Status AssemblyGenerator::WriteDataAssembly(absl::Span<const uint8_t> data_bytes,
                                                  uint32_t vram,
                                                  const std::filesystem::path& out_path) {
  std::string label_name = ResolveDataLabelName(vram);
  std::string data_asm;
  data_asm += ".include \"macro.inc\"\n\n.section .data, \"wa\"\n\n";
  data_asm += absl::StrFormat("dlabel %s\n", label_name);

  for (size_t offset = 0; offset + 4 <= data_bytes.size(); offset += 4) {
    uint32_t w = ReadBe32(data_bytes.data() + offset);
    absl::StrAppend(&data_asm, absl::StrFormat("    .word 0x%08X\n", w));
  }
  for (size_t offset = (data_bytes.size() / 4) * 4; offset < data_bytes.size(); ++offset) {
    absl::StrAppend(&data_asm, absl::StrFormat("    .byte 0x%02X\n", data_bytes[offset]));
  }
  data_asm += absl::StrFormat("enddlabel %s\n", label_name);

  std::filesystem::create_directories(out_path.parent_path());
  std::ofstream f(out_path, std::ios::out | std::ios::trunc);
  if (!f.is_open()) {
    return absl::InternalError(
        absl::StrFormat("AssemblyGenerator: Failed to open '%s' for writing", out_path.string()));
  }
  f << data_asm;
  return absl::OkStatus();
}

absl::Status AssemblyGenerator::WriteBssAssembly(size_t bss_size, uint32_t vram,
                                                 const std::filesystem::path& out_path) {
  std::string label_name = ResolveDataLabelName(vram);
  std::string bss_asm;
  bss_asm += ".include \"macro.inc\"\n\n.section .bss, \"wa\"\n\n";
  bss_asm += absl::StrFormat("dlabel %s\n", label_name);
  bss_asm += absl::StrFormat("    .space 0x%X\n", bss_size);
  bss_asm += absl::StrFormat("enddlabel %s\n", label_name);

  std::filesystem::create_directories(out_path.parent_path());
  std::ofstream f(out_path, std::ios::out | std::ios::trunc);
  if (!f.is_open()) {
    return absl::InternalError(
        absl::StrFormat("AssemblyGenerator: Failed to open '%s' for writing", out_path.string()));
  }
  f << bss_asm;
  return absl::OkStatus();
}

absl::StatusOr<size_t> AssemblyGenerator::WriteNonmatchingFunctions(
    std::string_view sub_name, const std::vector<DisassembledFunction>& functions) {
  std::filesystem::path nonmatchings_dir = options_.asm_dir / "nonmatchings" / sub_name;
  std::filesystem::create_directories(nonmatchings_dir);

  size_t count = 0;
  for (const auto& func : functions) {
    std::filesystem::path func_s_path = nonmatchings_dir / absl::StrCat(func.name, ".s");
    std::ofstream f(func_s_path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) {
      return absl::InternalError(absl::StrFormat(
          "AssemblyGenerator: Failed to open '%s' for writing", func_s_path.string()));
    }
    f << func.emitted_assembly << "\n";
    count++;
  }
  return count;
}

absl::Status AssemblyGenerator::WriteStandaloneAssembly(
    std::string_view sub_name, uint32_t rom_start, uint32_t rom_end, uint32_t vram,
    const std::vector<DisassembledFunction>& functions, const std::filesystem::path& out_path) {
  std::string emitted_s;
  emitted_s += absl::StrFormat(
      ".include \"macro.inc\"\n\n# Subsegment: %s [0x%X - 0x%X] VRAM: 0x%08X\n.set "
      "noat\n.set noreorder\n.set gp=64\n\n.section .text\n\n",
      sub_name, rom_start, rom_end, vram);
  for (const auto& func : functions) {
    emitted_s += func.emitted_assembly;
    emitted_s += "\n\n";
  }

  std::filesystem::create_directories(out_path.parent_path());
  std::ofstream s_file(out_path, std::ios::out | std::ios::trunc);
  if (!s_file.is_open()) {
    return absl::InternalError(
        absl::StrFormat("AssemblyGenerator: Failed to open '%s' for writing", out_path.string()));
  }
  s_file << emitted_s;
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
