#include "lifter/invert_function_prologue_epilogue.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "core/round_trip_test_cases.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

// Production `SubfunctionInverseProvider` wired directly to
// `InvertFunctionPrologueEpilogue` with zero dependence on
// `ToySubfunctionInverses`.
class ProductionPrologueEpilogueInverseProvider : public SubfunctionInverseProvider {
 public:
  PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view asm_text_x) const override {
    return ::rom_nom_nom::InvertFunctionPrologueEpilogue(asm_text_x);
  }
};

std::string ExtractInstructionOnlyLines(std::string_view asm_text) {
  std::vector<std::string> lines;
  for (std::string_view raw_line : absl::StrSplit(asm_text, '\n')) {
    const size_t hash_pos = raw_line.find('#');
    if (hash_pos != std::string_view::npos) {
      raw_line = raw_line.substr(0, hash_pos);
    }
    std::string_view line = absl::StripAsciiWhitespace(raw_line);
    if (line.empty() || line.front() == '.') {
      continue;
    }
    if (line.back() == ':' && line.find_first_of(" \t,()") == std::string_view::npos) {
      continue;
    }
    lines.emplace_back(line);
  }
  return absl::StrJoin(lines, "\n");
}

TEST(InvertFunctionPrologueEpilogueTest, SyntheticPositiveFrameConfigs) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionPrologueEpilogueInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  const std::vector<PrologueEpilogueFrameSpec> test_specs = {
      // 1. Leaf function with `var_size = 0, saved_gpr_mask = 0`:
      {.var_size = 0, .saved_gpr_mask = 0x00000000u},
      // 2. Stack-only leaf functions (`var_size = 8, 16, 24, 64, 256`, `mask = 0`):
      {.var_size = 8, .saved_gpr_mask = 0x00000000u},
      {.var_size = 16, .saved_gpr_mask = 0x00000000u},
      {.var_size = 24, .saved_gpr_mask = 0x00000000u},
      {.var_size = 64, .saved_gpr_mask = 0x00000000u},
      {.var_size = 256, .saved_gpr_mask = 0x00000000u},
      // 3. Non-leaf functions saving `$31` (`mask = 0x80000000u`, odd GPR count = 1):
      {.var_size = 0, .saved_gpr_mask = 0x80000000u},
      {.var_size = 8, .saved_gpr_mask = 0x80000000u},
      {.var_size = 16, .saved_gpr_mask = 0x80000000u},
      {.var_size = 32, .saved_gpr_mask = 0x80000000u},
      // 4. Non-leaf functions saving `$16 + $31` (`mask = 0x80010000u`, even GPR count = 2):
      {.var_size = 0, .saved_gpr_mask = 0x80010000u},
      {.var_size = 16, .saved_gpr_mask = 0x80010000u},
      {.var_size = 48, .saved_gpr_mask = 0x80010000u},
      // 5. Odd vs. even counts of saved GPRs (3, 4, 5, 6, 7, 8, 9, 10 regs):
      {.var_size = 0, .saved_gpr_mask = 0x80030000u},   // 3 regs ($16, $17, $31)
      {.var_size = 8, .saved_gpr_mask = 0x80070000u},   // 4 regs ($16..$18, $31)
      {.var_size = 16, .saved_gpr_mask = 0x800f0000u},  // 5 regs ($16..$19, $31)
      {.var_size = 24, .saved_gpr_mask = 0x801f0000u},  // 6 regs ($16..$20, $31)
      {.var_size = 32, .saved_gpr_mask = 0x803f0000u},  // 7 regs ($16..$21, $31)
      {.var_size = 40, .saved_gpr_mask = 0x807f0000u},  // 8 regs ($16..$22, $31)
      {.var_size = 48, .saved_gpr_mask = 0x80ff0000u},  // 9 regs ($16..$23, $31)
      // 6. All saveable callee-saved GPRs (`$16..$23 + $30 + $31`, 10 regs):
      {.var_size = 0, .saved_gpr_mask = 0xc0ff0000u},
      {.var_size = 64, .saved_gpr_mask = 0xc0ff0000u},
      // 7. Large stack frames (`var_size = 32768`, `var_size = 65536`) exercising
      //    `li $12`, `addu $13,$12,$sp`, and `addu $sp,$sp,$12`:
      {.var_size = 32768, .saved_gpr_mask = 0x00000000u},
      {.var_size = 32768, .saved_gpr_mask = 0x80000000u},
      {.var_size = 32768, .saved_gpr_mask = 0xc0ff0000u},
      {.var_size = 65536, .saved_gpr_mask = 0x00000000u},
      {.var_size = 65536, .saved_gpr_mask = 0x80030000u},
      {.var_size = 65536, .saved_gpr_mask = 0xc0ff0000u},
  };

  for (const PrologueEpilogueFrameSpec& expected : test_specs) {
    ctx.BeginFunction("harness_func");
    const std::string full_asm =
        hooks.RunFunctionPrologueEpilogue(expected.var_size, expected.saved_gpr_mask);

    // 1. Full assembly text (with `.ent`, `.frame`, `.mask`, `.fmask`, `.end`):
    const PrologueEpilogueFrameSpec recovered_full = InvertFunctionPrologueEpilogue(full_asm);
    EXPECT_EQ(recovered_full.var_size, expected.var_size) << full_asm;
    EXPECT_EQ(recovered_full.saved_gpr_mask, expected.saved_gpr_mask) << full_asm;

    const RightInverseResult full_res =
        verifier.VerifyFunctionPrologueEpilogueRightInverse(full_asm);
    EXPECT_TRUE(full_res.matches) << full_res.diff_detail << "\n" << full_asm;

    // 2. Instruction-only text (directives and function label stripped):
    const std::string insn_only_asm = ExtractInstructionOnlyLines(full_asm);
    ASSERT_FALSE(insn_only_asm.empty()) << full_asm;
    const PrologueEpilogueFrameSpec recovered_insns = InvertFunctionPrologueEpilogue(insn_only_asm);
    EXPECT_EQ(recovered_insns.var_size, expected.var_size) << insn_only_asm;
    EXPECT_EQ(recovered_insns.saved_gpr_mask, expected.saved_gpr_mask) << insn_only_asm;

    const RightInverseResult insn_res =
        verifier.VerifyFunctionPrologueEpilogueRightInverse(insn_only_asm);
    EXPECT_TRUE(insn_res.matches) << insn_res.diff_detail << "\n" << insn_only_asm;
  }
}

