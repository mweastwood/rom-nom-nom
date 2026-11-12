#include "lifter/invert_gas_macro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_mips_ip.h"
#include "lifter/pass00_final_and_gas.h"

namespace rom_nom_nom {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

// Production `SubfunctionInverseProvider` wired directly to `InvertGasMipsIp`,
// `InvertGasMacro`, `InvertGasAppendInsn`, and `InvertFinalAndGasFunction`
// with zero dependence on `ToySubfunctionInverses`.
class ProductionGasMacroInverseProvider : public SubfunctionInverseProvider {
 public:
  std::string InvertGasMipsIp(uint32_t word_x) const override {
    return ::rom_nom_nom::InvertGasMipsIp(word_x);
  }

  std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const override {
    return ::rom_nom_nom::InvertGasMacro(macro_words_x);
  }

  std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) const override {
    return ::rom_nom_nom::InvertGasAppendInsn(words_x);
  }

  std::vector<std::string> InvertGasAppendInsn(
      absl::Span<const uint32_t> words_x, absl::Span<const std::string> c_flags) const override {
    return ::rom_nom_nom::InvertGasAppendInsn(words_x, c_flags);
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) const override {
    return ::rom_nom_nom::lifter::InvertFinalAndGasFunction(input_x);
  }
};

TEST(InvertGasMacroTest, SyntheticMacroCategoriesRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionGasMacroInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  const std::vector<std::string> macro_asm_inputs = {
      // 1. `M_LI` (1-word signed 16-bit, 1-word unsigned 16-bit, 1-word upper 16-bit, 2-word
      // 32-bit):
      "li $2,0",
      "li $2,42",
      "li $2,-123",
      "li $2,0x8000",
      "li $2,0xffff",
      "li $2,0x12340000",
      "li $2,0x80000000",
      "li $2,0x12345678",
      "li $16,0x80012345",
      // 2. 32-bit immediate arithmetic/logical/comparison (`M_ADD_I`, `M_ADDU_I`, `M_DADD_I`,
      // `M_DADDU_I`, `M_SUB_I`, `M_SUBU_I`, `M_DSUB_I`, `M_DSUBU_I`, `M_AND_I`, `M_OR_I`,
      // `M_XOR_I`, `M_NOR_I`, `M_SLT_I`, `M_SLTU_I`, `M_SGT_I`, `M_SGTU_I`):
      "add $2,$4,0x12340000",
      "add $2,$4,0x12345678",
      "addu $2,$4,0x12340000",
      "addu $2,$4,0x12345678",
      "dadd $2,$4,0x12340000",
      "dadd $2,$4,0x12345678",
      "daddu $2,$4,0x12340000",
      "daddu $2,$4,0x12345678",
      "sub $2,$4,0x12340000",
      "sub $2,$4,0x12345678",
      "subu $2,$4,0x12340000",
      "subu $2,$4,0x12345678",
      "dsub $2,$4,0x12340000",
      "dsub $2,$4,0x12345678",
      "dsubu $2,$4,0x12340000",
      "dsubu $2,$4,0x12345678",
      "and $2,$4,0x12340000",
      "and $2,$4,0x12345678",
      "or $2,$4,0x12340000",
      "or $2,$4,0x12345678",
      "xor $2,$4,0x12340000",
      "xor $2,$4,0x12345678",
      "nor $2,$4,0x00ff",
      "nor $2,$4,0x12340000",
      "nor $2,$4,0x12345678",
      "slt $2,$4,0x12340000",
      "slt $2,$4,0x12345678",
      "sltu $2,$4,0x12340000",
      "sltu $2,$4,0x12345678",
      "sgt $2,$4,0x12340000",
      "sgt $2,$4,0x12345678",
      "sgtu $2,$4,0x12340000",
      "sgtu $2,$4,0x12345678",
      // 3. `M_ABS` / `M_DABS` (`dreg == sreg` and `dreg != sreg`):
      "abs $2,$2",
      "abs $2,$4",
      "dabs $2,$2",
      "dabs $2,$4",
      // 4. `M_MUL` / `M_DMUL` / `M_MUL_I` / `M_DMUL_I` / `M_MULO` / `M_DMULO` / `M_MULOU` /
      // `M_DMULOU`:
      "mul $2,$4,$5",
      "dmul $2,$4,$5",
      "mul $2,$4,7",
      "mul $2,$4,0x12345678",
      "dmul $2,$4,7",
      "dmul $2,$4,0x12345678",
      "mulo $2,$4,$5",
      "dmulo $2,$4,$5",
      "mulou $2,$4,$5",
      "dmulou $2,$4,$5",
      // 5. `M_DIV_3` / `M_DIVU_3` / `M_REM_3` / `M_REMU_3` / `M_DDIV_3` / `M_DDIVU_3` /
      // `M_DREM_3` / `M_DREMU_3`:
      "div $2,$4,$5",
      "rem $2,$4,$5",
      "divu $2,$4,$5",
      "remu $2,$4,$5",
      "ddiv $2,$4,$5",
      "drem $2,$4,$5",
      "ddivu $2,$4,$5",
      "dremu $2,$4,$5",
      // 6. `M_ROL` / `M_ROR` / `M_ROL_I` / `M_ROR_I`:
      "rol $2,$4,$5",
      "ror $2,$4,$5",
      "rol $2,$4,7",
      "ror $2,$4,7",
      // 7. `M_SEQ` / `M_SNE` / `M_SEQ_I` / `M_SNE_I` / `M_SGE` / `M_SGEU` / `M_SGE_I` /
      // `M_SGEU_I` / `M_SLE_I` / `M_SLEU_I`:
      "seq $2,$4,$5",
      "sne $2,$4,$5",
      "seq $2,$4,10",
      "sne $2,$4,10",
      "seq $2,$4,-10",
      "sne $2,$4,-10",
      "seq $2,$4,0x12340000",
      "sne $2,$4,0x12345678",
      "sge $2,$4,$5",
      "sgeu $2,$4,$5",
      "sge $2,$4,1",
      "sgeu $2,$4,1",
      "sge $2,$4,0x8000",
      "sgeu $2,$4,0x12345678",
      "sle $2,$4,10",
      "sleu $2,$4,0x12345678",
      // 8. Unaligned loads and stores (`M_ULW`, `M_ULD`, `M_ULH`, `M_ULHU`, `M_USW`, `M_USD`,
      // `M_USH`):
      "ulw $2,16($4)",
      "uld $2,16($4)",
      "ulh $2,16($4)",
      "ulhu $2,16($4)",
      "usw $2,16($4)",
      "usd $2,16($4)",
      "ush $2,16($4)",
  };

  for (const std::string& asm_line : macro_asm_inputs) {
    const std::vector<uint32_t> words_x = hooks.RunGasMacro(asm_line);
    ASSERT_FALSE(words_x.empty()) << "RunGasMacro failed on: " << asm_line;
    const RightInverseResult res = verifier.VerifyGasMacroRightInverse(words_x);
    EXPECT_TRUE(res.matches) << "Failed on '" << asm_line << "': " << res.diff_detail;
  }
}

