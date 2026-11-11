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

  RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) const override {
    return lifter::InvertFinalAndGasFunction(input_x);
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
  EXPECT_TRUE(InvertFinalAndGasFunction(absl::Span<const uint32_t>{}).rpo_blocks.empty());
  EXPECT_TRUE(InvertFinalAndGasFunction(Stage0FunctionInput{}).rpo_blocks.empty());
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(absl::Span<const uint32_t>{}).matches);
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(Stage0FunctionInput{}).matches);

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

TEST_F(Pass00FinalAndGasTest, Stage0EndToEndRelocationsJumpTablesFloatPoolsAndLinkedRom) {
  // 1. Multi-callee `jal`, distinct globals with addends, 6-arg `outgoing_args_size == 24`,
  //    `.rodata`/`.rdata` switch jump table, and constant-pool `double`s.
  constexpr std::string_view kComplexSource = R"(
extern int call_alpha(int);
extern int call_beta(int, int, int, int, int, int);
extern int g_arr_a[8];
extern int g_arr_b[8];
double eval_stage0(int op, int x) {
  switch (op) {
    case 0: return (double)call_alpha(x + g_arr_a[2]) * 3.141592653589793;
    case 1: return (double)call_beta(x, g_arr_b[3], 2, 3, 4, 5) + 1.5;
    case 2: return (double)(g_arr_a[1] - g_arr_b[4]);
    case 3: return (double)(x * 7);
    case 4: return (double)(x + 9);
    default: return 0.0;
  }
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-O2"}}) {
    const auto trace =
        hooks_.TraceForwardCompilation(kComplexSource, "eval_stage0", flags, "eval_stage0");
    const auto res = verifier_.VerifyFinalAndGasStageRightInverse(trace.stage0_input);
    EXPECT_TRUE(res.matches) << res.diff_detail;

    const RtlControlFlowGraph lifted = InvertFinalAndGasFunction(trace.stage0_input);
    EXPECT_EQ(lifted.sideband.outgoing_args_size, 24);
  }

  const auto trace = hooks_.TraceForwardCompilation(
      kComplexSource, "eval_stage0", {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-O2"}, "eval_stage0");

  // Negative checks: corrupting relocation symbol recovery, jump-table target,
  // or constant-pool literal words must fail `VerifyFinalAndGasStageRightInverse`.
  class WrongSymbolStage0Provider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& x) const override {
      RtlControlFlowGraph cfg = rom_nom_nom::lifter::InvertFinalAndGasFunction(x);
      for (auto& blk : cfg.rpo_blocks) {
        for (auto& pat : blk.instructions) {
          const size_t pos = pat.find("\"call_alpha\"");
          if (pos != std::string::npos) {
            pat.replace(pos, 12, "\"__callee_0\"");
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      return cfg;
    }
  };
  WrongSymbolStage0Provider wrong_sym_provider;
  gcc_harness::RightInverseVerifier wrong_sym_verifier(&ctx_, &hooks_, &wrong_sym_provider);
  EXPECT_FALSE(wrong_sym_verifier.VerifyFinalAndGasStageRightInverse(trace.stage0_input).matches);

  Stage0FunctionInput bad_jt = trace.stage0_input;
  ASSERT_FALSE(bad_jt.jump_tables.empty());
  bad_jt.jump_tables[0].target_word_indices[0] = 99999u;
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(bad_jt).matches);

  Stage0FunctionInput bad_pool = trace.stage0_input;
  ASSERT_FALSE(bad_pool.constant_pool.empty());
  bad_pool.constant_pool[0].words = {0x7ff80000u, 0x00000000u};
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(bad_pool).matches);

  // 2. Linked N64 ROM slice (`base_vram != 0` + `vram_to_symbol` without relocations),
  //    including mapped `jal` (`AudioUpdate`), unmapped `jal` (`func_80025000`), and
  //    intra-function local `j 0x80020024` targeting the shared epilogue:
  Stage0FunctionInput rom_input;
  rom_input.base_vram = 0x80020000u;
  rom_input.vram_to_symbol[0x80024000u] = "AudioUpdate";
  rom_input.vram_to_symbol[0x80028000u] = "OtherCallee";
  rom_input.machine_words = {
      0x27bdffe8u,  // 0 (0x80020000): addiu $sp, $sp, -24
      0x10800005u,  // 1 (0x80020004): beq $a0, $zero, +5 -> word 7 (0x8002001c)
      0xafbf0010u,  // 2 (0x80020008): sw $ra, 16($sp) (delay slot)
      0x0c009000u,  // 3 (0x8002000c): jal 0x80024000 (AudioUpdate)
      0x00000000u,  // 4 (0x80020010): nop
      0x08008009u,  // 5 (0x80020014): j 0x80020024 (intra-function jump -> word 9)
      0x00000000u,  // 6 (0x80020018): nop
      0x0c009400u,  // 7 (0x8002001c): jal 0x80025000 (unmapped func_80025000)
      0x00000000u,  // 8 (0x80020020): nop
      0x8fbf0010u,  // 9 (0x80020024): lw $ra, 16($sp)
      0x03e00008u,  // 10 (0x80020028): jr $ra
      0x27bd0018u,  // 11 (0x8002002c): addiu $sp, $sp, 24
  };
  const auto rom_res = verifier_.VerifyFinalAndGasStageRightInverse(rom_input);
  EXPECT_TRUE(rom_res.matches) << rom_res.diff_detail;

  const RtlControlFlowGraph rom_cfg = InvertFinalAndGasFunction(rom_input);
  ASSERT_EQ(rom_cfg.rpo_blocks.size(), 4u);
  EXPECT_EQ(rom_cfg.sideband.outgoing_args_size, 16);
  EXPECT_EQ(rom_cfg.sideband.frame_size_bytes, 0);
  ASSERT_GE(rom_cfg.rpo_blocks[1].instructions.size(), 1u);
  EXPECT_NE(rom_cfg.rpo_blocks[1].instructions[0].find("AudioUpdate"), std::string::npos)
      << rom_cfg.rpo_blocks[1].instructions[0];
  ASSERT_GE(rom_cfg.rpo_blocks[2].instructions.size(), 1u);
  EXPECT_NE(rom_cfg.rpo_blocks[2].instructions[0].find("func_80025000"), std::string::npos)
      << rom_cfg.rpo_blocks[2].instructions[0];

  // Linked-ROM negative checks: rewriting `"AudioUpdate"` to either a symbol
  // mapped at a different VRAM (`"OtherCallee"` @ `0x80028000`) or an unmapped
  // symbol (`"UnmappedCallee"`) must fail `VerifyFinalAndGasStageRightInverse`.
  class WrongLinkedRomSymbolProvider final : public gcc_harness::SubfunctionInverseProvider {
   public:
    explicit WrongLinkedRomSymbolProvider(std::string replacement_sym)
        : replacement_sym_(std::move(replacement_sym)) {}

    RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& x) const override {
      RtlControlFlowGraph cfg = rom_nom_nom::lifter::InvertFinalAndGasFunction(x);
      for (auto& blk : cfg.rpo_blocks) {
        for (auto& pat : blk.instructions) {
          const size_t pos = pat.find("\"AudioUpdate\"");
          if (pos != std::string::npos) {
            pat.replace(pos, 13, "\"" + replacement_sym_ + "\"");
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      return cfg;
    }

   private:
    std::string replacement_sym_;
  };
  WrongLinkedRomSymbolProvider wrong_mapped_provider("OtherCallee");
  gcc_harness::RightInverseVerifier wrong_mapped_verifier(&ctx_, &hooks_, &wrong_mapped_provider);
  EXPECT_FALSE(wrong_mapped_verifier.VerifyFinalAndGasStageRightInverse(rom_input).matches);

  WrongLinkedRomSymbolProvider wrong_unmapped_provider("UnmappedCallee");
  gcc_harness::RightInverseVerifier wrong_unmapped_verifier(&ctx_, &hooks_,
                                                            &wrong_unmapped_provider);
  EXPECT_FALSE(wrong_unmapped_verifier.VerifyFinalAndGasStageRightInverse(rom_input).matches);
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
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << "Test case: " << tc.test_name;

  const auto res_stage0 = verifier->VerifyFinalAndGasStageRightInverse(trace.stage0_input);
  EXPECT_TRUE(res_stage0.matches) << "Test case: " << tc.test_name << "\nDiff:\n"
                                  << res_stage0.diff_detail;

  Stage0FunctionInput padded_stage0 = trace.stage0_input;
  padded_stage0.machine_words = trace.final_machine_words;
  const RtlControlFlowGraph lifted_stage0_unpadded = InvertFinalAndGasFunction(trace.stage0_input);
  const RtlControlFlowGraph lifted_stage0_padded = InvertFinalAndGasFunction(padded_stage0);
  const auto cmp_stage0 = lifted_stage0_padded.CompareAgainst(lifted_stage0_unpadded);
  EXPECT_TRUE(cmp_stage0.MatchesExactly()) << "Test case: " << tc.test_name << "\n"
                                           << cmp_stage0.mismatch_detail;
  EXPECT_EQ(lifted_stage0_padded.sideband, lifted_stage0_unpadded.sideband)
      << "Test case: " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass00FinalAndGasRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
