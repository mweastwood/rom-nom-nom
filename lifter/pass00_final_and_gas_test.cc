#include "lifter/pass00_final_and_gas.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"
#include "lifter/invert_gas_mips_ip.h"

namespace rom_nom_nom::lifter {
namespace {

class ProductionStage0InverseProvider final : public gcc_harness::SubfunctionInverseProvider {
 public:
  std::string InvertGasMipsIp(uint32_t word_x) const override {
    return rom_nom_nom::InvertGasMipsIp(word_x);
  }

  std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const override {
    return rom_nom_nom::InvertGasMacro(macro_words_x);
  }

  std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x);
  }

  std::string InvertFinalScanInsn(std::string_view asm_line_x) const override {
    return lifter::InvertFinalScanInsn(asm_line_x);
  }

  gcc_harness::PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view asm_text_x) const override {
    const auto spec = lifter::InvertFunctionPrologueEpilogue(asm_text_x);
    return {.var_size = spec.var_size, .saved_gpr_mask = spec.saved_gpr_mask};
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(
      absl::Span<const uint32_t> machine_words_x) const override {
    return lifter::InvertFinalAndGasFunction(machine_words_x);
  }
};

class Pass00FinalAndGasTest : public ::testing::Test {
 protected:
  gcc_harness::GccContext ctx_;
  gcc_harness::FunctionHooks hooks_{&ctx_};
  ProductionStage0InverseProvider provider_;
  gcc_harness::RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_F(Pass00FinalAndGasTest, SyntheticPositiveUnscheduledLeaf) {
  const std::vector<uint32_t> words = {
      0x24820005u,  // addiu $v0, $a0, 5
      0x03e00008u,  // jr $ra
      0x00000000u,  // nop
  };
  const auto res = verifier_.VerifyFinalAndGasStageRightInverse(words);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(Pass00FinalAndGasTest, SyntheticPositiveScheduledLeaf) {
  const std::vector<uint32_t> words = {
      0x00851021u,  // addu $v0, $a0, $a1
      0x03e00008u,  // jr $ra
      0x00021080u,  // sll $v0, $v0, 2 (in return delay slot)
  };
  const auto res = verifier_.VerifyFinalAndGasStageRightInverse(words);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(Pass00FinalAndGasTest, SyntheticPositiveMultiBlockConditionalBranch) {
  const std::vector<uint32_t> words = {
      0x10800003u,  // beq $a0, $zero, $L2
      0x00851021u,  // addu $v0, $a0, $a1 (in branch delay slot)
      0x03e00008u,  // jr $ra
      0x24420001u,  // addiu $v0, $v0, 1 (in return delay slot)
      0x03e00008u,  // $L2: jr $ra
      0x00851023u,  // subu $v0, $a0, $a1 (in return delay slot)
  };
  const auto res = verifier_.VerifyFinalAndGasStageRightInverse(words);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(Pass00FinalAndGasTest, SyntheticPositiveLoopBackEdge) {
  const std::vector<uint32_t> words = {
      0x00001021u,  // addu $v0, $zero, $zero
      0x00441021u,  // $L2: addu $v0, $v0, $a0
      0x2484ffffu,  // addiu $a0, $a0, -1
      0x1480fffdu,  // bne $a0, $zero, $L2
      0x00000000u,  // nop
      0x03e00008u,  // jr $ra
      0x00000000u,  // nop
  };
  const auto res = verifier_.VerifyFinalAndGasStageRightInverse(words);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(Pass00FinalAndGasTest, SyntheticPositiveStackFrameAndSavedRegisters) {
  // Frame with total_size = 24 (`var_size = 16`, `saved_gpr_mask = 0x80010000` -> $ra, $s0).
  const std::vector<uint32_t> words = {
      0x27bdffe8u,  // addiu $sp, $sp, -24
      0xafbf0014u,  // sw $ra, 20($sp)
      0xafb00010u,  // sw $s0, 16($sp)
      0x00858021u,  // addu $s0, $a0, $a1
      0x02001021u,  // addu $v0, $s0, $zero
      0x8fbf0014u,  // lw $ra, 20($sp)
      0x8fb00010u,  // lw $s0, 16($sp)
      0x03e00008u,  // jr $ra
      0x27bd0018u,  // addiu $sp, $sp, 24 (in return delay slot)
  };
  const auto res = verifier_.VerifyFinalAndGasStageRightInverse(words);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(Pass00FinalAndGasTest, SyntheticPositiveLargeConstantMacroAndFloatingPoint) {
  const std::vector<uint32_t> li_words = {
      0x3c021234u,  // lui $v0, 0x1234
      0x34425678u,  // ori $v0, $v0, 0x5678
      0x03e00008u,  // jr $ra
      0x00821021u,  // addu $v0, $a0, $v0
  };
  const auto res_li = verifier_.VerifyFinalAndGasStageRightInverse(li_words);
  EXPECT_TRUE(res_li.matches) << res_li.diff_detail;

  const std::vector<uint32_t> fp_words = {
      0x460e6000u,  // add.s $f0, $f12, $f14
      0x03e00008u,  // jr $ra
      0x00000000u,  // nop
  };
  const auto res_fp = verifier_.VerifyFinalAndGasStageRightInverse(fp_words);
  EXPECT_TRUE(res_fp.matches) << res_fp.diff_detail;
}

TEST_F(Pass00FinalAndGasTest, SyntheticNegativeInvalidAndMalformedWords) {
  EXPECT_TRUE(InvertFinalAndGasFunction({}).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse({}).matches);

  const std::vector<uint32_t> bad1 = {0x7fffffffu};
  EXPECT_TRUE(InvertFinalAndGasFunction(bad1).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(bad1).matches);

  const std::vector<uint32_t> bad2 = {0x0000003fu};
  EXPECT_TRUE(InvertFinalAndGasFunction(bad2).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(bad2).matches);

  const std::vector<uint32_t> bad3 = {0x24820005u, 0x7fffffffu};
  EXPECT_TRUE(InvertFinalAndGasFunction(bad3).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(bad3).matches);

  const std::vector<uint32_t> bad_jal_nonzero = {
      0x0c000040u,  // jal 0x100
      0x00000000u,  // nop
      0x03e00008u,  // jr $ra
      0x00000000u,  // nop
  };
  EXPECT_TRUE(InvertFinalAndGasFunction(bad_jal_nonzero).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(bad_jal_nonzero).matches);
}

class Pass00FinalAndGasRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new gcc_harness::GccContext();
    hooks = new gcc_harness::FunctionHooks(ctx);
    provider = new ProductionStage0InverseProvider();
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
  static ProductionStage0InverseProvider* provider;
  static gcc_harness::RightInverseVerifier* verifier;
};

gcc_harness::GccContext* Pass00FinalAndGasRoundTripTest::ctx = nullptr;
gcc_harness::FunctionHooks* Pass00FinalAndGasRoundTripTest::hooks = nullptr;
ProductionStage0InverseProvider* Pass00FinalAndGasRoundTripTest::provider = nullptr;
gcc_harness::RightInverseVerifier* Pass00FinalAndGasRoundTripTest::verifier = nullptr;

TEST_P(Pass00FinalAndGasRoundTripTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);
  ASSERT_FALSE(trace.final_machine_words.empty()) << "Test case: " << tc.test_name;

  const std::vector<uint32_t> unpadded_words = hooks->RunGasMdAssemble(trace.final_asm_text);
  ASSERT_FALSE(unpadded_words.empty()) << "Test case: " << tc.test_name;
  const auto res = verifier->VerifyFinalAndGasStageRightInverse(unpadded_words);
  EXPECT_TRUE(res.matches) << "Test case: " << tc.test_name << "\nDiff:\n" << res.diff_detail;

  const RtlControlFlowGraph lifted_unpadded = InvertFinalAndGasFunction(unpadded_words);
  const RtlControlFlowGraph lifted_padded = InvertFinalAndGasFunction(trace.final_machine_words);
  const auto cmp = lifted_padded.CompareAgainst(lifted_unpadded);
  EXPECT_TRUE(cmp.MatchesExactly()) << "Test case: " << tc.test_name << "\n" << cmp.mismatch_detail;
  EXPECT_EQ(lifted_padded.sideband, lifted_unpadded.sideband) << "Test case: " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass00FinalAndGasRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