TEST(InvertGasMacroTest, Mips1TruncMacroRightInverseAndStreamCollapse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionGasMacroInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  CompilationProfile mips1_profile;
  mips1_profile.optimize_level = 2;
  mips1_profile.c_flags = {"-O2", "-mips1", "-Wa,-g"};
  hooks.ApplyCompilationProfile(mips1_profile);

  const std::vector<std::string> macro_lines = {"trunc.w.s $f0,$f12,$2", "trunc.w.d $f0,$f12,$6"};
  for (const std::string& macro_line : macro_lines) {
    const std::vector<uint32_t> words_x = hooks.RunGasMacro(macro_line);
    ASSERT_EQ(words_x.size(), 10u) << macro_line;
    const RightInverseResult res = verifier.VerifyGasMacroRightInverse(words_x);
    EXPECT_TRUE(res.matches) << "Failed on '" << macro_line << "': " << res.diff_detail;

    // Followed by a COP1 instruction (`mfc1`, where `InvertGasAppendInsnToItems` strips `w[9]`).
    const std::vector<uint32_t> cop1_stream =
        hooks.RunGasMdAssemble(macro_line + "\nmfc1 $2,$f0\njr $31\n");
    std::vector<lifter::AsmItem> cop1_items;
    ASSERT_TRUE(lifter::InvertGasAppendInsnToItems(cop1_stream, &cop1_items));
    EXPECT_EQ(lifter::CollapseGasMacrosAndFlattenLines(cop1_items),
              (std::vector<std::string>{macro_line, "mfc1 $2,$f0", "jr $31"}));

    // Followed by a non-COP1 ALU instruction (`addu`, where `w[9]` remains a trailing `nop` item).
    const std::vector<uint32_t> gpr_stream =
        hooks.RunGasMdAssemble(macro_line + "\naddu $3,$4,$5\njr $31\n");
    std::vector<lifter::AsmItem> gpr_items;
    ASSERT_TRUE(lifter::InvertGasAppendInsnToItems(gpr_stream, &gpr_items));
    EXPECT_EQ(lifter::CollapseGasMacrosAndFlattenLines(gpr_items),
              (std::vector<std::string>{macro_line, "addu $3,$4,$5", "jr $31"}));
  }
  hooks.ApplyCompilationProfile(CompilationProfile{});
}

TEST(InvertGasMacroTest, NegativeInvalidWordsAndNonMacroSequencesFailVerification) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionGasMacroInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. Empty span `{}` must return `""` and fail verification.
  EXPECT_EQ(InvertGasMacro({}), "");
  EXPECT_FALSE(verifier.VerifyGasMacroRightInverse({}).matches);

  // 2. Invalid/unrecognized 1-word encodings (reserved/unallocated opcodes).
  const std::vector<uint32_t> invalid_single_words = {
      0x7c000000u,  // Primary opcode 0x1f (unallocated)
      0x00000005u,  // SPECIAL funct 0x05 (unallocated)
      0x04070000u,  // REGIMM rt 0x07 (unallocated)
      0x43e00000u,  // COP0 invalid encoding
      0x47e00000u,  // COP1 invalid encoding
  };
  for (uint32_t bad_word : invalid_single_words) {
    const uint32_t span[1] = {bad_word};
    EXPECT_EQ(InvertGasMacro(span), "") << "Expected empty string for 0x" << std::hex << bad_word;
    EXPECT_FALSE(verifier.VerifyGasMacroRightInverse(span).matches)
        << "Expected verification failure for 0x" << std::hex << bad_word;
  }

  // 3. Multi-word non-macro sequences and sequences containing an invalid word.
  const std::vector<std::vector<uint32_t>> invalid_multi_words = {
      // Arbitrary 2-word non-macro sequence (`addu $2,$4,$5` + `subu $2,$4,$5`):
      {0x00851021u, 0x00851023u},
      // Mismatched registers in `lui` + `ori` (`lui $2, 0x1234` + `ori $3, $3, 0x5678`):
      {0x3c021234u, 0x34635678u},
      // `lui $2, 0x1234` + `ori $2, $2, 0` (`lo16 == 0` is 1-word `lui`, not 2-word `M_LI`):
      {0x3c021234u, 0x34420000u},
      // Mismatched offsets in `lwl` + `lwr` (`lwl $2, 0($4)` + `lwr $2, 4($4)` instead of +3):
      {0x88820000u, 0x98820004u},
      // Multi-word sequence containing an invalid instruction word:
      {0x3c021234u, 0x7c000000u},
      // Arbitrary 3-word non-macro sequence:
      {0x00851021u, 0x00851023u, 0x00851024u},
  };
  for (const auto& seq : invalid_multi_words) {
    EXPECT_EQ(InvertGasMacro(seq), "");
    EXPECT_FALSE(verifier.VerifyGasMacroRightInverse(seq).matches);
  }
}