TEST(InvertFunctionPrologueEpilogueTest, SyntheticNoreorderAndProloguePrefix) {
  // 1. `.set noreorder` leaf epilogue with delay slot (`tsize == 0, mask == 0`):
  {
    constexpr std::string_view kNoreorderLeaf = R"(
	.ent	leaf_fn
leaf_fn:
	.frame	$sp,0,$31		# vars= 0, regs= 0/0, args= 0, extra= 0
	.mask	0x00000000,0
	.fmask	0x00000000,0
	.set	noreorder
	j	$31
	addu	$2,$4,$5
	.set	reorder
	.end	leaf_fn
)";
    const PrologueEpilogueFrameSpec spec = InvertFunctionPrologueEpilogue(kNoreorderLeaf);
    EXPECT_EQ(spec.var_size, 0);
    EXPECT_EQ(spec.saved_gpr_mask, 0u);
  }

  // 2. `.set noreorder` `$31`-only epilogue with delay slot (`var_size = 16, mask = 0x80000000`):
  {
    constexpr std::string_view kNoreorderR31 = R"(
	.ent	nonleaf_fn
nonleaf_fn:
	.frame	$sp,24,$31		# vars= 16, regs= 1/0, args= 0, extra= 0
	.mask	0x80000000,-8
	.fmask	0x00000000,0
	.set	noreorder
	lw	$31,16($sp)
	move	$2,$16
	j	$31
	addu	$sp,$sp,24
	.set	reorder
	.end	nonleaf_fn
)";
    const PrologueEpilogueFrameSpec spec = InvertFunctionPrologueEpilogue(kNoreorderR31);
    EXPECT_EQ(spec.var_size, 16);
    EXPECT_EQ(spec.saved_gpr_mask, 0x80000000u);
  }

  // 3. Combined prologue + epilogue instruction sequence (`subu $sp` + `sw` + `lw` + `addu $sp` +
  // `j $31`):
  {
    constexpr std::string_view kCombinedPrologueEpilogue = R"(
	subu	$sp,$sp,32
	sw	$31,28($sp)
	sw	$16,24($sp)
	lw	$31,28($sp)
	lw	$16,24($sp)
	addu	$sp,$sp,32
	j	$31
)";
    const PrologueEpilogueFrameSpec spec =
        InvertFunctionPrologueEpilogue(kCombinedPrologueEpilogue);
    EXPECT_EQ(spec.var_size, 24);
    EXPECT_EQ(spec.saved_gpr_mask, 0x80010000u);
  }
}

