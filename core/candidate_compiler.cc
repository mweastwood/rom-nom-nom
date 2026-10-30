#include "core/candidate_compiler.h"

#include <elf.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
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
#include "core/compiler.h"
#include "core/elf_reader.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/relocation_applier.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

std::optional<uint32_t> InferTargetRodataVram(absl::Span<const Instruction> instructions) {
  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& jr_instruction = instructions[i];
    if (jr_instruction.opcode != Opcode::kJr || !jr_instruction.rs.has_value() ||
        *jr_instruction.rs == Register::kRa) {
      continue;
    }
    const Register target_register = *jr_instruction.rs;
    size_t search_start = (i > 12) ? (i - 12) : 0;
    for (size_t j = i; j > search_start; --j) {
      const auto& lw_instruction = instructions[j - 1];
      if (lw_instruction.opcode != Opcode::kLw || lw_instruction.rt != target_register ||
          !lw_instruction.rs.has_value()) {
        continue;
      }
      const Register base_register = *lw_instruction.rs;
      const int16_t low_immediate = lw_instruction.immediate;
      size_t lui_search_start = (j - 1 > 12) ? (j - 1 - 12) : 0;
      for (size_t k = j - 1; k > lui_search_start; --k) {
        const auto& lui_instruction = instructions[k - 1];
        if (lui_instruction.opcode == Opcode::kLui && lui_instruction.rt.has_value() &&
            (*lui_instruction.rt == base_register || *lui_instruction.rt == Register::kAt)) {
          uint32_t high_bits =
              static_cast<uint32_t>(static_cast<uint16_t>(lui_instruction.immediate)) << 16;
          uint32_t address = high_bits + static_cast<uint32_t>(static_cast<int32_t>(low_immediate));
          if ((address >> 28) == 0x8) {
            return address;
          }
        }
      }
    }
  }
  return std::nullopt;
}

CandidateCompiler::CandidateCompiler(CandidateCompileOptions options)
    : options_(std::move(options)) {
  options_.repo_root = FindRepoRoot(options_.repo_root);
}

