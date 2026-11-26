#include "lifter/invert_final_scan_insn.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/mips_multi_insn_idioms.h"

namespace rom_nom_nom::lifter {
namespace {

class ProductionFinalScanInsnInverseProvider final
    : public gcc_harness::SubfunctionInverseProvider {
 public:
  std::string InvertFinalScanInsn(std::string_view asm_line_x) const override {
    return lifter::InvertFinalScanInsn(asm_line_x);
  }

  RtlControlFlowGraph InvertFinalScanInsnStream(absl::Span<const std::string> asm_lines_x,
                                                const RtlSidebandState& sideband) const override {
    return lifter::InvertFinalScanInsnLinesToCfg(asm_lines_x, sideband);
  }
};

class InvertFinalScanInsnTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hooks_.ApplyCompilationProfile(
        CompilationProfile{.optimize_level = 2, .c_flags = {"-O2", "-mips3", "-Wa,-g"}});
  }

  gcc_harness::GccContext ctx_;
  gcc_harness::FunctionHooks hooks_{&ctx_};
  ProductionFinalScanInsnInverseProvider provider_;
  gcc_harness::RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_F(InvertFinalScanInsnTest, SyntheticPositiveIntegerAluAndShifts) {
  constexpr std::string_view kInstructions[] = {
      "nop",
      "move\t$2,$4",
      "move\t$2,$0",
      "li\t$2,0x0000002a",
      "li\t$2,-17",
      "addu\t$2,$4,$5",
      "addu\t$2,$4,16",
      "subu\t$2,$4,$5",
      "subu\t$2,$0,$4",
      "and\t$2,$4,$5",
      "andi\t$2,$4,0x00ff",
      "or\t$2,$4,$5",
      "ori\t$2,$4,0x1234",
      "xor\t$2,$4,$5",
      "xori\t$2,$4,0x0001",
      "nor\t$2,$0,$4",
      "nor\t$2,$4,$5",
      "slt\t$2,$4,$5",
      "slt\t$2,$4,10",
      "slt\t$2,$0,$4",
      "sltu\t$2,$4,$5",
      "sltu\t$2,$4,1",
      "sltu\t$2,$4,10",
      "sltu\t$2,$0,$4",
      "sll\t$2,$4,3",
      "sll\t$2,$4,$5",
      "sra\t$2,$4,5",
      "sra\t$2,$4,$5",
      "srl\t$2,$4,1",
      "srl\t$2,$4,$5",
      "dsll\t$2,$4,37",
      "dsrl\t$2,$4,37",
      "dsra\t$2,$4,37",
  };
  for (std::string_view insn : kInstructions) {
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(insn);
    EXPECT_TRUE(res.matches) << "insn: " << insn << " diff: " << res.diff_detail;
  }
  EXPECT_EQ(InvertFinalScanInsn("dsll32\t$2,$4,5"), InvertFinalScanInsn("dsll\t$2,$4,37"));
  EXPECT_EQ(InvertFinalScanInsn("dsrl32\t$2,$4,5"), InvertFinalScanInsn("dsrl\t$2,$4,37"));
  EXPECT_EQ(InvertFinalScanInsn("dsra32\t$2,$4,5"), InvertFinalScanInsn("dsra\t$2,$4,37"));
}

TEST_F(InvertFinalScanInsnTest, SyntheticPositiveMultiplyDivideAndHiLo) {
  constexpr std::string_view kInstructions[] = {
      "mult\t$4,$5",    "multu\t$4,$5",  "div\t$0,$4,$5",  "divu\t$0,$4,$5", "div\t$2,$4,$5",
      "divu\t$2,$4,$5", "rem\t$2,$4,$5", "remu\t$2,$4,$5", "mflo\t$2",       "mfhi\t$3",
      "mtlo\t$4",       "mthi\t$5",      "mtlo\t$0",
  };
  for (std::string_view insn : kInstructions) {
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(insn);
    EXPECT_TRUE(res.matches) << "insn: " << insn << " diff: " << res.diff_detail;
  }
}

TEST_F(InvertFinalScanInsnTest, SyntheticPositiveLoadsStoresAndAddresses) {
  constexpr std::string_view kInstructions[] = {
      "lw\t$2,0($4)",
      "lw\t$2,16($sp)",
      "lw\t$2,-8($fp)",
      "lw\t$2,ext_arr+4($4)",
      "lw\t$2,ext_global",
      "sw\t$4,0($5)",
      "sw\t$0,8($sp)",
      "lh\t$2,2($4)",
      "lhu\t$2,2($4)",
      "sh\t$4,2($5)",
      "lb\t$2,1($4)",
      "lbu\t$2,1($4)",
      "sb\t$4,1($5)",
      "ulw\t$2,1($4)",
      "usw\t$4,1($5)",
      "swr\t$2,3($4)",
      "lwl\t$2,0($5)\n\tlwr\t$2,3($5)\n\tswl\t$2,0($4)\n\tswr\t$2,3($4)",
      "lwl\t$2,0($5)\n\tlwr\t$2,3($5)\n\tswl\t$2,0($4)",
      "la\t$2,ext_global",
      "la\t$2,ext_arr+16",
      "la\t$2,$L2",
  };
  for (std::string_view insn : kInstructions) {
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(insn);
    EXPECT_TRUE(res.matches) << "insn: " << insn << " diff: " << res.diff_detail;
  }
  // Verify numeric aliases `$29`/`$30` also lift to identical stack/frame RTL.
  EXPECT_EQ(InvertFinalScanInsn("lw\t$2,16($29)"), InvertFinalScanInsn("lw\t$2,16($sp)"));
  EXPECT_EQ(InvertFinalScanInsn("lw\t$2,-8($30)"), InvertFinalScanInsn("lw\t$2,-8($fp)"));
}

TEST_F(InvertFinalScanInsnTest, SyntheticPositiveSplitHiLoInstructions) {
  constexpr std::string_view kInstructions[] = {
      "lui\t$16,%hi(g_foo)",       "lui\t$16,%hi(g_foo+16)",       "lui\t$16,%hi($L2)",
      "addiu\t$16,$16,%lo(g_foo)", "addiu\t$17,$16,%lo(g_foo+16)", "addiu\t$16,$16,%lo($L2)",
      "daddiu\t$16,$16,%lo($L2)",  "lw\t$2,%lo(g_foo+8)($16)",     "sw\t$4,%lo(g_foo)($16)",
      "lh\t$2,%lo(g_short)($16)",  "lhu\t$2,%lo(g_ushort)($16)",   "sh\t$4,%lo(g_short)($16)",
      "lb\t$2,%lo(g_byte)($16)",   "lbu\t$2,%lo(g_ubyte)($16)",    "sb\t$4,%lo(g_byte)($16)",
      "l.s\t$f0,%lo(g_flt)($16)",  "s.s\t$f0,%lo(g_flt)($16)",
  };
  for (std::string_view insn : kInstructions) {
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(insn);
    EXPECT_TRUE(res.matches) << "insn: " << insn << " diff: " << res.diff_detail;
  }
}

TEST_F(InvertFinalScanInsnTest, SyntheticPositiveFloatingPoint) {
  constexpr std::string_view kInstructions[] = {
      "l.s\t$f0,0($4)",
      "s.s\t$f0,4($4)",
      "l.d\t$f0,0($4)",
      "s.d\t$f0,8($4)",
      "li.s\t$2,1.50000000000000000000e0",
      "li.d\t$2,2.25000000000000000000e0",
      "mov.s\t$f0,$f12",
      "mov.d\t$f0,$f12",
      "mfc1\t$2,$f0",
      "mtc1\t$4,$f0",
      "mtc1\t$0,$f0",
      "add.s\t$f0,$f12,$f14",
      "add.d\t$f0,$f12,$f14",
      "sub.s\t$f0,$f12,$f14",
      "sub.d\t$f0,$f12,$f14",
      "mul.s\t$f0,$f12,$f14",
      "mul.d\t$f0,$f12,$f14",
      "div.s\t$f0,$f12,$f14",
      "div.d\t$f0,$f12,$f14",
      "abs.s\t$f0,$f12",
      "abs.d\t$f0,$f12",
      "neg.s\t$f0,$f12",
      "neg.d\t$f0,$f12",
      "sqrt.s\t$f0,$f12",
      "sqrt.d\t$f0,$f12",
      "cvt.d.s\t$f0,$f12",
      "cvt.s.d\t$f0,$f12",
      "trunc.w.s $f2,$f12,$2",
      "trunc.w.d $f2,$f12,$2",
      "mtc1\t$4,$f0\n\tcvt.s.w\t$f0,$f0",
      "mtc1\t$4,$f0\n\tcvt.d.w\t$f0,$f0",
      "l.s\t$f0,0($4)\n\tcvt.s.w\t$f0,$f0",
      "l.s\t$f0,0($4)\n\tcvt.d.w\t$f0,$f0",
      "c.eq.s\t$f12,$f14",
      "c.lt.s\t$f12,$f14",
      "c.le.s\t$f12,$f14",
      "c.eq.d\t$f12,$f14",
      "c.lt.d\t$f12,$f14",
      "c.le.d\t$f12,$f14",
  };
  for (std::string_view insn : kInstructions) {
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(insn);
    EXPECT_TRUE(res.matches) << "insn: " << insn << " diff: " << res.diff_detail;
  }
}

TEST_F(InvertFinalScanInsnTest, TruncWStandaloneAlternativeOnePreservesUnallocatedScratchClobber) {
  // `config/mips/mips.md:2485-2532` (`fix_truncdfsi2` / `fix_truncsfsi2`) +
  // `local-alloc.c:353-355` (`alloc_qty_for_scratch`, `case 'X': return;`):
  // standalone 1-line `trunc.w.{s,d} %0,%1,%2` is `which_alternative == 1`
  // whose operand 3 carries `*X` and stays `(clobber (scratch:{SF,DF}))`,
  // whereas 2-line `trunc.w.{s,d} + mfc1` is `which_alternative == 0` whose
  // operand 3 is allocated to a concrete FPR hard register `(clobber (reg:{SF,DF} ...))`.
  const std::string df_alt1 = InvertFinalScanInsn("trunc.w.d $f0,$f14,$2");
  EXPECT_NE(df_alt1.find("(clobber (scratch:DF))"), std::string::npos) << df_alt1;
  EXPECT_EQ(df_alt1.find("(clobber (reg:DF"), std::string::npos) << df_alt1;
  EXPECT_TRUE(verifier_.VerifyFinalScanInsnRightInverse("trunc.w.d $f0,$f14,$2").matches);

  const std::string sf_alt1 = InvertFinalScanInsn("trunc.w.s $f0,$f12,$2");
  EXPECT_NE(sf_alt1.find("(clobber (scratch:SF))"), std::string::npos) << sf_alt1;
  EXPECT_EQ(sf_alt1.find("(clobber (reg:SF"), std::string::npos) << sf_alt1;
  EXPECT_TRUE(verifier_.VerifyFinalScanInsnRightInverse("trunc.w.s $f0,$f12,$2").matches);

  // Negative: 2-line `which_alternative == 0` (`"=f"` operand 3) allocates a
  // concrete FPR hard-reg clobber rather than `(scratch:DF)`.
  const std::string df_alt0 = InvertFinalScanInsn("trunc.w.d $f0,$f14,$2\n\tmfc1\t$2,$f0");
  EXPECT_NE(df_alt0.find("(clobber (reg:DF 32 $f0))"), std::string::npos) << df_alt0;
  EXPECT_EQ(df_alt0.find("(clobber (scratch:DF))"), std::string::npos) << df_alt0;
  EXPECT_TRUE(
      verifier_.VerifyFinalScanInsnRightInverse("trunc.w.d $f0,$f14,$2\n\tmfc1\t$2,$f0").matches);
}

TEST_F(InvertFinalScanInsnTest, SyntheticPositiveBranchesJumpsAndCalls) {
  constexpr std::string_view kInstructions[] = {
      "j\t$31",         "j\t$2",          "j\t$L2",          "beq\t$4,$5,$L2",  "bne\t$4,$5,$L2",
      "beq\t$4,$0,$L2", "bne\t$4,$0,$L2", "beql\t$4,$5,$L2", "bnel\t$4,$0,$L2", "bgtz\t$4,$L2",
      "blez\t$4,$L2",   "bltz\t$4,$L2",   "bgez\t$4,$L2",    "bgtzl\t$4,$L2",   "blezl\t$4,$L2",
      "bltzl\t$4,$L2",  "bgezl\t$4,$L2",  "bc1t\t$L2",       "bc1f\t$L2",       "bc1tl\t$L2",
      "bc1fl\t$L2",     "jal\text_func",  "jal\t$31,$25",
  };
  for (std::string_view insn : kInstructions) {
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(insn);
    EXPECT_TRUE(res.matches) << "insn: " << insn << " diff: " << res.diff_detail;
  }
}

TEST_F(InvertFinalScanInsnTest, SyntheticNegativeUnrecognizedOrMalformed) {
  constexpr std::string_view kInvalidInputs[] = {
      "",
      "   \t  ",
      "# comment only",
      ".set\tnoreorder",
      ".ent\tfoo",
      "$L2:",
      "foo:",
      "invalid_op\t$2,$4,$5",
      "addu\t$2,$4",
      "addu\t$2,$4,$5,$6",
      "addu\t$99,$4,$5",
      "lw\t$2,16($99)",
      "lw\t$2,16($4",
      "beq\t$4,$5,not_a_local_label",
      "mtc1\t$4,$f0\n\taddu\t$2,$4,$5",
  };
  for (std::string_view bad : kInvalidInputs) {
    EXPECT_EQ(InvertFinalScanInsn(bad), "") << "Expected empty string for: '" << bad << "'";
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(bad);
    EXPECT_FALSE(res.matches) << "Expected failure for: '" << bad << "'";
  }
}

TEST_F(InvertFinalScanInsnTest, InvertFinalScanInsnLinesToCfgStreamTests) {
  // 1. Multi-block CFG with conditional branch, local `$L<n>` labels, and RPO
  //    successor edges.
  {
    const std::vector<std::string> asm_lines = {
        "beq\t$4,$0,$L2", "addu\t$2,$4,$5", "j\t$L3", "$L2:", "move\t$2,$0", "$L3:", "j\t$31",
    };
    RtlSidebandState sideband;
    sideband.frame_size_bytes = 0;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband);
    EXPECT_EQ(cfg.pass, RtlPassKind::kDbr);
    ASSERT_EQ(cfg.rpo_blocks.size(), 4u);

    // Block 0: conditional branch `beq $4,$0,$L2` -> canonical `[target, fallthrough]` `{2, 1}`.
    EXPECT_FALSE(cfg.rpo_blocks[0].has_label);
    const std::vector<size_t> expected_succ_0 = {2u, 1u};
    EXPECT_EQ(cfg.rpo_blocks[0].successors, expected_succ_0);

    // Block 1: fallthrough `addu $2,$4,$5` + unconditional `j $L3` -> target block 3.
    EXPECT_FALSE(cfg.rpo_blocks[1].has_label);
    const std::vector<size_t> expected_succ_1 = {3u};
    EXPECT_EQ(cfg.rpo_blocks[1].successors, expected_succ_1);

    // Block 2: `$L2:` `move $2,$0` -> fallthrough block 3.
    EXPECT_TRUE(cfg.rpo_blocks[2].has_label);
    const std::vector<size_t> expected_succ_2 = {3u};
    EXPECT_EQ(cfg.rpo_blocks[2].successors, expected_succ_2);

    // Block 3: `$L3:` `j $31` (`return_internal`) -> no successors.
    EXPECT_TRUE(cfg.rpo_blocks[3].has_label);
    EXPECT_TRUE(cfg.rpo_blocks[3].successors.empty());

    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        cfg, {}, 2, {"-O2", "-mips1"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. `.set noreorder` delay-slot `(sequence [(jump_insn ...) (insn ...)])`
  //    and `(sequence [(call_insn ...) (insn ...)])` bundles.
  {
    const std::vector<std::string> asm_lines = {
        ".set\tnoreorder", "jal\t__callee_0", "addiu\t$4,$0,1", ".set\treorder",
        ".set\tnoreorder", "j\t$31",          "addu\t$2,$2,$4", ".set\treorder",
    };
    RtlSidebandState sideband;
    sideband.frame_size_bytes = 16;
    sideband.saved_gpr_regnos = {31};
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband);
    ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
    EXPECT_EQ(cfg.sideband.frame_size_bytes, 16);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 2u);
    EXPECT_TRUE(absl::StartsWith(cfg.rpo_blocks[0].instructions[0], "(sequence [(call_insn "))
        << cfg.rpo_blocks[0].instructions[0];
    EXPECT_TRUE(
        absl::StartsWith(cfg.rpo_blocks[0].instructions[1],
                         "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] ))"))
        << cfg.rpo_blocks[0].instructions[1];
    EXPECT_TRUE(cfg.rpo_blocks[0].successors.empty());

    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        cfg, {}, 2, {"-O2", "-mips1"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 3. 2-line `mtc1` + `cvt.s.w` / `cvt.d.w` and multi-line `lwl`/`lwr`/`swl`/`swr`
  //    `movstrsi` block moves.
  {
    const std::vector<std::string> asm_lines = {
        "mtc1\t$4,$f0",  "cvt.s.w\t$f0,$f0", "mtc1\t$5,$f2",  "cvt.d.w\t$f2,$f2", "lwl\t$2,0($5)",
        "lwr\t$2,3($5)", "swl\t$2,0($4)",    "swr\t$2,3($4)", "j\t$31",
    };
    RtlSidebandState sideband;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband);
    ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[0], "(float:SF "))
        << cfg.rpo_blocks[0].instructions[0];
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[1], "(float:DF "))
        << cfg.rpo_blocks[0].instructions[1];
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[2], "(mem:BLK "))
        << cfg.rpo_blocks[0].instructions[2];
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[3], "(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        cfg, {}, 2, {"-O2", "-mips1"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 4. Negative cases: empty input or invalid/unrecognized assembly line causes
  //    `InvertFinalScanInsnLinesToCfg` to return an empty `RtlControlFlowGraph`.
  {
    RtlSidebandState sideband;
    EXPECT_TRUE(InvertFinalScanInsnLinesToCfg({}, sideband).rpo_blocks.empty());

    const std::vector<std::string> bad_insn_lines = {
        "addu\t$2,$4,$5",
        "invalid_mnemonic\t$2,$4,$5",
        "j\t$31",
    };
    EXPECT_TRUE(InvertFinalScanInsnLinesToCfg(bad_insn_lines, sideband).rpo_blocks.empty());

    const std::vector<std::string> unclosed_noreorder = {
        ".set\tnoreorder",
        "j\t$31",
        "nop",
    };
    EXPECT_TRUE(InvertFinalScanInsnLinesToCfg(unclosed_noreorder, sideband).rpo_blocks.empty());
  }

  // 5. Jump-table `Stage0ResolvedJumpTable` lifting (`tablejump` `PARALLEL`,
  //    `addr_vec_targets` preserving duplicates, deduplicated `successors`, and
  //    canonical sequential `L0` table label rewrite).
  {
    const std::vector<std::string> asm_lines = {
        "lw\t$2,$L100000($4)", "j\t$2",  "$L2:", "addiu\t$2,$0,10", "j\t$31", "$L3:",
        "addiu\t$2,$0,20",     "j\t$31",
    };
    const std::vector<Stage0ResolvedJumpTable> jump_tables = {
        Stage0ResolvedJumpTable{
            .table_label_token = "L100000",
            .target_labels = {"$L2", "$L3", "$L2"},
        },
    };
    RtlSidebandState sideband;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband, jump_tables);
    ASSERT_EQ(cfg.rpo_blocks.size(), 3u);
    const std::vector<size_t> expected_addr_vec = {1u, 2u, 1u};
    const std::vector<size_t> expected_succ = {1u, 2u};
    EXPECT_EQ(cfg.rpo_blocks[0].addr_vec_targets, expected_addr_vec);
    EXPECT_EQ(cfg.rpo_blocks[0].successors, expected_succ);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 2u);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[0], "(label_ref L0)"))
        << cfg.rpo_blocks[0].instructions[0];
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[1],
              "(parallel[ (set (pc) (reg:SI 2 v0)) (use (label_ref L0)) ] )");

    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        cfg, {}, 2, {"-O2", "-mips1"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 6. Multi-jump-table GPR dataflow disambiguation inside a loop with hoisted
  //    callee-saved table bases (`$16`/`$17`), CSE-shared `%hi`, a `jal` call,
  //    and REVERSED `.rodata` jump-table order (`L100000` is the SECOND `jr`,
  //    `L100001` is the FIRST `jr`!).
  {
    const std::vector<std::string> asm_lines = {
        // Block 0 (preheader): hoist both jump table bases with shared `%hi` and reversed order!
        "lui\t$16,%hi($L100000)",
        "addiu\t$17,$16,%lo($L100001)",  // $17 ($s1) = base of FIRST switch (L100001)
        "addiu\t$16,$16,%lo($L100000)",  // $16 ($s0) = base of SECOND switch (L100000)
        // Block 1 (loop header + call + first switch dispatch via $17):
        "$L2:",
        "jal\thelper_fn",
        "sll\t$2,$4,2",
        "addu\t$2,$2,$17",
        "lw\t$2,0($2)",
        "j\t$2",
        // Block 2 & 3 (cases of first switch, flowing to second switch `$L5:`):
        "$L3:",
        "addiu\t$5,$5,1",
        "j\t$L5",
        "$L4:",
        "addiu\t$5,$5,2",
        // Block 4 (`$L5:`: second switch dispatch via $16):
        "$L5:",
        "sll\t$3,$6,2",
        "addu\t$3,$16,$3",
        "lw\t$3,0($3)",
        "j\t$3",
        // Block 5 & 6 (cases of second switch, looping back to `$L2:` or returning):
        "$L6:",
        "bne\t$5,$0,$L2",
        "j\t$31",
        "$L7:",
        "j\t$31",
    };
    // Reversed `.rodata` order: `L100000` (for second switch: `$L6, $L7`) comes BEFORE
    // `L100001` (for first switch: `$L3, $L4, $L3`)!
    const std::vector<Stage0ResolvedJumpTable> jump_tables = {
        Stage0ResolvedJumpTable{
            .table_label_token = "L100000",
            .target_labels = {"$L6", "$L7"},
        },
        Stage0ResolvedJumpTable{
            .table_label_token = "L100001",
            .target_labels = {"$L3", "$L4", "$L3"},
        },
    };
    RtlSidebandState sideband;
    sideband.saved_gpr_regnos = {16, 17, 31};
    sideband.frame_size_bytes = 32;
    sideband.is_leaf_function = false;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband, jump_tables);
    ASSERT_GE(cfg.rpo_blocks.size(), 7u);

    // Block 1 (first switch `jr $2` fed by `$17` = `L100001`) MUST bind to `{2, 3, 2}` (`$L3, $L4,
    // $L3`), NOT `{5, 6}`!
    const std::vector<size_t> expected_first_targets = {2u, 3u, 2u};
    EXPECT_EQ(cfg.rpo_blocks[1].addr_vec_targets, expected_first_targets);

    // Block 4 (second switch `jr $3` fed by `$16` = `L100000`) MUST bind to `{5, 7}` (`$L6, $L7`;
    // Block 6 is the fallthrough `j $31` after `bne $5,$0,$L2`)!
    const std::vector<size_t> expected_second_targets = {5u, 7u};
    EXPECT_EQ(cfg.rpo_blocks[4].addr_vec_targets, expected_second_targets);

    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        cfg, {}, 2, {"-O2", "-mips1"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 7. Shared `%hi` 64KB page between jump table `L100000` and global function pointer
  //    `g_fn_ptr` (`lw $25,%lo(g_fn_ptr)($16); j $25`), plus an unreferenced jump table
  //    `L100099`: `kLoSum` must NEVER leak `$16`'s `%hi(L100000)` taint into `$25`, and
  //    no positional fallback may attach `L100099` to the function-pointer tail call `j $25`!
  {
    const std::vector<std::string> asm_lines = {
        "lui\t$16,%hi($L100000)",
        "beq\t$4,$0,$L2",
        "lw\t$25,%lo(g_fn_ptr)($16)",
        "j\t$25",
        "$L2:",
        "addiu\t$16,$16,%lo($L100000)",
        "sll\t$2,$5,2",
        "addu\t$2,$2,$16",
        "lw\t$2,0($2)",
        "j\t$2",
        "$L3:",
        "addiu\t$2,$0,10",
        "j\t$31",
        "$L4:",
        "addiu\t$2,$0,20",
        "j\t$31",
    };
    const std::vector<Stage0ResolvedJumpTable> jump_tables = {
        Stage0ResolvedJumpTable{
            .table_label_token = "L100000",
            .target_labels = {"$L3", "$L4"},
        },
        Stage0ResolvedJumpTable{
            .table_label_token = "L100099",
            .target_labels = {"$L4", "$L3"},
        },
    };
    RtlSidebandState sideband;
    sideband.saved_gpr_regnos = {16};
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband, jump_tables);
    ASSERT_EQ(cfg.rpo_blocks.size(), 5u);

    // Block 1 (`lw $25,%lo(g_fn_ptr)($16); j $25`) is a function-pointer tail call, NOT a switch!
    EXPECT_TRUE(cfg.rpo_blocks[1].addr_vec_targets.empty());
    EXPECT_TRUE(cfg.rpo_blocks[1].successors.empty());

    // Block 2 (`j $2` fed by `L100000`) binds cleanly to `{3, 4}`.
    const std::vector<size_t> expected_switch_targets = {3u, 4u};
    EXPECT_EQ(cfg.rpo_blocks[2].addr_vec_targets, expected_switch_targets);

    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        cfg, {}, 2, {"-O2", "-mips1"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 8. Adjacent 64-bit `dla` macro table bases (`dla $20, $L100000` followed by
  //    `dla $19, $L100001`) must NOT clobber `$20` (`dst + 1`) when writing `DImode` `$19`!
  {
    const std::vector<std::string> asm_lines = {
        "dla\t$20,$L100000",
        "dla\t$19,$L100001",
        "sll\t$2,$4,2",
        "addu\t$2,$2,$20",
        "lw\t$2,0($2)",
        "j\t$2",
        "$L2:",
        "sll\t$3,$5,2",
        "addu\t$3,$3,$19",
        "lw\t$3,0($3)",
        "j\t$3",
        "$L3:",
        "j\t$31",
        "$L4:",
        "j\t$31",
    };
    const std::vector<Stage0ResolvedJumpTable> jump_tables = {
        Stage0ResolvedJumpTable{
            .table_label_token = "L100000",
            .target_labels = {"$L2", "$L3"},
        },
        Stage0ResolvedJumpTable{
            .table_label_token = "L100001",
            .target_labels = {"$L3", "$L4"},
        },
    };
    RtlSidebandState sideband;
    sideband.saved_gpr_regnos = {19, 20};
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband, jump_tables);
    ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
    const std::vector<size_t> expected_jt0 = {1u, 2u};
    const std::vector<size_t> expected_jt1 = {2u, 3u};
    EXPECT_EQ(cfg.rpo_blocks[0].addr_vec_targets, expected_jt0);
    EXPECT_EQ(cfg.rpo_blocks[1].addr_vec_targets, expected_jt1);

    const auto snap = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kFinalScanInsn, cfg, {}, 2, {"-O2", "-mips2", "-mcpu=r4000"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 9. `.o`-disassembled `!TARGET_64BIT` 64-bit variable shift (`$L2:`, `$L3:`, `$L4:`)
  //    where `$L4:` is ALSO referenced by an earlier conditional branch (`beq $6,$0,$L4`),
  //    plus 2-line `trunc.w.s` + `mfc1` and 2-line `DImode` `li`.
  {
    const std::vector<std::string> asm_lines = {
        "li\t$2,0",
        "li\t$3,1",
        "beq\t$6,$0,$L4",
        "sll\t$7,$6,26",
        "bgez\t$7,$L2",
        "sllv\t$2,$5,$6",
        ".set\tnoreorder",
        "beq\t$0,$0,$L4",
        "addu\t$3,$0,$0",
        ".set\treorder",
        "$L2:",
        ".set\tnoreorder",
        "beq\t$7,$0,$L3",
        "sllv\t$2,$4,$6",
        ".set\treorder",
        "subu\t$7,$0,$6",
        "srlv\t$7,$5,$7",
        "or\t$2,$2,$7",
        "$L3:",
        "sllv\t$3,$5,$6",
        "$L4:",
        "trunc.w.s\t$f2,$f12",
        "mfc1\t$4,$f2",
        "addu\t$2,$2,$4",
        "j\t$31",
    };
    RtlSidebandState sideband;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(asm_lines, sideband);
    // Block 0 (`li` DImode + `beq $6,$0,$L4`), Block 1 (collapsed `ashift:DI`),
    // Block 2 (`$L4:` `fix_truncsfsi2` + `addu` + `return`).
    ASSERT_EQ(cfg.rpo_blocks.size(), 3u);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[0], "(set (reg:DI 2 v0) (const_int 1))");
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[1].instructions[0], "(ashift:DI "))
        << cfg.rpo_blocks[1].instructions[0];
    EXPECT_TRUE(cfg.rpo_blocks[2].has_label);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[2].instructions[0], "(fix:SI "))
        << cfg.rpo_blocks[2].instructions[0];

    const auto snap = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kFinalScanInsn, cfg, {}, 2, {"-O2", "-mips2", "-mcpu=r4000"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 10. `UpgradeR4000Mulsi3Insns` + `NarrowDuplicateConstLoadsInBlocks`: on `-mcpu=r4000`,
  //     consecutive `mult` + `mflo` (with dead `hi`/`lo`) fuse into single-insn `mulsi3_r4000`,
  //     while duplicate same-block constant loads `li $6,1` before a `QI` byte store (`sb`) and a
  //     subsequent `SI` word store (`sw`) recover `(set (reg:QI 6 a2) (const_int 1))` vs
  //     `(set (reg:SI 6 a2) (const_int 1))` and satisfy `VerifySubfunctionBoundaryRightInverse`.
  {
    const std::vector<std::string> asm_lines = {
        "mult\t$4,$5", "mflo\t$2", "li\t$6,1", "sb\t$6,0($7)", "li\t$6,1", "sw\t$6,4($7)", "j\t$31",
    };
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const CompilationProfile profile_r4000{
        .optimize_level = 2,
        .c_flags = {"-O2", "-mips2", "-mcpu=r4000"},
    };
    const RtlControlFlowGraph cfg =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &profile_r4000);
    ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[0],
                                  "(set (reg/i:SI 2 v0) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1)))"))
        << cfg.rpo_blocks[0].instructions[0];
    EXPECT_TRUE(
        absl::StrContains(cfg.rpo_blocks[0].instructions[1], "(set (reg:QI 6 a2) (const_int 1))"))
        << cfg.rpo_blocks[0].instructions[1];
    EXPECT_TRUE(
        absl::StrContains(cfg.rpo_blocks[0].instructions[3], "(set (reg:SI 6 a2) (const_int 1))"))
        << cfg.rpo_blocks[0].instructions[3];

    const auto snap = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kFinalScanInsn, cfg, {}, 2, {"-O2", "-mips2", "-mcpu=r4000"});
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertFinalScanInsnTest, O0SubwordTruncatingSubregsDistinguishesAluFromNarrowMemLoad) {
  // At `-O0`, GCC 2.7.2 `final_scan_insn` (`final.c:1933, 2095` `alter_subreg`)
  // rewrites `(subreg:QI|HI (reg:SI hr) 0)` into `(reg:QI|HI hr)` in place,
  // whereas narrow `movqi`/`movhi` memory loads (`lbu`/`lhu` disassembled as
  // `(zero_extend:SI (mem:QI|HI ...))`) originally used `(reg:QI|HI hr)`
  // directly. Verify `ReconstructO0FinalScanDeletions` wraps the subword store
  // after an `SImode` ALU `ori` into `(subreg:QI (reg:SI 5 a1) 0)` while
  // keeping the `lhu` + `sh` pair on `(reg:HI 3 v1)`, and satisfies
  // `VerifySubfunctionBoundaryRightInverse(kFinalScanInsn)` at `-O0`.
  const std::vector<std::string> body_asm_lines = {
      "ori\t$5,$4,3", "sb\t$5,0($fp)", "lhu\t$3,4($fp)", "sh\t$3,6($fp)", "lbu\t$2,0($fp)",
  };
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 8;
  sideband.frame_pointer_needed = true;
  sideband.is_leaf_function = true;
  sideband.saved_gpr_regnos = {30};
  const CompilationProfile o0_profile{
      .optimize_level = 0,
      .c_flags = {"-O0", "-mips2", "-mcpu=r4000"},
  };
  const RtlControlFlowGraph cfg =
      InvertFinalScanInsnLinesToCfg(body_asm_lines, sideband, {}, {}, &o0_profile);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);

  bool saw_wrapped_alu_subreg = false;
  bool saw_unwrapped_lhu_store = false;
  for (const std::string& insn : cfg.rpo_blocks[0].instructions) {
    if (absl::StrContains(insn, "(subreg:QI (reg:SI 5 a1) 0)")) {
      saw_wrapped_alu_subreg = true;
    }
    if (absl::StrContains(insn, "(reg:HI 3 v1)") &&
        !absl::StrContains(insn, "(subreg:HI (reg:SI 3 v1) 0)")) {
      saw_unwrapped_lhu_store = true;
    }
  }
  EXPECT_TRUE(saw_wrapped_alu_subreg) << cfg.FormatSummary();
  EXPECT_TRUE(saw_unwrapped_lhu_store) << cfg.FormatSummary();

  class O0FinalScanProfileProvider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    explicit O0FinalScanProfileProvider(const CompilationProfile* profile) : profile_(profile) {}
    RtlControlFlowGraph InvertFinalScanInsnStream(absl::Span<const std::string> asm_lines_x,
                                                  const RtlSidebandState& sb) const override {
      return lifter::InvertFinalScanInsnLinesToCfg(asm_lines_x, sb, {}, {}, profile_);
    }

   private:
    const CompilationProfile* profile_;
  };
  O0FinalScanProfileProvider o0_provider(&o0_profile);
  gcc_harness::RightInverseVerifier o0_verifier(&ctx_, &hooks_, &o0_provider);
  const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                      cfg, {}, 0, {"-O0", "-mips2", "-mcpu=r4000"});
  const auto res = o0_verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertFinalScanInsnTest, ReconstructsO0NoConflictBlockDiSubregsAndMatchesBoundary) {
  const std::vector<std::string> body_asm_lines = {
      "sw\t$4,16($fp)", "lw\t$5,16($fp)", "sra\t$4,$5,31",
      "lw\t$3,16($fp)", "move\t$2,$4",    "sd\t$2,8($fp)",
  };
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 16;
  sideband.frame_pointer_needed = true;
  sideband.is_leaf_function = true;
  sideband.saved_gpr_regnos = {30};
  const CompilationProfile o0_profile{
      .optimize_level = 0,
      .c_flags = {"-O0", "-mips2", "-mcpu=r4000"},
  };
  const RtlControlFlowGraph cfg =
      InvertFinalScanInsnLinesToCfg(body_asm_lines, sideband, {}, {}, &o0_profile);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);

  bool saw_clobber_di = false;
  bool saw_subreg_lo = false;
  bool saw_subreg_hi = false;
  for (const std::string& insn : cfg.rpo_blocks[0].instructions) {
    if (insn == "(clobber (reg:DI 2 v0))") {
      saw_clobber_di = true;
    }
    if (absl::StrContains(insn, "(set (subreg:SI (reg:DI 2 v0) 1) ")) {
      saw_subreg_lo = true;
    }
    if (absl::StrContains(insn, "(set (subreg:SI (reg:DI 2 v0) 0) ")) {
      saw_subreg_hi = true;
    }
  }
  EXPECT_TRUE(saw_clobber_di) << cfg.FormatSummary();
  EXPECT_TRUE(saw_subreg_lo) << cfg.FormatSummary();
  EXPECT_TRUE(saw_subreg_hi) << cfg.FormatSummary();

  class O0DiSubregProfileProvider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    explicit O0DiSubregProfileProvider(const CompilationProfile* profile) : profile_(profile) {}
    RtlControlFlowGraph InvertFinalScanInsnStream(absl::Span<const std::string> asm_lines_x,
                                                  const RtlSidebandState& sb) const override {
      return lifter::InvertFinalScanInsnLinesToCfg(asm_lines_x, sb, {}, {}, profile_);
    }

   private:
    const CompilationProfile* profile_;
  };
  O0DiSubregProfileProvider o0_provider(&o0_profile);
  gcc_harness::RightInverseVerifier o0_verifier(&ctx_, &hooks_, &o0_provider);
  const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                      cfg, {}, 0, {"-O0", "-mips2", "-mcpu=r4000"});
  const auto res = o0_verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertFinalScanInsnTest, MultiSwitchAndHoistedGlobalsBoundaryRightInverse) {
  constexpr const char* kMultiSwitchSrc = R"(
extern int g_arr_a[16];
extern int g_arr_b[16];
int step_fn(int);
int multi_switch_final_scan(int n, int x, int y) {
  int acc = 0;
  int i;
  for (i = 0; i < n; ++i) {
    x = step_fn(x + i);
    switch (x & 7) {
      case 0: acc += g_arr_a[0]; break;
      case 1: acc += g_arr_a[3]; break;
      case 2: acc -= g_arr_b[1]; break;
      case 3: acc ^= g_arr_b[5]; break;
      case 4: acc += 17; break;
      default: acc -= 3; break;
    }
    switch ((y + i) & 7) {
      case 0: g_arr_a[2] = acc; break;
      case 1: g_arr_b[4] = acc + 1; break;
      case 2: acc += g_arr_b[7]; break;
      case 3: acc -= g_arr_a[6]; break;
      case 4: acc ^= 99; break;
      default: break;
    }
  }
  return acc;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips1", "-G", "0", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "0", "-Wa,-g"}}) {
    const auto trace = hooks_.TraceForwardCompilation(kMultiSwitchSrc, "multi_switch_final_scan",
                                                      flags, "multi_switch_final_scan");
    ASSERT_GE(trace.stage0_input.jump_tables.size(), 2u);
    bool found_snapshot = false;
    for (const auto& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id != gcc_harness::SubfunctionId::kFinalScanInsn) {
        continue;
      }
      found_snapshot = true;
      const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(res.matches) << res.diff_detail;
    }
    EXPECT_TRUE(found_snapshot);
  }
}

class CorruptedFinalScanInsnInverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  std::string InvertFinalScanInsn(std::string_view /*asm_line_x*/) const override {
    return "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))";
  }

  RtlControlFlowGraph InvertFinalScanInsnStream(absl::Span<const std::string> /*asm_lines_x*/,
                                                const RtlSidebandState& sideband) const override {
    RtlControlFlowGraph corrupted;
    corrupted.pass = RtlPassKind::kDbr;
    corrupted.sideband = sideband;
    RtlBasicBlock blk;
    blk.id = 0;
    blk.instructions = {
        "(set (reg:SI 2 v0) (const_int 999))",
        "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
    };
    blk.SyncRtlInsnsFromInstructions();
    corrupted.rpo_blocks = {blk};
    return corrupted;
  }
};

TEST_F(InvertFinalScanInsnTest, NegativeVerifierRejectsCorruptedInverseProvider) {
  CorruptedFinalScanInsnInverseProvider bad_provider;
  gcc_harness::RightInverseVerifier bad_verifier(&ctx_, &hooks_, &bad_provider);

  const auto good_insn = verifier_.VerifyFinalScanInsnRightInverse("addu\t$2,$4,$5");
  EXPECT_TRUE(good_insn.matches) << good_insn.diff_detail;
  const auto bad_insn = bad_verifier.VerifyFinalScanInsnRightInverse("addu\t$2,$4,$5");
  EXPECT_FALSE(bad_insn.matches);
  EXPECT_FALSE(bad_insn.diff_detail.empty());

  constexpr std::string_view kSrc = R"(
int add_pair_for_final_scan(int a, int b) {
  return a + b + 3;
}
)";
  const auto trace = hooks_.TraceForwardCompilation(kSrc, "add_pair_for_final_scan",
                                                    {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                                                    "add_pair_for_final_scan");
  bool found_snapshot = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != gcc_harness::SubfunctionId::kFinalScanInsn) {
      continue;
    }
    found_snapshot = true;
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(snap).matches);
    const auto bad_stream = bad_verifier.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_FALSE(bad_stream.matches);
    EXPECT_FALSE(bad_stream.diff_detail.empty());
  }
  EXPECT_TRUE(found_snapshot);
}