TEST(InvertFunctionPrologueEpilogueTest, SyntheticNegativeInputsRejected) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionPrologueEpilogueInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  const std::vector<std::string> invalid_inputs = {
      // 1. Empty or whitespace-only text:
      "",
      "   \t\n  \n ",
      "# comment only\n\t# another comment\n",
      // 2. Garbage directives or unknown instructions:
      "\t.bogus\t123\n\tj\t$31\n",
      "\tinvalid_mnemonic\t$2,$3\n\tj\t$31\n",
      "\tsubu\t$sp,$sp,16\n",  // Missing epilogue / `j $31`
      // 3. Unaligned `var_size` / `total_size`:
      "\t.frame\t$sp,12,$31\t\t# vars= 12, regs= 0/0, args= 0, extra= 0\n"
      "\t.mask\t0x00000000,0\n"
      "\taddu\t$sp,$sp,12\n"
      "\tj\t$31\n",
      "\taddu\t$sp,$sp,20\n\tj\t$31\n",
      // 4. Contradictory `.frame` vs `addu $sp,$sp,<tsize>`:
      "\t.frame\t$sp,16,$31\t\t# vars= 16, regs= 0/0, args= 0, extra= 0\n"
      "\t.mask\t0x00000000,0\n"
      "\taddu\t$sp,$sp,32\n"
      "\tj\t$31\n",
      // 5. Contradictory `.mask` vs `lw` registers:
      "\t.frame\t$sp,16,$31\t\t# vars= 8, regs= 1/0, args= 0, extra= 0\n"
      "\t.mask\t0x80000000,-8\n"
      "\tlw\t$16,8($sp)\n"
      "\taddu\t$sp,$sp,16\n"
      "\tj\t$31\n",
      // 6. Caller-saved registers in `.mask` or `lw`:
      "\t.frame\t$sp,16,$31\t\t# vars= 8, regs= 1/0, args= 0, extra= 0\n"
      "\t.mask\t0x00000004,-8\n"
      "\tlw\t$2,8($sp)\n"
      "\taddu\t$sp,$sp,16\n"
      "\tj\t$31\n",
      "\tlw\t$8,8($sp)\n\taddu\t$sp,$sp,16\n\tj\t$31\n",
      // 7. Wrong `lw` stack offsets (e.g. offset 0 instead of 8, or wrong `.mask` save offset):
      "\tlw\t$31,0($sp)\n\taddu\t$sp,$sp,16\n\tj\t$31\n",
      "\t.frame\t$sp,16,$31\t\t# vars= 8, regs= 1/0, args= 0, extra= 0\n"
      "\t.mask\t0x80000000,-4\n"
      "\tlw\t$31,8($sp)\n"
      "\taddu\t$sp,$sp,16\n"
      "\tj\t$31\n",
      // 8. Ascending instead of descending `lw` register order:
      "\tlw\t$16,12($sp)\n\tlw\t$31,8($sp)\n\taddu\t$sp,$sp,16\n\tj\t$31\n",
      // 9. Mismatched `.ent` and `.end` function names:
      "\t.ent\tfoo\nfoo:\n\tj\t$31\n\t.end\tbar\n",
  };

  for (const std::string& bad_asm : invalid_inputs) {
    const PrologueEpilogueFrameSpec spec = InvertFunctionPrologueEpilogue(bad_asm);
    EXPECT_LT(spec.var_size, 0) << "Expected rejection for:\n" << bad_asm;
    EXPECT_EQ(spec.saved_gpr_mask, 0u) << "Expected zero mask on rejection for:\n" << bad_asm;
    EXPECT_FALSE(verifier.VerifyFunctionPrologueEpilogueRightInverse(bad_asm).matches)
        << "Expected verifier failure for:\n"
        << bad_asm;
  }
}

