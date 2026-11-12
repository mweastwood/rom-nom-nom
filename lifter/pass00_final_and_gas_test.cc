#include "lifter/pass00_final_and_gas.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
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
    return {
        .var_size = spec.var_size,
        .saved_gpr_mask = spec.saved_gpr_mask,
        .saved_fpr_mask = spec.saved_fpr_mask,
    };
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
  rom_input.profile.optimize_level = 2;
  rom_input.profile.c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-O2"};
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

TEST_F(Pass00FinalAndGasTest, ConstantPoolsSavedFprMultiEpilogueAndO0LocalSeams) {
  // 1. Constant pools across `SFmode`, `DFmode`, `-0.0`, `NaN`, and `-G 0` split `%hi`/`%lo`
  //    vs `-G 8` `.sdata`, plus callee-saved FPR (`$f20`) preservation across calls:
  constexpr std::string_view kFpPoolAndSavedFprSource = R"(
extern double ext_fp_call(double);
double compute_fp_pool(double x, int flag) {
  double acc = x * 3.141592653589793116 + (-0.0);
  double keep = acc + 2.718281828459045091;
  double r = ext_fp_call(acc);
  if (flag) {
    return r + keep;
  }
  return r - keep * 1.414213562373095145;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "0", "-Wa,-O2"},
        std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
    const auto trace = hooks_.TraceForwardCompilation(kFpPoolAndSavedFprSource, "compute_fp_pool",
                                                      flags, "fp_pool_saved_fpr");
    const auto res = verifier_.VerifyFinalAndGasStageRightInverse(trace.stage0_input);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    const auto suffix_res =
        verifier_.VerifySuffixRightInverse(RtlPassKind::kDbr, trace.stage0_input);
    EXPECT_TRUE(suffix_res.matches) << suffix_res.diff_detail;

    const RtlControlFlowGraph lifted = InvertFinalAndGasFunction(trace.stage0_input);
    EXPECT_FALSE(lifted.sideband.constant_pool.empty());
    bool saw_unchanging_pool_ref = false;
    for (const auto& blk : lifted.rpo_blocks) {
      for (const auto& pat : blk.instructions) {
        auto parsed = ParsePatternAndNotes(pat);
        if (!parsed.ok()) {
          continue;
        }
        parsed->pattern.Visit([&](const RtlExpr& node) {
          if (node.op == RtlOp::kSymbolRef && node.flags.unchanging &&
              node.mode == RtlMachineMode::kSi) {
            saw_unchanging_pool_ref = true;
          }
        });
      }
    }
    EXPECT_TRUE(saw_unchanging_pool_ref);
  }

  // 2. Explicit NaN (`0x7ff8000000000001`) and `-0.0` (`0x8000000000000000`) `.rdata` pool
  //    entries assembled via GAS:
  constexpr std::string_view kNanPoolAsm = R"(
	.rdata
	.align	3
$LC0:
	.word	0x7ff80000
	.word	0x00000001
$LC1:
	.word	0x80000000
	.word	0x00000000
	.text
	.align	2
	.ent	nan_negzero
nan_negzero:
	.frame	$sp,0,$31
	.mask	0x00000000,0
	.fmask	0x00000000,0
	l.d	$f2,$LC0
	l.d	$f4,$LC1
	add.d	$f0,$f2,$f4
	j	$31
	.end	nan_negzero
)";
  Stage0FunctionInput nan_stage0 = hooks_.RunGasMdAssembleWithMetadata(kNanPoolAsm);
  nan_stage0.profile.optimize_level = 2;
  nan_stage0.profile.c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-G", "0", "-Wa,-g"};
  const auto nan_res = verifier_.VerifyFinalAndGasStageRightInverse(nan_stage0);
  EXPECT_TRUE(nan_res.matches) << nan_res.diff_detail;

  // 3. `-O0` local variables + post-call `volatile` local store must not inflate
  //    `outgoing_args_size` beyond 16:
  constexpr std::string_view kO0LocalsSource = R"(
