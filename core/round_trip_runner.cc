#include "core/round_trip_runner.h"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/compiler.h"
#include "core/disassembler.h"
#include "core/elf_reader.h"
#include "core/register_tracker.h"
#include "core/relocation_applier.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"

namespace rom_nom_nom {
namespace {

void WriteTextFile(const std::filesystem::path& path, std::string_view content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  ofs.write(content.data(), content.size());
}

std::function<std::optional<uint32_t>(uint32_t)> CreateRodataMemoryReader(
    const ElfReader& reader, const ElfFunction& orig_func, uint32_t rodata_vram,
    uint32_t base_vram) {
  const ElfSection* rodata_sec = reader.FindSection(".rodata");
  if (rodata_sec == nullptr) {
    rodata_sec = reader.FindSection(".rdata");
  }
  if (rodata_sec == nullptr) {
    return nullptr;
  }
  std::vector<uint8_t> rodata_bytes = rodata_sec->data;
  uint32_t text_offset = orig_func.section_offset;
  uint32_t text_size = static_cast<uint32_t>(orig_func.raw_words.size() * 4);

  return [rodata_vram, rodata_bytes = std::move(rodata_bytes), text_offset, text_size,
          base_vram](uint32_t vram) -> std::optional<uint32_t> {
    if (vram >= rodata_vram && vram + 4 <= rodata_vram + rodata_bytes.size()) {
      uint32_t offset = vram - rodata_vram;
      uint32_t word = (static_cast<uint32_t>(rodata_bytes[offset]) << 24) |
                      (static_cast<uint32_t>(rodata_bytes[offset + 1]) << 16) |
                      (static_cast<uint32_t>(rodata_bytes[offset + 2]) << 8) |
                      static_cast<uint32_t>(rodata_bytes[offset + 3]);
      if (word >= 0x80000000) {
        return word;
      }
      if (word >= text_offset && word < text_offset + text_size && (word & 3) == 0) {
        return base_vram + (word - text_offset);
      }
      return word;
    }
    return std::nullopt;
  };
}

}  // namespace

std::filesystem::path RoundTripRunner::FindWorkspaceFile(std::string_view relative_path) {
  if (std::filesystem::exists(relative_path)) {
    return std::filesystem::absolute(relative_path);
  }
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
  return relative_path;
}

std::string RoundTripRunner::EmitBaseTypesHeader(std::string_view extra_declarations) {
  std::string out = R"(#ifndef TYPES_H
#define TYPES_H

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;

typedef float f32;
typedef double f64;

typedef struct { u8 b[4]; } UnalignedWord;
typedef struct { u8 b[6]; } UnalignedBlock6;
typedef struct { u8 b[8]; } UnalignedBlock8;
typedef struct { u8 b[12]; } UnalignedBlock12;
typedef struct { u8 b[16]; } UnalignedBlock16;
)";
  if (!extra_declarations.empty()) {
    absl::StrAppend(&out, "\n", extra_declarations, "\n");
  }
  absl::StrAppend(&out, "\n#endif  // TYPES_H\n");
  return out;
}

absl::StatusOr<std::unique_ptr<RoundTripRunner>> RoundTripRunner::Create() {
  ToolchainOptions toolchain_opts;
  toolchain_opts.repo_root = FindWorkspaceFile(".");
  toolchain_opts.mode = ToolchainMode::kOriginal;

  auto toolchain_or = Toolchain::Discover(toolchain_opts);
  if (!toolchain_or.ok()) {
    return toolchain_or.status();
  }
  return std::make_unique<RoundTripRunner>(std::move(*toolchain_or));
}

RoundTripRunner::RoundTripRunner(Toolchain toolchain)
    : toolchain_(std::move(toolchain)), compiler_(toolchain_) {}

