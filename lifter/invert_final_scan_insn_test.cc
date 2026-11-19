#include "lifter/invert_final_scan_insn.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

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