TEST_F(InvertFinalScanInsnTest,
       RecoversO0FinalScanAndMultiUseSubwordZeroExtendAndScalarCallReturn) {
  // 1. `-O0 -mips1` GP32 `DImode` zero store and signed division trap sequence
  //    reconstructed in Stage 0 (`reconstruct_o0_final_scan`).
  {
    constexpr std::string_view kO0Src = R"(
int clear_pair_and_divide(long long* dst, int num, int den) {
  *dst = 0LL;
  return num / den;
}
)";
    const auto trace = hooks_.TraceForwardCompilation(
        kO0Src, "clear_pair_and_divide", {"-O0", "-mips1", "-Wa,-g"}, "clear_pair_and_divide");
    bool found = false;
    for (const auto& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id == gcc_harness::SubfunctionId::kFinalScanInsn) {
        found = true;
        const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
        EXPECT_TRUE(res.matches) << res.diff_detail;
      }
    }
    EXPECT_TRUE(found);
  }

  // 2. Multi-use unsigned byte load feeding `zero_extend:SI` and cross-branch
  //    `(subreg:SI (reg:QI ...) 0)` arithmetic (`-O2 -mips1 -funsigned-char -Wa,-O2`).
  {
    constexpr std::string_view kSubwordSrc = R"(
void fold_ascii_upper(unsigned char* s, int mark) {
  for (; *s != 0; ++s) {
    unsigned char c = *s;
    if (mark != 0) {
      continue;
    }
    if ((unsigned char)(c - 65u) < 26u) {
      *s = (unsigned char)(c + 32u);
    }
  }
}
)";
    const auto trace = hooks_.TraceForwardCompilation(
        kSubwordSrc, "fold_ascii_upper", {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
        "fold_ascii_upper");
    bool found = false;
    for (const auto& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id == gcc_harness::SubfunctionId::kFinalScanInsn) {
        found = true;
        const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
        EXPECT_TRUE(res.matches) << res.diff_detail;
      }
    }
    EXPECT_TRUE(found);
  }

  // 3. Discarded scalar `CALL_INSN` return with an unrelated `$3` scratch write on
  //    an early exit must preserve `(set (reg:SI 2 v0) (call ...))` rather than `DImode`.
  {
    constexpr std::string_view kCallSrc = R"(
extern int side_effect_step(int x);
int guard_then_discard_call(int* p, int flag) {
  if (flag == 0) {
    int v = *p + 7;
    *p = v;
    return v ^ 3;
  }
  side_effect_step(flag);
  return 0;
}
)";
    const auto trace =
        hooks_.TraceForwardCompilation(kCallSrc, "guard_then_discard_call",
                                       {"-O2", "-mips2", "-Wa,-g"}, "guard_then_discard_call");
    bool found = false;
    for (const auto& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id == gcc_harness::SubfunctionId::kFinalScanInsn) {
        found = true;
        const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
        EXPECT_TRUE(res.matches) << res.diff_detail;
        const CompilationProfile prof{snap.optimize_level, snap.c_flags, snap.gp_symbol_sizes};
        const RtlControlFlowGraph lifted = InvertFinalScanInsnLinesToCfg(
            snap.post_asm_lines, snap.post_state.sideband, /*jump_tables=*/{},
            /*constant_pool=*/{}, &prof);
        for (const RtlBasicBlock& blk : lifted.rpo_blocks) {
          for (const std::string& insn : blk.instructions) {
            if (absl::StrContains(insn, "(call ")) {
              EXPECT_FALSE(absl::StrContains(insn, "(reg:DI 2 v0)")) << insn;
            }
          }
        }
      }
    }
    EXPECT_TRUE(found);
  }
}

TEST_F(InvertFinalScanInsnTest,
       RecoversPrologueHoistedUnchangingPtrMemFlagAndPropagatesInEntryBlock) {
  // In `sched2`, a non-volatile general-pointer load `lw $5, 0($4)` can only be hoisted ahead of a
  // `$sp`-based callee-saved prologue store `sw $31, 20($sp)` when `RTX_UNCHANGING_P(mem) == 1`
  // (`mem/u`). Verify Stage 0 `InvertFinalScanInsnLinesToCfg` recovers `mem/u` directly in
  // `.12.dbr` both on the hoisted load (`0($4)`) and on subsequent entry-block reads (`4($4)`)
  // before `$4` is clobbered by `jal`, while stopping propagation after `jal` clobbers caller-saved
  // `$4`.
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};

  RtlSidebandState sideband;
  sideband.frame_size_bytes = 24;
  sideband.outgoing_args_size = 16;
  sideband.saved_gpr_regnos = {16, 31};
  sideband.is_leaf_function = false;

  const std::vector<std::string> asm_lines = {
      "subu\t$sp,$sp,24", "lw\t$5,0($4)", "sw\t$31,20($sp)", "sw\t$16,16($sp)", "lw\t$16,4($4)",
      "jal\t__callee_0",  "move\t$4,$5",  "lw\t$6,8($4)",    "addu\t$2,$2,$16", "addu\t$2,$2,$6",
  };

  const RtlControlFlowGraph cfg =
      InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &o2_prof);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
  ASSERT_GE(cfg.rpo_blocks[0].instructions.size(), 7u);

  const auto hoisted_load = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[1]);
  ASSERT_TRUE(hoisted_load.ok()) << hoisted_load.status();
  ASSERT_TRUE(hoisted_load->pattern.IsSet());
  ASSERT_EQ(hoisted_load->pattern.SetSrc().op, RtlOp::kMem);
  EXPECT_TRUE(hoisted_load->pattern.SetSrc().flags.unchanging) << cfg.rpo_blocks[0].instructions[1];

  const auto downstream_load = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[4]);
  ASSERT_TRUE(downstream_load.ok()) << downstream_load.status();
  ASSERT_TRUE(downstream_load->pattern.IsSet());
  ASSERT_EQ(downstream_load->pattern.SetSrc().op, RtlOp::kMem);
  EXPECT_TRUE(downstream_load->pattern.SetSrc().flags.unchanging)
      << cfg.rpo_blocks[0].instructions[4];

  // After `jal` clobbers caller-saved `$4`, `lw $6, 8($4)` at index 7 must NOT be marked `mem/u`.
  ASSERT_GE(cfg.rpo_blocks[0].instructions.size(), 8u);
  const auto post_call_load = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[7]);
  ASSERT_TRUE(post_call_load.ok()) << post_call_load.status();
  ASSERT_TRUE(post_call_load->pattern.IsSet());
  ASSERT_EQ(post_call_load->pattern.SetSrc().op, RtlOp::kMem);
  EXPECT_FALSE(post_call_load->pattern.SetSrc().flags.unchanging)
      << cfg.rpo_blocks[0].instructions[7];

  // Also test on a forward-compiled `const` struct pointer function with a call.
  constexpr std::string_view kConstSrc = R"(
