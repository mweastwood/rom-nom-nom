#include "lifter/invert_gas_macro.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_mips_ip.h"

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
// `InvertGasMacro`, and `InvertGasAppendInsn` with zero dependence on
// `ToySubfunctionInverses`.
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