TEST(InvertGasMacroTest, CollapseGasMacrosAndFlattenLinesStreamTests) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // 1. Collapsing multi-word `div`, `rem`, `divu`, `remu`, and 2-word `li`
  //    macros from `InvertGasAppendInsnToItems` streams.
  const std::vector<std::string> multi_word_macros = {
      "div $2,$4,$5", "rem $2,$4,$5", "divu $2,$4,$5", "remu $2,$4,$5", "li $2,0x12345678",
  };
  for (const std::string& macro_line : multi_word_macros) {
    const std::vector<uint32_t> words = hooks.RunGasMdAssemble(macro_line + "\njr $31\n");
    ASSERT_GT(words.size(), 2u) << macro_line;
    std::vector<AsmItem> items;
    ASSERT_TRUE(InvertGasAppendInsnToItems(words, &items)) << macro_line;
    const std::vector<std::string> collapsed = CollapseGasMacrosAndFlattenLines(items);
    const std::vector<std::string> expected = {macro_line, "jr $31"};
    EXPECT_EQ(collapsed, expected) << "Failed collapsing: " << macro_line;
  }

  // 2. Symbolic `la` (`lui` + `addiu`), absolute & indexed symbolic loads/stores
  //    (`lui` + `lw`/`sw`), and paired `l.d`/`s.d` (`lwc1`/`swc1` pairs).
  {
    const std::vector<AsmItem> sym_items = {
        // `la $2,__sym_0+16`:
        {.kind = AsmItemKind::kReorderInsn, .line = "lui $2,0x0"},
        {.kind = AsmItemKind::kReorderInsn, .line = "addiu $2,$2,16"},
        // Absolute `lw $3,__sym_0`:
        {.kind = AsmItemKind::kReorderInsn, .line = "lui $3,0x0"},
        {.kind = AsmItemKind::kReorderInsn, .line = "lw $3,0($3)"},
        // Absolute `sw $4,__sym_0-4`:
        {.kind = AsmItemKind::kReorderInsn, .line = "lui $1,0x0"},
        {.kind = AsmItemKind::kReorderInsn, .line = "sw $4,-4($1)"},
        // Indexed `lw $5,__sym_0+8($6)`:
        {.kind = AsmItemKind::kReorderInsn, .line = "lui $1,0x0"},
        {.kind = AsmItemKind::kReorderInsn, .line = "addu $1,$1,$6"},
        {.kind = AsmItemKind::kReorderInsn, .line = "lw $5,8($1)"},
        // Absolute paired `l.d $f0,__sym_0` (GAS uses `$1` as `tempreg` for FPRs):
        {.kind = AsmItemKind::kReorderInsn, .line = "lui $1,0x0"},
        {.kind = AsmItemKind::kReorderInsn, .line = "lwc1 $f0,0($1)"},
        {.kind = AsmItemKind::kReorderInsn, .line = "lwc1 $f1,4($1)"},
        // Indexed paired `s.d $f2,__sym_0+16($4)`:
        {.kind = AsmItemKind::kReorderInsn, .line = "lui $1,0x0"},
        {.kind = AsmItemKind::kReorderInsn, .line = "addu $1,$1,$4"},
        {.kind = AsmItemKind::kReorderInsn, .line = "swc1 $f2,16($1)"},
        {.kind = AsmItemKind::kReorderInsn, .line = "swc1 $f3,20($1)"},
    };
    const std::vector<std::string> lines = CollapseGasMacrosAndFlattenLines(sym_items);
    const std::vector<std::string> expected = {
        "la $2,__sym_0+16",    "lw $3,__sym_0",   "sw $4,__sym_0-4",
        "lw $5,__sym_0+8($6)", "l.d $f0,__sym_0", "s.d $f2,__sym_0+16($4)",
    };
    EXPECT_EQ(lines, expected);
  }

  // 3. `jal 0x0` -> `jal __callee_0` while leaving non-zero `jal 0x100`
  //    untouched so it fails downstream; also test negative non-zero `lui`
  //    (not collapsed into `la`).
  {
    const std::vector<AsmItem> jal_and_non_macro_items = {
        {.kind = AsmItemKind::kReorderInsn, .line = "jal 0x0", .word = 0x0c000000u},
        {.kind = AsmItemKind::kReorderInsn, .line = "jal 0x100", .word = 0x0c000040u},
        {.kind = AsmItemKind::kReorderInsn, .line = "lui $2,0x1234", .word = 0x3c021234u},
        {.kind = AsmItemKind::kReorderInsn, .line = "addiu $2,$2,16", .word = 0x24420010u},
    };
    const std::vector<std::string> lines =
        CollapseGasMacrosAndFlattenLines(jal_and_non_macro_items);
    const std::vector<std::string> expected = {
        "jal __callee_0",
        "jal 0x100",
        "lui $2,0x1234",
        "addiu $2,$2,16",
    };
    EXPECT_EQ(lines, expected);
  }

  // 4. Flattening `.set noreorder` (`kNoreorderBranchSeq` with/without
  //    `delay_label`) and un-swapped branch items into assembly lines.
  {
    const std::vector<AsmItem> branch_items = {
        // `kNoreorderBranchSeq` with `jal 0x0` and `delay_label`:
        {.kind = AsmItemKind::kNoreorderBranchSeq,
         .line = "jal 0x0",
         .delay_line = "addiu $4,$0,1",
         .delay_label = "$L2:",
         .word = 0x0c000000u,
         .delay_word = 0x24040001u},
        // Un-swapped `[delay_item, branch_item]` without active hazard ->
        // flattened into `.set noreorder / jr $31 / addiu $2,$4,5 / .set reorder`:
        {.kind = AsmItemKind::kReorderInsn,
         .line = "addiu $2,$4,5",
         .word = 0x24820005u,
         .unswapped_from_branch = true,
         .preceded_by_active_hazard = false},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "jr $31",
         .word = 0x03e00008u,
         .unswapped_from_branch = true},
        // Un-swapped `[delay_item, branch_item]` preceded by active hazard ->
        // kept as reorder lines `[delay_line, branch_line]`:
        {.kind = AsmItemKind::kReorderInsn,
         .line = "addiu $2,$4,9",
         .word = 0x24820009u,
         .unswapped_from_branch = true,
         .preceded_by_active_hazard = true},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "jr $31",
         .word = 0x03e00008u,
         .unswapped_from_branch = true},
    };
    const std::vector<std::string> lines = CollapseGasMacrosAndFlattenLines(branch_items);
    const std::vector<std::string> expected = {
        ".set\tnoreorder", "jal __callee_0",  "$L2:",   "addiu $4,$0,1",
        ".set\treorder",   ".set\tnoreorder", "jr $31", "addiu $2,$4,5",
        ".set\treorder",   "addiu $2,$4,9",   "jr $31",
    };
    EXPECT_EQ(lines, expected);
  }

  // 4a. Relocated ELF `Stage0FunctionInput` recovery:
  //     - Distinct `jal` callees via relocations (`Foo` vs `Bar`)
  //     - Distinct globals with addends (`g_alpha+8`) and split `%hi`/`%lo` when
  //       `$tempreg` is reused by a second `%lo`
  {
    Stage0FunctionInput stage0;
    stage0.relocations = {
        {.word_index = 0, .kind = Stage0RelocKind::kMips26, .symbol_name = "Foo", .addend = 0},
        {.word_index = 1, .kind = Stage0RelocKind::kMips26, .symbol_name = "Bar", .addend = 0},
        {.word_index = 2,
         .kind = Stage0RelocKind::kMipsHi16,
         .symbol_name = "g_alpha",
         .addend = 8},
        {.word_index = 3,
         .kind = Stage0RelocKind::kMipsLo16,
         .symbol_name = "g_alpha",
         .addend = 8},
        // Split `%hi`/`%lo` where `$3` is reused for a second `%lo` load:
        {.word_index = 4, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "g_pair", .addend = 0},
        {.word_index = 5, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_pair", .addend = 0},
        {.word_index = 6, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_pair", .addend = 4},
    };
    const std::vector<AsmItem> reloc_items = {
        {.kind = AsmItemKind::kReorderInsn,
         .line = "jal __callee_0",
         .word = 0x0c000000u,
         .word_index = 0},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "jal __callee_0",
         .word = 0x0c000000u,
         .word_index = 1},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "lui $2,0x0",
         .word = 0x3c020000u,
         .word_index = 2},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "lw $2,8($2)",
         .word = 0x8c420008u,
         .word_index = 3},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "lui $3,0x0",
         .word = 0x3c030000u,
         .word_index = 4},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "lw $4,0($3)",
         .word = 0x8c640000u,
         .word_index = 5},
        {.kind = AsmItemKind::kReorderInsn,
         .line = "lw $5,4($3)",
         .word = 0x8c650004u,
         .word_index = 6},
    };
    const absl::flat_hash_map<size_t, std::string> word_to_label;
    std::vector<Stage0ResolvedJumpTable> resolved_jts;
    const std::vector<std::string> lines =
        CollapseGasMacrosAndFlattenLines(reloc_items, stage0, word_to_label, &resolved_jts);
    const std::vector<std::string> expected = {
        "jal Foo",
        "jal Bar",
        "lw $2,g_alpha+8",
        "lui $3,%hi(g_pair)",
        "lw $4,%lo(g_pair)($3)",
        "lw $5,%lo(g_pair+4)($3)",
    };
    EXPECT_EQ(lines, expected);
    EXPECT_TRUE(resolved_jts.empty());
  }

  // 4b. Linked N64 ROM `Stage0FunctionInput` (`base_vram != 0`, empty relocations):
  //     - Resolves `jal` via `vram_to_symbol` (`RomHelper`) or fallback `func_<08x>`
  {
    Stage0FunctionInput rom_stage0;
    rom_stage0.base_vram = 0x80040000u;
    rom_stage0.vram_to_symbol[0x80051234u] = "RomHelper";
    const std::vector<AsmItem> rom_items = {
        // Linked ROM `jal 0x80051234` (`target26 = (0x80051234 >> 2) & 0x03ffffff = 0x0001448d`):
        {.kind = AsmItemKind::kReorderInsn,
         .line = "jal 0x51234",
         .word = 0x0c01448du,
         .word_index = 0},
        // Linked ROM `jal 0x80060000` without symbol entry -> `func_80060000`:
        {.kind = AsmItemKind::kReorderInsn,
         .line = "jal 0x60000",
         .word = 0x0c018000u,
         .word_index = 1},
    };
    const absl::flat_hash_map<size_t, std::string> word_to_label;
    std::vector<Stage0ResolvedJumpTable> resolved_jts;
    const std::vector<std::string> lines =
        CollapseGasMacrosAndFlattenLines(rom_items, rom_stage0, word_to_label, &resolved_jts);
    const std::vector<std::string> expected = {
        "jal RomHelper",
        "jal func_80060000",
    };
    EXPECT_EQ(lines, expected);
  }
}