struct Pair {
  const int a;
  const int b;
};
extern int ext_transform(int x);
int sum_const_pair_impl(const struct Pair* p) {
  int x = p->a;
  int y = p->b;
  return ext_transform(x) + y;
}
)";
  const auto trace = hooks_.TraceForwardCompilation(
      kConstSrc, "sum_const_pair_impl", {"-O2", "-mips2", "-Wa,-g"}, "sum_const_pair_impl");
  bool found = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == gcc_harness::SubfunctionId::kFinalScanInsn) {
      found = true;
      const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(res.matches) << res.diff_detail;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(InvertFinalScanInsnTest,
       Gp32DiNotMovstrsiFirstScratchAndInterSrcReadExtendSubregRightInverse) {
  // 1. `!gp64` `DImode` `(not:DI ...)` folding (`one_cmpldi2`): two `nor` instructions on an
  //    even/odd GPR pair witnessed by `DImode` arithmetic fold into `(not:DI (reg:DI ...))`.
  {
    const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1", "-Wa,-g"}};
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "nor\t$8,$0,$4",  "nor\t$9,$0,$5",   "addu\t$3,$9,$7", "sltu\t$10,$3,$7",
        "addu\t$2,$8,$6", "addu\t$2,$2,$10", "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    bool saw_not_di = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(not:DI (reg:DI 4 a0))")) {
        saw_not_di = true;
      }
    }
    EXPECT_TRUE(saw_not_di) << lifted.FormatSummary();
    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        lifted, {}, 2, prof.c_flags);
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. First-insn `movstrsi` scratch recovery when `src_base` (`$5 a1`) dies on `movstrsi`.
  {
    const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "lw\t$6,0($5)",  "lw\t$7,4($5)",  "lw\t$8,8($5)",  "lw\t$2,12($5)", "sw\t$6,0($4)",
        "sw\t$7,4($4)",  "sw\t$8,8($4)",  "sw\t$2,12($4)", "lw\t$6,16($5)", "lw\t$7,20($5)",
        "sw\t$6,16($4)", "sw\t$7,20($4)", "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    bool saw_blk_copy = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(mem:BLK ")) {
        saw_blk_copy = true;
      }
    }
    EXPECT_TRUE(saw_blk_copy) << lifted.FormatSummary();
    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        lifted, {}, 2, prof.c_flags);
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 3. `RestoreNoConflictExtendClobbersInBlock` `inter_src_read` `(subreg:SI (reg:DI dh) 0/1)` and
  //    `DImode` consumer `REG_DEAD` note when `src_reg` (`$4 a0`) is read between the two halves of
  //    a sign-extension into `(reg:DI 6 a2)` before an in-place `adddi3` on `(reg:DI 6 a2)`.
  {
    const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "move\t$7,$4",    "addu\t$8,$4,1",  "sra\t$6,$4,31",  "addu\t$7,$7,$3",
        "sltu\t$9,$7,$3", "addu\t$6,$6,$2", "addu\t$6,$6,$9", "xor\t$7,$7,$8",
        "move\t$2,$6",    "move\t$3,$7",    "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    bool saw_subreg_hi = false;
    bool saw_subreg_lo = false;
    bool saw_consumer_dead = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(set (subreg:SI (reg:DI 6 a2) 0)")) {
        saw_subreg_hi = true;
      }
      if (absl::StrContains(insn, "(set (subreg:SI (reg:DI 6 a2) 1)")) {
        saw_subreg_lo = true;
      }
      if (absl::StrContains(insn, "(plus:DI (reg:DI 6 a2)") &&
          absl::StrContains(insn, "REG_DEAD (reg:DI 6 a2)")) {
        saw_consumer_dead = true;
      }
    }
    EXPECT_TRUE(saw_subreg_hi) << lifted.FormatSummary();
    EXPECT_TRUE(saw_subreg_lo) << lifted.FormatSummary();
    EXPECT_TRUE(saw_consumer_dead) << lifted.FormatSummary();

    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        lifted, {}, 2, prof.c_flags);
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertFinalScanInsnTest, RestoreAlterSubregAndNarrowDeadSubwordConstLoads) {
  // 1. `final.c:1928-1946, 2095-2107` (`alter_subreg`): narrow `QI`/`HI` stores
  //    fed by `SImode` ALU and an `SImode`-read incoming ABI argument reconstruct
  //    `(subreg:QI/HI (reg:SI ...) 0)` while `MEM` base addresses stay unwrapped.
  {
    const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "addu\t$2,$4,1", "sb\t$2,0($5)", "sh\t$4,2($5)", "move\t$2,$4", "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    bool saw_qi_subreg = false;
    bool saw_hi_arg_subreg = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(subreg:QI (reg:SI 2 v0) 0)")) {
        saw_qi_subreg = true;
      }
      if (absl::StrContains(insn, "(subreg:HI (reg:SI 4 a0) 0)")) {
        saw_hi_arg_subreg = true;
      }
      EXPECT_FALSE(absl::StrContains(insn, "(subreg:QI (reg:SI 5 a1)")) << insn;
    }
    EXPECT_TRUE(saw_qi_subreg) << lifted.FormatSummary();
    EXPECT_TRUE(saw_hi_arg_subreg) << lifted.FormatSummary();
    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        lifted, {}, 2, prof.c_flags);
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. `mips.md:3364, 3416` (`movhi_internal2`/`movqi_internal2`) + `mips.c:1078`:
  //    block-local dead constant loads (`li $2, 255`, `ori $3, $0, 65535`) consumed
  //    exclusively in `QI`/`HI` narrow to `QI`/`HI`, whereas `li $6, 255` consumed
  //    in `SI` stays `SImode`.
  {
    const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "li\t$2,255", "sb\t$2,0($4)", "ori\t$3,$0,65535", "sh\t$3,2($4)",
        "li\t$6,255", "sw\t$6,4($4)", "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    bool saw_qi_const = false;
    bool saw_hi_const = false;
    bool saw_si_const = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(set (reg:QI 2 v0) (const_int 255))")) {
        saw_qi_const = true;
      }
      if (absl::StrContains(insn, "(set (reg:HI 3 v1) (const_int 65535))")) {
        saw_hi_const = true;
      }
      if (absl::StrContains(insn, "(set (reg:SI 6 a2) (const_int 255))")) {
        saw_si_const = true;
      }
    }
    EXPECT_TRUE(saw_qi_const) << lifted.FormatSummary();
    EXPECT_TRUE(saw_hi_const) << lifted.FormatSummary();
    EXPECT_TRUE(saw_si_const) << lifted.FormatSummary();
    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        lifted, {}, 2, prof.c_flags);
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 3. Negative contrasts: an entry ABI arg read solely in `QImode` (`sb $5, 0($4)`)
  //    with no `SImode` read must NOT be wrapped in `(subreg:QI (reg:SI ...) 0)`,
  //    and `-O0` `lhu`/`sh` stays unwrapped for `ReconstructO0FinalScanDeletions`.
  {
    const CompilationProfile prof_o2{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "sb\t$5,0($4)",
        "j\t$31",
    };
    const RtlControlFlowGraph lifted_o2 =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof_o2);
    ASSERT_EQ(lifted_o2.rpo_blocks.size(), 1u);
    for (const std::string& insn : lifted_o2.rpo_blocks[0].instructions) {
      EXPECT_FALSE(absl::StrContains(insn, "(subreg:QI ")) << lifted_o2.FormatSummary();
    }
    const auto snap_o2 = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kFinalScanInsn, lifted_o2, {}, 2, prof_o2.c_flags);
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(snap_o2).matches);

    const CompilationProfile prof_o0{.optimize_level = 0, .c_flags = {"-O0", "-mips2", "-Wa,-g"}};
    const std::vector<std::string> o0_lines = {
        "lhu\t$2,0($4)",
        "sh\t$2,2($4)",
        "j\t$31",
    };
    const RtlControlFlowGraph lifted_o0 =
        InvertFinalScanInsnLinesToCfg(o0_lines, sideband, {}, {}, &prof_o0);
    ASSERT_EQ(lifted_o0.rpo_blocks.size(), 1u);
    for (const std::string& insn : lifted_o0.rpo_blocks[0].instructions) {
      EXPECT_FALSE(absl::StrContains(insn, "(subreg:HI ")) << lifted_o0.FormatSummary();
    }
    const auto snap_o0 = hooks_.ExecuteSubfunctionBoundary(
        gcc_harness::SubfunctionId::kFinalScanInsn, lifted_o0, {}, 0, prof_o0.c_flags);
    EXPECT_TRUE(verifier_.VerifySubfunctionBoundaryRightInverse(snap_o0).matches);
  }

  // 4. Outgoing call-argument `$a0..$a3` constant load followed by a `jal`
  //    must NOT be narrowed to `QI`/`HI` before `ReconstructMissingCallFusage`
  //    runs, whereas non-argument `$v0` followed by `sb` + `jal` narrows cleanly.
  {
    const CompilationProfile prof_o2{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
    RtlSidebandState sideband;
    sideband.is_leaf_function = false;
    sideband.frame_size_bytes = 24;
    sideband.outgoing_args_size = 16;
    sideband.saved_gpr_regnos = {31};
    const std::vector<std::string> asm_lines = {
        "li\t$2,7", "sb\t$2,0($5)", "li\t$4,1", "sb\t$4,1($5)", "jal\tcallee",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof_o2);
    ASSERT_FALSE(lifted.rpo_blocks.empty());
    bool saw_qi_v0 = false;
    bool saw_si_a0 = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(set (reg:QI 2 v0) (const_int 7))")) {
        saw_qi_v0 = true;
      }
      if (absl::StrContains(insn, "(set (reg:SI 4 a0) (const_int 1))")) {
        saw_si_a0 = true;
      }
      EXPECT_FALSE(absl::StrContains(insn, "(set (reg:QI 4 a0) (const_int 1))"))
          << lifted.FormatSummary();
    }
    EXPECT_TRUE(saw_qi_v0) << lifted.FormatSummary();
    EXPECT_TRUE(saw_si_a0) << lifted.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest, FoldFinalScanDiConstDoubleFromGasExpandedLuiOri) {
  // Mirrors `config/mips/mips.c:1368-1402` (`mips_move_2words`, `!TARGET_64BIT`):
  // KMC GAS expands `li $2, 0x00ff00ff; li $3, 0x55aa55aa` into 4 instructions
  // (`lui; ori; lui; ori`). When `$2` is witnessed as a `DImode` pair, Stage 0
  // must fold all 4 instructions into `(set (reg:DI 2 v0) (const_double ...))`;
  // when `$2`/`$3` are only used in `SImode`, the 4 instructions stay uncollapsed.
  const CompilationProfile prof_o2{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  RtlSidebandState sideband;
  sideband.is_leaf_function = true;
  sideband.frame_size_bytes = 0;

  // Positive: `lw $2,16($sp); lw $3,20($sp); and $4,$4,$2; and $5,$5,$3`
  // witnesses `$2` as a `DI` even pair, followed by 4-insn `lui/ori` constant
  // load into `($2, $3)` and `xor:DI` return.
  {
    const std::vector<std::string> asm_lines = {
        "lw\t$2,16($sp)", "lw\t$3,20($sp)",    "and\t$4,$4,$2",  "and\t$5,$5,$3",
        "lui\t$2,0x00ff", "ori\t$2,$2,0x00ff", "lui\t$3,0x55aa", "ori\t$3,$3,0x55aa",
        "xor\t$2,$4,$2",  "xor\t$3,$5,$3",     "jr\t$31",        "nop",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof_o2);
    ASSERT_FALSE(lifted.rpo_blocks.empty());
    bool saw_di_const_double = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(set (reg:DI 2 v0) (const_double") &&
          absl::StrContains(insn, "1437226410 16711935")) {
        saw_di_const_double = true;
      }
    }
    EXPECT_TRUE(saw_di_const_double) << lifted.FormatSummary();
  }

  // Negative: without any `DI` witness on `$2`/`$3`, a 4-insn `lui/ori` sequence
  // consumed by an `SImode` add must NOT fold into `(reg:DI 2 v0)`.
  {
    const std::vector<std::string> asm_lines = {
        "lui\t$2,0x00ff",
        "ori\t$2,$2,0x00ff",
        "lui\t$3,0x55aa",
        "ori\t$3,$3,0x55aa",
        "addu\t$2,$2,$3",
        "jr\t$31",
        "nop",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof_o2);
    ASSERT_FALSE(lifted.rpo_blocks.empty());
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      EXPECT_FALSE(absl::StrContains(insn, "(set (reg:DI 2 v0)")) << lifted.FormatSummary();
    }
  }
}

TEST_F(InvertFinalScanInsnTest, RestoreNoConflictExtendClobbersDstOverlapsSrcWithPriorRead) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
  // Positive (`optabs.c:2485-2525`, `final.c:1928-1946, 2095-2107`, `sched.c:2779-3018`):
  // when `matched_dh` (`4 a0`) overlaps `src_reg` (`4 a0`) and an earlier insn reads
  // `src_reg`, `RestoreNoConflictExtendClobbersInBlock` wraps both subwords in
  // `(subreg:SI (reg:DI 4 a0) 1/0)`, attaches `REG_NO_CONFLICT (reg:SI 4 a0)`, and
  // prepends `REG_DEAD (reg:SI 4 a0)` ahead of `REG_NO_CONFLICT` on `last_ext`.
  {
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "addu\t$9,$4,1",  "move\t$5,$4",    "sra\t$4,$4,31",  "addu\t$5,$5,$3",
        "sltu\t$8,$5,$3", "addu\t$4,$4,$2", "addu\t$4,$4,$8", "xor\t$5,$5,$9",
        "move\t$2,$4",    "move\t$3,$5",    "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    bool saw_clobber = false;
    bool saw_subreg_lo = false;
    bool saw_subreg_hi_with_dead_first = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(clobber (reg:DI 4 a0))") &&
          absl::StrContains(insn, "REG_LIBCALL")) {
        saw_clobber = true;
      }
      if (absl::StrContains(insn, "(set (subreg:SI (reg:DI 4 a0) 1)") &&
          absl::StrContains(insn, "REG_NO_CONFLICT (reg:SI 4 a0)")) {
        saw_subreg_lo = true;
      }
      if (absl::StrContains(insn, "(set (subreg:SI (reg:DI 4 a0) 0)")) {
        const size_t dead_pos = insn.find("REG_DEAD (reg:SI 4 a0)");
        const size_t nc_pos = insn.find("REG_NO_CONFLICT (reg:SI 4 a0)");
        if (dead_pos != std::string::npos && nc_pos != std::string::npos && dead_pos < nc_pos) {
          saw_subreg_hi_with_dead_first = true;
        }
      }
    }
    EXPECT_TRUE(saw_clobber) << lifted.FormatSummary();
    EXPECT_TRUE(saw_subreg_lo) << lifted.FormatSummary();
    EXPECT_TRUE(saw_subreg_hi_with_dead_first) << lifted.FormatSummary();
    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        lifted, {}, 2, prof.c_flags);
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative contrast: without a prior read of `src_reg` (`4 a0`) before the
  // overlapping sign-extension, subwords remain plain hard-register sets without
  // `REG_NO_CONFLICT` wrapping.
  {
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "move\t$5,$4", "sra\t$4,$4,31", "sw\t$4,0($6)", "sw\t$5,4($6)", "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      EXPECT_FALSE(absl::StrContains(insn, "(subreg:SI (reg:DI 4 a0)")) << lifted.FormatSummary();
    }
  }
}

TEST_F(InvertFinalScanInsnTest, NarrowsCrossBlockAndDelaySlotSubwordConstLoadsPositiveAndNegative) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-O2"}, {}};
  hooks_.ApplyCompilationProfile(prof);

  // 1. Positive (`config/mips/mips.md:3364, 3416`, `config/mips/mips.c:1078`):
  //    a cross-block `li $2, 193` (in a jump delay slot) and a fallthrough
  //    `li $2, 197` feeding a shared successor `sb $2, 0($4)` both narrow from
  //    `(reg:SI 2 v0)` to `(reg:QI 2 v0)`.
  {
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "beq\t$5,$0,$L2", ".set\tnoreorder", "j\t$L3", "li\t$2,193",   ".set\treorder",
        "$L2:",           "li\t$2,197",      "$L3:",   "sb\t$2,0($4)", "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    const std::string summary = lifted.FormatSummary();
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:QI 2 v0) (const_int 193))")) << summary;
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:QI 2 v0) (const_int 197))")) << summary;
    const auto snap = hooks_.ExecuteSubfunctionBoundary(gcc_harness::SubfunctionId::kFinalScanInsn,
                                                        lifted, {}, 2, prof.c_flags);
    const auto res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. Negative contrast: when a reachable successor reads `$2` in `SImode`
  //    (`sw $2, 0($4)`), neither the delay-slot nor fallthrough `li $2, C` is
  //    narrowed to `QImode`.
  {
    RtlSidebandState sideband;
    sideband.is_leaf_function = true;
    const std::vector<std::string> asm_lines = {
        "beq\t$5,$0,$L2", ".set\tnoreorder", "j\t$L3", "li\t$2,193",   ".set\treorder",
        "$L2:",           "li\t$2,197",      "$L3:",   "sw\t$2,0($4)", "j\t$31",
    };
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(asm_lines, sideband, {}, {}, &prof);
    const std::string summary = lifted.FormatSummary();
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SI 2 v0) (const_int 193))")) << summary;
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SI 2 v0) (const_int 197))")) << summary;
  }
}

TEST_F(InvertFinalScanInsnTest, MarkPrologueHoistedUnchangingPtrMemsAscendingLuidAndFlushTrapped) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-O2"}, {}};
  hooks_.ApplyCompilationProfile(prof);

  // Mirrors `sched.c:1740, 1748, 2428`: when >32 MEM ops precede a `jal` and
  // two callee-saved `mem/u` loads from the same base pointer `$2` (`v0`) are
  // hoisted ahead of `last_prologue_store_step`, `(REG_DEAD $v0)` on a
  // flush-trapped call-used `mem/u` load after `first_flush_store_step` must
  // transfer to the LARGEST `l_s` (the second hoisted load, ascending `.09.greg`
  // LUID order) — NOT the first hoisted load (positive). When a non-`mem/u`
  // arithmetic consumer reads `$v0` after `c.consumer_s` before the call
  // (negative), `(REG_DEAD $v0)` must NOT transfer to the prologue-hoisted load.
  auto build_asm = [](bool extra_non_mem_v0_use_before_call) {
    std::vector<std::string> asm_lines = {
        "subu\t$sp,$sp,128",
        "sw\t$31,124($sp)",
        "sw\t$16,120($sp)",
        "sw\t$17,116($sp)",
        "move\t$2,$4",
        // Two callee-saved `mem/u` loads from `$2` hoisted ahead of the last prologue save.
        "lw\t$16,0($2)",
        "lw\t$17,4($2)",
        "sw\t$18,112($sp)",
    };
    // Emit 31 independent stack loads + a spill store to trip `pending_lists_len > 32`.
    for (int k = 0; k < 31; ++k) {
      asm_lines.push_back("lw\t$8," + std::to_string((k % 8) * 4 + 32) + "($sp)");
    }
    // `first_flush_store_step` (`pending_lists_len > 32` at this store) + body sp store.
    asm_lines.push_back("sw\t$8,64($sp)");
    // Flush-trapped call-used `mem/u` load from `$2` (holds `(REG_DEAD $v0)` before transfer).
    asm_lines.push_back("lw\t$9,8($2)");
    if (extra_non_mem_v0_use_before_call) {
      // Consume both `$16` and `$17` first, then read `$2` in a non-`mem/u` op
      // before `jal` so `breg_used_other_after_consumer` blocks the transfer.
      asm_lines.push_back("addu\t$10,$9,$16");
      asm_lines.push_back("addu\t$11,$10,$17");
      asm_lines.push_back("addu\t$4,$11,$2");
    } else {
      asm_lines.push_back("addu\t$4,$9,$16");
    }
    asm_lines.push_back("jal\t__callee_0");
    // Use `$17` after the call so `c.consumer_s` for the second hoisted load is post-call.
    asm_lines.push_back("addu\t$2,$2,$17");
    return asm_lines;
  };

  RtlSidebandState sideband;
  sideband.is_leaf_function = false;
  sideband.frame_size_bytes = 128;
  sideband.outgoing_args_size = 16;
  sideband.saved_gpr_regnos = {16, 17, 18, 31};

  // Positive: `(REG_DEAD (reg:SI 2 v0))` transfers to the second hoisted load
  // (`lw $17, 4($2)` — largest `l_s`), NOT the first (`lw $16, 0($2)`).
  {
    const RtlControlFlowGraph lifted = InvertFinalScanInsnLinesToCfg(
        build_asm(/*extra_non_mem_v0_use_before_call=*/false), sideband, {}, {}, &prof);
    ASSERT_FALSE(lifted.rpo_blocks.empty());
    bool first_load_has_dead_v0 = false;
    bool second_load_has_dead_v0 = false;
    bool trapped_load_has_dead_v0 = false;
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      const bool has_dead_v0 = absl::StrContains(insn, "REG_DEAD (reg:SI 2 v0)");
      if (absl::StrContains(insn, "(set (reg:SI 16 s0) (mem/u:SI (reg:SI 2 v0)))")) {
        first_load_has_dead_v0 = has_dead_v0;
      }
      if (absl::StrContains(
              insn, "(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4))))")) {
        second_load_has_dead_v0 = has_dead_v0;
      }
      if (absl::StrContains(
              insn, "(set (reg:SI 9 t1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 8))))")) {
        trapped_load_has_dead_v0 = has_dead_v0;
      }
    }
    EXPECT_FALSE(first_load_has_dead_v0) << lifted.FormatSummary();
    EXPECT_TRUE(second_load_has_dead_v0) << lifted.FormatSummary();
    EXPECT_FALSE(trapped_load_has_dead_v0) << lifted.FormatSummary();
  }

  // Negative: when a non-`mem/u` consumer (`addu $10, $9, $2`) uses `$2` before
  // the call, neither hoisted prologue load receives `(REG_DEAD (reg:SI 2 v0))`.
  {
    const RtlControlFlowGraph lifted = InvertFinalScanInsnLinesToCfg(
        build_asm(/*extra_non_mem_v0_use_before_call=*/true), sideband, {}, {}, &prof);
    ASSERT_FALSE(lifted.rpo_blocks.empty());
    for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "(set (reg:SI 16 s0) (mem/u:SI") ||
          absl::StrContains(insn, "(set (reg:SI 17 s1) (mem/u:SI")) {
        EXPECT_FALSE(absl::StrContains(insn, "REG_DEAD (reg:SI 2 v0)")) << lifted.FormatSummary();
      }
    }
  }
}