absl::StatusOr<PreparedRoundTripCase> RoundTripRunner::PrepareCase(
    const RoundTripTestCase& test_case, const std::filesystem::path& work_dir) {
  std::error_code ec;
  std::filesystem::create_directories(work_dir, ec);
  if (ec) {
    return absl::InternalError(absl::StrFormat("Failed to create working directory '%s': %s",
                                               work_dir.string(), ec.message()));
  }

  std::filesystem::path types_h = work_dir / "types.h";
  WriteTextFile(types_h, EmitBaseTypesHeader(test_case.options.extra_types_header));

  PreparedRoundTripCase prepared;
  prepared.test_case = test_case;
  prepared.work_dir = work_dir;
  prepared.toolchain = &toolchain_;
  prepared.compiler = &compiler_;
  prepared.full_orig_c = absl::StrContains(test_case.original_c, "types.h")
                             ? test_case.original_c
                             : absl::StrCat("#include \"types.h\"\n", test_case.original_c);
  prepared.orig_c_path = work_dir / absl::StrCat("orig_", test_case.function_name, ".c");
  prepared.orig_o_path = work_dir / absl::StrCat("orig_", test_case.function_name, ".o");
  WriteTextFile(prepared.orig_c_path, prepared.full_orig_c);

  auto orig_compile_status = compiler_.CompileC({
      .src_file = prepared.orig_c_path,
      .out_obj = prepared.orig_o_path,
      .opt_flags = test_case.options.c_flags,
      .include_dirs = {work_dir},
  });
  if (!orig_compile_status.ok()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to compile original C code for '%s':\n%s\nOriginal C:\n%s", test_case.function_name,
        orig_compile_status.message(), prepared.full_orig_c));
  }

  auto orig_reader_or = ElfReader::LoadFromFile(prepared.orig_o_path);
  if (!orig_reader_or.ok()) {
    return orig_reader_or.status();
  }
  prepared.orig_elf = std::move(*orig_reader_or);

  auto orig_func_or = prepared.orig_elf.ExtractFunction(test_case.function_name);
  if (!orig_func_or.ok()) {
    return absl::NotFoundError(
        absl::StrFormat("Function '%s' not found in compiled original object: %s",
                        test_case.function_name, orig_func_or.status().message()));
  }
  prepared.orig_func = std::move(*orig_func_or);
  prepared.patched_orig_words = prepared.orig_func.raw_words;

  if (!prepared.orig_func.relocations.empty()) {
    SymbolRegistry reg;
    uint32_t next_func_vram = 0x80040000;
    uint32_t next_data_vram = 0x80050000;
    uint32_t next_rom_vram = 0x00D5A670;

    for (const auto& rel : prepared.orig_func.relocations) {
      if (rel.symbol_name.empty() || prepared.sym_to_vram.contains(rel.symbol_name)) continue;

      auto* entry = reg.add_entries();
      entry->set_name(rel.symbol_name);
      if (rel.symbol_name == ".text") {
        prepared.sym_to_vram[rel.symbol_name] = test_case.options.base_vram;
        entry->set_address(test_case.options.base_vram);
        entry->set_type(SYMBOL_FUNC);
      } else if (rel.type == R_MIPS_26) {
        prepared.sym_to_vram[rel.symbol_name] = next_func_vram;
        entry->set_address(next_func_vram);
        entry->set_type(SYMBOL_FUNC);
        next_func_vram += 0x1000;
      } else if (absl::StartsWith(rel.symbol_name, "rom_") ||
                 absl::StartsWith(rel.symbol_name, "D_00")) {
        prepared.sym_to_vram[rel.symbol_name] = next_rom_vram;
        entry->set_address(next_rom_vram);
        entry->set_type(SYMBOL_DATA);
        next_rom_vram += 0x8000;
      } else {
        prepared.sym_to_vram[rel.symbol_name] = next_data_vram;
        entry->set_address(next_data_vram);
        entry->set_type(SYMBOL_DATA);
        next_data_vram += 0x1000;
      }
    }

    auto sym_idx_or = SymbolIndex::FromProto(std::move(reg));
    if (!sym_idx_or.ok()) {
      return sym_idx_or.status();
    }
    prepared.symbol_index = std::make_unique<SymbolIndex>(std::move(*sym_idx_or));

    auto applied = RelocationApplier::Apply(prepared.orig_func, *prepared.symbol_index);
    prepared.patched_orig_words = std::move(applied.patched_words);

    if (auto it = prepared.sym_to_vram.find(".rodata"); it != prepared.sym_to_vram.end()) {
      prepared.rodata_memory_reader = CreateRodataMemoryReader(
          prepared.orig_elf, prepared.orig_func, it->second, test_case.options.base_vram);
    } else if (auto it_r = prepared.sym_to_vram.find(".rdata");
               it_r != prepared.sym_to_vram.end()) {
      prepared.rodata_memory_reader = CreateRodataMemoryReader(
          prepared.orig_elf, prepared.orig_func, it_r->second, test_case.options.base_vram);
    }
  }

  return prepared;
}