TEST(InvertFunctionPrologueEpilogueTest, ExtractFunctionPrologueEpilogueStreamTests) {
  // 1. Leaf function with `tsize == 0`: `jr $31` is preserved in `asm_lines`,
  //    `frame_size_bytes = 0`, `saved_gpr_regnos` empty.
  {
    std::vector<std::string> asm_lines = {
        "addu $2,$4,$5",
        "jr $31",
    };
    RtlSidebandState sideband;
    EXPECT_FALSE(HasFramePointerMoveInPrologue(asm_lines));
    ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
    const std::vector<std::string> expected_lines = {
        "addu $2,$4,$5",
        "jr $31",
    };
    EXPECT_EQ(asm_lines, expected_lines);
    EXPECT_FALSE(sideband.frame_pointer_needed);
    EXPECT_EQ(sideband.frame_size_bytes, 0);
    EXPECT_TRUE(sideband.saved_gpr_regnos.empty());
    EXPECT_TRUE(sideband.saved_fpr_regnos.empty());
    EXPECT_FALSE(sideband.epilogue_delay_pattern.has_value());
  }

  // 2. Standard `tsize > 0` epilogue with saved GPRs (`$16`, `$31`) and FPRs
  //    (`$f20`): trailing epilogue lines are stripped from `asm_lines` and
  //    `sideband` is populated.
  {
    std::vector<std::string> asm_lines = {
        "addiu $29,$29,-32", "sw $31,20($29)", "sw $16,16($29)", "s.d $f20,24($29)",
        "addu $2,$4,$5",     "lw $31,20($29)", "lw $16,16($29)", "l.d $f20,24($29)",
        "addiu $29,$29,32",  "jr $31",
    };
    RtlSidebandState sideband;
    ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
    const std::vector<std::string> expected_lines = {
        "addiu $29,$29,-32", "sw $31,20($29)", "sw $16,16($29)",
        "s.d $f20,24($29)",  "addu $2,$4,$5",
    };
    EXPECT_EQ(asm_lines, expected_lines);
    EXPECT_FALSE(sideband.frame_pointer_needed);
    EXPECT_EQ(sideband.frame_size_bytes, 16);
    const std::vector<int> expected_gprs = {16, 31};
    EXPECT_EQ(sideband.saved_gpr_regnos, expected_gprs);
    const std::vector<int> expected_fprs = {52};  // 32 + 20 ($f20)
    EXPECT_EQ(sideband.saved_fpr_regnos, expected_fprs);
    EXPECT_FALSE(sideband.epilogue_delay_pattern.has_value());
  }

  // 3. Frame pointer (`move $30,$29` in prologue and `move $29,$30` in
  //    epilogue) setting `sideband.frame_pointer_needed = true`.
  {
    std::vector<std::string> asm_lines = {
        "addiu $29,$29,-24", "sw $31,20($29)", "sw $30,16($29)", "move $30,$29",
        "addu $2,$4,$5",     "move $29,$30",   "lw $31,20($29)", "lw $30,16($29)",
        "addiu $29,$29,24",  "jr $31",
    };
    RtlSidebandState sideband;
    EXPECT_TRUE(HasFramePointerMoveInPrologue(asm_lines));
    ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
    const std::vector<std::string> expected_lines = {
        "addiu $29,$29,-24", "sw $31,20($29)", "sw $30,16($29)", "move $30,$29", "addu $2,$4,$5",
    };
    EXPECT_EQ(asm_lines, expected_lines);
    EXPECT_TRUE(sideband.frame_pointer_needed);
    EXPECT_EQ(sideband.frame_size_bytes, 16);
    const std::vector<int> expected_gprs = {30, 31};
    EXPECT_EQ(sideband.saved_gpr_regnos, expected_gprs);
  }

  // 4. Epilogue delay slot (`lw $31,...`, `<delay_insn>`, `.set noreorder`,
  //    `jr $31`, `addiu $29,$29,<tsize>`, `.set reorder`) setting
  //    `sideband.epilogue_delay_pattern`.
  {
    std::vector<std::string> asm_lines = {
        "addiu $29,$29,-24", "sw $31,16($29)",  "jal __callee_0", "lw $31,16($29)",
        "addu $2,$16,$0",    ".set\tnoreorder", "jr $31",         "addiu $29,$29,24",
        ".set\treorder",
    };
    RtlSidebandState sideband;
    ASSERT_TRUE(TryExtractAndStripTrailingEpilogue(&asm_lines, &sideband));
    const std::vector<std::string> expected_lines = {
        "addiu $29,$29,-24",
        "sw $31,16($29)",
        "jal __callee_0",
    };
    EXPECT_EQ(asm_lines, expected_lines);
    EXPECT_EQ(sideband.frame_size_bytes, 16);
    const std::vector<int> expected_gprs = {31};
    EXPECT_EQ(sideband.saved_gpr_regnos, expected_gprs);
    EXPECT_EQ(sideband.epilogue_delay_pattern,
              "(set (reg:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 0 $0)))");
  }

  // 5. `or $30,$29,$0` / `addu $30,$29,$0` prologue FP moves and `or $29,$30,$0`
  //    epilogue FP restore, plus `outgoing_args_size == 24` when the function
  //    writes a 5th/6th call argument at `16($29)` and `20($29)` without reading
  //    from `[16, 24)` or taking `&sp + off`.
  {
    std::vector<std::string> asm_lines = {
        "addiu $29,$29,-32", "sw $31,28($29)", "sw $30,24($29)",   "or $30,$29,$0",
        "sw $4,16($29)",     "sw $5,20($29)",  "jal Foo",          "or $29,$30,$0",
        "lw $31,28($29)",    "lw $30,24($29)", "addiu $29,$29,32", "jr $31",
    };
    RtlSidebandState sideband;
    EXPECT_TRUE(HasFramePointerMoveInPrologue(asm_lines));
    ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
    EXPECT_TRUE(sideband.frame_pointer_needed);
    EXPECT_EQ(sideband.outgoing_args_size, 24);
    EXPECT_EQ(sideband.frame_size_bytes, 0);
    const std::vector<int> expected_gprs = {30, 31};
    EXPECT_EQ(sideband.saved_gpr_regnos, expected_gprs);
  }

  // 6. Local variable read at `16($29)` prevents mistaking local frame space
  //    for `outgoing_args_size > 16` (`outgoing_args_size == 16`, `frame_size_bytes == 8`).
  {
    std::vector<std::string> asm_lines = {
        "addiu $29,$29,-32", "sw $31,24($29)", "sw $4,16($29)",    "jal Foo",
        "lw $2,16($29)",     "lw $31,24($29)", "addiu $29,$29,32", "jr $31",
    };
    RtlSidebandState sideband;
    ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
    EXPECT_EQ(sideband.outgoing_args_size, 16);
    EXPECT_EQ(sideband.frame_size_bytes, 8);
  }

  // 7. Interior early-return multi-epilogue + non-trailing epilogue followed by
  //    an out-of-line loop block.
  {
    std::vector<std::string> asm_lines = {
        "addiu $29,$29,-24",
        "sw $31,16($29)",
        "beq $4,$0,$L2",
        "jal Foo",
        // Interior early-return epilogue:
        "lw $31,16($29)",
        "addiu $29,$29,24",
        "jr $31",
        "$L2:",
        "jal Bar",
        // Non-trailing main epilogue followed by out-of-line loop block `$L3`:
        "lw $31,16($29)",
        "addiu $29,$29,24",
        "jr $31",
        "$L3:",
        "addiu $4,$4,-1",
        "bne $4,$0,$L3",
        "j $L2",
    };
    RtlSidebandState sideband;
    ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
    EXPECT_EQ(sideband.outgoing_args_size, 16);
    EXPECT_EQ(sideband.frame_size_bytes, 0);
    const std::vector<int> expected_gprs = {31};
    EXPECT_EQ(sideband.saved_gpr_regnos, expected_gprs);
    const std::vector<std::string> expected_lines = {
        "addiu $29,$29,-24", "sw $31,16($29)", "beq $4,$0,$L2",
        "jal Foo",           "jr\t$31",        "$L2:",
        "jal Bar",           "jr\t$31",        "$L3:",
        "addiu $4,$4,-1",    "bne $4,$0,$L3",  "j $L2",
    };
    EXPECT_EQ(asm_lines, expected_lines);
  }
}