absl::StatusOr<CompiledCandidateFunction> CandidateCompiler::CompileFunction(
    const std::filesystem::path& source_file, std::string_view function_name, uint32_t target_vram,
    const std::vector<std::string>& c_flags_override,
    const std::vector<std::filesystem::path>& extra_include_dirs,
    std::optional<uint32_t> target_rodata_vram) const {
  std::filesystem::path resolved_source = source_file;
  if (resolved_source.is_relative()) {
    resolved_source = options_.repo_root / resolved_source;
  }
  std::error_code error_code;
  if (!std::filesystem::exists(resolved_source, error_code)) {
    return absl::NotFoundError(
        absl::StrFormat("Specified source file does not exist: %s", source_file.string()));
  }

  auto toolchain_or = Toolchain::Discover({
      .repo_root = options_.repo_root,
      .mode = ToolchainMode::kOriginal,
  });
  if (!toolchain_or.ok()) {
    return toolchain_or.status();
  }
  Compiler compiler(*toolchain_or);

  std::vector<std::string> flags = c_flags_override;
  if (flags.empty() && options_.config != nullptr) {
    flags = ResolveCFlags(*options_.config, resolved_source.stem().string());
  }
  if (flags.empty()) {
    flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  }

  std::filesystem::path assembly_dir =
      options_.repo_root / "bazel-bin" / "asm" / options_.game_name;
  if (!std::filesystem::exists(assembly_dir, error_code)) {
    assembly_dir = options_.repo_root / "asm" / options_.game_name;
  }

  std::vector<std::filesystem::path> includes;
  for (const auto& include_dir : extra_include_dirs) {
    includes.push_back(include_dir.is_relative() ? (options_.repo_root / include_dir)
                                                 : include_dir);
  }
  if (options_.config != nullptr && options_.config->include_dirs_size() > 0) {
    for (const auto& dir : options_.config->include_dirs()) {
      std::filesystem::path include_path(dir);
      includes.push_back(include_path.is_relative() ? (options_.repo_root / include_path)
                                                    : include_path);
    }
  } else {
    includes.push_back(options_.repo_root);
    includes.push_back(options_.repo_root / "src" / options_.game_name);
    includes.push_back(options_.repo_root / "src" / "c" / options_.game_name);
    includes.push_back(options_.repo_root / "src" / "cc" / options_.game_name);
  }
  includes.push_back(assembly_dir);
  includes.push_back(options_.repo_root / "bazel-bin");
  if (assembly_dir.has_parent_path()) {
    includes.push_back(assembly_dir.parent_path());
  }

  std::filesystem::path macro_inc;
  if (options_.config != nullptr && !options_.config->macro_inc().empty()) {
    std::filesystem::path macro_path(options_.config->macro_inc());
    macro_inc = macro_path.is_relative() ? (options_.repo_root / macro_path) : macro_path;
  } else {
    macro_inc = FindMacroInc(options_.repo_root, options_.game_name);
  }

  std::error_code temp_ec;
  std::filesystem::path temporary_dir = std::filesystem::temp_directory_path(temp_ec);
  if (temp_ec || temporary_dir.empty()) {
    temporary_dir = "/tmp";
  }
  static std::atomic<uint64_t> temp_counter{0};
  uint64_t unique_id = temp_counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temporary_object =
      temporary_dir / absl::StrCat("diff_", resolved_source.stem().string(), "_", function_name,
                                   "_", static_cast<uint64_t>(getpid()), "_", unique_id, ".o");

  auto compile_status = compiler.CompileC({
      .src_file = resolved_source,
      .out_obj = temporary_object,
      .opt_flags = flags,
      .include_dirs = includes,
      .macro_inc = macro_inc,
      .timeout = absl::Seconds(5),
  });
  if (!compile_status.ok()) {
    return absl::InternalError(absl::StrCat("Compilation failed for ",
                                            resolved_source.filename().string(), ":\n",
                                            compile_status.message()));
  }

  auto reader_or = ElfReader::LoadFromFile(temporary_object);
  std::filesystem::remove(temporary_object, error_code);
  if (!reader_or.ok()) {
    return reader_or.status();
  }

  auto function_or = reader_or->ExtractFunction(function_name);
  if (!function_or.ok()) {
    return absl::NotFoundError(
        absl::StrFormat("Function '%s' not found in compiled object file %s: %s", function_name,
                        resolved_source.filename().string(), function_or.status().message()));
  }
  const ElfFunction& matched_elf_function = *function_or;

  CompiledCandidateFunction out;
  out.section_offset = matched_elf_function.section_offset;
  out.size = matched_elf_function.size;

  uint32_t text_base_vram = target_vram - matched_elf_function.section_offset;
  std::optional<uint32_t> rodata_base_vram;
  if (target_rodata_vram.has_value()) {
    int16_t first_rodata_low_addend = 0;
    for (const auto& relocation : matched_elf_function.relocations) {
      if (relocation.type == R_MIPS_LO16 &&
          (relocation.symbol_name == ".rdata" || relocation.symbol_name == ".rodata")) {
        uint32_t low_word_index = relocation.function_offset / 4;
        if (low_word_index < matched_elf_function.raw_words.size()) {
          first_rodata_low_addend =
              static_cast<int16_t>(matched_elf_function.raw_words[low_word_index] & 0xFFFF);
        }
        break;
      }
    }
    rodata_base_vram =
        *target_rodata_vram - static_cast<uint32_t>(static_cast<int32_t>(first_rodata_low_addend));
  }

  auto symbol_lookup = [&](std::string_view name) -> std::optional<uint32_t> {
    if (name == ".text") {
      return text_base_vram;
    }
    if ((name == ".rdata" || name == ".rodata") && rodata_base_vram.has_value()) {
      return *rodata_base_vram;
    }
    return ResolveSymbolOrSynthesizedAddress(name, options_.symbols);
  };
  RelocationApplyResult applied = RelocationApplier::Apply(matched_elf_function, symbol_lookup);
  out.patched_words = std::move(applied.patched_words);
  out.instructions.reserve(out.patched_words.size());
  for (size_t i = 0; i < out.patched_words.size(); ++i) {
    auto instruction_or =
        DecodeInstruction(out.patched_words[i], target_vram + static_cast<uint32_t>(i * 4));
    if (instruction_or.ok()) {
      out.instructions.push_back(*instruction_or);
    }
  }
  return out;
}

}  // namespace rom_nom_nom