TEST(InvertGasMacroTest, LinkedRomReachingDefinitionsHiLoAndGpResolution) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionGasMacroInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1a. Hoisted `lui $16` (`$s0`) surviving across `jal` into two branches &
  //     shared across two globals on the same 64KB page (`g_alpha` at `0x80030010`,
  //     `g_beta` at `0x80030020`), plus `vram_to_symbol_size` struct field across
  //     a 64KB signed `%lo` carry boundary (`g_actor` at `0x80037ff8`, size 32 ->
  //     field `+16` at `0x80038008`: `%hi = 0x8004`, `%lo = -0x7ff8` = `0x8008`)
  //     and `.rdata` float constant-pool entry (`$LC0` at `0x8005fff8`),
  //     while caller-saved `lui $8` (`$t0`) BEFORE `jal` IS clobbered by `jal`
  //     and pure integer constants (`lui` + `ori` even at `0x80030010` == `&g_alpha`,
  //     standalone `lui 0x8003`, and non-symbol `lui` + `addiu`) produce ZERO
  //     bogus relocations!
  Stage0FunctionInput rom;
  rom.profile = {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
  rom.base_vram = 0x80020000u;
  rom.vram_to_symbol[0x80025000u] = "CalleeFn";
  rom.vram_to_symbol[0x80030000u] = "g_page_base";
  rom.vram_to_symbol[0x80030010u] = "g_alpha";
  rom.vram_to_symbol[0x80030020u] = "g_beta";
  rom.vram_to_symbol[0x80037ff8u] = "g_actor";
  rom.vram_to_symbol_size[0x80037ff8u] = 32u;
  rom.constant_pool.push_back(Stage0ConstantPoolEntry{
      .label_name = "",
      .section_name = ".rdata",
      .byte_size = 4,
      .words = {0x3f9e0652u},
      .entry_vram = 0x8005fff8u,
  });

  rom.machine_words = {
      0x27bdffe8u,  // 0: addiu $sp, $sp, -24
      0xafbf0014u,  // 1: sw $ra, 20($sp)
      0xafb00010u,  // 2: sw $s0, 16($sp)
      0x3c108003u,  // 3: lui $s0, 0x8003 (hoisted & shared across g_alpha and g_beta!)
      0x3c088003u,  // 4: lui $t0, 0x8003 (caller-saved $t0, clobbered by jal!)
      0x0c009400u,  // 5: jal 0x80025000 (CalleeFn)
      0x00000000u,  // 6: nop
      0x8d020010u,  // 7: lw $v0, 0x0010($t0) -> MUST NOT resolve ($t0 clobbered by jal!)
      0x3c038003u,  // 8: lui $v1, 0x8003     -> integer `li $v1, 0x80030010` (`ori`, NOT `%lo`!)
      0x34630010u,  // 9: ori $v1, $v1, 0x0010
      0x3c098003u,  // 10: lui $t1, 0x8003    -> standalone `lui` with no `%lo` consumer!
      0x3c0a1234u,  // 11: lui $t2, 0x1234    -> non-symbol `lui` + `addiu` (`0x12340008`)
      0x254b0008u,  // 12: addiu $t3, $t2, 8
      0x10800003u,  // 13: beq $a0, $zero, word 17
      0x8e040010u,  // 14: delay slot: lw $a0, 0x0010($s0) -> g_alpha!
      0x3c028004u,  // 15: lui $v0, 0x8004 -> g_actor+16 (`0x80038008`)
      0x8c428008u,  // 16: lw $v0, -0x7ff8($v0) -> g_actor+16!
      0x8e050020u,  // 17: target: lw $a1, 0x0020($s0) -> g_beta (sharing lui at word 3!)
      0x3c018006u,  // 18: lui $at, 0x8006 -> %hi($LC0) (`0x8005fff8`)
      0xc420fff8u,  // 19: lwc1 $f0, -8($at) -> %lo($LC0)!
      0x8fbf0014u,  // 20: lw $ra, 20($sp)
      0x8fb00010u,  // 21: lw $s0, 16($sp)
      0x27bd0018u,  // 22: addiu $sp, $sp, 24
      0x03e00008u,  // 23: jr $ra
      0x00000000u,  // 24: nop
  };

  std::vector<Stage0JumpTable> norm_jts;
  std::vector<Stage0ConstantPoolEntry> norm_cps;
  const std::vector<Stage0Relocation> relocs =
      SynthesizeLinkedRomRelocations(rom, &norm_jts, &norm_cps);
  ASSERT_EQ(norm_cps.size(), 1u);
  EXPECT_EQ(norm_cps[0].label_name, "$LC0");

  const std::vector<Stage0Relocation> expected_relocs = {
      {.word_index = 3, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "g_alpha", .addend = 0},
      {.word_index = 14, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_alpha", .addend = 0},
      {.word_index = 15,
       .kind = Stage0RelocKind::kMipsHi16,
       .symbol_name = "g_actor",
       .addend = 16},
      {.word_index = 16,
       .kind = Stage0RelocKind::kMipsLo16,
       .symbol_name = "g_actor",
       .addend = 16},
      {.word_index = 17, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_beta", .addend = 0},
      {.word_index = 18, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "$LC0", .addend = 0},
      {.word_index = 19, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "$LC0", .addend = 0},
  };
  EXPECT_EQ(relocs, expected_relocs);
  const RightInverseResult rom_res = verifier.VerifyFinalAndGasStageRightInverse(rom);
  EXPECT_TRUE(rom_res.matches) << rom_res.diff_detail;

  // 1b. Unit check for external `-G 8` small-data (`gp_vram`) `kMipsGpRel16` and
  //     `.lit4` `kMipsLiteral` relocation synthesis in `SynthesizeLinkedRomRelocations`:
  {
    Stage0FunctionInput gp_unit_rom;
    gp_unit_rom.base_vram = 0x80020000u;
    gp_unit_rom.gp_vram = 0x80060000u;
    gp_unit_rom.vram_to_symbol[0x8005fff0u] = "g_sdata_var";  // gp_vram - 16
    gp_unit_rom.constant_pool.push_back(Stage0ConstantPoolEntry{
        .label_name = "",
        .section_name = ".lit4",
        .byte_size = 4,
        .words = {0x3f9e0652u},
        .entry_vram = 0x8005fff8u,  // gp_vram - 8
    });
    gp_unit_rom.machine_words = {
        0x8f86fff0u,  // 0: lw $a2, -16($gp) -> kMipsGpRel16 g_sdata_var
        0xc780fff8u,  // 1: lwc1 $f0, -8($gp) -> kMipsLiteral .Llit0
        0x03e00008u,  // 2: jr $ra
        0x00000000u,  // 3: nop
    };
    std::vector<Stage0JumpTable> gp_jts;
    std::vector<Stage0ConstantPoolEntry> gp_cps;
    const std::vector<Stage0Relocation> gp_relocs =
        SynthesizeLinkedRomRelocations(gp_unit_rom, &gp_jts, &gp_cps);
    ASSERT_EQ(gp_cps.size(), 1u);
    EXPECT_EQ(gp_cps[0].label_name, ".Llit0");
    const std::vector<Stage0Relocation> expected_gp_relocs = {
        {.word_index = 0,
         .kind = Stage0RelocKind::kMipsGpRel16,
         .symbol_name = "g_sdata_var",
         .addend = 0},
        {.word_index = 1,
         .kind = Stage0RelocKind::kMipsLiteral,
         .symbol_name = ".Llit0",
         .addend = 0},
    };
    EXPECT_EQ(gp_relocs, expected_gp_relocs);
  }

  // 2a. Branch-likely (`beql`) partial-availability soundness: if the fallthrough
  //     path (`word 2`, where the delay-slot `lui $t0, 0x8003` was annulled!) falls
  //     straight through into `word 3` without defining `$t0` or returning, `$t0` is
  //     NOT a valid `%hi` pointer along all paths reaching `word 3`, so NEITHER `word 2`
  //     NOR `word 3` may synthesize a bogus `%lo` relocation!
  Stage0FunctionInput beql_partial_rom;
  beql_partial_rom.base_vram = 0x80020000u;
  beql_partial_rom.vram_to_symbol[0x80030010u] = "g_alpha";
  beql_partial_rom.machine_words = {
      0x50800002u,  // 0: beql $a0, $zero, word 3
      0x3c088003u,  // 1: delay slot (annulled on fallthrough!): lui $t0, 0x8003
      0x8d020010u,  // 2: fallthrough: lw $v0, 0x0010($t0) (falls into word 3!)
      0x8d030010u,  // 3: merge point: lw $v1, 0x0010($t0) -> unsafe ($t0 uninit on fallthrough)!
      0x03e00008u,  // 4: jr $ra
      0x00000000u,  // 5: nop
  };
  EXPECT_TRUE(SynthesizeLinkedRomRelocations(beql_partial_rom).empty());

  // 2b. Branch-likely (`beql`) annulled delay slot when the fallthrough block returns:
  //     `lui $t0, 0x8003` in the delay slot of `beql` reaches ONLY the taken target
  //     (`word 5`), NEVER the annulled fallthrough (`word 2`)!
  Stage0FunctionInput beql_rom;
  beql_rom.profile = {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
  beql_rom.base_vram = 0x80020000u;
  beql_rom.vram_to_symbol[0x80030010u] = "g_alpha";
  beql_rom.machine_words = {
      0x50800004u,  // 0: beql $a0, $zero, word 5
      0x3c088003u,  // 1: delay slot (annulled on fallthrough!): lui $t0, 0x8003
      0x8d020010u,  // 2: fallthrough: lw $v0, 0x0010($t0) -> MUST NOT resolve!
      0x03e00008u,  // 3: jr $ra (terminates fallthrough path!)
      0x00000000u,  // 4: nop
      0x8d030010u,  // 5: taken target: lw $v1, 0x0010($t0) -> RESOLVES to g_alpha!
      0x03e00008u,  // 6: jr $ra
      0x00000000u,  // 7: nop
  };
  const std::vector<Stage0Relocation> beql_relocs = SynthesizeLinkedRomRelocations(beql_rom);
  const std::vector<Stage0Relocation> expected_beql_relocs = {
      {.word_index = 1, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "g_alpha", .addend = 0},
      {.word_index = 5, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_alpha", .addend = 0},
  };
  EXPECT_EQ(beql_relocs, expected_beql_relocs);
  const RightInverseResult beql_res = verifier.VerifyFinalAndGasStageRightInverse(beql_rom);
  EXPECT_TRUE(beql_res.matches) << beql_res.diff_detail;

  // 3. Multi-switch + early function-pointer tail-call (`jr $t9`) `jr` provenance
  //    disambiguation: neither `jr $t9` (word 4) nor Switch 1 (`jr $v0`, word 10)
  //    may broadcast `$t1.has_non_hi_def = true` into Switch 2's case blocks
  //    (words 23 and 26) after `lui $t1, %hi(g_foo)` is hoisted at word 15!
  Stage0FunctionInput multi_jr_rom;
  multi_jr_rom.profile = {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
  multi_jr_rom.base_vram = 0x80020000u;
  multi_jr_rom.vram_to_symbol[0x80030010u] = "g_foo";
  multi_jr_rom.vram_to_symbol[0x80030020u] = "g_fn_ptr";
  multi_jr_rom.jump_tables = {
      Stage0JumpTable{
          .label_name = "",
          .target_word_indices = {12u, 14u},
          .table_vram = 0x80040100u,
      },
      Stage0JumpTable{
          .label_name = "",
          .target_word_indices = {23u, 26u},
          .table_vram = 0x80040200u,
      },
  };
  multi_jr_rom.machine_words = {
      0x10800005u,  // 0: beq $a0, $zero, word 6
      0x00000000u,  // 1: nop
      0x3c198003u,  // 2: lui $t9, 0x8003 -> %hi(g_fn_ptr)
      0x8f390020u,  // 3: lw $t9, 0x0020($t9) -> %lo(g_fn_ptr)
      0x03200008u,  // 4: jr $t9 (untainted function-pointer tail call!)
      0x00000000u,  // 5: nop
      0x00051080u,  // 6: Switch 1: sll $v0, $a1, 2
      0x3c018004u,  // 7: lui $at, 0x8004 -> %hi(__jt_0)
      0x00220821u,  // 8: addu $at, $at, $v0
      0x8c220100u,  // 9: lw $v0, 0x0100($at) -> %lo(__jt_0)
      0x00400008u,  // 10: jr $v0 (tainted ONLY with jt_0 -> {12, 14}!)
      0x00000000u,  // 11: nop
      0x0800800fu,  // 12: Switch 1 case 0: j word 15 (0x8002003c)
      0x24c60001u,  // 13: delay slot: addiu $a2, $a2, 1
      0x24c60002u,  // 14: Switch 1 case 1: addiu $a2, $a2, 2
      0x3c098003u,  // 15: Hoisted ahead of Switch 2: lui $t1, 0x8003 -> %hi(g_foo)!
      0x3c0a8004u,  // 16: lui $t2, 0x8004 -> %hi(__jt_1)
      0x254a0200u,  // 17: addiu $t2, $t2, 0x0200 -> %lo(__jt_1)
      0x00061880u,  // 18: sll $v1, $a2, 2
      0x006a1821u,  // 19: addu $v1, $v1, $t2
      0x8c630000u,  // 20: lw $v1, 0($v1)
      0x00600008u,  // 21: jr $v1 (tainted ONLY with jt_1 -> {23, 26}!)
      0x00000000u,  // 22: nop
      0x8d220010u,  // 23: Switch 2 case 0: lw $v0, 0x0010($t1) -> MUST resolve to g_foo!
      0x03e00008u,  // 24: jr $ra
      0x00000000u,  // 25: nop
      0xad260010u,  // 26: Switch 2 case 1: sw $a2, 0x0010($t1) -> MUST resolve to g_foo!
      0x03e00008u,  // 27: jr $ra
      0x00000000u,  // 28: nop
  };
  const std::vector<Stage0Relocation> multi_jr_relocs =
      SynthesizeLinkedRomRelocations(multi_jr_rom);
  const std::vector<Stage0Relocation> expected_multi_jr_relocs = {
      {.word_index = 2, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "g_fn_ptr", .addend = 0},
      {.word_index = 3, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_fn_ptr", .addend = 0},
      {.word_index = 7, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "__jt_0", .addend = 0},
      {.word_index = 9, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "__jt_0", .addend = 0},
      {.word_index = 15, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "g_foo", .addend = 0},
      {.word_index = 16, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "__jt_1", .addend = 0},
      {.word_index = 17, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "__jt_1", .addend = 0},
      {.word_index = 23, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_foo", .addend = 0},
      {.word_index = 26, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_foo", .addend = 0},
  };
  EXPECT_EQ(multi_jr_relocs, expected_multi_jr_relocs);
  const RightInverseResult multi_jr_res = verifier.VerifyFinalAndGasStageRightInverse(multi_jr_rom);
  EXPECT_TRUE(multi_jr_res.matches) << multi_jr_res.diff_detail;

  // 4. `jal` delay-slot `%lo` consuming caller-saved `$t0` right before `jal` clobbers `$t0`:
  //    the delay slot (`word 4`) executes BEFORE the callee and MUST resolve `g_alpha`, while
  //    the post-call load (`word 5`) sees `$t0` clobbered and MUST NOT resolve!
  Stage0FunctionInput jal_delay_rom;
  jal_delay_rom.profile = {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
  jal_delay_rom.base_vram = 0x80020000u;
  jal_delay_rom.vram_to_symbol[0x80025000u] = "CalleeFn";
  jal_delay_rom.vram_to_symbol[0x80030010u] = "g_alpha";
  jal_delay_rom.machine_words = {
      0x27bdffe8u,  // 0: addiu $sp, $sp, -24
      0xafbf0010u,  // 1: sw $ra, 16($sp)
      0x3c088003u,  // 2: lui $t0, 0x8003
      0x0c009400u,  // 3: jal 0x80025000 (CalleeFn)
      0x8d040010u,  // 4: delay slot: lw $a0, 0x0010($t0) -> RESOLVES to g_alpha!
      0x8d020010u,  // 5: post-call: lw $v0, 0x0010($t0) -> MUST NOT resolve ($t0 clobbered)!
      0x8fbf0010u,  // 6: lw $ra, 16($sp)
      0x27bd0018u,  // 7: addiu $sp, $sp, 24
      0x03e00008u,  // 8: jr $ra
      0x00000000u,  // 9: nop
  };
  const std::vector<Stage0Relocation> jal_delay_relocs =
      SynthesizeLinkedRomRelocations(jal_delay_rom);
  const std::vector<Stage0Relocation> expected_jal_delay_relocs = {
      {.word_index = 2, .kind = Stage0RelocKind::kMipsHi16, .symbol_name = "g_alpha", .addend = 0},
      {.word_index = 4, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_alpha", .addend = 0},
  };
  EXPECT_EQ(jal_delay_relocs, expected_jal_delay_relocs);
  const RightInverseResult jal_delay_res =
      verifier.VerifyFinalAndGasStageRightInverse(jal_delay_rom);
  EXPECT_TRUE(jal_delay_res.matches) << jal_delay_res.diff_detail;

  // 5. Shared `lui $s0, 0x8003` (`word 2`) feeding `g_struct + 16` (`0x80030000 + 16` at
  //    `word 3`) followed by `g_other + 0` (`0x80037ff8` at `word 4`): `R_MIPS_HI16` at
  //    `word 2` MUST pair with the immediately following `R_MIPS_LO16` (`word 3`:
  //    `("g_struct", 16)`), NEVER `("g_other", 0)` (which would carry `0x80037ff8 + 0x0010`
  //    onto page `0x8004` in Binutils 2.6 `elf32-mips.c`)!
  Stage0FunctionInput hi_order_rom;
  hi_order_rom.profile = {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
  hi_order_rom.base_vram = 0x80020000u;
  hi_order_rom.vram_to_symbol[0x80030000u] = "g_struct";
  hi_order_rom.vram_to_symbol_size[0x80030000u] = 32u;
  hi_order_rom.vram_to_symbol[0x80037ff8u] = "g_other";
  hi_order_rom.machine_words = {
      0x27bdfff8u,  // 0: addiu $sp, $sp, -8
      0xafb00000u,  // 1: sw $s0, 0($sp)
      0x3c108003u,  // 2: lui $s0, 0x8003
      0x8e020010u,  // 3: lw $v0, 0x0010($s0) -> g_struct + 16
      0x8e037ff8u,  // 4: lw $v1, 0x7ff8($s0) -> g_other + 0
      0x8fb00000u,  // 5: lw $s0, 0($sp)
      0x27bd0008u,  // 6: addiu $sp, $sp, 8
      0x03e00008u,  // 7: jr $ra
      0x00000000u,  // 8: nop
  };
  const std::vector<Stage0Relocation> hi_order_relocs =
      SynthesizeLinkedRomRelocations(hi_order_rom);
  const std::vector<Stage0Relocation> expected_hi_order_relocs = {
      {.word_index = 2,
       .kind = Stage0RelocKind::kMipsHi16,
       .symbol_name = "g_struct",
       .addend = 16},
      {.word_index = 3,
       .kind = Stage0RelocKind::kMipsLo16,
       .symbol_name = "g_struct",
       .addend = 16},
      {.word_index = 4, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "g_other", .addend = 0},
  };
  EXPECT_EQ(hi_order_relocs, expected_hi_order_relocs);
  const RightInverseResult hi_order_res = verifier.VerifyFinalAndGasStageRightInverse(hi_order_rom);
  EXPECT_TRUE(hi_order_res.matches) << hi_order_res.diff_detail;
}

class InvertGasMacroRoundTripCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionGasMacroInverseProvider();
    verifier = new RightInverseVerifier(ctx, hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
  static ProductionGasMacroInverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* InvertGasMacroRoundTripCorpusTest::ctx = nullptr;
FunctionHooks* InvertGasMacroRoundTripCorpusTest::hooks = nullptr;
ProductionGasMacroInverseProvider* InvertGasMacroRoundTripCorpusTest::provider = nullptr;
RightInverseVerifier* InvertGasMacroRoundTripCorpusTest::verifier = nullptr;

TEST_P(InvertGasMacroRoundTripCorpusTest, GasMacroBoundaryRoundTripsBitExact) {
  const RoundTripTestCase& tc = GetParam();
  const FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);

  bool found_macro_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kGasMacro) {
      continue;
    }
    found_macro_snap = true;
    EXPECT_FALSE(snap.post_machine_words.empty()) << tc.test_name;
    const RightInverseResult boundary_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(boundary_res.matches) << tc.test_name << ": " << boundary_res.diff_detail;
  }
  EXPECT_TRUE(found_macro_snap) << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, InvertGasMacroRoundTripCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