class InvertFunctionPrologueEpilogueRoundTripCorpusTest
    : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionPrologueEpilogueInverseProvider();
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
  static ProductionPrologueEpilogueInverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* InvertFunctionPrologueEpilogueRoundTripCorpusTest::ctx = nullptr;
FunctionHooks* InvertFunctionPrologueEpilogueRoundTripCorpusTest::hooks = nullptr;
ProductionPrologueEpilogueInverseProvider*
    InvertFunctionPrologueEpilogueRoundTripCorpusTest::provider = nullptr;
RightInverseVerifier* InvertFunctionPrologueEpilogueRoundTripCorpusTest::verifier = nullptr;

TEST_P(InvertFunctionPrologueEpilogueRoundTripCorpusTest,
       FunctionPrologueEpilogueBoundaryRoundTripsBitExact) {
  const RoundTripTestCase& tc = GetParam();
  const FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);

  bool found_prologue_epilogue_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kFunctionPrologueEpilogue) {
      continue;
    }
    found_prologue_epilogue_snap = true;
    EXPECT_FALSE(snap.post_asm_lines.empty()) << tc.test_name;
    const RightInverseResult boundary_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(boundary_res.matches) << tc.test_name << ": " << boundary_res.diff_detail;
  }
  EXPECT_TRUE(found_prologue_epilogue_snap) << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, InvertFunctionPrologueEpilogueRoundTripCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
