#include "lifter/invert_final_scan_insn.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/match.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
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
  };
  for (std::string_view insn : kInstructions) {
    const auto res = verifier_.VerifyFinalScanInsnRightInverse(insn);
    EXPECT_TRUE(res.matches) << "insn: " << insn << " diff: " << res.diff_detail;
  }
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
    EXPECT_TRUE(res.matches) << "Test case: " << tc.test_name << "\nDiff:\n" << res.diff_detail;
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