absl::StatusOr<RoundTripCaseOutcome> RoundTripRunner::RunCase(const RoundTripTestCase& test_case,
                                                              RoundTripStage* stage) {
  if (stage == nullptr) {
    return absl::InvalidArgumentError("RoundTripRunner::RunCase: stage must not be null.");
  }

  static std::atomic<uint64_t> dir_counter{0};
  uint64_t unique_id = dir_counter.fetch_add(1, std::memory_order_relaxed);
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  std::filesystem::path tmp_base =
      (test_tmpdir != nullptr) ? test_tmpdir : std::filesystem::temp_directory_path();
  std::filesystem::path work_dir =
      tmp_base / absl::StrCat("round_trip_", stage->Name(), "_", getpid(), "_", unique_id);

  auto cleanup = [&]() {
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
  };

  auto prepared_or = PrepareCase(test_case, work_dir);
  if (!prepared_or.ok()) {
    cleanup();
    return prepared_or.status();
  }

  auto outcome_or = stage->Execute(*prepared_or);
  cleanup();
  if (!outcome_or.ok()) {
    return outcome_or.status();
  }

  RecordOutcome(*outcome_or);
  return outcome_or;
}

void RoundTripRunner::RecordOutcome(const RoundTripCaseOutcome& outcome) {
  std::lock_guard<std::mutex> lock(mutex_);
  outcomes_.push_back(outcome);
}

size_t RoundTripRunner::TotalExecuted() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outcomes_.size();
}

size_t RoundTripRunner::StructurallyIsomorphicCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const auto& o : outcomes_) {
    if (o.structurally_isomorphic) ++count;
  }
  return count;
}

size_t RoundTripRunner::BitExactCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const auto& o : outcomes_) {
    if (o.bit_exact) ++count;
  }
  return count;
}

std::string RoundTripRunner::FormatSuiteSummaryReport(std::string_view suite_title) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const size_t total = outcomes_.size();
  size_t frame_matches = 0;
  size_t saved_reg_matches = 0;
  size_t cfg_matches = 0;
  size_t isomorphic_matches = 0;
  size_t bit_exact_matches = 0;
  size_t total_target_insns = 0;
  size_t total_lcs_insns = 0;
  std::vector<std::string> non_bit_exact_names;

  for (const auto& o : outcomes_) {
    if (o.structural_comparison.frame_size_matches) ++frame_matches;
    if (o.structural_comparison.saved_registers_match) ++saved_reg_matches;
    if (o.structural_comparison.cfg_topology_matches) ++cfg_matches;
    if (o.structurally_isomorphic) ++isomorphic_matches;
    if (o.bit_exact) {
      ++bit_exact_matches;
    } else {
      non_bit_exact_names.push_back(o.test_name);
    }
    total_target_insns += o.structural_comparison.target.instruction_count;
    total_lcs_insns += o.structural_comparison.normalized_lcs_instructions;
  }

  auto pct = [](size_t num, size_t den) -> double {
    return den > 0 ? (static_cast<double>(num) * 100.0 / static_cast<double>(den)) : 100.0;
  };

  std::string report;
  absl::StrAppend(
      &report,
      "================================================================================\n");
  absl::StrAppendFormat(&report, "[%s]\n", suite_title);
  absl::StrAppendFormat(&report, "Total Test Cases Evaluated:     %zu\n", total);
  absl::StrAppendFormat(&report, "  1. Stack Frame Size Match:    %zu / %zu (%.2f%%)\n",
                        frame_matches, total, pct(frame_matches, total));
  absl::StrAppendFormat(&report, "  2. Callee-Saved Reg Match:    %zu / %zu (%.2f%%)\n",
                        saved_reg_matches, total, pct(saved_reg_matches, total));
  absl::StrAppendFormat(&report, "  3. CFG Block & Branch Match:  %zu / %zu (%.2f%%)\n",
                        cfg_matches, total, pct(cfg_matches, total));
  absl::StrAppendFormat(&report, "     Structurally Isomorphic:   %zu / %zu (%.2f%%)\n",
                        isomorphic_matches, total, pct(isomorphic_matches, total));
  absl::StrAppendFormat(&report, "  4. Normalized Instruction LCS:%zu / %zu (%.2f%%)\n",
                        total_lcs_insns, total_target_insns,
                        pct(total_lcs_insns, total_target_insns));
  absl::StrAppendFormat(&report, "  5. Bit-Exact Match:           %zu / %zu (%.2f%%)\n",
                        bit_exact_matches, total, pct(bit_exact_matches, total));
  if (!non_bit_exact_names.empty()) {
    absl::StrAppendFormat(&report, "  Non-Bit-Exact Cases (%zu):\n", non_bit_exact_names.size());
    for (const auto& name : non_bit_exact_names) {
      absl::StrAppendFormat(&report, "    - %s\n", name);
    }
  }
  absl::StrAppend(
      &report,
      "================================================================================\n");
  return report;
}

