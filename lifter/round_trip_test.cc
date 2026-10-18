#include <elf.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/compiler.h"
#include "core/disassembler.h"
#include "core/elf_reader.h"
#include "core/mips.h"
#include "core/relocation_applier.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"
#include "gtest/gtest.h"
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

struct RoundTripOptions {
  std::vector<std::string> c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  uint32_t base_vram = 0x80020000;
  std::string extra_types_header;
};

class RoundTripTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    std::filesystem::path tmp_base =
        (test_tmpdir != nullptr) ? test_tmpdir : std::filesystem::temp_directory_path();
    work_dir_ = tmp_base / absl::StrCat("lifter_round_trip_", getpid());
    std::filesystem::create_directories(work_dir_);

    // Setup types.h
    std::filesystem::path types_h = work_dir_ / "types.h";
    std::string types_content = R"(#ifndef TYPES_H
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

#endif // TYPES_H
)";
    WriteTextFile(types_h, types_content);

    ToolchainOptions toolchain_opts;
    toolchain_opts.repo_root = FindWorkspaceFile(".");
    toolchain_opts.mode = ToolchainMode::kOriginal;

    auto toolchain_or = Toolchain::Discover(toolchain_opts);
    ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();
    compiler_ = std::make_unique<Compiler>(*toolchain_or);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work_dir_, ec);
  }

  void AssertRoundTripBitExact(std::string_view func_name, std::string_view original_c,
                               const RoundTripOptions& options = {}) {
    // 1. Prepare types header if custom types requested
    if (!options.extra_types_header.empty()) {
      std::filesystem::path types_h = work_dir_ / "types.h";
      std::string types_content = R"(#ifndef TYPES_H
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

)" + options.extra_types_header + "\n#endif // TYPES_H\n";
      WriteTextFile(types_h, types_content);
    }

    // 2. Prepare original C source file
    std::string full_orig_c;
    if (!absl::StrContains(original_c, "types.h")) {
      full_orig_c = absl::StrCat("#include \"types.h\"\n", original_c);
    } else {
      full_orig_c = std::string(original_c);
    }

    std::filesystem::path orig_c_path = work_dir_ / absl::StrCat("orig_", func_name, ".c");
    std::filesystem::path orig_o_path = work_dir_ / absl::StrCat("orig_", func_name, ".o");
    WriteTextFile(orig_c_path, full_orig_c);

    // 3. Compile original C with GCC 2.7.2
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

    // 4. Extract function from original .o
    auto orig_reader_or = ElfReader::LoadFromFile(orig_o_path);
    ASSERT_TRUE(orig_reader_or.ok()) << orig_reader_or.status();
    auto orig_func_or = orig_reader_or->ExtractFunction(func_name);
    ASSERT_TRUE(orig_func_or.ok())
        << "Function '" << func_name
        << "' not found in compiled original object: " << orig_func_or.status().message();
    ElfFunction orig_func = *orig_func_or;

    // 5. Handle relocations and build SymbolIndex
    std::unique_ptr<SymbolIndex> symbol_index;
    std::vector<uint32_t> words_to_lift = orig_func.raw_words;

    if (!orig_func.relocations.empty()) {
      SymbolRegistry reg;
      uint32_t next_func_vram = 0x80040000;
      uint32_t next_data_vram = 0x80050000;

      absl::flat_hash_map<std::string, uint32_t> sym_to_vram;
      for (const auto& rel : orig_func.relocations) {
        if (rel.symbol_name.empty()) continue;
        if (sym_to_vram.contains(rel.symbol_name)) continue;

        auto* entry = reg.add_entries();
        entry->set_name(rel.symbol_name);
        if (rel.type == R_MIPS_26) {
          sym_to_vram[rel.symbol_name] = next_func_vram;
          entry->set_address(next_func_vram);
          entry->set_type(SYMBOL_FUNC);
          next_func_vram += 0x1000;
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
    }

    // 6. Lift MIPS binary to C using LifterPipeline
    auto loaded_or = FunctionLoader::FromWords(std::string(func_name), words_to_lift,
                                               options.base_vram, symbol_index.get());
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

    // 7. Recompile lifted C with the exact same GCC 2.7.2 flags
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

    // 8. Extract function from recompiled lifted .o
    auto cand_reader_or = ElfReader::LoadFromFile(cand_o_path);
    ASSERT_TRUE(cand_reader_or.ok()) << cand_reader_or.status();
    auto cand_func_or = cand_reader_or->ExtractFunction(func_name);
    ASSERT_TRUE(cand_func_or.ok())
        << "Function '" << func_name
        << "' not found in recompiled lifted object: " << cand_func_or.status().message();
    ElfFunction cand_func = *cand_func_or;

    // 9. Compare TargetFunction vs CandidateFunction for 100% bit-exactness!
    std::string diff_diagnostic;
    bool is_bit_exact = CompareFunctions(orig_func, cand_func, options.base_vram, full_orig_c,
                                         lifted_c, &diff_diagnostic);

    EXPECT_TRUE(is_bit_exact) << diff_diagnostic;
  }

  bool CompareFunctions(const ElfFunction& orig, const ElfFunction& cand, uint32_t base_vram,
                        std::string_view orig_c, std::string_view lifted_c,
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
          if (orig_rel.type != cand_rel.type || orig_rel.symbol_name != cand_rel.symbol_name) {
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

 private:
  std::filesystem::path work_dir_;
  std::unique_ptr<Compiler> compiler_;
};

// -----------------------------------------------------------------------------
// 1. Basic Arithmetic & Bitwise Operations
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, ArithmeticAddTwo) {
  AssertRoundTripBitExact("AddTwo", R"(
s32 AddTwo(s32 a, s32 b) {
    return a + b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticSubTwo) {
  AssertRoundTripBitExact("SubTwo", R"(
s32 SubTwo(s32 a, s32 b) {
    return a - b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseAnd) {
  AssertRoundTripBitExact("BitwiseAnd", R"(
s32 BitwiseAnd(s32 a, s32 b) {
    return a & b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseOr) {
  AssertRoundTripBitExact("BitwiseOr", R"(
s32 BitwiseOr(s32 a, s32 b) {
    return a | b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseXor) {
  AssertRoundTripBitExact("BitwiseXor", R"(
s32 BitwiseXor(s32 a, s32 b) {
    return a ^ b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseNot) {
  AssertRoundTripBitExact("BitwiseNot", R"(
s32 BitwiseNot(s32 a) {
    return ~a;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftLeftImm) {
  AssertRoundTripBitExact("ShiftLeftImm", R"(
s32 ShiftLeftImm(s32 a) {
    return a << 4;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftRightArithmeticImm) {
  AssertRoundTripBitExact("ShiftRightArithmeticImm", R"(
s32 ShiftRightArithmeticImm(s32 a) {
    return a >> 3;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftRightLogicalImm) {
  AssertRoundTripBitExact("ShiftRightLogicalImm", R"(
u32 ShiftRightLogicalImm(u32 a) {
    return a >> 3;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftLeftVariable) {
  AssertRoundTripBitExact("ShiftLeftVariable", R"(
s32 ShiftLeftVariable(s32 a, s32 b) {
    return a << b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticAddImmediate) {
  AssertRoundTripBitExact("AddImmediate", R"(
s32 AddImmediate(s32 a) {
    return a + 1234;
}
)");
}

TEST_F(RoundTripTest, ArithmeticLargeImmediate) {
  AssertRoundTripBitExact("LargeImmediate", R"(
u32 LargeImmediate(u32 a) {
    return a + 0x12345678;
}
)");
}

// -----------------------------------------------------------------------------
// 2. Signed & Unsigned Integer Loads, Stores, and Casts
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, IntegerLoadS8) {
  AssertRoundTripBitExact("LoadS8", R"(
s32 LoadS8(s8* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreS8) {
  AssertRoundTripBitExact("StoreS8", R"(
void StoreS8(s8* p, s8 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadU8) {
  AssertRoundTripBitExact("LoadU8", R"(
u32 LoadU8(u8* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreU8) {
  AssertRoundTripBitExact("StoreU8", R"(
void StoreU8(u8* p, u8 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadS16) {
  AssertRoundTripBitExact("LoadS16", R"(
s32 LoadS16(s16* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreS16) {
  AssertRoundTripBitExact("StoreS16", R"(
void StoreS16(s16* p, s16 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadU16) {
  AssertRoundTripBitExact("LoadU16", R"(
u32 LoadU16(u16* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreU16) {
  AssertRoundTripBitExact("StoreU16", R"(
void StoreU16(u16* p, u16 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadS32) {
  AssertRoundTripBitExact("LoadS32", R"(
s32 LoadS32(s32* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreS32) {
  AssertRoundTripBitExact("StoreS32", R"(
void StoreS32(s32* p, s32 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerCastU8ToU32) {
  AssertRoundTripBitExact("CastU8ToU32", R"(
u32 CastU8ToU32(u32 a) {
    return (u8)a;
}
)");
}

TEST_F(RoundTripTest, IntegerCastU16ToU32) {
  AssertRoundTripBitExact("CastU16ToU32", R"(
u32 CastU16ToU32(u32 a) {
    return (u16)a;
}
)");
}

TEST_F(RoundTripTest, IntegerCastS16ToS32) {
  AssertRoundTripBitExact("CastS16ToS32", R"(
s32 CastS16ToS32(s32 a) {
    return (s16)a;
}
)");
}

// -----------------------------------------------------------------------------
// 3. Pointer Dereferences, Struct Field Offsets, and Array Indexing
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, PointerDereference) {
  AssertRoundTripBitExact("PointerDeref", R"(
s32 PointerDeref(s32* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, PointerArrayIndexConst) {
  AssertRoundTripBitExact("ArrayIndexConst", R"(
s32 ArrayIndexConst(s32* arr) {
    return arr[3];
}
)");
}

TEST_F(RoundTripTest, PointerArrayIndexVar) {
  AssertRoundTripBitExact("ArrayIndexVar", R"(
s32 ArrayIndexVar(s32* arr, s32 idx) {
    return arr[idx];
}
)");
}

TEST_F(RoundTripTest, PointerArrayWrite) {
  AssertRoundTripBitExact("ArrayWrite", R"(
void ArrayWrite(s32* arr, s32 idx, s32 val) {
    arr[idx] = val;
}
)");
}

TEST_F(RoundTripTest, StructFieldAccess) {
  std::string entity_def = R"(
typedef struct {
    s32 id;
    s16 x;
    s16 y;
    u32 flags;
} Entity;
)";
  RoundTripOptions opts;
  opts.extra_types_header = entity_def;

  AssertRoundTripBitExact("GetEntityId", R"(
s32 GetEntityId(Entity* e) {
    return e->id;
}
)",
                          opts);

  AssertRoundTripBitExact("GetEntityX", R"(
s32 GetEntityX(Entity* e) {
    return e->x;
}
)",
                          opts);

  AssertRoundTripBitExact("SetEntityY", R"(
void SetEntityY(Entity* e, s16 y) {
    e->y = y;
}
)",
                          opts);
}

// -----------------------------------------------------------------------------
// 4. Simple and Nested If / If-Else Branches and Early Returns
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, BranchSignum) {
  AssertRoundTripBitExact("Signum", R"(
s32 Signum(s32 a) {
    if (a > 0) {
        return 1;
    } else if (a < 0) {
        return -1;
    } else {
        return 0;
    }
}
)");
}

TEST_F(RoundTripTest, BranchIfElseReturnConst) {
  AssertRoundTripBitExact("IfElseReturnConst", R"(
s32 IfElseReturnConst(s32 a) {
    if (a > 0) {
        return 10;
    } else {
        return 20;
    }
}
)");
}

TEST_F(RoundTripTest, BranchIfElseAssign) {
  AssertRoundTripBitExact("IfElseAssign", R"(
s32 IfElseAssign(s32 a, s32 b) {
    s32 res;
    if (a > 10) {
        res = a + b;
    } else {
        res = a - b;
    }
    return res;
}
)");
}

// In EarlyReturnIfZero, the lifter structures the control flow into a single return:
//   v0 = arg1; if (arg0 != 0) v0 = (arg0 + v0); return v0;
// When recompiled, GCC 2.7.2 emits commutative 'addu $v0, $v0, $a0' instead of
// original 'addu $v0, $a0, $v0'.
TEST_F(RoundTripTest, DISABLED_BranchEarlyReturnIfZero) {
  AssertRoundTripBitExact("EarlyReturnIfZero", R"(
s32 EarlyReturnIfZero(s32 a, s32 b) {
    if (a == 0) {
        return b;
    }
    return a + b;
}
)");
}

// In MaxTwo, $v0 is used in the 'slt $v1, $a1, $v0' comparison feeding 'beql'.
// DetermineReturnsV0's heuristic treats $v0 used by non-branch instructions as an
// intermediate temporary, inferring a void return type.
TEST_F(RoundTripTest, DISABLED_BranchMaxTwo) {
  AssertRoundTripBitExact("MaxTwo", R"(
s32 MaxTwo(s32 a, s32 b) {
    if (a > b) {
        return a;
    } else {
        return b;
    }
}
)");
}

// GCC 2.7.2 compiles 'a >= min && a <= max' into branchless boolean logic.
// Lifter decomposes into intermediate variables, leading GCC 2.7.2 to schedule
// the subexpressions in reverse order:
// Original: slt $a1, $a0, $a1; xori; slt $v0, $a2, $a0; xori; and $v0, $a1, $v0
// Recompiled: slt $v0, $a2, $a0; xori; slt $a0, $a0, $a1; xori; and $v0, $v0, $a0
TEST_F(RoundTripTest, DISABLED_BranchInRange) {
  AssertRoundTripBitExact("InRange", R"(
s32 InRange(s32 a, s32 min, s32 max) {
    if (a >= min && a <= max) {
        return 1;
    }
    return 0;
}
)");
}

// GCC 2.7.2 compiles 'a < min || a > max' into branchless logic using 'or'.
// Due to commutative operand swap in the accumulator, the final instruction is
// 'or $v0, $v0, $a1' instead of 'or $v0, $a1, $v0'.
TEST_F(RoundTripTest, DISABLED_BranchIsOutside) {
  AssertRoundTripBitExact("IsOutside", R"(
s32 IsOutside(s32 a, s32 min, s32 max) {
    if (a < min || a > max) {
        return 1;
    }
    return 0;
}
)");
}

// -----------------------------------------------------------------------------
// 5. Loops (Do-While, While, For Loops)
// -----------------------------------------------------------------------------

// GCC 2.7.2 with -O2 rotates 'do { total += count; count--; } while (count > 0);'
// into a backward branch using 'bgtzl' in the loop latch.
// The control flow structurer emits 'while (1) { arg0 = arg0 - 1; if (arg0 <= 0) break; total =
// total + arg0; }'. GCC compiles the lifted loop using a forward conditional branch ('blez') and
// jump ('j'), rather than a single 'bgtzl'.
TEST_F(RoundTripTest, DISABLED_LoopDoWhileCountDown) {
  AssertRoundTripBitExact("DoWhileCountDown", R"(
s32 DoWhileCountDown(s32 count) {
    s32 total = 0;
    do {
        total += count;
        count--;
    } while (count > 0);
    return total;
}
)");
}

// GCC 2.7.2 rotates 'while (count > 0)' loops into a guarded loop with an entry
// branch ('blez') and backward branch ('bgtzl').
// The control flow structurer reconstructs this as 'if (arg0 > 0) { while (1) { ... break; } }',
// causing GCC to compile it with forward conditional branch + jump rather than 'bgtzl'.
TEST_F(RoundTripTest, DISABLED_LoopWhileLoop) {
  AssertRoundTripBitExact("WhileLoop", R"(
s32 WhileLoop(s32 count) {
    s32 total = 0;
    while (count > 0) {
        total += count;
        count--;
    }
    return total;
}
)");
}

// For-loops are lowered into guarded while-loops. Recompiled lifted C emits
// forward conditional break + jump, generating 'blez', 'addiu', 'slt', 'beq', 'j',
// rather than the compact 'bnel' loop produced by original C.
TEST_F(RoundTripTest, DISABLED_LoopForLoopSum) {
  AssertRoundTripBitExact("ForLoopSum", R"(
s32 ForLoopSum(s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}
)");
}

// In original C 'ArraySum(s32* arr, s32 n)', GCC 2.7.2 allocates an 8-byte stack frame
// ('addiu $sp, $sp, -8'). In lifted C using pointer arithmetic ('*(s32*)arg0; arg0 += 4'),
// GCC 2.7.2 does not allocate a stack frame, and register allocation chooses $v0/$v1
// in reverse order.
TEST_F(RoundTripTest, DISABLED_LoopArraySum) {
  AssertRoundTripBitExact("ArraySum", R"(
s32 ArraySum(s32* arr, s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}
)");
}

// -----------------------------------------------------------------------------
// 6. Function Calls (Leaf to Non-Leaf, Callee-Saved Registers)
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, CallDirectOneArg) {
  AssertRoundTripBitExact("CallDirectOneArg", R"(
extern s32 CalleeOne(s32 a);

s32 CallDirectOneArg(s32 a) {
    return CalleeOne(a);
}
)");
}

TEST_F(RoundTripTest, CallDirectTwoArgs) {
  AssertRoundTripBitExact("CallDirectTwoArgs", R"(
extern s32 CalleeTwo(s32 a, s32 b);

s32 CallDirectTwoArgs(s32 a, s32 b) {
    return CalleeTwo(a, b);
}
)");
}

TEST_F(RoundTripTest, CallNonTailAdd) {
  AssertRoundTripBitExact("CallNonTailAdd", R"(
extern s32 CalleeOne(s32 a);

s32 CallNonTailAdd(s32 a) {
    return CalleeOne(a) + 1;
}
)");
}

TEST_F(RoundTripTest, CallPreserveArgAcrossCall) {
  AssertRoundTripBitExact("CallPreserveArgAcrossCall", R"(
extern s32 CalleeOne(s32 a);

s32 CallPreserveArgAcrossCall(s32 a) {
    return CalleeOne(a) + a;
}
)");
}

// -----------------------------------------------------------------------------
// 7. Global Variable Reads & Writes
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, GlobalReadS32) {
  AssertRoundTripBitExact("ReadGlobalS32", R"(
extern s32 gGlobalS32;

s32 ReadGlobalS32(void) {
    return gGlobalS32;
}
)");
}

TEST_F(RoundTripTest, GlobalWriteS32) {
  AssertRoundTripBitExact("WriteGlobalS32", R"(
extern s32 gGlobalS32;

void WriteGlobalS32(s32 v) {
    gGlobalS32 = v;
}
)");
}

TEST_F(RoundTripTest, GlobalReadU16) {
  AssertRoundTripBitExact("ReadGlobalU16", R"(
extern u16 gGlobalU16;

u32 ReadGlobalU16(void) {
    return gGlobalU16;
}
)");
}

TEST_F(RoundTripTest, GlobalWriteS8) {
  AssertRoundTripBitExact("WriteGlobalS8", R"(
extern s8 gGlobalS8;

void WriteGlobalS8(s8 v) {
    gGlobalS8 = v;
}
)");
}

// -----------------------------------------------------------------------------
// 8. Floating-Point Arithmetic & Conversions
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, FloatAdd) {
  AssertRoundTripBitExact("AddFloat", R"(
f32 AddFloat(f32 a, f32 b) {
    return a + b;
}
)");
}

TEST_F(RoundTripTest, FloatSub) {
  AssertRoundTripBitExact("SubFloat", R"(
f32 SubFloat(f32 a, f32 b) {
    return a - b;
}
)");
}

TEST_F(RoundTripTest, FloatMul) {
  AssertRoundTripBitExact("MulFloat", R"(
f32 MulFloat(f32 a, f32 b) {
    return a * b;
}
)");
}

TEST_F(RoundTripTest, FloatDiv) {
  AssertRoundTripBitExact("DivFloat", R"(
f32 DivFloat(f32 a, f32 b) {
    return a / b;
}
)");
}

TEST_F(RoundTripTest, FloatNeg) {
  AssertRoundTripBitExact("NegFloat", R"(
f32 NegFloat(f32 a) {
    return -a;
}
)");
}

TEST_F(RoundTripTest, DoubleAdd) {
  AssertRoundTripBitExact("AddDouble", R"(
f64 AddDouble(f64 a, f64 b) {
    return a + b;
}
)");
}

TEST_F(RoundTripTest, DoubleSub) {
  AssertRoundTripBitExact("SubDouble", R"(
f64 SubDouble(f64 a, f64 b) {
    return a - b;
}
)");
}

TEST_F(RoundTripTest, DoubleMul) {
  AssertRoundTripBitExact("MulDouble", R"(
f64 MulDouble(f64 a, f64 b) {
    return a * b;
}
)");
}

TEST_F(RoundTripTest, FloatConvertIntToFloat) {
  AssertRoundTripBitExact("ConvertIntToFloat", R"(
f32 ConvertIntToFloat(s32 a) {
    return (f32)a;
}
)");
}

TEST_F(RoundTripTest, FloatConvertFloatToInt) {
  AssertRoundTripBitExact("ConvertFloatToInt", R"(
s32 ConvertFloatToInt(f32 a) {
    return (s32)a;
}
)");
}

TEST_F(RoundTripTest, FloatConvertFloatToDouble) {
  AssertRoundTripBitExact("ConvertFloatToDouble", R"(
f64 ConvertFloatToDouble(f32 a) {
    return (f64)a;
}
)");
}

TEST_F(RoundTripTest, FloatConvertDoubleToFloat) {
  AssertRoundTripBitExact("ConvertDoubleToFloat", R"(
f32 ConvertDoubleToFloat(f64 a) {
    return (f32)a;
}
)");
}

}  // namespace
}  // namespace rom_nom_nom