TEST_F(InvertFinalScanInsnTest, IndexedArrayAccessMarksInStructWithoutUnsoundSpillUnchanging) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mcpu=r4000"};
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;
  sideband.saved_gpr_regnos = {31};

  // Positive: `base + (idx << 2)` SImode load/store gets `mem/s`.
  // Negative: QImode `sb` does NOT get `mem/s`, and a mutable `int *a` whose
  // `addu` precedes a non-prologue `$sp` spill store does NOT receive `/u`
  // (`RTX_UNCHANGING_P`) without genuine scheduling/hoisting proof.
  const std::vector<std::string> lines = {
      "subu\t$sp,$sp,32", "sll\t$8,$5,2",     "addu\t$9,$8,$4",
      "sw\t$31,24($sp)",  "lw\t$10,0($9)",    "addu\t$11,$8,$6",
      "sw\t$10,16($sp)",  // non-prologue stack spill store
      "lw\t$12,0($11)",   // mutable pointer after spill store -> `mem/s`, NOT `/u`
      "addu\t$13,$sp,8",  "addu\t$14,$8,$13",
      "sw\t$12,20($sp)",  // non-prologue stack spill store
      "lw\t$15,0($14)",   // stack-derived base -> `mem/s`, NOT `/u`
      "sw\t$12,0($9)",    // indexed SImode store -> `mem/s`
      "sb\t$12,0($9)",    // QImode store -> NOT `mem/s`
      "addu\t$2,$12,$15",
  };
  const RtlControlFlowGraph lifted = InvertFinalScanInsnLinesToCfg(lines, sideband, {}, {}, &prof);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  bool saw_prologue_sep_load = false;
  bool saw_spill_sep_load = false;
  bool saw_stack_base_load = false;
  bool saw_si_store = false;
  bool saw_qi_store = false;
  for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
    if (absl::StrContains(insn, "(set (reg:SI 10 t2) (mem")) {
      saw_prologue_sep_load = true;
      EXPECT_TRUE(absl::StrContains(insn, "(mem/s:SI (reg:SI 9 t1))")) << insn;
      EXPECT_FALSE(absl::StrContains(insn, "/u:")) << insn;
    }
    if (absl::StrContains(insn, "(set (reg:SI 12 t4) (mem")) {
      saw_spill_sep_load = true;
      EXPECT_TRUE(absl::StrContains(insn, "(mem/s:SI (reg:SI 11 t3))")) << insn;
      EXPECT_FALSE(absl::StrContains(insn, "/u:")) << insn;
    }
    if (absl::StrContains(insn, "(set (reg:SI 15 t7) (mem")) {
      saw_stack_base_load = true;
      EXPECT_TRUE(absl::StrContains(insn, "(mem/s:SI (reg:SI 14 t6))")) << insn;
      EXPECT_FALSE(absl::StrContains(insn, "/u:")) << insn;
    }
    if (absl::StrContains(insn, "(mem/s:SI (reg:SI 9 t1)) (reg:SI 12 t4)")) {
      saw_si_store = true;
    }
    if (absl::StrContains(insn, "(mem:QI (reg:SI 9 t1))")) {
      saw_qi_store = true;
    }
  }
  EXPECT_TRUE(saw_prologue_sep_load) << lifted.FormatSummary();
  EXPECT_TRUE(saw_spill_sep_load) << lifted.FormatSummary();
  EXPECT_TRUE(saw_stack_base_load) << lifted.FormatSummary();
  EXPECT_TRUE(saw_si_store) << lifted.FormatSummary();
  EXPECT_TRUE(saw_qi_store) << lifted.FormatSummary();
}

TEST_F(InvertFinalScanInsnTest, PrologueHoistedUnchangingPtrPropagatesThroughPlusRegAndPriority) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mips1"};
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;
  sideband.saved_gpr_regnos = {31, 16};

  // Positive 1: `$4` is proven `unchanging` via prologue-hoisted load `lw $8, 0($4)`
  // before `sw $31, 24($sp)`; `addu $9, $4, $5` (`plus(reg, reg)`) propagates
  // `unchanging_ptr_regs` so `lw $10, 0($9)` receives `mem/u:SI`.
  // Positive 2: `mul.s $f10, $f12, $f14` (fmul SF latency 7 on -mips1) ->
  // `s.s $f10, 8($sp)` has `data_prio = 7`, followed by `lw $16, 0($6)`
  // (dst `$16` unread in block) and immediately followed by independent
  // `add.s $f0, $f12, $f14` (`data_prio = 1 < 7`, fresh dst `$f0`): proves
  // `lw $16, 0($6)` must carry `/u` (`sched.c:828, 1488, 2395`).
  const std::vector<std::string> lines = {
      "subu\t$sp,$sp,32", "lw\t$8,0($4)",         "sw\t$31,24($sp)",       "sw\t$16,20($sp)",
      "addu\t$9,$4,$5",   "lw\t$10,0($9)",        "mul.s\t$f10,$f12,$f14", "s.s\t$f10,8($sp)",
      "lw\t$16,0($6)",    "add.s\t$f0,$f12,$f14", "addu\t$2,$8,$10",
  };
  const RtlControlFlowGraph lifted = InvertFinalScanInsnLinesToCfg(lines, sideband, {}, {}, &prof);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  bool saw_plus_reg_propagated = false;
  bool saw_priority_inverted = false;
  for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
    if (absl::StrContains(insn, "(set (reg:SI 10 t2) (mem")) {
      saw_plus_reg_propagated = true;
      EXPECT_TRUE(absl::StrContains(insn, "(mem/u:SI (reg:SI 9 t1))")) << insn;
    }
    if (absl::StrContains(insn, "(set (reg:SI 16 s0) (mem")) {
      saw_priority_inverted = true;
      EXPECT_TRUE(absl::StrContains(insn, "(mem/u:SI (reg:SI 6 a2))")) << insn;
    }
  }
  EXPECT_TRUE(saw_plus_reg_propagated) << lifted.FormatSummary();
  EXPECT_TRUE(saw_priority_inverted) << lifted.FormatSummary();
}

TEST_F(InvertFinalScanInsnTest, PriorityInvertedUnchangingLoadRequiresPureDataCone) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mips1"};
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;
  sideband.saved_gpr_regnos = {31, 16};

  // Negative: `S (s.s $f10, 8($sp))` -> `P (lw $11, 0($5))` -> `L (lw $16, 0($6))`
  // -> `X (addu $12, $11, $4)`. Even though `X` has no direct `mem` and writes
  // fresh `$12` with `data_prio[X] = 2 < data_prio[S] = 7`, `X` transitively
  // reads `P` (a memory load), so `pure_data_cone[X]` is false and `L` must NOT
  // receive `/u` (`sched.c:828, 1488, 2395`).
  const std::vector<std::string> lines = {
      "subu\t$sp,$sp,32",      "sw\t$31,24($sp)",  "sw\t$16,20($sp)",
      "mul.s\t$f10,$f12,$f14", "s.s\t$f10,8($sp)", "lw\t$11,0($5)",
      "lw\t$16,0($6)",         "addu\t$12,$11,$4", "addu\t$2,$12,$4",
  };
  const RtlControlFlowGraph lifted = InvertFinalScanInsnLinesToCfg(lines, sideband, {}, {}, &prof);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  bool saw_mutable_load = false;
  for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
    if (absl::StrContains(insn, "(set (reg:SI 16 s0) (mem")) {
      saw_mutable_load = true;
      EXPECT_FALSE(absl::StrContains(insn, "mem/u")) << insn;
    }
  }
  EXPECT_TRUE(saw_mutable_load) << lifted.FormatSummary();
}

TEST_F(InvertFinalScanInsnTest, VariadicPrologueArgHomeStoresNotFoldedIntoDf) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mcpu=r4000"};
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;

  // Negative: pre-`$sp`-decrement variadic `$a2/$a3` home stores at `8($sp)` /
  // `12($sp)` (`mips.c:4998-5020`) must remain separate `SImode` stores.
  // Positive: post-`$sp`-decrement paired stores at `16($sp)` / `20($sp)` fold
  // into a single `mem:DF` store.
  const std::vector<std::string> lines = {
      "sw\t$6,8($sp)",  "sw\t$7,12($sp)", "subu\t$sp,$sp,32",
      "sw\t$4,16($sp)", "sw\t$5,20($sp)", "l.d\t$f0,16($sp)",
  };
  const RtlControlFlowGraph lifted = InvertFinalScanInsnLinesToCfg(lines, sideband, {}, {}, &prof);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  bool saw_a2_si = false;
  bool saw_a3_si = false;
  bool saw_post_sp_di_or_df = false;
  for (const std::string& insn : lifted.rpo_blocks[0].instructions) {
    if (absl::StrContains(insn,
                          "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 8))) (reg:SI 6 a2))")) {
      saw_a2_si = true;
    }
    if (absl::StrContains(insn,
                          "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 12))) (reg:SI 7 a3))")) {
      saw_a3_si = true;
    }
    if (absl::StrContains(insn, "(mem:DI (plus:SI (reg:SI 29 sp) (const_int 16)))") ||
        absl::StrContains(insn, "(mem:DF (plus:SI (reg:SI 29 sp) (const_int 16)))")) {
      saw_post_sp_di_or_df = true;
    }
  }
  EXPECT_TRUE(saw_a2_si) << lifted.FormatSummary();
  EXPECT_TRUE(saw_a3_si) << lifted.FormatSummary();
  EXPECT_TRUE(saw_post_sp_di_or_df) << lifted.FormatSummary();
}

