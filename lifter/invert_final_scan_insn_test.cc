#include "lifter/invert_final_scan_insn.h"

#include <string>
#include <string_view>

#include "absl/strings/match.h"
#include "core/round_trip_test_cases.h"
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

    // Block 0: conditional branch `beq $4,$0,$L2` -> fallthrough block 1, target block 2.
    EXPECT_FALSE(cfg.rpo_blocks[0].has_label);
    const std::vector<size_t> expected_succ_0 = {1u, 2u};
    EXPECT_EQ(cfg.rpo_blocks[0].successors, expected_succ_0);

    // Block 1: fallthrough `addu $2,$4,$5` + unconditional `j $L3` -> target block 3.
    EXPECT_FALSE(cfg.rpo_blocks[1].has_label);
    const std::vector<size_t> expected_succ_1 = {3u};
    EXPECT_EQ(cfg.rpo_blocks[1].successors, expected_succ_1);

    // Block 2: `$L2:` `move $2,$0` -> fallthrough block 3.
    EXPECT_TRUE(cfg.rpo_blocks[2].has_label);
    const std::vector<size_t> expected_succ_2 = {3u};
    EXPECT_EQ(cfg.rpo_blocks[2].successors, expected_succ_2);

    // Block 3: `$L3:` `j $31` (`(return)`) -> no successors.
    EXPECT_TRUE(cfg.rpo_blocks[3].has_label);
    EXPECT_TRUE(cfg.rpo_blocks[3].successors.empty());
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
        absl::StartsWith(cfg.rpo_blocks[0].instructions[1], "(sequence [(jump_insn (return))"))
        << cfg.rpo_blocks[0].instructions[1];
    EXPECT_TRUE(cfg.rpo_blocks[0].successors.empty());
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