std::vector<uint32_t> RoundTripRunner::ApplyCandidateRelocations(
    const PreparedRoundTripCase& prepared, const ElfFunction& cand_func) {
  const uint32_t base_vram = prepared.test_case.options.base_vram;
  const uint32_t text_base_vram = base_vram - cand_func.section_offset;
  auto lookup = [&](std::string_view name) -> std::optional<uint32_t> {
    if (name == ".text") {
      return text_base_vram;
    }
    if (name == ".rodata" || name == ".rdata") {
      auto it = prepared.sym_to_vram.find(".rodata");
      if (it != prepared.sym_to_vram.end()) return it->second;
      auto it_r = prepared.sym_to_vram.find(".rdata");
      if (it_r != prepared.sym_to_vram.end()) return it_r->second;
      return 0x80050000;
    }
    auto it = prepared.sym_to_vram.find(name);
    if (it != prepared.sym_to_vram.end()) {
      return it->second;
    }
    return ResolveSymbolOrSynthesizedAddress(name, prepared.symbol_index.get());
  };
  return RelocationApplier::Apply(cand_func, lookup).patched_words;
}

FunctionStructuralComparison RoundTripRunner::EvaluateStructuralIsomorphism(
    const PreparedRoundTripCase& prepared, const ElfFunction& cand_func, std::string_view cand_c,
    std::string* diagnostic_out) {
  const uint32_t base_vram = prepared.test_case.options.base_vram;
  std::vector<uint32_t> patched_cand_words = ApplyCandidateRelocations(prepared, cand_func);
  FunctionStructuralComparison comp = CompareFunctionStructure(
      prepared.patched_orig_words, base_vram, patched_cand_words, base_vram);

  if (!comp.IsStructurallyIsomorphic() && diagnostic_out != nullptr) {
    std::string bit_exact_diff;
    (void)CompareFunctionsBitExact(prepared.orig_func, cand_func, base_vram, prepared.full_orig_c,
                                   cand_c, &bit_exact_diff);
    *diagnostic_out = absl::StrFormat(
        "\n================================================================================\n"
        "STRUCTURAL ISOMORPHISM FAILURE: Function '%s' (Test '%s')\n"
        "================================================================================\n"
        "  1. Stack Frame Size:   Target = %u bytes | Candidate = %u bytes [%s]\n"
        "  2. Saved GPR Mask:     Target = 0x%08X | Candidate = 0x%08X [%s]\n"
        "     Saved FPR Mask:     Target = 0x%08X | Candidate = 0x%08X\n"
        "  3. CFG Basic Blocks:   Target = %zu | Candidate = %zu [%s]\n"
        "     CFG Branch Count:   Target = %zu | Candidate = %zu\n"
        "  4. Normalized LCS:     %zu / %zu instructions (%.2f%%)\n"
        "%s",
        prepared.orig_func.name, prepared.test_case.test_name, comp.target.frame_size,
        comp.candidate.frame_size, comp.frame_size_matches ? "OK" : "MISMATCH",
        comp.target.saved_gpr_mask, comp.candidate.saved_gpr_mask,
        comp.saved_registers_match ? "OK" : "MISMATCH", comp.target.saved_fpr_mask,
        comp.candidate.saved_fpr_mask, comp.target.basic_block_count,
        comp.candidate.basic_block_count, comp.cfg_topology_matches ? "OK" : "MISMATCH",
        comp.target.branch_count, comp.candidate.branch_count, comp.normalized_lcs_instructions,
        comp.target.instruction_count, comp.NormalizedLcsPercentage(), bit_exact_diff);
  }
  return comp;
}