TEST_F(InvertFinalScanInsnTest, RestoreNoConflictExtendClobbersZeroExtendHoistAndMuldi3Subregs) {
  // Positive + negative for Fix 42D (`is_zero_extend_hi_after_lo` subreg wrap
  // + load-stall clobber placement), Fix 42H-2 (callee-saved clobber hoisted
  // across call into unfilled `const_double -> FP arith` stall), and Fix 42H-3
  // (`muldi3` 3-multiply `SUBREG` operand restoration).
  auto make_zext_cfg = [](bool include_load_before_alu) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    RtlBasicBlock blk;
    blk.id = 0;
    if (include_load_before_alu) {
      blk.instructions.push_back("(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))");
      blk.instructions.push_back("(set (reg:SI 8 t0) (xor:SI (reg:SI 8 t0) (const_int 7)))");
    } else {
      blk.instructions.push_back("(set (reg:SI 8 t0) (xor:SI (reg:SI 4 a0) (const_int 7)))");
    }
    blk.instructions.push_back("(set (reg:SI 11 t3) (reg:SI 8 t0))");
    blk.instructions.push_back("(set (reg:SI 10 t2) (const_int 0))");
    blk.instructions.push_back(
        "(parallel[ (set (reg:DI 2 v0) (plus:DI (reg:DI 6 a2) (reg:DI 10 t2))) (clobber (reg:SI 4 "
        "a0)) ] )");
    blk.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(std::move(blk));
    return cfg;
  };
  // Positive 42D: load-stalled ALU producer places clobber before ALU and wraps subregs.
  {
    RtlControlFlowGraph cfg = make_zext_cfg(/*include_load_before_alu=*/true);
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    ASSERT_GE(cfg.rpo_blocks[0].instructions.size(), 6u);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[1], "(clobber (reg:DI 10 t2))"))
        << cfg.FormatSummary();
    EXPECT_TRUE(
        absl::StrContains(cfg.rpo_blocks[0].instructions[3], "(subreg:SI (reg:DI 10 t2) 1)"))
        << cfg.FormatSummary();
    EXPECT_TRUE(
        absl::StrContains(cfg.rpo_blocks[0].instructions[4], "(subreg:SI (reg:DI 10 t2) 0)"))
        << cfg.FormatSummary();
  }
  // Negative 42D: without a preceding load stall, clobber stays immediately before `lo`;
  // and when `dh == src_reg` (destination tied to source), subregs are NOT wrapped without
  // `REG_NO_CONFLICT`.
  {
    RtlControlFlowGraph cfg = make_zext_cfg(/*include_load_before_alu=*/false);
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    ASSERT_GE(cfg.rpo_blocks[0].instructions.size(), 5u);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[1], "(clobber (reg:DI 10 t2))"))
        << cfg.FormatSummary();
  }
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    RtlBasicBlock blk;
    blk.id = 0;
    blk.instructions = {
        "(set (reg:SI 8 t0) (reg:SI 5 a1))",
        "(set (reg:SI 9 t1) (reg:SI 8 t0))",
        "(set (reg:SI 8 t0) (const_int 0))",
        "(parallel[ (set (reg:DI 2 v0) (plus:DI (reg:DI 6 a2) (reg:DI 8 t0))) (clobber (reg:SI 4 "
        "a0)) ] )",
    };
    blk.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(std::move(blk));
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    ASSERT_GE(cfg.rpo_blocks[0].instructions.size(), 5u);
    EXPECT_FALSE(absl::StrContains(cfg.rpo_blocks[0].instructions[2], "subreg:SI"))
        << cfg.FormatSummary();
    EXPECT_FALSE(absl::StrContains(cfg.rpo_blocks[0].instructions[3], "subreg:SI"))
        << cfg.FormatSummary();
  }

  // Positive + negative 42H-2: callee-saved `s2/s3` clobber hoists across call
  // into unfilled `const_double -> FP arith` stall; caller-saved `t2/t3` does NOT.
  auto make_cross_call_clobber_cfg = [](int dh, int dl) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    RtlBasicBlock blk;
    blk.id = 0;
    blk.instructions = {
        "(set (reg:SI 4 a0) (reg:SI 22 s6))",
        "(set (reg:SF 52 $f20) (const_double:SF (cc0) 0 1071644672))",
        "(set (reg:SF 52 $f20) (mult:SF (reg:SF 54 $f22) (reg:SF 52 $f20)))",
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int "
        "16))) (clobber (reg:SI 31 ra)) ] )",
        absl::StrCat("(set (reg:SI ", dl, " ", (dl == 19 ? "s3" : "t3"), ") (reg:SI 2 v0))"),
        absl::StrCat("(set (reg:SI ", dh, " ", (dh == 18 ? "s2" : "t2"),
                     ") (ashiftrt:SI (reg:SI 2 v0) (const_int 31)))"),
        absl::StrCat("(set (reg/i:DI 2 v0) (xor:DI (reg:DI 2 v0) (reg:DI ", dh, " ",
                     (dh == 18 ? "s2" : "t2"), ")))"),
    };
    blk.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(std::move(blk));
    return cfg;
  };
  {
    RtlControlFlowGraph cfg = make_cross_call_clobber_cfg(18, 19);
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    ASSERT_GE(cfg.rpo_blocks[0].instructions.size(), 8u);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[2], "(clobber (reg:DI 18 s2))"))
        << cfg.FormatSummary();
  }
  {
    RtlControlFlowGraph cfg = make_cross_call_clobber_cfg(10, 11);
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    ASSERT_GE(cfg.rpo_blocks[0].instructions.size(), 8u);
    EXPECT_FALSE(absl::StrContains(cfg.rpo_blocks[0].instructions[2], "(clobber (reg:DI 10 t2))"))
        << cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[4], "(clobber (reg:DI 10 t2))"))
        << cfg.FormatSummary();
  }

  // Positive + negative 42H-3: 32-bit `muldi3` 3-multiply sequence restores
  // `(subreg:SI (reg:DI b_hi) 0/1)` when `b_hi` (`v0`) is clobbered inside the
  // `last_mul -> mflo` window under `runs_sched2`, and skips when `b_hi` is
  // only clobbered after `mflo` or under `!runs_sched2`.
  auto make_muldi3_cfg = [](bool clobber_before_mflo) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    RtlBasicBlock blk;
    blk.id = 0;
    blk.instructions = {
        "(parallel[ (set (reg:DI 64 hi) (mult:DI (zero_extend:DI (reg:SI 23 s7)) (zero_extend:DI "
        "(reg:SI 3 v1)))) (clobber (reg:SI 66 accum)) ] )",
        "(set (reg:DI 18 s2) (reg:DI 64 hi))",
        "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 23 s7) (reg:SI 2 v0))) (clobber (reg:SI "
        "64 hi)) (clobber (reg:SI 66 accum)) ] )",
        "(set (reg:SI 9 t1) (reg:SI 65 lo))",
        "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 3 v1) (reg:SI 22 s6))) (clobber (reg:SI "
        "64 hi)) (clobber (reg:SI 66 accum)) ] )",
    };
    const std::string clobber_v0 =
        "(parallel[ (set (reg:DI 6 a2) (plus:DI (reg:DI 6 a2) (const_int 7))) (clobber (reg:SI "
        "2 v0)) ] )";
    const std::string mflo = "(set (reg:SI 8 t0) (reg:SI 65 lo))";
    if (clobber_before_mflo) {
      blk.instructions.push_back(clobber_v0);
      blk.instructions.push_back(mflo);
    } else {
      blk.instructions.push_back(mflo);
      blk.instructions.push_back(clobber_v0);
    }
    blk.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(std::move(blk));
    return cfg;
  };
  {
    RtlControlFlowGraph cfg = make_muldi3_cfg(/*clobber_before_mflo=*/true);
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[0], "(subreg:SI (reg:DI 2 v0) 1)"))
        << cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[4], "(subreg:SI (reg:DI 2 v0) 1)"))
        << cfg.FormatSummary();
    EXPECT_FALSE(absl::StrContains(cfg.rpo_blocks[0].instructions[4], "(subreg:SI (reg:DI 22 s6)"))
        << cfg.FormatSummary();
  }
  {
    RtlControlFlowGraph cfg = make_muldi3_cfg(/*clobber_before_mflo=*/false);
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    EXPECT_FALSE(absl::StrContains(cfg.rpo_blocks[0].instructions[4], "subreg:SI"))
        << cfg.FormatSummary();
  }
  {
    RtlControlFlowGraph cfg = make_muldi3_cfg(/*clobber_before_mflo=*/true);
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/false);
    EXPECT_FALSE(absl::StrContains(cfg.rpo_blocks[0].instructions[4], "subreg:SI"))
        << cfg.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest, Adddi3ScratchFoldsBeforeCallUnlessReadByNonCall) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mcpu=r4000"};
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;
  sideband.saved_gpr_regnos = {31};

  // Positive: `adddi3_internal_1` whose scratch `$5` (`$a1`) immediately
  // precedes a `jal` folds cleanly because conservative call arg uses do not
  // pin a dead internal carry scratch.
  const std::vector<std::string> fold_lines = {
      "subu\t$sp,$sp,32", "sw\t$31,16($sp)",  "addu\t$21,$21,$3", "sltu\t$5,$21,$3",
      "addu\t$20,$20,$2", "addu\t$20,$20,$5", "move\t$4,$22",     "jal\tRecordStage",
  };
  const RtlControlFlowGraph folded =
      InvertFinalScanInsnLinesToCfg(fold_lines, sideband, {}, {}, &prof);
  ASSERT_EQ(folded.rpo_blocks.size(), 1u);
  bool saw_adddi = false;
  for (const std::string& insn : folded.rpo_blocks[0].instructions) {
    if (absl::StrContains(insn, "(set (reg:DI 20 s4) (plus:DI (reg:DI 20 s4) (reg:DI 2 v0)))") &&
        absl::StrContains(insn, "(clobber (reg:SI 5 a1))")) {
      saw_adddi = true;
    }
  }
  EXPECT_TRUE(saw_adddi) << folded.FormatSummary();

  // Negative: when `$5` is genuinely read by a subsequent non-call instruction,
  // `adddi3_internal_1` must NOT fold.
  std::vector<std::string> nofold_lines = fold_lines;
  nofold_lines.insert(nofold_lines.begin() + 6, "addu\t$4,$22,$5");
  const RtlControlFlowGraph not_folded =
      InvertFinalScanInsnLinesToCfg(nofold_lines, sideband, {}, {}, &prof);
  ASSERT_EQ(not_folded.rpo_blocks.size(), 1u);
  for (const std::string& insn : not_folded.rpo_blocks[0].instructions) {
    EXPECT_FALSE(absl::StrContains(insn, "(plus:DI (reg:DI 20 s4) (reg:DI 2 v0))"))
        << not_folded.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest, MdOutputSpillMarksUnchangingLoadsAndTransfersInheritedDeadNotes) {
  CompilationProfile o2_prof;
  o2_prof.optimize_level = 2;
  o2_prof.c_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;
  sideband.saved_gpr_regnos = {16, 17, 18, 19};

  // Positive (`reload1.c:5186-5258, 6583, 6631`, `sched.c:817-839, 852`):
  // `mult $4,$5` -> dependent load `lw $4,0($6)` (anti-dependent on `$4` read
  // by `mult`, proving it originated after `sw $16/$17, 16($sp)`) + independent
  // load `lw $8,0($7)` -> `mfhi/mflo $16/$17` -> `sw $16/$17, 16($sp)` (never
  // read) + inherited `move $2/$3, $16/$17` -> matching-operand `adddi3`
  // consumer on `$2/$3`. Stage 0 marks ONLY the `must_follow_m` load
  // `lw $4,0($6)` as `mem/u` (leaving independent `lw $8,0($7)` mutable so
  // `anti_dependence` with later stores is preserved) and transfers
  // `REG_DEAD (reg:DI 16)` to `REG_DEAD (mem:DI 16($sp))` on `adddi3`.
  const std::vector<std::string> base_lines = {
      "subu\t$sp,$sp,32", "sw\t$16,24($sp)", "mult\t$4,$5",     "lw\t$4,0($6)",    "lw\t$8,0($7)",
      "mfhi\t$16",        "mflo\t$17",       "sw\t$16,16($sp)", "sw\t$17,20($sp)", "move\t$2,$16",
      "move\t$3,$17",     "addu\t$3,$3,$19", "sltu\t$9,$3,$19", "addu\t$2,$2,$18", "addu\t$2,$2,$9",
      "addu\t$2,$2,$4",   "addu\t$2,$2,$8",
  };
  {
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(base_lines, sideband, {}, {}, &o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    const std::string summary = lifted.FormatSummary();
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SI 4 a0) (mem/u:SI (reg:SI 6 a2)))"))
        << summary;
    // Negative (F2 Class-B soundness): independent `lw $8,0($7)` in
    // `(m_idx, s1_idx)` could have preceded `mult` in `.09.greg`, so it must
    // NOT be marked `mem/u`.
    EXPECT_FALSE(absl::StrContains(summary, "(set (reg:SI 8 t0) (mem/u:SI (reg:SI 7 a3)))"))
        << summary;
    EXPECT_TRUE(
        absl::StrContains(summary, "REG_DEAD (mem:DI (plus:SI (reg:SI 29 sp) (const_int 16)))"))
        << summary;
  }

  // Negative 1: an intervening non-stack store before `lw $4,0($6)` prevents
  // marking `lw $4,0($6)` as `mem/u` (`sched.c:817-839`).
  {
    std::vector<std::string> with_store = base_lines;
    with_store.insert(with_store.begin() + 3, "sw\t$5,0($7)");
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(with_store, sideband, {}, {}, &o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_FALSE(
        absl::StrContains(lifted.FormatSummary(), "(set (reg:SI 4 a0) (mem/u:SI (reg:SI 6 a2)))"))
        << lifted.FormatSummary();
  }

  // Negative 2: when the stack slot `16($sp)` IS read later, `REG_DEAD (mem:DI)`
  // is NOT transferred onto the `adddi3` consumer.
  {
    std::vector<std::string> with_slot_read = base_lines;
    with_slot_read.push_back("lw\t$10,16($sp)");
    with_slot_read.push_back("addu\t$2,$2,$10");
    const RtlControlFlowGraph lifted =
        InvertFinalScanInsnLinesToCfg(with_slot_read, sideband, {}, {}, &o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_FALSE(absl::StrContains(lifted.FormatSummary(),
                                   "REG_DEAD (mem:DI (plus:SI (reg:SI 29 sp) (const_int 16)))"))
        << lifted.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest, StripsInheritedCrossBankFpReloadDeadNotesOnlyWithoutSched2) {
  // Mirrors `reload1.c:5186-5258` (`StripInheritedCrossBankFpReloadDeadNotes`):
  // at `-O1` (`!RunsSched2()`), an inherited cross-bank GPR->FPR reload
  // (`mtc1 $s1,$f6` -> `add.s` -> `sub.s`) has no `REG_DEAD $f6` when the
  // backing GPR `$s1` remains live after the FP consumers (positive), whereas
  // `-O2` (`RunsSched2()`) or a dead backing GPR keeps `REG_DEAD $f6` (negative).
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;
  sideband.saved_gpr_regnos = {17};

  const std::vector<std::string> live_backing_lines = {
      "subu\t$sp,$sp,32",   "sw\t$17,16($sp)", "add.s\t$f6,$f0,$f2", "mfc1\t$17,$f6",
      "sub.s\t$f4,$f6,$f2", "mtc1\t$17,$f8",   "add.s\t$f0,$f4,$f8",
  };
  const std::vector<std::string> dead_backing_lines = {
      "subu\t$sp,$sp,32", "sw\t$17,16($sp)",    "add.s\t$f6,$f0,$f2",
      "mfc1\t$17,$f6",    "sub.s\t$f0,$f6,$f2",
  };

  CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips2"}};
  CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  auto has_dead_f6_on_sub = [](const RtlControlFlowGraph& cfg) {
    for (const std::string& insn : cfg.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, "minus:SF") &&
          absl::StrContains(insn, "REG_DEAD (reg:SF 38 $f6)")) {
        return true;
      }
    }
    return false;
  };

  // Positive (`-O1`, backing `$17` live after `sub.s`): `REG_DEAD $f6` stripped.
  const RtlControlFlowGraph o1_live =
      InvertFinalScanInsnLinesToCfg(live_backing_lines, sideband, {}, {}, &o1_prof);
  ASSERT_EQ(o1_live.rpo_blocks.size(), 1u);
  EXPECT_FALSE(has_dead_f6_on_sub(o1_live)) << o1_live.FormatSummary();

  // Negative 1 (`-O2`, `RunsSched2()`): `REG_DEAD $f6` preserved.
  const RtlControlFlowGraph o2_live =
      InvertFinalScanInsnLinesToCfg(live_backing_lines, sideband, {}, {}, &o2_prof);
  ASSERT_EQ(o2_live.rpo_blocks.size(), 1u);
  EXPECT_TRUE(has_dead_f6_on_sub(o2_live)) << o2_live.FormatSummary();

  // Negative 2 (`-O1`, backing `$17` dead at `sub.s`): `REG_DEAD $f6` preserved.
  const RtlControlFlowGraph o1_dead =
      InvertFinalScanInsnLinesToCfg(dead_backing_lines, sideband, {}, {}, &o1_prof);
  ASSERT_EQ(o1_dead.rpo_blocks.size(), 1u);
  EXPECT_TRUE(has_dead_f6_on_sub(o1_dead)) << o1_dead.FormatSummary();
}

TEST_F(InvertFinalScanInsnTest, PromotesPureSfModeCalleeSavedGprsAndLeavesIntegerGprsUntouched) {
  // Mirrors `mips.md:3449-3460` (`movsf_internal1`, `PromoteGprAllocatedSfModeRegs`):
  // callee-saved GPRs whose non-prologue/epilogue uses are purely `SFmode`
  // (`mtc1`/`mfc1`) + `lw`/`lui` defs are promoted to `SFmode` (with `lui`
  // folded into IEEE-754 `const_double:SF`), while integer-used callee-saved
  // GPRs remain `SImode`.
  CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  RtlSidebandState sideband;
  sideband.frame_size_bytes = 32;
  sideband.saved_gpr_regnos = {17, 18, 19};

  const std::vector<std::string> lines = {
      "subu\t$sp,$sp,32", "sw\t$17,16($sp)",    "sw\t$18,20($sp)", "sw\t$19,24($sp)",
      "lw\t$17,0($4)",    "lui\t$18,0x3f80",    "lw\t$19,4($4)",   "mtc1\t$17,$f4",
      "mtc1\t$18,$f6",    "add.s\t$f0,$f4,$f6", "addu\t$2,$19,$5",
  };
  const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(lines, sideband, {}, {}, &o2_prof);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
  const std::string summary = cfg.FormatSummary();
  EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SF 17 s1) (mem:SF")) << summary;
  EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SF 18 s2) (const_double:SF")) << summary;
  EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SI 19 s3) (mem:SI")) << summary;
  EXPECT_FALSE(absl::StrContains(summary, "(reg:SF 19 s3)")) << summary;
}

