#include "lifter/round_trip_test_util.h"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/compiler.h"
#include "core/disassembler.h"
#include "core/elf_reader.h"
#include "core/relocation_applier.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"
#include "lifter/c_emitter.h"
#include "lifter/function_loader.h"
#include "lifter/lifter_pipeline.h"

namespace rom_nom_nom {
namespace {

std::filesystem::path FindWorkspaceFile(std::string_view relative_path) {
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

void WriteTextFile(const std::filesystem::path& path, std::string_view content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  ofs.write(content.data(), content.size());
}

MemoryReader CreateRodataMemoryReader(const ElfReader& reader, const ElfFunction& orig_func,
                                      uint32_t rodata_vram, uint32_t base_vram) {
  const ElfSection* rodata_sec = reader.FindSection(".rodata");
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

Toolchain* shared_toolchain = nullptr;

}  // namespace

void RoundTripTest::SetUpTestSuite() {
  ToolchainOptions toolchain_opts;
  toolchain_opts.repo_root = FindWorkspaceFile(".");
  toolchain_opts.mode = ToolchainMode::kOriginal;

  auto toolchain_or = Toolchain::Discover(toolchain_opts);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();
  shared_toolchain = new Toolchain(std::move(*toolchain_or));
}

void RoundTripTest::TearDownTestSuite() {
  delete shared_toolchain;
  shared_toolchain = nullptr;
}

void RoundTripTest::SetUp() {
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  std::filesystem::path tmp_base =
      (test_tmpdir != nullptr) ? test_tmpdir : std::filesystem::temp_directory_path();
  work_dir_ = tmp_base / absl::StrCat("lifter_round_trip_", getpid());
  std::filesystem::create_directories(work_dir_);

  std::filesystem::path types_h = work_dir_ / "types.h";
  TypesHeaderOptions types_opts;
  types_opts.include_math_intrinsics = false;
  WriteTextFile(types_h, CEmitter::EmitTypesHeader(types_opts));

  ASSERT_NE(shared_toolchain, nullptr);
  compiler_ = std::make_unique<Compiler>(*shared_toolchain);
}

void RoundTripTest::TearDown() {
  std::error_code ec;
  std::filesystem::remove_all(work_dir_, ec);
}

void RoundTripTest::AssertRoundTripBitExact(std::string_view func_name, std::string_view original_c,
                                            const RoundTripOptions& options) {
  if (!options.extra_types_header.empty()) {
    std::filesystem::path types_h = work_dir_ / "types.h";
    TypesHeaderOptions types_opts;
    types_opts.include_math_intrinsics = false;
    types_opts.extra_declarations = options.extra_types_header;
    WriteTextFile(types_h, CEmitter::EmitTypesHeader(types_opts));
  }

  std::string full_orig_c = absl::StrContains(original_c, "types.h")
                                ? std::string(original_c)
                                : absl::StrCat("#include \"types.h\"\n", original_c);

  std::filesystem::path orig_c_path = work_dir_ / absl::StrCat("orig_", func_name, ".c");
  std::filesystem::path orig_o_path = work_dir_ / absl::StrCat("orig_", func_name, ".o");
  WriteTextFile(orig_c_path, full_orig_c);

  auto orig_compile_status = compiler_->CompileC({
      .src_file = orig_c_path,
      .out_obj = orig_o_path,
      .opt_flags = options.c_flags,
      .include_dirs = {work_dir_},
  });
  ASSERT_TRUE(orig_compile_status.ok())
      << "Failed to compile original C code for '" << func_name << "':\n"
      << orig_compile_status.message() << "\nOriginal C:\n"
      << full_orig_c;

  auto orig_reader_or = ElfReader::LoadFromFile(orig_o_path);
  ASSERT_TRUE(orig_reader_or.ok()) << orig_reader_or.status();
  auto orig_func_or = orig_reader_or->ExtractFunction(func_name);
  ASSERT_TRUE(orig_func_or.ok()) << "Function '" << func_name
                                 << "' not found in compiled original object: "
                                 << orig_func_or.status().message();
  ElfFunction orig_func = *orig_func_or;

  std::unique_ptr<SymbolIndex> symbol_index;
  std::vector<uint32_t> words_to_lift = orig_func.raw_words;
  MemoryReader memory_reader = nullptr;

  if (!orig_func.relocations.empty()) {
    SymbolRegistry reg;
    uint32_t next_func_vram = 0x80040000;
    uint32_t next_data_vram = 0x80050000;
    uint32_t next_rom_vram = 0x00D5A670;

    absl::flat_hash_map<std::string, uint32_t> sym_to_vram;
    for (const auto& rel : orig_func.relocations) {
      if (rel.symbol_name.empty() || sym_to_vram.contains(rel.symbol_name)) continue;

      auto* entry = reg.add_entries();
      entry->set_name(rel.symbol_name);
      if (rel.symbol_name == ".text") {
        sym_to_vram[rel.symbol_name] = options.base_vram;
        entry->set_address(options.base_vram);
        entry->set_type(SYMBOL_FUNC);
      } else if (rel.type == R_MIPS_26) {
        sym_to_vram[rel.symbol_name] = next_func_vram;
        entry->set_address(next_func_vram);
        entry->set_type(SYMBOL_FUNC);
        next_func_vram += 0x1000;
      } else if (absl::StartsWith(rel.symbol_name, "rom_") ||
                 absl::StartsWith(rel.symbol_name, "D_00")) {
        sym_to_vram[rel.symbol_name] = next_rom_vram;
        entry->set_address(next_rom_vram);
        entry->set_type(SYMBOL_DATA);
        next_rom_vram += 0x8000;
      } else {
        sym_to_vram[rel.symbol_name] = next_data_vram;
        entry->set_address(next_data_vram);
        entry->set_type(SYMBOL_DATA);
        next_data_vram += 0x1000;
      }
    }

    auto sym_idx_or = SymbolIndex::FromProto(std::move(reg));
    ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();
    symbol_index = std::make_unique<SymbolIndex>(std::move(*sym_idx_or));

    auto applied = RelocationApplier::Apply(orig_func, *symbol_index);
    words_to_lift = std::move(applied.patched_words);

    if (auto it = sym_to_vram.find(".rodata"); it != sym_to_vram.end()) {
      memory_reader =
          CreateRodataMemoryReader(*orig_reader_or, orig_func, it->second, options.base_vram);
    }
  }

  auto loaded_or =
      FunctionLoader::FromWords(std::string(func_name), words_to_lift, options.base_vram,
                                symbol_index.get(), /*game_config=*/nullptr, memory_reader);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  LifterPipelineOptions lifter_opts;
  lifter_opts.includes = {"types.h"};
  lifter_opts.format_with_clang = false;
  LifterPipeline lifter(lifter_opts);

  auto decompile_or = lifter.DecompileFunction(*loaded_or);
  ASSERT_TRUE(decompile_or.ok()) << "Lifter failed to decompile function '" << func_name
                                 << "': " << decompile_or.status().message();

  CTranslationUnit tu;
  tu.includes = {"types.h"};
  tu.symbol_index = symbol_index.get();
  tu.functions.push_back(std::move(*decompile_or->ast));

  CEmitterOptions emitter_opts;
  emitter_opts.includes = {"types.h"};
  emitter_opts.format_with_clang = false;
  std::string lifted_c = CEmitter::EmitTranslationUnit(tu, emitter_opts);

  std::filesystem::path cand_c_path = work_dir_ / absl::StrCat("cand_", func_name, ".c");
  std::filesystem::path cand_o_path = work_dir_ / absl::StrCat("cand_", func_name, ".o");
  WriteTextFile(cand_c_path, lifted_c);

  auto cand_compile_status = compiler_->CompileC({
      .src_file = cand_c_path,
      .out_obj = cand_o_path,
      .opt_flags = options.c_flags,
      .include_dirs = {work_dir_},
  });
  ASSERT_TRUE(cand_compile_status.ok())
      << "Failed to compile lifted C code for '" << func_name << "':\n"
      << cand_compile_status.message() << "\nLifted C:\n"
      << lifted_c;

  auto cand_reader_or = ElfReader::LoadFromFile(cand_o_path);
  ASSERT_TRUE(cand_reader_or.ok()) << cand_reader_or.status();
  auto cand_func_or = cand_reader_or->ExtractFunction(func_name);
  ASSERT_TRUE(cand_func_or.ok()) << "Function '" << func_name
                                 << "' not found in recompiled lifted object: "
                                 << cand_func_or.status().message();
  ElfFunction cand_func = *cand_func_or;

  std::string diff_diagnostic;
  bool is_bit_exact = CompareFunctions(orig_func, cand_func, options.base_vram, full_orig_c,
                                       lifted_c, &diff_diagnostic);
  EXPECT_TRUE(is_bit_exact) << diff_diagnostic;
}

bool RoundTripTest::CompareFunctions(const ElfFunction& orig, const ElfFunction& cand,
                                     uint32_t base_vram, std::string_view orig_c,
                                     std::string_view lifted_c, std::string* diagnostic_out) {
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
                                 "Recompiled Lifted MIPS", "Match");
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
            (orig_rel.symbol_name == ".rodata" && absl::StartsWith(cand_rel.symbol_name, "D_"));
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

  if (!bit_exact) {
    *diagnostic_out = absl::StrFormat(
        "\n================================================================================\n"
        "LIFTER ROUND-TRIP BIT-EXACTNESS FAILURE: Function '%s'\n"
        "================================================================================\n"
        "ORIGINAL C:\n"
        "--------------------------------------------------------------------------------\n"
        "%s\n"
        "--------------------------------------------------------------------------------\n"
        "LIFTED C:\n"
        "--------------------------------------------------------------------------------\n"
        "%s\n"
        "--------------------------------------------------------------------------------\n"
        "DISASSEMBLY COMPARISON (Original vs Recompiled Lifted):\n"
        "--------------------------------------------------------------------------------\n"
        "%s"
        "================================================================================\n",
        orig.name, orig_c, lifted_c, disasm_diff);
    return false;
  }

  return true;
}

}  // namespace rom_nom_nom