extern int one_arg_callee(int);
int o0_locals_func(int a, int b) {
  int local_x = a + 3;
  int local_y = b * 5;
  int c = one_arg_callee(local_x);
  volatile int post_call_dead = c + local_y;
  return c + local_y;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
    const auto trace =
        hooks_.TraceForwardCompilation(kO0LocalsSource, "o0_locals_func", flags, "o0_locals_test");
    const RtlControlFlowGraph lifted = InvertFinalAndGasFunction(trace.stage0_input);
    EXPECT_EQ(lifted.sideband.outgoing_args_size, 16);
    const auto res = verifier_.VerifyFinalAndGasStageRightInverse(trace.stage0_input);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

Stage0FunctionInput SimulateLinkedRomStage0Input(const Stage0FunctionInput& reloc_in,
                                                 bool same_hi_page_for_globals = false) {
  Stage0FunctionInput linked;
  linked.machine_words = reloc_in.machine_words;
  linked.base_vram = 0x80020000u;
  linked.gp_vram = 0x80060000u;
  linked.profile = reloc_in.profile;
  linked.jump_tables = reloc_in.jump_tables;
  linked.constant_pool = reloc_in.constant_pool;

  std::vector<std::pair<std::string, uint32_t>> sym_vrams;
  auto find_sym = [&](const std::string& s, uint32_t* out) -> bool {
    for (const auto& [k, v] : sym_vrams) {
      if (k == s) {
        *out = v;
        return true;
      }
    }
    return false;
  };

  uint32_t next_rodata = 0x80030000u;
  uint32_t next_func = 0x80040000u;
  uint32_t next_gp_data = 0x80059000u;
  uint32_t next_data = same_hi_page_for_globals ? 0x80031000u : 0x80070000u;

  for (Stage0JumpTable& jt : linked.jump_tables) {
    jt.table_vram = next_rodata;
    sym_vrams.emplace_back(jt.label_name, jt.table_vram);
    next_rodata += static_cast<uint32_t>(jt.target_word_indices.size() * 4u + 16u);
  }
  for (Stage0ConstantPoolEntry& cp : linked.constant_pool) {
    if (cp.section_name == ".lit4" || cp.section_name == ".lit8" || cp.section_name == ".sdata") {
      cp.entry_vram = next_gp_data;
      next_gp_data += 16u;
    } else {
      cp.entry_vram = next_rodata;
      next_rodata += 16u;
    }
    sym_vrams.emplace_back(cp.label_name, cp.entry_vram);
  }

  for (const Stage0Relocation& rel : reloc_in.relocations) {
    uint32_t existing = 0;
    if (find_sym(rel.symbol_name, &existing)) {
      continue;
    }
    int64_t max_addend = 0;
    bool any_gp = false;
    bool any_call = false;
    for (const Stage0Relocation& r2 : reloc_in.relocations) {
      if (r2.symbol_name == rel.symbol_name) {
        if (r2.addend > max_addend) {
          max_addend = r2.addend;
        }
        if (r2.kind == Stage0RelocKind::kMipsGpRel16 || r2.kind == Stage0RelocKind::kMipsLiteral) {
          any_gp = true;
        }
        if (r2.kind == Stage0RelocKind::kMips26) {
          any_call = true;
        }
      }
    }
    uint32_t assigned = 0;
    const uint32_t span_bytes = static_cast<uint32_t>((max_addend > 0 ? max_addend : 0) + 16);
    if (any_call) {
      assigned = next_func;
      next_func += 64u;
    } else if (any_gp) {
      assigned = next_gp_data;
      next_gp_data += (span_bytes + 15u) & ~15u;
    } else {
      assigned = next_data;
      next_data += (span_bytes + 63u) & ~63u;
    }
    sym_vrams.emplace_back(rel.symbol_name, assigned);
    linked.vram_to_symbol[assigned] = rel.symbol_name;
    if (max_addend > 0) {
      linked.vram_to_symbol_size[assigned] = span_bytes;
    }
  }

  std::vector<bool> relocated_word(linked.machine_words.size(), false);
  for (const Stage0Relocation& rel : reloc_in.relocations) {
    uint32_t sym_vram = 0;
    if (rel.word_index >= linked.machine_words.size() || !find_sym(rel.symbol_name, &sym_vram)) {
      continue;
    }
    relocated_word[rel.word_index] = true;
    const uint32_t full_addr = static_cast<uint32_t>(static_cast<int64_t>(sym_vram) + rel.addend);
    if (rel.kind == Stage0RelocKind::kMips26) {
      linked.machine_words[rel.word_index] =
          (linked.machine_words[rel.word_index] & 0xfc000000u) | ((full_addr >> 2) & 0x03ffffffu);
    } else if (rel.kind == Stage0RelocKind::kMipsHi16) {
      const uint32_t hi = ((full_addr + 0x8000u) >> 16) & 0xffffu;
      linked.machine_words[rel.word_index] =
          (linked.machine_words[rel.word_index] & 0xffff0000u) | hi;
    } else if (rel.kind == Stage0RelocKind::kMipsLo16) {
      linked.machine_words[rel.word_index] =
          (linked.machine_words[rel.word_index] & 0xffff0000u) | (full_addr & 0xffffu);
    } else if (rel.kind == Stage0RelocKind::kMipsGpRel16 ||
               rel.kind == Stage0RelocKind::kMipsLiteral) {
      const int32_t delta = static_cast<int32_t>(full_addr - linked.gp_vram);
      linked.machine_words[rel.word_index] = (linked.machine_words[rel.word_index] & 0xffff0000u) |
                                             (static_cast<uint32_t>(delta) & 0xffffu);
    }
  }
  for (size_t idx = 0; idx < linked.machine_words.size();) {
    const uint32_t w = linked.machine_words[idx];
    if (!relocated_word[idx] && ((w >> 26) & 0x3fu) == 0x02u) {
      const uint32_t local_idx = w & 0x03ffffffu;
      const uint32_t target_vram = linked.base_vram + local_idx * 4u;
      linked.machine_words[idx] = 0x08000000u | ((target_vram >> 2) & 0x03ffffffu);
    }
    const uint32_t op = (w >> 26) & 0x3fu;
    const uint32_t funct = w & 0x3fu;
    const bool has_delay =
        (op == 0x01u || op == 0x02u || op == 0x03u || (op >= 0x04u && op <= 0x07u) ||
         (op >= 0x14u && op <= 0x17u) || (op == 0x00u && (funct == 0x08u || funct == 0x09u)) ||
         (op == 0x11u && ((w >> 21) & 0x1fu) == 0x08u));
    idx += has_delay ? 2u : 1u;
  }
  return linked;
}

TEST_F(Pass00FinalAndGasTest, LinkedRomHiLoGpMultiJumpTableConstantPoolsAndAdversarialRejections) {
  // 1 & 3 & 4: Linked-ROM hoisted/shared `lui`, `vram_to_symbol_size` struct/array
  // addends, `$gp`-relative `.sdata`, `.rdata`/`.lit8` float & double constant pools
  // (`-mips1` paired `lwc1` AND `-mips2` `ldc1`), and pure 32-bit integer constants!
  constexpr std::string_view kLinkedGlobalsAndPoolsSrc = R"(
struct WorldState {
  int mode;
  int timer;
  int score;
  int bonus;
  double gravity;
};
extern struct WorldState g_world;
extern int g_table_a[16];
extern int g_table_b[16];
extern int g_small_flag;
extern int step_callback(int, double);

int update_world(int n, int idx) {
  int total = 0x12345678;
  int k;
  for (k = 0; k < n; ++k) {
    if (g_small_flag > 0) {
      total += g_world.score + g_table_a[idx] + 0x80031000;
    } else {
      total -= g_world.bonus + g_table_b[idx];
    }
    total = step_callback(total, g_world.gravity * 3.141592653589793 + 1.4142135623730951);
  }
  g_world.timer = total;
  return total;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips1", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "0", "-Wa,-g"}}) {
    const auto trace = hooks_.TraceForwardCompilation(kLinkedGlobalsAndPoolsSrc, "update_world",
                                                      flags, "linked_rom_update_world");
    const Stage0FunctionInput linked =
        SimulateLinkedRomStage0Input(trace.stage0_input, /*same_hi_page_for_globals=*/true);
    const auto res_stage0 = verifier_.VerifyFinalAndGasStageRightInverse(linked);
    EXPECT_TRUE(res_stage0.matches) << res_stage0.diff_detail;
    const auto res_suffix = verifier_.VerifySuffixRightInverse(RtlPassKind::kDbr, linked);
    EXPECT_TRUE(res_suffix.matches) << res_suffix.diff_detail;

    auto count_symbol_refs = [](const RtlControlFlowGraph& cfg, std::string_view target_sym) {
      size_t count = 0;
      for (const auto& blk : cfg.rpo_blocks) {
        for (const auto& pat : blk.instructions) {
          auto parsed = ParsePatternAndNotes(pat);
          if (!parsed.ok()) {
            continue;
          }
          parsed->pattern.Visit([&](const RtlExpr& node) {
            if (node.op == RtlOp::kSymbolRef && node.str_val == target_sym) {
              ++count;
            }
          });
        }
      }
      return count;
    };
    const size_t full_world_refs = count_symbol_refs(InvertFinalAndGasFunction(linked), "g_world");
    EXPECT_GE(full_world_refs, 4u);

    // Also verify when constant-pool string labels are stripped (`""`) as in raw binary extraction!
    Stage0FunctionInput raw_unnamed = linked;
    for (auto& cp : raw_unnamed.constant_pool) {
      cp.label_name.clear();
    }
    EXPECT_TRUE(verifier_.VerifyFinalAndGasStageRightInverse(raw_unnamed).matches);

    // Adversarial negative checks on linked ROM with real `InvertFinalAndGasFunction`:
    if (!linked.constant_pool.empty()) {
      Stage0FunctionInput bad_cp_vram = linked;
      bad_cp_vram.constant_pool[0].entry_vram += 32u;
      EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(bad_cp_vram).matches);
    }
    // Truncating `g_world`'s `vram_to_symbol_size` to 4 bytes prevents struct-member
    // resolution at offsets +4, +8, +12, +16 (`timer`, `score`, `bonus`, `gravity`):
    uint32_t world_vram = 0;
    for (const auto& [vram, sym] : linked.vram_to_symbol) {
      if (sym == "g_world") {
        world_vram = vram;
        break;
      }
    }
    ASSERT_NE(world_vram, 0u);
    Stage0FunctionInput truncated_struct = linked;
    truncated_struct.vram_to_symbol_size[world_vram] = 4u;
    EXPECT_EQ(count_symbol_refs(InvertFinalAndGasFunction(truncated_struct), "g_world"), 0u);
  }

  // 2: Multi-jump-table function (>= 2 `switch` jump tables in a loop, including
  //    reversed `.rodata` jump-table order vs RPO block order!) on both `.o` and
  //    linked-ROM `Stage0FunctionInput`.
  constexpr std::string_view kMultiSwitchLoopSrc = R"(
extern int ext_op(int, int);
int run_dual_vm(int count, const int* opcodes, const int* operands) {
  int acc = 7;
  int i;
  for (i = 0; i < count; ++i) {
    switch (opcodes[i]) {
      case 0: acc += operands[i]; break;
      case 1: acc -= operands[i]; break;
      case 2: acc ^= (operands[i] << 2); break;
      case 3: acc = ext_op(acc, operands[i]); break;
      case 4: acc += 19; break;
      case 5: acc -= 31; break;
      default: acc += 1; break;
    }
    switch (operands[i] & 7) {
      case 0: acc += 100; break;
      case 1: acc -= 200; break;
      case 2: acc = ext_op(operands[i], acc); break;
      case 3: acc ^= 0x55aa; break;
      case 4: acc += 400; break;
      case 5: acc -= 500; break;
      default: break;
    }
  }
  return acc;
}
)";
  const auto vm_trace = hooks_.TraceForwardCompilation(kMultiSwitchLoopSrc, "run_dual_vm",
                                                       {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                                                       "dual_vm_stage0_test");
  ASSERT_GE(vm_trace.stage0_input.jump_tables.size(), 2u);

  // Relocatable `.o`:
  EXPECT_TRUE(verifier_.VerifyFinalAndGasStageRightInverse(vm_trace.stage0_input).matches);
  EXPECT_TRUE(verifier_.VerifySuffixRightInverse(RtlPassKind::kDbr, vm_trace.stage0_input).matches);

  // Linked ROM (normal `.rodata` order AND reversed `.rodata` order + stripped JT names!):
  Stage0FunctionInput vm_linked =
      SimulateLinkedRomStage0Input(vm_trace.stage0_input, /*same_hi_page_for_globals=*/true);
  const auto vm_ok = verifier_.VerifyFinalAndGasStageRightInverse(vm_linked);
  EXPECT_TRUE(vm_ok.matches) << vm_ok.diff_detail;

  Stage0FunctionInput vm_linked_rev = vm_linked;
  std::reverse(vm_linked_rev.jump_tables.begin(), vm_linked_rev.jump_tables.end());
  for (auto& jt : vm_linked_rev.jump_tables) {
    jt.label_name.clear();
  }
  const auto vm_rev_ok = verifier_.VerifyFinalAndGasStageRightInverse(vm_linked_rev);
  EXPECT_TRUE(vm_rev_ok.matches) << vm_rev_ok.diff_detail;
  const auto vm_rev_suf = verifier_.VerifySuffixRightInverse(RtlPassKind::kDbr, vm_linked_rev);
  EXPECT_TRUE(vm_rev_suf.matches) << vm_rev_suf.diff_detail;
  const RtlControlFlowGraph lifted_vm_reloc = InvertFinalAndGasFunction(vm_trace.stage0_input);
  const RtlControlFlowGraph lifted_vm_rev = InvertFinalAndGasFunction(vm_linked_rev);
  EXPECT_TRUE(lifted_vm_rev.CompareAgainst(lifted_vm_reloc).MatchesExactly());

  // Adversarial negative: shifting `table_vram` on `vm_linked` leaves the indirect jump
  // unresolved and must fail `VerifyFinalAndGasStageRightInverse`!
  Stage0FunctionInput vm_bad_jt_vram = vm_linked;
  vm_bad_jt_vram.jump_tables[0].table_vram += 64u;
  EXPECT_FALSE(verifier_.VerifyFinalAndGasStageRightInverse(vm_bad_jt_vram).matches);

  // Swapping `table_vram` on `vm_linked` binds the two switches to each other's target tables,
  // which must NOT match `lifted_vm_reloc`!
  Stage0FunctionInput vm_swapped_jt = vm_linked;
  std::swap(vm_swapped_jt.jump_tables[0].table_vram, vm_swapped_jt.jump_tables[1].table_vram);
  EXPECT_FALSE(
      InvertFinalAndGasFunction(vm_swapped_jt).CompareAgainst(lifted_vm_reloc).MatchesExactly());
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
  const auto res_suffix = verifier->VerifySuffixRightInverse(RtlPassKind::kDbr, trace.stage0_input);
  EXPECT_TRUE(res_suffix.matches) << "Test case: " << tc.test_name << "\nSuffix diff:\n"
                                  << res_suffix.diff_detail;

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