TEST_F(InvertFinalScanInsnTest,
       MarksSunkOutputReloadCrossedUnchangingLoadsOnlyWithoutFpOrVaryingStores) {
  // Mirrors `reload1.c:6583, 6631` + `sched.c:837-839`
  // (`MarkSunkOutputReloadCrossedUnchangingLoads`):
  // when `!frame_pointer_needed` and no varying non-stack store exists in the
  // function, a non-stack varying load (`l.s $f2, 4($4)`) sitting between a
  // producer (`add.s $f8, $f12, $f14`) and a sunk read-back `$sp` output-reload
  // store (`s.s $f8, 20($sp)` later read by `l.s $f6, 20($sp)`) must have
  // carried `/u` (`MEM_UNCHANGING_P`) so `sched2` could sink the `$sp` store
  // past it.
  const CompilationProfile mips1_o2{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  auto build_lines = [](bool include_varying_store_before, bool make_load_dependent_on_prod,
                        bool include_varying_store_after) {
    std::vector<std::string> lines = {
        "subu\t$sp,$sp,40",
        "sw\t$31,36($sp)",
    };
    if (include_varying_store_before) {
      lines.push_back("s.s\t$f12,0($5)");
    }
    lines.push_back("add.s\t$f8,$f12,$f14");
    // When `make_load_dependent_on_prod` is true, overwrite `$f14` (read by
    // `prod`), establishing an anti-dependence (`must_follow_prod == true`)
    // that proves the load originated after `s.s $f8, 20($sp)`.
    lines.push_back(make_load_dependent_on_prod ? "l.s\t$f14,4($4)" : "l.s\t$f2,4($4)");
    lines.push_back("s.s\t$f8,20($sp)");
    if (include_varying_store_after) {
      lines.push_back("s.s\t$f8,0($5)");
    }
    lines.push_back(make_load_dependent_on_prod ? "add.s\t$f4,$f8,$f14" : "add.s\t$f4,$f8,$f2");
    lines.push_back("l.s\t$f6,20($sp)");
    lines.push_back("add.s\t$f0,$f6,$f4");
    return lines;
  };

  auto crossed_load_has_u = [](const RtlControlFlowGraph& cfg, std::string_view dst_reg) {
    for (const std::string& insn : cfg.rpo_blocks[0].instructions) {
      if (absl::StrContains(insn, absl::StrCat("(set ", dst_reg, " (mem"))) {
        return absl::StrContains(insn, "(mem/u:SF ");
      }
    }
    return false;
  };

  // Positive: `!frame_pointer_needed`, no varying store, big-endian aligned
  // spill slot `20($sp)` (`20 % 8 == 4 == 8 - 4`), and load anti-dependent on
  // `prod` (`must_follow_prod == true`) -> crossed load gets `/u`.
  {
    RtlSidebandState sideband;
    sideband.frame_size_bytes = 40;
    sideband.saved_gpr_regnos = {31};
    sideband.frame_pointer_needed = false;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(
        build_lines(/*include_varying_store_before=*/false, /*make_load_dependent_on_prod=*/true,
                    /*include_varying_store_after=*/false),
        sideband, {}, {}, &mips1_o2);
    ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
    EXPECT_TRUE(crossed_load_has_u(cfg, "(reg:SF 46 $f14)")) << cfg.FormatSummary();
  }

  // Negative 1: `frame_pointer_needed == true` -> crossed load does NOT get `/u`.
  {
    RtlSidebandState sideband;
    sideband.frame_size_bytes = 40;
    sideband.saved_gpr_regnos = {30, 31};
    sideband.frame_pointer_needed = true;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(
        build_lines(/*include_varying_store_before=*/false, /*make_load_dependent_on_prod=*/true,
                    /*include_varying_store_after=*/false),
        sideband, {}, {}, &mips1_o2);
    ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
    EXPECT_FALSE(crossed_load_has_u(cfg, "(reg:SF 46 $f14)")) << cfg.FormatSummary();
  }

  // Negative 2: preceding varying non-stack store (`s.s $f12, 0($5)`) ->
  // crossed load does NOT get `/u`.
  {
    RtlSidebandState sideband;
    sideband.frame_size_bytes = 40;
    sideband.saved_gpr_regnos = {31};
    sideband.frame_pointer_needed = false;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(
        build_lines(/*include_varying_store_before=*/true, /*make_load_dependent_on_prod=*/true,
                    /*include_varying_store_after=*/false),
        sideband, {}, {}, &mips1_o2);
    ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
    EXPECT_FALSE(crossed_load_has_u(cfg, "(reg:SF 46 $f14)")) << cfg.FormatSummary();
  }

  // Negative 3 (`sched.c:828, 852`, `rtlanal.c:101-102`): an independent
  // mutable load (`l.s $f2, 4($4)`, `!must_follow_prod`) in `(prod, s)`
  // followed by a varying store after `s` does NOT receive `/u`.
  {
    RtlSidebandState sideband;
    sideband.frame_size_bytes = 40;
    sideband.saved_gpr_regnos = {31};
    sideband.frame_pointer_needed = false;
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(
        build_lines(/*include_varying_store_before=*/false, /*make_load_dependent_on_prod=*/false,
                    /*include_varying_store_after=*/true),
        sideband, {}, {}, &mips1_o2);
    ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
    EXPECT_FALSE(crossed_load_has_u(cfg, "(reg:SF 34 $f2)")) << cfg.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest,
       RestoresDiWordSetccSubregsFoldsAshrdi32Adddi1AndReconstructsO0DiSubwords) {
  const CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips2", "-Wa,-g"}};
  const CompilationProfile o0_prof{.optimize_level = 0, .c_flags = {"-O0", "-mips2", "-Wa,-g"}};

  // 1. `final.c:1933, 2095-2107` (`RestoreAlterSubregNarrowingOperands`):
  //    `DImode` word `setcc` (`slt $2, $4, $6`) restores `(subreg:SI (reg:DI 4 a0) 0)`
  //    and `(subreg:SI (reg:DI 6 a2) 0)` when both operands reach as `DI` word 0,
  //    while an unfolded `subdi3` carry `sltu` followed by `subu` stays bare `SI`.
  {
    RtlSidebandState sb;
    const std::vector<std::string> pos_lines = {
        "lw\t$4,0($16)", "lw\t$5,4($16)",  "lw\t$6,8($16)",  "lw\t$7,12($16)",
        "addu\t$5,$5,1", "sltu\t$8,$5,1",  "addu\t$4,$4,$8", "addu\t$7,$7,1",
        "sltu\t$8,$7,1", "addu\t$6,$6,$8", "slt\t$2,$4,$6",
    };
    const RtlControlFlowGraph pos = InvertFinalScanInsnLinesToCfg(pos_lines, sb, {}, {}, &o1_prof);
    const std::string pos_s = pos.FormatSummary();
    EXPECT_TRUE(
        absl::StrContains(pos_s, "(lt:SI (subreg:SI (reg:DI 4 a0) 0) (subreg:SI (reg:DI 6 a2) 0))"))
        << pos_s;

    const std::vector<std::string> neg_lines = {
        "lw\t$4,0($16)",  "lw\t$5,4($16)",  "lw\t$6,8($16)", "lw\t$7,12($16)", "addu\t$5,$5,1",
        "sltu\t$8,$5,1",  "addu\t$4,$4,$8", "addu\t$7,$7,1", "sltu\t$8,$7,1",  "addu\t$6,$6,$8",
        "sltu\t$2,$5,$7", "subu\t$4,$4,$2", "move\t$2,$4",
    };
    const RtlControlFlowGraph neg = InvertFinalScanInsnLinesToCfg(neg_lines, sb, {}, {}, &o1_prof);
    const std::string neg_s = neg.FormatSummary();
    EXPECT_FALSE(absl::StrContains(neg_s, "(ltu:SI (subreg:SI (reg:DI 4 a0) 1)")) << neg_s;
  }

  // 2. `config/mips/mips.md:3983-3996, 4015-4028` (`ashrdi3_internal2` `k_lo == 0`)
  //    and `config/mips/mips.md:482-494` (`adddi3_internal_2` `C == 1` via `sltiu/eq 0`):
  //    `sra $5, $6, 0; sra $4, $6, 31` folds to `ashiftrt:DI 32` both when `$7`
  //    is dead and when `$7` stays live in `xor $2, $7, $5` (positive), while
  //    narrow sign-extend `sll $6, $6, 16; sra $5, $6, 16; sra $4, $6, 31`
  //    stays unfolded (negative).
  //    `addu $3, $5, 1; sltu $8, $3, 1; addu $2, $4, $8` folds to `plus:DI ... 1`.
  {
    RtlSidebandState sb;
    const std::vector<std::string> pos_lines = {
        "lw\t$6,0($16)", "lw\t$7,4($16)", "sra\t$5,$6,0",   "sra\t$4,$6,31",
        "addu\t$3,$5,1", "sltu\t$8,$3,1", "addu\t$2,$4,$8",
    };
    const RtlControlFlowGraph pos = InvertFinalScanInsnLinesToCfg(pos_lines, sb, {}, {}, &o0_prof);
    const std::string pos_s = pos.FormatSummary();
    EXPECT_TRUE(absl::StrContains(pos_s, "(ashiftrt:DI (reg:DI 6 a2) (const_int 32))")) << pos_s;
    EXPECT_TRUE(absl::StrContains(pos_s, "(plus:DI (reg:DI 4 a0) (const_int 1))")) << pos_s;

    const std::vector<std::string> live_lo_lines = {
        "lw\t$6,0($16)", "lw\t$7,4($16)", "sra\t$5,$6,0", "sra\t$4,$6,31", "xor\t$2,$7,$5",
    };
    const RtlControlFlowGraph live_lo =
        InvertFinalScanInsnLinesToCfg(live_lo_lines, sb, {}, {}, &o1_prof);
    const std::string live_lo_s = live_lo.FormatSummary();
    EXPECT_TRUE(absl::StrContains(live_lo_s, "(ashiftrt:DI (reg:DI 6 a2) (const_int 32))"))
        << live_lo_s;

    const std::vector<std::string> neg_lines = {
        "sll\t$6,$6,16",
        "sra\t$5,$6,16",
        "sra\t$4,$6,31",
        "xor\t$2,$7,$5",
    };
    const RtlControlFlowGraph neg = InvertFinalScanInsnLinesToCfg(neg_lines, sb, {}, {}, &o1_prof);
    const std::string neg_s = neg.FormatSummary();
    EXPECT_FALSE(absl::StrContains(neg_s, "(ashiftrt:DI (reg:DI 6 a2)")) << neg_s;
  }

  // 3. `-O0` `ReconstructO0FinalScanDeletions`:
  //    - `optabs.c:2445-2488`: `extendsidi2` `(clobber (reg:DI hr))` is placed
  //      immediately before the first word store (after `lw; sra`), and `used_after`
  //      permits `end_idx` to overwrite `(reg:DI hr)`.
  //    - `WrapO0SingleBlockDiSubregs`: wraps multi-insn `SI` subword reads of a
  //      computed `DI` def (`adddi3`).
  {
    RtlSidebandState sb;
    sb.frame_pointer_needed = true;
    sb.frame_size_bytes = 32;
    sb.saved_gpr_regnos = {30};
    const std::vector<std::string> lines = {
        "subu\t$sp,$sp,32", "sw\t$fp,24($sp)", "move\t$fp,$sp",   "lw\t$6,0($fp)",
        "sra\t$7,$6,31",    "lw\t$5,0($fp)",   "move\t$4,$7",     "addu\t$5,$5,3",
        "sltu\t$2,$5,3",    "addu\t$4,$4,$2",  "addu\t$8,$5,$16", "addu\t$9,$4,$17",
        "addu\t$2,$8,$9",   "move\t$sp,$fp",   "lw\t$fp,24($sp)", "addu\t$sp,$sp,32",
    };
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(lines, sb, {}, {}, &o0_prof);
    const std::string s = cfg.FormatSummary();
    const size_t sra_pos = s.find("(ashiftrt:SI ");
    const size_t clob_pos = s.find("(clobber (reg:DI 4 a0))");
    ASSERT_NE(sra_pos, std::string::npos) << s;
    ASSERT_NE(clob_pos, std::string::npos) << s;
    EXPECT_LT(sra_pos, clob_pos) << s;
    EXPECT_TRUE(absl::StrContains(s, "(subreg:SI (reg:DI 4 a0) 1)")) << s;
    EXPECT_TRUE(absl::StrContains(s, "(subreg:SI (reg:DI 4 a0) 0)")) << s;
  }
}

TEST_F(InvertFinalScanInsnTest,
       Stage0ClassBTighteningsAndMultiInsnDiExtendMuldi3SubregRestoration) {
  const CompilationProfile o0_prof{.optimize_level = 0, .c_flags = {"-O0", "-mips2", "-Wa,-g"}};
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  // 1. Negative for `reconstruct_o0_final_scan.cc` (`config/mips/mips.md:412-416,
  //    3722-3727`): `-O0` non-earlyclobber `DImode` ops (`subdi3`, constant-count
  //    `ashldi3`) must NOT collapse a trailing `(set dst scratch)` copy.
  {
    RtlSidebandState sb;
    sb.frame_pointer_needed = true;
    sb.frame_size_bytes = 32;
    sb.saved_gpr_regnos = {30};
    const std::vector<std::string> sub_lines = {
        "subu\t$sp,$sp,32", "sw\t$fp,24($sp)",  "move\t$fp,$sp",  "lw\t$4,0($fp)",
        "lw\t$5,4($fp)",    "lw\t$6,8($fp)",    "lw\t$7,12($fp)", "sltu\t$8,$5,$7",
        "subu\t$3,$5,$7",   "subu\t$2,$4,$6",   "subu\t$2,$2,$8", "move\t$4,$2",
        "move\t$5,$3",      "move\t$2,$4",      "move\t$3,$5",    "move\t$sp,$fp",
        "lw\t$fp,24($sp)",  "addu\t$sp,$sp,32",
    };
    const RtlControlFlowGraph sub_cfg =
        InvertFinalScanInsnLinesToCfg(sub_lines, sb, {}, {}, &o0_prof);
    const std::string sub_s = sub_cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(sub_s, "(set (reg:DI 4 a0) (reg:DI 2 v0))")) << sub_s;

    const std::vector<std::string> csh_lines = {
        "subu\t$sp,$sp,32", "sw\t$fp,24($sp)", "move\t$fp,$sp",   "lw\t$4,0($fp)",
        "lw\t$5,4($fp)",    "sll\t$2,$4,4",    "srl\t$8,$5,28",   "or\t$2,$2,$8",
        "sll\t$3,$5,4",     "move\t$4,$2",     "move\t$5,$3",     "move\t$2,$4",
        "move\t$3,$5",      "move\t$sp,$fp",   "lw\t$fp,24($sp)", "addu\t$sp,$sp,32",
    };
    const RtlControlFlowGraph csh_cfg =
        InvertFinalScanInsnLinesToCfg(csh_lines, sb, {}, {}, &o0_prof);
    const std::string csh_s = csh_cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(csh_s, "(set (reg:DI 4 a0) (reg:DI 2 v0))")) << csh_s;
  }

  // 2. Negative for `MarkSunkOutputReloadCrossedUnchangingLoads` Case B
  //    (`sched.c:2425-2435`): a non-spill `[sp+off]` store between the spill store
  //    and the `mem/s` load must block `/u` tagging.
  {
    RtlSidebandState sb;
    sb.frame_size_bytes = 32;
    sb.outgoing_args_size = 16;
    const std::vector<std::string> lines = {
        "subu\t$sp,$sp,32", "sw\t$4,24($sp)", "sw\t$5,16($sp)",
        "lw\t$2,0($6)",     "addu\t$2,$2,$7", "addu\t$sp,$sp,32",
    };
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(lines, sb, {}, {}, &o2_prof);
    const std::string s = cfg.FormatSummary();
    EXPECT_FALSE(absl::StrContains(s, "mem/s/u:SI (reg:SI 6 a2)")) << s;
  }

  // 3. `RestoreNoConflictExtendClobbers`:
  //    (a) Disjoint `extendsidi2` with `lo = src; hi = ashiftrt(src, 31)` restores
  //        `(clobber (reg:DI dh))` and `(subreg:SI (reg:DI dh) 1/0)`.
  //    (b) `RestoreMuldi3SubregOperandsInBlock` handles `mulsi3_r4000` GPR dests,
  //        strips premature single-word `REG_DEAD` on `first_mul`, and upgrades
  //        `last_mul`'s `(REG_DEAD (reg:SI r_lo))` to `(REG_DEAD (reg:DI r_hi))`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(set (reg:SI 19 s3) (reg:SI 2 v0))",
        "(set (reg:SI 18 s2) (ashiftrt:SI (reg:SI 2 v0) (const_int 31))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
        "(set (reg/i:DI 2 v0) (xor:DI (reg:DI 18 s2) (reg:DI 6 a2))) "
        "(expr_list:REG_DEAD (reg:DI 18 s2) (nil))",
        "(use (reg/i:DI 2 v0))",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    const std::string s = cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(s, "(clobber (reg:DI 18 s2))")) << s;
    EXPECT_TRUE(absl::StrContains(s, "(set (subreg:SI (reg:DI 18 s2) 1) (reg:SI 2 v0))")) << s;
    EXPECT_TRUE(
        absl::StrContains(s, "(set (subreg:SI (reg:DI 18 s2) 0) (ashiftrt:SI (reg:SI 2 v0)"))
        << s;

    // Negative: `lo = ashiftrt(src, 16); hi = ashiftrt(src, 31)` (`extendhisi2 -> extendsidi2`)
    // is not a plain reg-to-reg `lo = src` copy and must not wrap `18 s2` into `subreg:SI`.
    RtlControlFlowGraph neg_hi;
    neg_hi.pass = RtlPassKind::kSched2;
    neg_hi.rpo_blocks.resize(1);
    neg_hi.rpo_blocks[0].id = 0;
    neg_hi.rpo_blocks[0].instructions = {
        "(set (reg:SI 6 a2) (ashift:SI (reg:SI 6 a2) (const_int 16)))",
        "(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 6 a2) (const_int 16)))",
        "(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 6 a2) (const_int 31))) "
        "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
        "(use (reg/i:DI 4 a0))",
    };
    neg_hi.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    RestoreNoConflictExtendClobbers(&neg_hi, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    EXPECT_FALSE(absl::StrContains(neg_hi.FormatSummary(), "(subreg:SI (reg:DI 4 a0)"))
        << neg_hi.FormatSummary();
  }
  {
    auto build_muldi3_cfg = [](bool last_writes_hi) {
      RtlControlFlowGraph cfg;
      cfg.pass = RtlPassKind::kSched2;
      cfg.rpo_blocks.resize(1);
      cfg.rpo_blocks[0].id = 0;
      const std::string last_dst = last_writes_hi ? "(reg:SI 2 v0)" : "(reg:SI 5 a1)";
      cfg.rpo_blocks[0].instructions = {
          "(parallel[ (set (reg:DI 64 hi) (mult:DI (zero_extend:DI (reg:SI 23 s7)) "
          "(zero_extend:DI (reg:SI 3 v1)))) (clobber (reg:SI 66 accum)) ] )",
          "(parallel[ (set (reg:SI 4 a0) (mult:SI (reg:SI 23 s7) (reg:SI 2 v0))) "
          "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] ) "
          "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_UNUSED (reg:SI 66 accum) "
          "(expr_list:REG_UNUSED (reg:SI 65 lo) (expr_list:REG_UNUSED (reg:SI 64 hi) (nil)))))",
          absl::StrCat("(parallel[ (set ", last_dst,
                       " (mult:SI (reg:SI 3 v1) (reg:SI 22 s6))) "
                       "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) "
                       "(clobber (reg:SI 66 accum)) ] ) "
                       "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))"),
          "(use (reg/i:SI 2 v0))",
      };
      cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      return cfg;
    };

    RtlControlFlowGraph pos = build_muldi3_cfg(/*last_writes_hi=*/true);
    RestoreNoConflictExtendClobbers(&pos, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/false);
    EXPECT_TRUE(absl::StrContains(pos.rpo_blocks[0].instructions[1], "(subreg:SI (reg:DI 2 v0) 0)"))
        << pos.FormatSummary();
    EXPECT_FALSE(absl::StrContains(pos.rpo_blocks[0].instructions[1], "REG_DEAD (reg:SI 2 v0)"))
        << pos.FormatSummary();
    EXPECT_TRUE(absl::StrContains(pos.rpo_blocks[0].instructions[1],
                                  "REG_UNUSED (reg:SI 64 hi) (expr_list:REG_UNUSED (reg:SI 65 lo) "
                                  "(expr_list:REG_UNUSED (reg:SI 66 accum)"))
        << pos.FormatSummary();
    EXPECT_TRUE(absl::StrContains(pos.rpo_blocks[0].instructions[2], "REG_DEAD (reg:DI 2 v0)"))
        << pos.FormatSummary();

    RtlControlFlowGraph neg = build_muldi3_cfg(/*last_writes_hi=*/false);
    RestoreNoConflictExtendClobbers(&neg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/false);
    EXPECT_FALSE(absl::StrContains(neg.rpo_blocks[0].instructions[1], "(subreg:SI (reg:DI 2 v0)"))
        << neg.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest,
       Stage0MdDiGapLoadUnchangingAndAshrdi3KloZeroAndMuldi3InterveningDead) {
  const CompilationProfile o2_prof{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
  };

  // 1. Positive + Negative (`sched.c:828, 834-839, 852, 858-863`,
  //    `rtlanal.c:101-102`, `reload1.c:6631`):
  //    a load hoisted into the `mult:DI -> mfhi/mflo` latency gap ahead of a
  //    stack spill store (`8($sp)`) with no preceding non-stack or non-spill
  //    `[sp+off]` store and whose only later non-spill store transitively
  //    depends on it recovers `mem/u` (positive); when a non-stack store or a
  //    non-spill `[sp+off]` store precedes `mult:DI`, or an independent
  //    non-spill `[sp+off]` store follows `k`, `/u` is NOT set (negatives).
  enum class MdGapCase {
    kPositive,
    kNegNonStackStoreBeforeMult,
    kNegNonSpillSpStoreBeforeMult,
    kNegIndepNonSpillSpStoreAfterK,
  };
  auto build_md_gap_lines = [](MdGapCase mode) {
    std::vector<std::string> lines = {
        "subu\t$sp,$sp,24",
    };
    if (mode == MdGapCase::kNegNonStackStoreBeforeMult) {
      lines.push_back("sw\t$8,0($13)");
    } else if (mode == MdGapCase::kNegNonSpillSpStoreBeforeMult) {
      lines.push_back("sw\t$8,4($sp)");
    }
    lines.push_back("lw\t$3,0($11)");
    lines.push_back("xor\t$2,$3,$25");
    lines.push_back("mult\t$3,$2");
    lines.push_back("lw\t$4,0($12)");
    lines.push_back("mfhi\t$16");
    lines.push_back("mflo\t$17");
    lines.push_back("addu\t$2,$4,$24");
    lines.push_back("sw\t$16,8($sp)");
    lines.push_back("sw\t$17,12($sp)");
    if (mode == MdGapCase::kNegIndepNonSpillSpStoreAfterK) {
      lines.push_back("sw\t$8,4($sp)");
    }
    lines.push_back("sw\t$2,0($13)");
    lines.push_back("addu\t$sp,$sp,24");
    return lines;
  };
  {
    RtlSidebandState sb;
    sb.frame_size_bytes = 24;
    const RtlControlFlowGraph pos = InvertFinalScanInsnLinesToCfg(
        build_md_gap_lines(MdGapCase::kPositive), sb, {}, {}, &o2_prof);
    EXPECT_TRUE(absl::StrContains(pos.FormatSummary(), "mem/u:SI (reg:SI 12 t4)"))
        << pos.FormatSummary();

    for (MdGapCase neg_case :
         {MdGapCase::kNegNonStackStoreBeforeMult, MdGapCase::kNegNonSpillSpStoreBeforeMult,
          MdGapCase::kNegIndepNonSpillSpStoreAfterK}) {
      const RtlControlFlowGraph neg =
          InvertFinalScanInsnLinesToCfg(build_md_gap_lines(neg_case), sb, {}, {}, &o2_prof);
      EXPECT_FALSE(absl::StrContains(neg.FormatSummary(), "mem/u:SI (reg:SI 12 t4)"))
          << neg.FormatSummary();
    }
  }

  // 2. Positive + Negative (`config/mips/mips.md:3983-3996, 4015-4028`):
  //    `ashrdi3_internal2` with `k_lo == 0` (`sra $3, $4, 0; sra $2, $4, 31`)
  //    folds to `(ashiftrt:DI (reg:DI 4 a0) (const_int 32))` even when `$5 a1`
  //    (`s0 + 1`) remains live in the following `xor $2, $5, $3` (positive),
  //    whereas narrow sign-extend `sll $4, $4, 16; sra $3, $4, 16; sra $2, $4, 31`
  //    does NOT fold (negative).
  {
    auto fold_insns = [](const std::vector<std::string>& insn_strs) {
      RtlControlFlowGraph cfg;
      cfg.pass = RtlPassKind::kSched2;
      cfg.rpo_blocks.resize(1);
      cfg.rpo_blocks[0].id = 0;
      cfg.rpo_blocks[0].instructions = insn_strs;
      cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      std::vector<RtlPatternAndNotes> parsed;
      parsed.reserve(insn_strs.size());
      for (const std::string& s : insn_strs) {
        auto pn = ParsePatternAndNotes(s);
        EXPECT_TRUE(pn.ok()) << s;
        parsed.push_back(std::move(*pn));
      }
      TryFoldGp32DiMultiInsnIdiomsInBlock(cfg, /*b=*/0, /*mark_newly_folded_sentinel=*/false,
                                          &parsed);
      std::string out;
      for (const auto& pn : parsed) {
        absl::StrAppend(&out, FormatPatternAndNotes(pn.pattern, pn.reg_notes), "\n");
      }
      return out;
    };

    const std::string pos = fold_insns({
        "(set (reg:SI 4 a0) (mem:SI (reg:SI 16 s0)))",
        "(set (reg:SI 5 a1) (mem:SI (plus:SI (reg:SI 16 s0) (const_int 4))))",
        "(set (reg:SI 3 v1) (ashiftrt:SI (reg:SI 4 a0) (const_int 0)))",
        "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))",
        "(set (reg/i:SI 2 v0) (xor:SI (reg:SI 5 a1) (reg:SI 3 v1)))",
        "(use (reg/i:SI 2 v0))",
    });
    EXPECT_TRUE(absl::StrContains(pos, "(ashiftrt:DI (reg:DI 4 a0) (const_int 32))")) << pos;

    const std::string neg = fold_insns({
        "(set (reg:SI 4 a0) (ashift:SI (reg:SI 4 a0) (const_int 16)))",
        "(set (reg:SI 3 v1) (ashiftrt:SI (reg:SI 4 a0) (const_int 16)))",
        "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))",
        "(set (reg/i:SI 2 v0) (xor:SI (reg:SI 5 a1) (reg:SI 3 v1)))",
        "(use (reg/i:SI 2 v0))",
    });
    EXPECT_FALSE(absl::StrContains(neg, "(ashiftrt:DI ")) << neg;
  }

  // 3. `RestoreMuldi3SubregOperandsInBlock` (`runs_sched2 = true`, F2 polish):
  //    when an intervening instruction between `first_mul` and `last_mul`
  //    carries `(REG_DEAD (reg:SI 2 v0))`, `last_mul`'s `(REG_DEAD (reg:SI 3 v1))`
  //    must NOT be upgraded to `(REG_DEAD (reg:DI 2 v0))` (negative); without
  //    the intervening death (positive), it IS upgraded to `DImode`.
  {
    auto build_sched2_muldi3 = [](bool intervening_hi_dead) {
      RtlControlFlowGraph cfg;
      cfg.pass = RtlPassKind::kSched2;
      cfg.rpo_blocks.resize(1);
      cfg.rpo_blocks[0].id = 0;
      std::vector<std::string> insns = {
          "(parallel[ (set (reg:DI 64 hi) (mult:DI (zero_extend:DI (reg:SI 23 s7)) "
          "(zero_extend:DI (reg:SI 3 v1)))) (clobber (reg:SI 66 accum)) ] )",
          "(parallel[ (set (reg:SI 4 a0) (mult:SI (reg:SI 23 s7) (reg:SI 2 v0))) "
          "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] )",
      };
      if (intervening_hi_dead) {
        insns.push_back(
            "(set (reg:SI 8 t0) (plus:SI (reg:SI 2 v0) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
      }
      insns.push_back(
          "(parallel[ (set (reg:SI 2 v0) (mult:SI (reg:SI 3 v1) (reg:SI 22 s6))) "
          "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] ) "
          "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
      insns.push_back("(use (reg/i:SI 2 v0))");
      cfg.rpo_blocks[0].instructions = std::move(insns);
      cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      return cfg;
    };

    RtlControlFlowGraph neg = build_sched2_muldi3(/*intervening_hi_dead=*/true);
    RestoreNoConflictExtendClobbers(&neg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    EXPECT_FALSE(absl::StrContains(neg.rpo_blocks[0].instructions[3], "REG_DEAD (reg:DI 2 v0)"))
        << neg.FormatSummary();

    RtlControlFlowGraph pos = build_sched2_muldi3(/*intervening_hi_dead=*/false);
    RestoreNoConflictExtendClobbers(&pos, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
    EXPECT_TRUE(absl::StrContains(pos.rpo_blocks[0].instructions[2], "REG_DEAD (reg:DI 2 v0)"))
        << pos.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest, Stage0IndexedStructArrayInStructMemsPositiveAndNegative) {
  // Mirrors `expr.c:4569-4578, 4880-4890` + `sched.c:834-882`:
  // `MarkIndexedArrayAccessInStructMems` tracks scaled struct-array element
  // strides including linear combinations `((idx << 1) + idx) << 2` (stride 12)
  // and marks non-`QImode` loads/stores (including `zero_extend(mem:HI)`) with
  // `0 <= off && off + sizeof(mode) <= stride` as `MEM_IN_STRUCT_P` (`/s`),
  // while leaving `QImode` accesses and out-of-stride offsets unmarked.
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  RtlSidebandState sb;
  const std::vector<std::string> lines = {
      "sll\t$2,$5,1",  "addu\t$2,$2,$5", "sll\t$2,$2,2",  "addu\t$2,$2,$4", "sw\t$6,4($2)",
      "lhu\t$3,8($2)", "lb\t$7,10($2)",  "lw\t$8,12($2)", "addu\t$2,$3,$7", "addu\t$2,$2,$8",
  };
  const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(lines, sb, {}, {}, &o2_prof);
  const std::string summary = cfg.FormatSummary();
  // Positive: `sw $6, 4($2)` (off 4, sz 4 <= 12) and `lhu $3, 8($2)` (off 8, sz 2 <= 12).
  EXPECT_TRUE(absl::StrContains(summary, "mem/s:SI (plus:SI (reg:SI 2 v0) (const_int 4))"))
      << summary;
  EXPECT_TRUE(
      absl::StrContains(summary, "zero_extend:SI (mem/s:HI (plus:SI (reg:SI 2 v0) (const_int 8)))"))
      << summary;
  // Negative 1: `QImode` access (`lb $7, 10($2)`) must NOT receive `/s`.
  EXPECT_FALSE(absl::StrContains(summary, "mem/s:QI")) << summary;
  // Negative 2: out-of-stride offset (`lw $8, 12($2)`, 12 + 4 > 12) must NOT receive `/s`.
  EXPECT_FALSE(absl::StrContains(summary, "mem/s:SI (plus:SI (reg:SI 2 v0) (const_int 12))"))
      << summary;

  // Negative 3: clobbering `$5` (the unscaled `base_reg`) invalidates linear
  // combination `scaled_idx - $5` while keeping `$2`'s existing `stride` valid
  // for `base_ptr + $2` (`sll $2,$5,1; addiu $5,$6,1; subu $2,$2,$5; ...`).
  {
    const std::vector<std::string> stale_base_lines = {
        "sll\t$2,$5,2", "addiu\t$5,$6,1", "subu\t$2,$2,$5", "addu\t$2,$2,$4", "lw\t$3,0($2)",
    };
    const RtlControlFlowGraph stale_cfg =
        InvertFinalScanInsnLinesToCfg(stale_base_lines, sb, {}, {}, &o2_prof);
    EXPECT_FALSE(absl::StrContains(stale_cfg.FormatSummary(), "mem/s:SI"))
        << stale_cfg.FormatSummary();
  }
}

TEST_F(InvertFinalScanInsnTest, OverlapExtendsidi2WrapsPriorDefAndForbidsPartnerClobber) {
  // Mirrors `optabs.c:2485-2525` (`emit_no_conflict_block`) + `local-alloc.c:1317-1352`
  // (`combine_regs`):
  // 1. A prior single-word def `move $8, $7` feeding an overlapping `extendsidi2`
  //    (`move $9, $8; sra $8, $8, 31`) wraps in `REG_LIBCALL`, `(subreg:SI (reg:DI 8 t0) 1/0)`,
  //    `REG_NO_CONFLICT (reg:SI 8 t0)`, and `(REG_DEAD (reg:SI 8 t0))`.
  // 2. An earlier `ashldi3_internal2` (`sll $14, $19, 5; move $15, $0`) must not pick
  //    `$3 (v1)` as its unused clobber while `$2 (v0)` is live into a forward overlapping
  //    `move $3, $2; sra $2, $2, 31`, whereas without that forward `extendsidi2`, `$3` is free.
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  RtlSidebandState sb;
  sb.frame_size_bytes = 0;

  // Positive 1 vs Negative 1: `-O2` overlapping `extendsidi2` after `move $8, $7` + independent
  // load wraps block-local `(reg:DI 8 t0)` in `REG_NO_CONFLICT`, whereas when `(reg:DI 2 v0)`
  // stays live as the hard return register (`local-alloc.c:1774, 1807-1838`), it must NOT split
  // `move $2, $4` into a `REG_NO_CONFLICT` pseudo.
  {
    const std::vector<std::string> lines = {
        "move\t$8,$7",   "lw\t$12,0($4)",  "lw\t$13,4($4)",  "move\t$9,$8",
        "sra\t$8,$8,31", "xor\t$2,$12,$8", "xor\t$3,$13,$9",
    };
    const RtlControlFlowGraph cfg = InvertFinalScanInsnLinesToCfg(lines, sb, {}, {}, &o2_prof);
    const std::string s = cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(s, "(clobber (reg:DI 8 t0)) (insn_list:REG_LIBCALL")) << s;
    EXPECT_TRUE(absl::StrContains(s, "(set (subreg:SI (reg:DI 8 t0) 1) (reg:SI 8 t0))")) << s;
    EXPECT_TRUE(absl::StrContains(s, "REG_NO_CONFLICT (reg:SI 8 t0)")) << s;

    const std::vector<std::string> neg_ret_lines = {
        "move\t$2,$4", "move\t$8,$5",    "move\t$3,$2",    "sra\t$2,$2,31",  "move\t$9,$8",
        "move\t$8,$0", "addu\t$3,$3,$9", "sltu\t$6,$3,$9", "addu\t$2,$2,$8", "addu\t$2,$2,$6",
    };
    const RtlControlFlowGraph neg_ret_cfg =
        InvertFinalScanInsnLinesToCfg(neg_ret_lines, sb, {}, {}, &o2_prof);
    EXPECT_FALSE(absl::StrContains(neg_ret_cfg.FormatSummary(), "REG_NO_CONFLICT (reg:SI 2 v0)"))
        << neg_ret_cfg.FormatSummary();
  }

  // Positive 2 vs Negative: `ashldi3_internal2` avoids `$3 (v1)` iff `$2 (v0)` feeds
  // a forward overlapping `move $3, $2; sra $2, $2, 31`.
  {
    const std::vector<std::string> pos_lines = {
        "move\t$2,$7", "lw\t$18,0($4)", "lw\t$19,4($4)",  "sll\t$14,$19,5", "move\t$15,$0",
        "move\t$3,$2", "sra\t$2,$2,31", "xor\t$2,$14,$2", "xor\t$3,$15,$3",
    };
    const RtlControlFlowGraph pos_cfg =
        InvertFinalScanInsnLinesToCfg(pos_lines, sb, {}, {}, &o1_prof);
    EXPECT_FALSE(absl::StrContains(pos_cfg.FormatSummary(), "(clobber (reg:SI 3 v1))"))
        << pos_cfg.FormatSummary();

    const std::vector<std::string> neg_lines = {
        "move\t$2,$7",  "lw\t$18,0($4)",   "lw\t$19,4($4)", "sll\t$14,$19,5",
        "move\t$15,$0", "addu\t$2,$14,$2", "move\t$3,$15",
    };
    const RtlControlFlowGraph neg_cfg =
        InvertFinalScanInsnLinesToCfg(neg_lines, sb, {}, {}, &o1_prof);
    EXPECT_TRUE(absl::StrContains(neg_cfg.FormatSummary(), "(clobber (reg:SI 3 v1))"))
        << neg_cfg.FormatSummary();
  }
}

class InvertFinalScanInsnRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new gcc_harness::GccContext();
    hooks = new gcc_harness::FunctionHooks(ctx);
    provider = new ProductionFinalScanInsnInverseProvider();
    verifier = new gcc_harness::RightInverseVerifier(ctx, hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static gcc_harness::GccContext* ctx;
  static gcc_harness::FunctionHooks* hooks;
  static ProductionFinalScanInsnInverseProvider* provider;
  static gcc_harness::RightInverseVerifier* verifier;
};

gcc_harness::GccContext* InvertFinalScanInsnRoundTripTest::ctx = nullptr;
gcc_harness::FunctionHooks* InvertFinalScanInsnRoundTripTest::hooks = nullptr;
ProductionFinalScanInsnInverseProvider* InvertFinalScanInsnRoundTripTest::provider = nullptr;
gcc_harness::RightInverseVerifier* InvertFinalScanInsnRoundTripTest::verifier = nullptr;

TEST_P(InvertFinalScanInsnRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  bool found_snapshot = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != gcc_harness::SubfunctionId::kFinalScanInsn) {
      continue;
    }
    found_snapshot = true;
    const auto res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "Test case: " << tc.test_name << "\nDiff:\n"
                             << res.diff_detail << "\nExpected asm:\n"
                             << res.expected_summary;
  }
  EXPECT_TRUE(found_snapshot) << "Missing kFinalScanInsn snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, InvertFinalScanInsnRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