bool RoundTripRunner::CompareFunctionsBitExact(const ElfFunction& orig, const ElfFunction& cand,
                                               uint32_t base_vram, std::string_view orig_c,
                                               std::string_view cand_c,
                                               std::string* diagnostic_out) {
  absl::flat_hash_map<uint32_t, ElfRelocation> orig_relocs;
  for (const auto& rel : orig.relocations) {
    orig_relocs[rel.function_offset] = rel;
  }
  absl::flat_hash_map<uint32_t, ElfRelocation> cand_relocs;
  for (const auto& rel : cand.relocations) {
    cand_relocs[rel.function_offset] = rel;
  }

  size_t max_words = std::max(orig.raw_words.size(), cand.raw_words.size());
  bool bit_exact = (orig.raw_words.size() == cand.raw_words.size());

  DisassemblerOptions disasm_opts;
  disasm_opts.emit_line_comments = false;
  disasm_opts.emit_function_framing = false;
  Disassembler disasm(nullptr, disasm_opts);

  std::string disasm_diff;
  disasm_diff += absl::StrFormat("%-8s %-42s %-42s %s\n", "Offset", "Original MIPS",
                                 "Recompiled Candidate MIPS", "Match");
  disasm_diff += std::string(105, '-') + "\n";

  for (size_t i = 0; i < max_words; ++i) {
    uint32_t offset = static_cast<uint32_t>(i * 4);
    uint32_t vram = base_vram + offset;

    std::string orig_str;
    std::string cand_str;
    bool line_matches = true;

    auto orig_it = orig_relocs.find(offset);
    auto cand_it = cand_relocs.find(offset);
    bool orig_has_rel = (orig_it != orig_relocs.end());
    bool cand_has_rel = (cand_it != cand_relocs.end());

    if (i < orig.raw_words.size()) {
      uint32_t word = orig.raw_words[i];
      std::string asm_text = disasm.DisassembleInstruction(word, vram);
      if (orig_has_rel) {
        orig_str =
            absl::StrFormat("0x%08X  %-20s [%s]", word, asm_text, orig_it->second.symbol_name);
      } else {
        orig_str = absl::StrFormat("0x%08X  %s", word, asm_text);
      }
    } else {
      orig_str = "(missing)";
      line_matches = false;
    }

    if (i < cand.raw_words.size()) {
      uint32_t word = cand.raw_words[i];
      std::string asm_text = disasm.DisassembleInstruction(word, vram);
      if (cand_has_rel) {
        cand_str =
            absl::StrFormat("0x%08X  %-20s [%s]", word, asm_text, cand_it->second.symbol_name);
      } else {
        cand_str = absl::StrFormat("0x%08X  %s", word, asm_text);
      }
    } else {
      cand_str = "(missing)";
      line_matches = false;
    }

    if (i < orig.raw_words.size() && i < cand.raw_words.size()) {
      if (orig_has_rel != cand_has_rel) {
        line_matches = false;
      } else if (!orig_has_rel) {
        if (orig.raw_words[i] != cand.raw_words[i]) {
          line_matches = false;
        }
      } else {
        const auto& orig_rel = orig_it->second;
        const auto& cand_rel = cand_it->second;
        bool sym_matches =
            (orig_rel.symbol_name == cand_rel.symbol_name) ||
            ((orig_rel.symbol_name == ".rodata" || orig_rel.symbol_name == ".rdata") &&
             absl::StartsWith(cand_rel.symbol_name, "D_"));
        if (orig_rel.type != cand_rel.type || !sym_matches) {
          line_matches = false;
        } else {
          uint32_t mask = 0xFFFFFFFF;
          if (orig_rel.type == R_MIPS_26) {
            mask = 0xFC000000;
          } else if (orig_rel.type == R_MIPS_HI16 || orig_rel.type == R_MIPS_LO16) {
            mask = 0xFFFF0000;
          }
          if ((orig.raw_words[i] & mask) != (cand.raw_words[i] & mask)) {
            line_matches = false;
          }
        }
      }
    }

    if (!line_matches) {
      bit_exact = false;
    }

    disasm_diff += absl::StrFormat("0x%04X   %-42s %-42s %s\n", offset, orig_str, cand_str,
                                   line_matches ? "OK" : "MISMATCH");
  }

  if (!bit_exact && diagnostic_out != nullptr) {
    *diagnostic_out = absl::StrFormat(
        "\n================================================================================\n"
        "ROUND-TRIP BIT-EXACTNESS FAILURE: Function '%s'\n"
        "================================================================================\n"
        "ORIGINAL C:\n"
        "--------------------------------------------------------------------------------\n"
        "%s\n"
        "--------------------------------------------------------------------------------\n"
        "CANDIDATE C:\n"
        "--------------------------------------------------------------------------------\n"
        "%s\n"
        "--------------------------------------------------------------------------------\n"
        "DISASSEMBLY COMPARISON (Original vs Recompiled Candidate):\n"
        "--------------------------------------------------------------------------------\n"
        "%s"
        "================================================================================\n",
        orig.name, orig_c, cand_c, disasm_diff);
  }

  return bit_exact;
}

}  // namespace rom_nom_nom
