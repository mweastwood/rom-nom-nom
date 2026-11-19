#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gcc_harness/toy_subfunction_inverses.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::gcc_harness {
namespace {

TEST(GccContextTest, ParseRtxAndFormatRtxPreserveFlagsAndModes) {
  GccContext ctx;
  ctx.BeginFunction("flag_test");

  Rtx user_reg = ctx.ParseRtx("(reg/v:SI 72)");
  ASSERT_NE(user_reg, nullptr);
  EXPECT_EQ(user_reg->code, kReg);
  EXPECT_EQ(user_reg->mode, kSiMode);
  EXPECT_EQ(user_reg->volatil, 1u);
  EXPECT_EQ(ctx.FormatRtx(user_reg), "(reg/v:SI 72)");

  Rtx ret_reg = ctx.ParseRtx("(reg/i:SI 2 v0)");
  ASSERT_NE(ret_reg, nullptr);
  EXPECT_EQ(ret_reg->code, kReg);
  EXPECT_EQ(ret_reg->integrated, 1u);
  EXPECT_EQ(ctx.FormatRtx(ret_reg), "(reg/i:SI 2 v0)");

  Rtx struct_mem = ctx.ParseRtx("(mem/s:HI (plus:SI (reg:SI 4 a0) (const_int 6)))");
  ASSERT_NE(struct_mem, nullptr);
  EXPECT_EQ(struct_mem->code, kMem);
  EXPECT_EQ(struct_mem->mode, kHiMode);
  EXPECT_EQ(struct_mem->in_struct, 1u);
  EXPECT_EQ(ctx.FormatRtx(struct_mem), "(mem/s:HI (plus:SI (reg:SI 4 a0) (const_int 6)))");

  Rtx const_mem = ctx.ParseRtx("(mem/u:SI (symbol_ref:SI (\"g_table\")))");
  ASSERT_NE(const_mem, nullptr);
  EXPECT_EQ(const_mem->code, kMem);
  EXPECT_EQ(const_mem->unchanging, 1u);

  // Verify `ParseRtx` preserves explicit mode on comparison inside
  // `if_then_else` without secret mode coercion.
  Rtx cond_void =
      ctx.ParseRtx("(if_then_else (eq (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc))");
  ASSERT_NE(cond_void, nullptr);
  EXPECT_EQ(cond_void->fld[0].rtx->mode, kVoidMode);

  Rtx cond_si =
      ctx.ParseRtx("(if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc))");
  ASSERT_NE(cond_si, nullptr);
  EXPECT_EQ(cond_si->fld[0].rtx->mode, kSiMode);
}

TEST(GccContextTest, PseudoRegistersSharePointerIdentityWhileHardRegFlagsAreIsolated) {
  GccContext ctx;
  ctx.BeginFunction("reg_identity_test");

  Rtx p1 = ctx.ParseRtx("(reg:SI 75)");
  Rtx p2 = ctx.ParseRtx("(reg/v:SI 75)");
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(p1, p2);
  EXPECT_EQ(p1->volatil, 1u);

  Rtx hard_i = ctx.ParseRtx("(reg/i:SI 2 v0)");
  Rtx hard_plain = ctx.ParseRtx("(reg:SI 2 v0)");
  ASSERT_NE(hard_i, nullptr);
  ASSERT_NE(hard_plain, nullptr);
  EXPECT_NE(hard_i, hard_plain);
  EXPECT_EQ(hard_i->integrated, 1u);
  EXPECT_EQ(hard_plain->integrated, 0u);
}

TEST(GccContextTest, MaterializeAndDematerializeMultiBlockCfgWithSequencesNotesAndLabels) {
  GccContext ctx;
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(3);

  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(note NOTE_INSN_LOOP_BEG)");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L0) (pc)))) (insn (set (reg/v:SI 3 v1) "
      "(const_int 1)))])");
  cfg.rpo_blocks[0].successors = {2, 1};

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg/v:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  cfg.rpo_blocks[1].AddPatternOnly("(note NOTE_INSN_LOOP_END)");
  cfg.rpo_blocks[1].successors = {2};

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  cfg.rpo_blocks[2].successors = {};

  Rtx head = ctx.MaterializeFromCfg(cfg);
  ASSERT_NE(head, nullptr);

  RtlControlFlowGraph round_trip = ctx.DematerializeToCfg(head, RtlPassKind::kDbr);
  RtlCfgPassComparison cmp = round_trip.CompareAgainst(cfg);
  EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
}

TEST(GccContextTest, MaterializeAndDematerializeSyncsRtlSidebandState) {
  GccContext ctx;
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kLreg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  cfg.sideband.max_regno = 76;
  cfg.sideband.reg_renumber[72] = 2;
  cfg.sideband.reg_renumber[74] = 16;
  cfg.sideband.reg_n_sets[72] = 1;
  cfg.sideband.reg_n_deaths[72] = 1;
  cfg.sideband.reg_live_length[72] = 4;
  cfg.sideband.reg_n_calls_crossed[74] = 2;
  cfg.sideband.basic_block_live_at_start[0].insert(4);
  cfg.sideband.basic_block_live_at_start[0].insert(72);
  cfg.sideband.epilogue_delay_pattern = "(set (reg:SI 2 v0) (const_int 9))";

  Rtx head = ctx.MaterializeFromCfg(cfg);
  ASSERT_NE(head, nullptr);
  EXPECT_GE(ctx.MaxRegNum(), 76);
  EXPECT_EQ(ctx.GetRegRenumber(72), 2);
  EXPECT_EQ(ctx.GetRegRenumber(74), 16);
  EXPECT_EQ(ctx.GetRegRenumber(73), -1);

  ctx.SetRegRenumber(75, 3);
  EXPECT_EQ(ctx.GetRegRenumber(75), 3);

  RtlControlFlowGraph demat = ctx.DematerializeToCfg(head, RtlPassKind::kLreg);
  EXPECT_GE(demat.sideband.max_regno, 76);
  EXPECT_EQ(demat.sideband.reg_renumber[72], 2);
  EXPECT_EQ(demat.sideband.reg_renumber[74], 16);
  EXPECT_EQ(demat.sideband.reg_renumber[75], 3);
  EXPECT_EQ(demat.sideband.reg_n_sets[72], 1);
  EXPECT_EQ(demat.sideband.reg_n_deaths[72], 1);
  EXPECT_EQ(demat.sideband.reg_live_length[72], 4);
  EXPECT_EQ(demat.sideband.reg_n_calls_crossed[74], 2);
  EXPECT_TRUE(demat.sideband.basic_block_live_at_start[0].contains(4));
  EXPECT_TRUE(demat.sideband.basic_block_live_at_start[0].contains(72));
  ASSERT_TRUE(demat.sideband.epilogue_delay_pattern.has_value());
  EXPECT_EQ(*demat.sideband.epilogue_delay_pattern, "(set (reg:SI 2 v0) (const_int 9))");
}

TEST(FunctionHooksTest, Stage0FinalAndGasHooksEmitAndAssembleMachineWords) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("stage0_hook_test");
  Rtx insn = ctx.EmitPattern("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 5)))");
  ASSERT_NE(insn, nullptr);

  std::string single_asm = hooks.RunFinalScanInsn(insn);
  EXPECT_TRUE(absl::StrContains(single_asm, "addu\t$2,$4,5")) << single_asm;

  uint32_t ip_word = hooks.RunGasMipsIp("addiu $2,$4,5");
  EXPECT_EQ(ip_word, 0x24820005u);

  uint32_t single_word = hooks.RunGasAppendInsn("addiu $2,$4,5");
  EXPECT_EQ(single_word, 0x24820005u);

  std::vector<uint32_t> macro_words = hooks.RunGasMacro("li $2,0x12345678");
  ASSERT_EQ(macro_words.size(), 2u);
  EXPECT_EQ(macro_words[0], 0x3c021234u);
  EXPECT_EQ(macro_words[1], 0x34425678u);

  std::string prologue = hooks.RunFunctionPrologue(0);
  std::string epilogue = hooks.RunFunctionEpilogue(0);
  EXPECT_TRUE(absl::StrContains(prologue, ".ent\tstage0_hook_test"));
  EXPECT_TRUE(absl::StrContains(epilogue, "j\t$31"));

  std::vector<uint32_t> branch_words = hooks.RunGasMdAssemble(
      "\tbeq\t$4,$0,$L2\n"
      "\tnop\n"
      "\taddiu\t$2,$4,1\n"
      "$L2:\n"
      "\tj\t$31\n"
      "\tnop\n");
  ASSERT_EQ(branch_words.size(), 5u);
  EXPECT_EQ(branch_words[0], 0x10800002u);
  EXPECT_EQ(branch_words[1], 0x00000000u);
  EXPECT_EQ(branch_words[2], 0x24820001u);
  EXPECT_EQ(branch_words[3], 0x03e00008u);
  EXPECT_EQ(branch_words[4], 0x00000000u);
}

TEST(FunctionHooksTest, Stage1ReorgSubfunctionHooksFillDelaySlotsAndReturns) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // Test `end_of_function_label` cleanup + genuine `make_return_insns`:
  ctx.BeginFunction("reorg_return_test");
  ctx.EmitPattern("(set (reg/i:SI 2 v0) (const_int 7))");
  ctx.EmitPattern("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  ctx.EmitPattern("(barrier)");
  ctx.EmitPattern("(code_label L0)");

  hooks.RunMakeReturnInsns();
  EXPECT_TRUE(absl::StrContains(ctx.DumpInsnChain(), "(return)")) << ctx.DumpInsnChain();
  EXPECT_FALSE(absl::StrContains(ctx.DumpInsnChain(), "code_label")) << ctx.DumpInsnChain();

  // Test `fill_simple_delay_slots`:
  ctx.BeginFunction("reorg_simple_delay_test");
  ctx.EmitPattern("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  ctx.EmitPattern("(set (pc) (label_ref L0))");
  ctx.EmitPattern("(barrier)");
  ctx.EmitPattern("(code_label L0)");
  hooks.RunFillSimpleDelaySlots(/*non_jumps_p=*/0);
  RtlControlFlowGraph simple_cfg = ctx.DematerializeToCfg();
  ASSERT_FALSE(simple_cfg.rpo_blocks.empty());
  ASSERT_FALSE(simple_cfg.rpo_blocks[0].instructions.empty());
  EXPECT_TRUE(absl::StartsWith(simple_cfg.rpo_blocks[0].instructions[0], "(sequence ["));
}

TEST(FunctionHooksTest, Stage2JumpCrossJumpHooksDetectAndMergeIdenticalTails) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("cross_jump_hook_test");
  ctx.EmitPattern(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))");
  ctx.EmitPattern("(set (reg:SI 3 v1) (const_int 10))");
  Rtx tail1 = ctx.EmitPattern("(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))");
  Rtx j1 = ctx.EmitPattern("(set (pc) (label_ref L1))");
  ctx.EmitPattern("(barrier)");
  ctx.EmitPattern("(code_label L0)");
  ctx.EmitPattern("(set (reg:SI 3 v1) (const_int 20))");
  Rtx tail2 = ctx.EmitPattern("(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))");
  Rtx l1 = ctx.EmitPattern("(code_label L1)");

  CrossJumpSearchResult cj = hooks.RunFindCrossJump(j1, l1, 1);
  EXPECT_EQ(cj.e1, tail1);
  EXPECT_EQ(cj.e2, tail2);

  hooks.RunDoCrossJump(j1, cj.e1, cj.e2);
  RtlControlFlowGraph after_cj = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kJump2);
  EXPECT_EQ(after_cj.BlockCount(), 4u);
}

TEST(FunctionHooksTest, Stage3SchedHooksScheduleBasicBlock) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("sched_hook_test");
  ctx.EmitPattern("(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2)))");
  ctx.EmitPattern("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
  ctx.EmitPattern("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1)))");
  ctx.EmitPattern("(use (reg/i:SI 2 v0))");

  hooks.RunScheduleBlock(0);
  RtlControlFlowGraph scheduled = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kSched2);
  ASSERT_EQ(scheduled.BlockCount(), 1u);
  ASSERT_EQ(scheduled.rpo_blocks[0].instructions.size(), 4u);
  // Load latency scheduling moves the independent `(reg:SI 3 v1)` addition
  // between the load and its consumer.
  EXPECT_TRUE(absl::StrContains(scheduled.rpo_blocks[0].instructions[1], "(reg:SI 3 v1)"));
}

TEST(FunctionHooksTest, Stage4And5AllocHooksAllocateLocalAndGlobalPseudos) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("alloc_hook_test");
  ctx.EmitPattern("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  ctx.EmitPattern("(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 2)))");
  ctx.EmitPattern("(use (reg/i:SI 2 v0))");

  hooks.RunLocalAlloc();
  ASSERT_NE(reg_renumber, nullptr);
  EXPECT_GE(reg_renumber[72], 0);
  hooks.RunAlterReg(72, -1);

  RtlControlFlowGraph allocated = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kLreg);
  ASSERT_EQ(allocated.BlockCount(), 1u);
  EXPECT_FALSE(absl::StrContains(allocated.rpo_blocks[0].instructions[0], "(reg:SI 72)"));
}

TEST(FunctionHooksTest, Stage7CombineHooksSimplifyAndCombineLinkedInsns) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("combine_hook_test");
  Rtx i2 = ctx.EmitPattern("(set (reg:SI 2 v0) (lt:SI (reg:SI 4 a0) (const_int 1)))");
  Rtx i3 = ctx.EmitPattern("(set (reg:SI 2 v0) (xor:SI (reg:SI 2 v0) (const_int 1)))");
  ctx.EmitPattern("(use (reg/i:SI 2 v0))");

  // Direct test of `simplify_rtx`:
  Rtx x = ctx.ParseRtx("(xor:SI (lt:SI (reg:SI 4 a0) (const_int 1)) (const_int 1))");
  Rtx simplified = hooks.RunSimplifyRtx(x, kSiMode);
  ASSERT_NE(simplified, nullptr);
  EXPECT_EQ(ctx.FormatRtx(simplified), "(ge:SI (reg:SI 4 a0) (const_int 1))");

  // Direct test of `try_combine`:
  i3->fld[5].rtx = gen_rtx(kInsnList, kVoidMode, i2, nullptr);
  hooks.RunTryCombine(i3, i2, nullptr);
  EXPECT_EQ(i2->code, kNote);
  EXPECT_EQ(i2->fld[4].rtint, kNoteInsnDeleted);
  EXPECT_EQ(ctx.FormatRtx(i3->fld[3].rtx),
            "(set (reg:SI 2 v0) (gt:SI (reg:SI 4 a0) (const_int 0)))");
}

TEST(FunctionHooksTest, Stage8FlowHooksDeleteDeadStoresAndAttachRegDeadNotes) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("flow_hook_test");
  Rtx dead_insn = ctx.EmitPattern("(set (reg:SI 2 v0) (const_int 1))");
  ctx.EmitPattern("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 2)))");
  Rtx live_insn = ctx.EmitPattern("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 3)))");
  ctx.EmitPattern("(use (reg/i:SI 2 v0))");

  hooks.RunFlowAnalysis();
  EXPECT_EQ(dead_insn->code, kNote);
  EXPECT_EQ(dead_insn->fld[4].rtint, kNoteInsnDeleted);
  ASSERT_NE(live_insn->fld[6].rtx, nullptr);
  EXPECT_EQ(live_insn->fld[6].rtx->mode, kRegDead);
}

TEST(FunctionHooksTest, Stage9And11CseHooksFoldIdentityXorAndConstants) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("cse_hook_test");
  Rtx expr = ctx.ParseRtx("(plus:SI (const_int 10) (const_int 22))");
  Rtx folded = hooks.RunFoldRtx(expr);
  ASSERT_NE(folded, nullptr);
  EXPECT_EQ(ctx.FormatRtx(folded), "(const_int 32)");

  ctx.EmitPattern("(set (reg:SI 72) (const_int 5))");
  Rtx add_insn = ctx.EmitPattern("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (const_int 3)))");
  hooks.RunCseMain(/*first=*/nullptr, /*nregs=*/0, /*after_loop=*/0);
  EXPECT_EQ(ctx.FormatRtx(add_insn->fld[3].rtx), "(set (reg/i:SI 2 v0) (const_int 8))");
}

TEST(FunctionHooksTest, Stage10LoopOptimizeHoistsLoopInvariantComputation) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  ctx.BeginFunction("loop_hook_test");
  ctx.EmitPattern("(set (reg:SI 74) (reg:SI 4 a0))");
  ctx.EmitPattern("(set (reg:SI 73) (const_int 0))");
  ctx.EmitPattern("(note NOTE_INSN_LOOP_BEG)");
  ctx.EmitPattern("(code_label L0)");
  ctx.EmitPattern("(set (reg:SI 72) (plus:SI (reg:SI 74) (const_int 16)))");
  ctx.EmitPattern("(set (reg:SI 73) (plus:SI (reg:SI 73) (reg:SI 72)))");
  ctx.EmitPattern("(note NOTE_INSN_LOOP_CONT)");
  ctx.EmitPattern(
      "(set (pc) (if_then_else (lt:SI (reg:SI 73) (const_int 100)) "
      "(label_ref L0) (pc)))");
  ctx.EmitPattern("(note NOTE_INSN_LOOP_END)");
  ctx.EmitPattern("(set (reg/i:SI 2 v0) (reg:SI 73))");
  ctx.EmitPattern("(use (reg/i:SI 2 v0))");

  hooks.RunLoopOptimize(/*first=*/nullptr, /*unroll_p=*/0);
  RtlControlFlowGraph loop_cfg = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kLoop);
  ASSERT_GE(loop_cfg.BlockCount(), 2u);
  bool found_in_preheader = false;
  for (const std::string& pat : loop_cfg.rpo_blocks[0].instructions) {
    if (absl::StrContains(pat, "(set (reg:SI 72) (plus:SI (reg:SI 74) (const_int 16)))")) {
      found_in_preheader = true;
    }
  }
  EXPECT_TRUE(found_in_preheader);
}

// =============================================================================
// 1-to-1 Right-Inverse Property Verification Tests: f_i(g_i(x)) == x
// across all 26 individual GCC 2.7.2 & Binutils 2.6 GAS functions
// =============================================================================

TEST(RightInverseVerifierTest,
     Stage0SubfunctionRightInversesForMipsIpMacroAppendInsnFinalScanAndPrologue) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 1. `tc-mips.c:mips_ip` right-inverse:
  RightInverseResult ip_res1 = verifier.VerifyGasMipsIpRightInverse(0x24820005u);  // addiu $2,$4,5
  EXPECT_TRUE(ip_res1.matches) << ip_res1.diff_detail;
  RightInverseResult ip_res2 = verifier.VerifyGasMipsIpRightInverse(0x00851021u);  // addu $2,$4,$5
  EXPECT_TRUE(ip_res2.matches) << ip_res2.diff_detail;

  // 2. `tc-mips.c:macro` right-inverse (`li $v0, 0x80012345`):
  const std::vector<uint32_t> macro_words = {
      0x3c028001u,  // lui $v0, 0x8001
      0x34422345u,  // ori $v0, $v0, 0x2345
  };
  RightInverseResult macro_res = verifier.VerifyGasMacroRightInverse(macro_words);
  EXPECT_TRUE(macro_res.matches) << macro_res.diff_detail;

  // 3. `tc-mips.c:append_insn` (`.set reorder`) right-inverse:
  // Case 3a: GAS swaps `addiu $2,$4,5` into the delay slot of `jr $31`.
  const std::vector<uint32_t> append_swap_words = {
      0x03e00008u,  // jr $31
      0x24820005u,  // addiu $2,$4,5
  };
  RightInverseResult append_swap_res = verifier.VerifyGasAppendInsnRightInverse(append_swap_words);
  EXPECT_TRUE(append_swap_res.matches) << append_swap_res.diff_detail;

  // Case 3b: GAS inserts `nop` into the delay slot of `jr $31` when `mips_emit_delays` is active.
  const std::vector<uint32_t> append_nop_words = {
      0x03e00008u,  // jr $31
      0x00000000u,  // nop
  };
  RightInverseResult append_nop_res = verifier.VerifyGasAppendInsnRightInverse(append_nop_words);
  EXPECT_TRUE(append_nop_res.matches) << append_nop_res.diff_detail;

  // 4. `final.c:final_scan_insn` right-inverse:
  RightInverseResult final_scan_res1 = verifier.VerifyFinalScanInsnRightInverse("addu\t$2,$4,$5");
  EXPECT_TRUE(final_scan_res1.matches) << final_scan_res1.diff_detail;
  RightInverseResult final_scan_res2 = verifier.VerifyFinalScanInsnRightInverse("lw\t$2,16($4)");
  EXPECT_TRUE(final_scan_res2.matches) << final_scan_res2.diff_detail;

  // 5. `mips.c:function_prologue` + `mips.c:function_epilogue` right-inverse:
  ctx.BeginFunction("prologue_epilogue_verify_fn");
  const std::string target_prologue_epilogue =
      hooks.RunFunctionPrologueEpilogue(/*frame_size=*/24,
                                        /*saved_gpr_mask=*/0x80030000u);
  RightInverseResult prol_res =
      verifier.VerifyFunctionPrologueEpilogueRightInverse(target_prologue_epilogue);
  EXPECT_TRUE(prol_res.matches) << prol_res.diff_detail;

  // Whole-stage `final` + `md_assemble` right-inverse on unscheduled & scheduled words:
  const std::vector<uint32_t> unscheduled_words = {
      0x24820005u,  // addiu $v0, $a0, 5
      0x03e00008u,  // jr $ra
      0x00000000u,  // nop
  };
  RightInverseResult stage0_res1 = verifier.VerifyFinalAndGasStageRightInverse(unscheduled_words);
  EXPECT_TRUE(stage0_res1.matches) << stage0_res1.diff_detail;

  const std::vector<uint32_t> scheduled_words = {
      0x00851021u,  // addu $v0, $a0, $a1
      0x03e00008u,  // jr $ra
      0x00021080u,  // sll $v0, $v0, 2 (in return delay slot)
  };
  RightInverseResult stage0_res2 = verifier.VerifyFinalAndGasStageRightInverse(scheduled_words);
  EXPECT_TRUE(stage0_res2.matches) << stage0_res2.diff_detail;
}

TEST(RightInverseVerifierTest, Stage1DbrScheduleAndSubfunctionsRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 6. `reorg.c:make_return_insns` + `end_of_function_label` right-inverse:
  RtlControlFlowGraph ret_cfg;
  ret_cfg.pass = RtlPassKind::kDbr;
  ret_cfg.rpo_blocks.resize(1);
  ret_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
      "(insn (set (reg/i:SI 2 v0) (const_int 42)))])");
  RightInverseResult ret_res = verifier.VerifyMakeReturnInsnsRightInverse(ret_cfg);
  EXPECT_TRUE(ret_res.matches) << ret_res.diff_detail;

  // 7. `reorg.c:relax_delay_slots` right-inverse (including multi-block B1 branch):
  RtlControlFlowGraph relax_cfg;
  relax_cfg.pass = RtlPassKind::kDbr;
  relax_cfg.rpo_blocks.resize(4);
  relax_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (lt:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))");
  relax_cfg.rpo_blocks[0].successors = {2, 1};
  relax_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 3 v1) (const_int 0)) "
      "(label_ref L1) (pc)))");
  relax_cfg.rpo_blocks[1].successors = {3, 2};
  relax_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 1))");
  relax_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  relax_cfg.rpo_blocks[3].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 2))");
  relax_cfg.rpo_blocks[3].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  RightInverseResult relax_res = verifier.VerifyRelaxDelaySlotsRightInverse(relax_cfg);
  EXPECT_TRUE(relax_res.matches) << relax_res.diff_detail;

  // 8. `reorg.c:fill_eager_delay_slots` right-inverse on BOTH fallthrough (`insn`)
  // and target-thread (`insn/s`) stolen delay slots:
  RtlControlFlowGraph eager_fallthrough_cfg;
  eager_fallthrough_cfg.pass = RtlPassKind::kDbr;
  eager_fallthrough_cfg.rpo_blocks.resize(3);
  eager_fallthrough_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L0) (pc)))) (insn (set (reg:SI 3 v1) "
      "(const_int 1)))])");
  eager_fallthrough_cfg.rpo_blocks[0].successors = {2, 1};
  eager_fallthrough_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  eager_fallthrough_cfg.rpo_blocks[1].successors = {2};
  eager_fallthrough_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))");
  eager_fallthrough_cfg.rpo_blocks[2].AddPatternOnly(
      "(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  eager_fallthrough_cfg.sideband.basic_block_live_at_start = {
      {0, {2, 4, 5, 31}}, {1, {3, 4, 5, 31}}, {2, {2, 4, 31}}};
  RightInverseResult eager_ft_res =
      verifier.VerifyFillEagerDelaySlotsRightInverse(eager_fallthrough_cfg);
  EXPECT_TRUE(eager_ft_res.matches) << eager_ft_res.diff_detail;

  // Case 8b: Target-thread stolen delay slot (`insn/s`):
  RtlControlFlowGraph eager_target_cfg;
  eager_target_cfg.pass = RtlPassKind::kDbr;
  eager_target_cfg.rpo_blocks.resize(3);
  eager_target_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
      "(const_int 0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 3 v1) "
      "(const_int 9)))])");
  eager_target_cfg.rpo_blocks[0].successors = {2, 1};
  eager_target_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 5 a1) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  eager_target_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 5 a1))");
  eager_target_cfg.rpo_blocks[1].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  eager_target_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))");
  eager_target_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  eager_target_cfg.sideband.basic_block_live_at_start = {
      {0, {4, 5, 31}}, {1, {4, 31}}, {2, {3, 5, 31}}};
  RightInverseResult eager_tgt_res =
      verifier.VerifyFillEagerDelaySlotsRightInverse(eager_target_cfg);
  EXPECT_TRUE(eager_tgt_res.matches) << eager_tgt_res.diff_detail;

  // 9. `reorg.c:fill_simple_delay_slots` right-inverse:
  RtlControlFlowGraph simple_cfg;
  simple_cfg.pass = RtlPassKind::kDbr;
  simple_cfg.rpo_blocks.resize(2);
  simple_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (label_ref L0))) "
      "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3))))])");
  simple_cfg.rpo_blocks[0].successors = {1};
  simple_cfg.rpo_blocks[1].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  RightInverseResult simple_res = verifier.VerifyFillSimpleDelaySlotsRightInverse(simple_cfg);
  EXPECT_TRUE(simple_res.matches) << simple_res.diff_detail;

  // 10. `reorg.c:dbr_schedule` right-inverse:
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks.resize(1);
  dbr_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
      "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))))])");
  RightInverseResult dbr_res = verifier.VerifyDbrStageRightInverse(dbr_cfg);
  EXPECT_TRUE(dbr_res.matches) << dbr_res.diff_detail;
}

TEST(RightInverseVerifierTest, Stage2Jump2CrossJumpRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 11. `jump.c:do_cross_jump` + `find_cross_jump` right-inverse:
  RtlControlFlowGraph jump2_cfg;
  jump2_cfg.pass = RtlPassKind::kJump2;
  jump2_cfg.rpo_blocks.resize(4);
  jump2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))");
  jump2_cfg.rpo_blocks[0].successors = {2, 1};

  jump2_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 6 a2) (const_int 1))");
  jump2_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 3 v1) (const_int 10))");
  jump2_cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref L1))");
  jump2_cfg.rpo_blocks[1].successors = {3};

  jump2_cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 6 a2) (const_int 2))");
  jump2_cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 3 v1) (const_int 20))");
  jump2_cfg.rpo_blocks[2].successors = {3};

  jump2_cfg.rpo_blocks[3].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))");
  jump2_cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 2 v0) (reg:SI 3 v1))");
  jump2_cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");
  jump2_cfg.rpo_blocks[3].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  jump2_cfg.rpo_blocks[3].successors = {};

  RightInverseResult res = verifier.VerifyDoCrossJumpRightInverse(jump2_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST(RightInverseVerifierTest, Stage3Sched2AndStage6Sched1RightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 12. `sched.c:schedule_block` / `schedule_insns` right-inverse:
  RtlControlFlowGraph sched2_cfg;
  sched2_cfg.pass = RtlPassKind::kSched2;
  sched2_cfg.rpo_blocks.resize(1);
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sched2_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  RightInverseResult sb_res = verifier.VerifyScheduleBlockRightInverse(sched2_cfg);
  EXPECT_TRUE(sb_res.matches) << sb_res.diff_detail;

  RightInverseResult sched2_res = verifier.VerifySched2StageRightInverse(sched2_cfg);
  EXPECT_TRUE(sched2_res.matches) << sched2_res.diff_detail;

  RtlControlFlowGraph sched1_cfg = sched2_cfg;
  sched1_cfg.pass = RtlPassKind::kSched;
  RightInverseResult sched1_res = verifier.VerifySched1StageRightInverse(sched1_cfg);
  EXPECT_TRUE(sched1_res.matches) << sched1_res.diff_detail;
}

TEST(RightInverseVerifierTest, Stage4And5AlterRegBlockAllocAndUpdateEquivRegsRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 13. `reload1.c:alter_reg` & `global.c:global_alloc` right-inverse:
  RtlControlFlowGraph greg_cfg;
  greg_cfg.pass = RtlPassKind::kGreg;
  greg_cfg.rpo_blocks.resize(1);
  greg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  greg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  greg_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  RightInverseResult alter_res = verifier.VerifyAlterRegRightInverse(greg_cfg);
  EXPECT_TRUE(alter_res.matches) << alter_res.diff_detail;

  RightInverseResult greg_res = verifier.VerifyGlobalReloadStageRightInverse(greg_cfg);
  EXPECT_TRUE(greg_res.matches) << greg_res.diff_detail;

  // `global.c:global_alloc` boundary right-inverse (`VerifyGlobalAllocRightInverse`):
  RtlControlFlowGraph global_alloc_pre;
  global_alloc_pre.pass = RtlPassKind::kLreg;
  global_alloc_pre.rpo_blocks.resize(1);
  global_alloc_pre.rpo_blocks[0].id = 0;
  global_alloc_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))");
  global_alloc_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 2)))");
  global_alloc_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  const SubfunctionBoundarySnapshot global_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, global_alloc_pre);
  ASSERT_TRUE(global_snap.post_state.sideband.reg_renumber.contains(72));
  RightInverseResult global_alloc_res =
      verifier.VerifyGlobalAllocRightInverse(global_snap.post_state);
  EXPECT_TRUE(global_alloc_res.matches) << global_alloc_res.diff_detail;

  // `reload1.c:reload` boundary right-inverse (`VerifyReloadRightInverse`):
  RtlControlFlowGraph reload_pre;
  reload_pre.pass = RtlPassKind::kGreg;
  reload_pre.rpo_blocks.resize(1);
  reload_pre.rpo_blocks[0].id = 0;
  reload_pre.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (mem:SI (reg:SI 30 fp)))");
  reload_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  const SubfunctionBoundarySnapshot reload_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kReload, reload_pre);
  ASSERT_EQ(reload_snap.post_state.BlockCount(), 1u);
  EXPECT_EQ(reload_snap.post_state.rpo_blocks[0].instructions[0],
            "(set (reg/i:SI 2 v0) (mem:SI (reg:SI 29 sp)))");
  RightInverseResult reload_res = verifier.VerifyReloadRightInverse(reload_snap.post_state);
  EXPECT_TRUE(reload_res.matches) << reload_res.diff_detail;

  // 14. `local-alloc.c:block_alloc` right-inverse (scratch clobber allocation):
  RtlControlFlowGraph block_alloc_cfg;
  block_alloc_cfg.pass = RtlPassKind::kLreg;
  block_alloc_cfg.rpo_blocks.resize(1);
  block_alloc_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) "
      "(expr_list:REG_UNUSED (reg:SI 64 hi) "
      "(expr_list:REG_UNUSED (reg:SI 66 accum) (nil)))))");
  block_alloc_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (reg:SI 65 lo)) "
      "(expr_list:REG_DEAD (reg:SI 65 lo) (nil))");
  block_alloc_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  RightInverseResult ba_res = verifier.VerifyBlockAllocRightInverse(block_alloc_cfg);
  EXPECT_TRUE(ba_res.matches) << ba_res.diff_detail;

  // 15. `local-alloc.c:update_equiv_regs` right-inverse:
  RtlControlFlowGraph equiv_cfg;
  equiv_cfg.pass = RtlPassKind::kLreg;
  equiv_cfg.rpo_blocks.resize(1);
  equiv_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  equiv_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  equiv_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 73) (const_int 2)))");
  equiv_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  equiv_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  RightInverseResult equiv_res = verifier.VerifyUpdateEquivRegsRightInverse(equiv_cfg);
  EXPECT_TRUE(equiv_res.matches) << equiv_res.diff_detail;

  RightInverseResult lreg_res = verifier.VerifyLocalAllocStageRightInverse(equiv_cfg);
  EXPECT_TRUE(lreg_res.matches) << lreg_res.diff_detail;

  // Verify exact reverse compilation order in Stage 4 & Stage 5 verifiers:
  // - Stage 5 (`VerifyLocalAllocStageRightInverse`): `InvertBlockAlloc` BEFORE
  // `InvertUpdateEquivRegs`
  // - Stage 4 (`VerifyGlobalReloadStageRightInverse`): `InvertAlterReg` -> `InvertReload` ->
  // `InvertGlobalAlloc`
  class OrderRecordingProvider final : public ToySubfunctionInverses {
   public:
    mutable std::vector<std::string> calls;

    RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& x) const override {
      calls.push_back("InvertBlockAlloc");
      return ToySubfunctionInverses::InvertBlockAlloc(x);
    }
    RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x) const override {
      calls.push_back("InvertUpdateEquivRegs");
      return ToySubfunctionInverses::InvertUpdateEquivRegs(x);
    }
    RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& x,
                                       int pseudo_regno) const override {
      calls.push_back("InvertAlterReg");
      return ToySubfunctionInverses::InvertAlterReg(x, pseudo_regno);
    }
    RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& x) const override {
      calls.push_back("InvertReload");
      return ToySubfunctionInverses::InvertReload(x);
    }
    RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& x) const override {
      calls.push_back("InvertGlobalAlloc");
      return ToySubfunctionInverses::InvertGlobalAlloc(x);
    }
  };

  OrderRecordingProvider order_provider;
  RightInverseVerifier order_verifier(&ctx, &hooks, &order_provider);
  EXPECT_TRUE(order_verifier.VerifyLocalAllocStageRightInverse(equiv_cfg).matches);
  EXPECT_EQ(order_provider.calls,
            (std::vector<std::string>{"InvertBlockAlloc", "InvertUpdateEquivRegs"}));

  order_provider.calls.clear();
  EXPECT_TRUE(order_verifier.VerifyGlobalReloadStageRightInverse(greg_cfg).matches);
  EXPECT_EQ(order_provider.calls,
            (std::vector<std::string>{"InvertAlterReg", "InvertReload", "InvertGlobalAlloc"}));

  // Verify `RunReload(first, /*global=*/1)` safely initializes `global.c` tables
  // even when reload spills an eliminable/occupied hard register (`retry_global_alloc`)
  // without relying on a live `global_alloc` stack frame.
  ctx.BeginFunction("reload_global_one_fn");
  RtlControlFlowGraph global_reload_cfg;
  global_reload_cfg.pass = RtlPassKind::kGreg;
  global_reload_cfg.sideband.max_regno = 74;
  global_reload_cfg.sideband.reg_renumber[72] = 30;  // Allocated to `$fp`, spans diamond blocks.
  global_reload_cfg.sideband.reg_renumber[73] = 2;
  global_reload_cfg.rpo_blocks.resize(3);
  global_reload_cfg.rpo_blocks[0].id = 0;
  global_reload_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 3)))");
  global_reload_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  global_reload_cfg.rpo_blocks[0].successors = {1, 2};
  global_reload_cfg.rpo_blocks[1].id = 1;
  global_reload_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 10)))");
  global_reload_cfg.rpo_blocks[1].successors = {2};
  global_reload_cfg.rpo_blocks[2].id = 2;
  global_reload_cfg.rpo_blocks[2].has_label = true;
  global_reload_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg:SI 73) (plus:SI (reg:SI 72) (reg:SI 5 a1)))");
  global_reload_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 73))");
  global_reload_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
  SubfunctionBoundarySnapshot global_reload_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kReload, global_reload_cfg, {},
                                       /*optimize_level=*/2, {"-O2", "-fno-omit-frame-pointer"});
  ASSERT_EQ(global_reload_snap.post_state.BlockCount(), 3u);
  // Because `global == 1` (`optimize_level > 0`) and `-fno-omit-frame-pointer`
  // forced `spill_hard_reg(30, /*global=*/1, ...)`, `retry_global_alloc`
  // reallocated pseudo `72` into another hard GPR instead of spilling to `mem`.
  EXPECT_TRUE(global_reload_snap.post_state.sideband.reg_renumber.contains(72));
  EXPECT_NE(global_reload_snap.post_state.sideband.reg_renumber.at(72), 30);
  EXPECT_GE(global_reload_snap.post_state.sideband.reg_renumber.at(72), 0);

  // Verify single-BB `global_alloc` pseudo classification in `PrepareGlobalReloadState`:
  // `local_alloc` never allocates `$fp` (`30`), so a single-BB pseudo with
  // `reg_renumber[72] = 30` must still receive a `reg_allocno` entry and be
  // reallocated to a free GPR by `retry_global_alloc` under `-fno-omit-frame-pointer`.
  ctx.BeginFunction("reload_global_single_bb_fp_fn");
  RtlControlFlowGraph single_bb_fp_cfg;
  single_bb_fp_cfg.pass = RtlPassKind::kGreg;
  single_bb_fp_cfg.sideband.max_regno = 73;
  single_bb_fp_cfg.sideband.reg_renumber[72] = 30;
  single_bb_fp_cfg.rpo_blocks.resize(1);
  single_bb_fp_cfg.rpo_blocks[0].id = 0;
  single_bb_fp_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 5)))");
  single_bb_fp_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 1)))");
  single_bb_fp_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  SubfunctionBoundarySnapshot single_bb_fp_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kReload, single_bb_fp_cfg, {},
                                       /*optimize_level=*/2, {"-O2", "-fno-omit-frame-pointer"});
  ASSERT_EQ(single_bb_fp_snap.post_state.BlockCount(), 1u);
  EXPECT_TRUE(single_bb_fp_snap.post_state.sideband.reg_renumber.contains(72));
  EXPECT_NE(single_bb_fp_snap.post_state.sideband.reg_renumber.at(72), 30);
  EXPECT_GE(single_bb_fp_snap.post_state.sideband.reg_renumber.at(72), 0);

  // Verify `-O0` (`optimize == 0`) stage-level right-inverse replay:
  // - `VerifyLocalAllocStageRightInverse` skips `local_alloc` (`toplev.c:3048-3053`).
  // - `VerifyGlobalReloadStageRightInverse` runs `stupid_life_analysis` +
  //   `RunReload(nullptr, /*global=*/0)` (`toplev.c:3079-3083`), allocating
  //   `register int acc` to `$s0` (`16`) rather than `global_alloc`'s `$v0` (`2`).
  const FunctionCompilationTrace o0_stage_trace = hooks.TraceForwardCompilation(
      "int add_two_reg(int a, int b) { register int acc = a + b; return acc; }\n", "add_two_reg",
      {"-O0"}, "stage4_stage5_o0_verify");
  ASSERT_EQ(optimize, 0);
  ASSERT_TRUE(o0_stage_trace.pass_snapshots.contains(RtlPassKind::kLreg));
  ASSERT_TRUE(o0_stage_trace.pass_snapshots.contains(RtlPassKind::kGreg));
  RightInverseResult o0_lreg_res = verifier.VerifyLocalAllocStageRightInverse(
      o0_stage_trace.pass_snapshots.at(RtlPassKind::kLreg));
  EXPECT_TRUE(o0_lreg_res.matches) << o0_lreg_res.diff_detail;
  RightInverseResult o0_greg_res = verifier.VerifyGlobalReloadStageRightInverse(
      o0_stage_trace.pass_snapshots.at(RtlPassKind::kGreg));
  EXPECT_TRUE(o0_greg_res.matches) << o0_greg_res.diff_detail;

  // Restore default `-O2` compiler flags for subsequent tests in the suite.
  hooks.ExecuteSubfunctionBoundary(SubfunctionId::kReload, reload_pre, {}, /*optimize_level=*/2,
                                   {"-O2"});
}

TEST(RightInverseVerifierTest, Stage7SimplifyRtxSubstAndTryCombineRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 16. `combine.c:simplify_rtx` right-inverse:
  RightInverseResult simp_res =
      verifier.VerifySimplifyRtxRightInverse("(ge:SI (reg:SI 4 a0) (const_int 1))");
  EXPECT_TRUE(simp_res.matches) << simp_res.diff_detail;

  // 17. `combine.c:subst` right-inverse:
  RightInverseResult subst_res =
      verifier.VerifySubstRightInverse("(set (reg:SI 2 v0) (gt:SI (reg:SI 4 a0) (const_int 0)))");
  EXPECT_TRUE(subst_res.matches) << subst_res.diff_detail;

  // 18. `combine.c:try_combine` & `combine_instructions` right-inverse:
  RtlControlFlowGraph combine_cfg;
  combine_cfg.pass = RtlPassKind::kCombine;
  combine_cfg.rpo_blocks.resize(1);
  combine_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (gt:SI (reg:SI 4 a0) (const_int 0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  combine_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  RightInverseResult tc_res = verifier.VerifyTryCombineRightInverse(combine_cfg);
  EXPECT_TRUE(tc_res.matches) << tc_res.diff_detail;

  RightInverseResult comb_res = verifier.VerifyCombineStageRightInverse(combine_cfg);
  EXPECT_TRUE(comb_res.matches) << comb_res.diff_detail;
}

TEST(RightInverseVerifierTest, Stage8FindBasicBlocksAndPropagateBlockRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 19. `flow.c:find_basic_blocks` right-inverse:
  RtlControlFlowGraph fbb_cfg;
  fbb_cfg.pass = RtlPassKind::kFlow;
  fbb_cfg.rpo_blocks.resize(1);
  fbb_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  fbb_cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  RightInverseResult fbb_res = verifier.VerifyFindBasicBlocksRightInverse(fbb_cfg);
  EXPECT_TRUE(fbb_res.matches) << fbb_res.diff_detail;

  // 20. `flow.c:propagate_block` / `life_analysis` / `flow_analysis` right-inverse:
  RtlControlFlowGraph flow_cfg;
  flow_cfg.pass = RtlPassKind::kFlow;
  flow_cfg.rpo_blocks.resize(1);
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (reg:SI 4 a0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (reg:SI 5 a1)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 74) (lt:SI (reg:SI 72) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 72) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil)))");
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (reg:SI 74)) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))");
  flow_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  RightInverseResult prop_res = verifier.VerifyPropagateBlockRightInverse(flow_cfg);
  EXPECT_TRUE(prop_res.matches) << prop_res.diff_detail;

  RightInverseResult flow_res = verifier.VerifyFlowStageRightInverse(flow_cfg);
  EXPECT_TRUE(flow_res.matches) << flow_res.diff_detail;
}

TEST(RightInverseVerifierTest, Stage9And11FoldRtxAndCseBasicBlockRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 21. `cse.c:fold_rtx` right-inverse:
  RightInverseResult fold_res = verifier.VerifyFoldRtxRightInverse("(const_int 32)");
  EXPECT_TRUE(fold_res.matches) << fold_res.diff_detail;

  // 22. `cse.c:cse_basic_block` & `cse_main` right-inverse:
  RtlControlFlowGraph cse_cfg;
  cse_cfg.pass = RtlPassKind::kCse;
  cse_cfg.rpo_blocks.resize(1);
  cse_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  cse_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (reg:SI 72))");
  cse_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 1)))");
  cse_cfg.rpo_blocks[0].AddPatternOnly("(use (reg:SI 73))");
  cse_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  RightInverseResult cse_bb_res = verifier.VerifyCseBasicBlockRightInverse(cse_cfg);
  EXPECT_TRUE(cse_bb_res.matches) << cse_bb_res.diff_detail;

  RightInverseResult cse1_res = verifier.VerifyCse1StageRightInverse(cse_cfg);
  EXPECT_TRUE(cse1_res.matches) << cse1_res.diff_detail;

  RtlControlFlowGraph cse2_cfg = cse_cfg;
  cse2_cfg.pass = RtlPassKind::kCse2;
  RightInverseResult cse2_res = verifier.VerifyCse2StageRightInverse(cse2_cfg);
  EXPECT_TRUE(cse2_res.matches) << cse2_res.diff_detail;
}

TEST(RightInverseVerifierTest, Stage10ScanLoopAndStrengthReduceRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 23. `loop.c:scan_loop` & `loop_optimize` right-inverse:
  RtlControlFlowGraph loop_cfg;
  loop_cfg.pass = RtlPassKind::kLoop;
  loop_cfg.rpo_blocks.resize(3);
  loop_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 74) (reg:SI 4 a0))");
  loop_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (const_int 0))");
  loop_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 74) (const_int 16)))");
  loop_cfg.rpo_blocks[0].AddPatternOnly("(note NOTE_INSN_LOOP_BEG)");
  loop_cfg.rpo_blocks[0].successors = {1};

  loop_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 73) (reg:SI 72)))");
  loop_cfg.rpo_blocks[1].AddPatternOnly("(note NOTE_INSN_LOOP_CONT)");
  loop_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (lt:SI (reg:SI 73) (const_int 100)) "
      "(label_ref L0) (pc)))");
  loop_cfg.rpo_blocks[1].successors = {1, 2};

  loop_cfg.rpo_blocks[2].AddPatternOnly("(note NOTE_INSN_LOOP_END)");
  loop_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 73))");
  loop_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
  loop_cfg.rpo_blocks[2].successors = {};

  RightInverseResult scan_res = verifier.VerifyScanLoopRightInverse(loop_cfg);
  EXPECT_TRUE(scan_res.matches) << scan_res.diff_detail;

  RightInverseResult loop_res = verifier.VerifyLoopStageRightInverse(loop_cfg);
  EXPECT_TRUE(loop_res.matches) << loop_res.diff_detail;

  // 24. `loop.c:strength_reduce` right-inverse:
  RtlControlFlowGraph sr_cfg;
  sr_cfg.pass = RtlPassKind::kLoop;
  sr_cfg.sideband.pointer_regnos = {79};
  sr_cfg.rpo_blocks.resize(3);
  sr_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0))");
  sr_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (const_int 0))");
  sr_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 74) (const_int 0))");
  sr_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 79) (reg:SI 72))");
  sr_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 74) (const_int 99))");
  sr_cfg.rpo_blocks[0].AddPatternOnly("(note NOTE_INSN_LOOP_BEG)");
  sr_cfg.rpo_blocks[0].successors = {1};

  sr_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 76) (mem:SI (reg:SI 79)))");
  sr_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 73) (reg:SI 76)))");
  sr_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 74) (plus:SI (reg:SI 74) (const_int -1)))");
  sr_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 79) (plus:SI (reg:SI 79) (const_int 4)))");
  sr_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (ge:SI (reg:SI 74) (const_int 0)) "
      "(label_ref L0) (pc))) (expr_list:REG_NONNEG (nil) (nil))");
  sr_cfg.rpo_blocks[1].successors = {1, 2};

  sr_cfg.rpo_blocks[2].AddPatternOnly("(note NOTE_INSN_LOOP_END)");
  sr_cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 74) (const_int 100))");
  sr_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 73))");
  sr_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
  sr_cfg.rpo_blocks[2].successors = {};

  RightInverseResult sr_res = verifier.VerifyStrengthReduceRightInverse(sr_cfg);
  EXPECT_TRUE(sr_res.matches) << sr_res.diff_detail;
}

TEST(RightInverseVerifierTest, Stage12JumpRedirectAndJump1OptimizeRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  // 25. `jump.c:redirect_jump` / `invert_jump` right-inverse:
  RtlControlFlowGraph redirect_cfg;
  redirect_cfg.pass = RtlPassKind::kJump;
  redirect_cfg.rpo_blocks.resize(3);
  redirect_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L0) (pc)))");
  redirect_cfg.rpo_blocks[0].successors = {2, 1};
  redirect_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 1))");
  redirect_cfg.rpo_blocks[1].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  redirect_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 2))");
  redirect_cfg.rpo_blocks[2].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  RightInverseResult redir_res = verifier.VerifyJumpRedirectRightInverse(redirect_cfg);
  EXPECT_TRUE(redir_res.matches) << redir_res.diff_detail;

  // 26. `jump.c:jump_optimize` (`cross_jump=0, noop_moves=0`) right-inverse:
  RtlControlFlowGraph expected_jump;
  expected_jump.pass = RtlPassKind::kJump;
  expected_jump.rpo_blocks.resize(1);
  expected_jump.rpo_blocks[0].AddPatternOnly("(set (reg/v:SI 72) (reg:SI 4 a0))");
  expected_jump.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg/v:SI 72) (const_int 1)))");

  RightInverseResult jump1_res = verifier.VerifyJump1StageRightInverse(expected_jump);
  EXPECT_TRUE(jump1_res.matches) << jump1_res.diff_detail;

  // Verify explicit `SubfunctionInverseProvider` injection into `RightInverseVerifier`:
  const ToySubfunctionInverses custom_provider;
  RightInverseVerifier custom_verifier(&ctx, &hooks, &custom_provider);
  EXPECT_EQ(custom_verifier.InverseProvider(), &custom_provider);
  RightInverseResult custom_res = custom_verifier.VerifyJump1StageRightInverse(expected_jump);
  EXPECT_TRUE(custom_res.matches) << custom_res.diff_detail;
}

TEST(FunctionHooksTest, Stage13TreeToRtlExpansionHooksExecuteInProcess) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  FunctionDeclaration two_arg_fn(CType::S32(), "harness_func",
                                 {
                                     CParameter{CType::S32(), "p0"},
                                     CParameter{CType::S32(), "p1"},
                                 },
                                 std::make_unique<CompoundStatement>());

  // 13. `RunExpandFunctionStart`:
  RtlControlFlowGraph start_cfg = hooks.RunExpandFunctionStart(two_arg_fn);
  ASSERT_EQ(start_cfg.rpo_blocks.size(), 1u);
  EXPECT_EQ(start_cfg.rpo_blocks[0].instructions.size(), 2u);

  // 12. `RunExpandDecl`:
  VariableDeclarationStatement local_decl(
      CType::S32(), "local_var",
      CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Integer(5)));
  RtlControlFlowGraph decl_cfg = hooks.RunExpandDecl(local_decl, two_arg_fn);
  ASSERT_FALSE(decl_cfg.rpo_blocks.empty());
  EXPECT_FALSE(decl_cfg.rpo_blocks[0].instructions.empty());

  // 4. `RunExpandExpr`:
  std::unique_ptr<CExpression> add_expr =
      CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Identifier("p1"));
  RtlControlFlowGraph expr_cfg = hooks.RunExpandExpr(*add_expr, two_arg_fn);
  ASSERT_EQ(expr_cfg.rpo_blocks.size(), 1u);
  ASSERT_EQ(expr_cfg.rpo_blocks[0].instructions.size(), 1u);

  // 5. `RunExpandAssignment`:
  RtlControlFlowGraph assign_cfg =
      hooks.RunExpandAssignment(*CExpression::Identifier("p0"), *add_expr, two_arg_fn);
  ASSERT_EQ(assign_cfg.rpo_blocks.size(), 1u);
  ASSERT_FALSE(assign_cfg.rpo_blocks[0].instructions.empty());

  // 6. `RunDoStoreFlag`:
  std::unique_ptr<CExpression> lt_expr =
      CExpression::Binary("<", CExpression::Identifier("p0"), CExpression::Identifier("p1"));
  RtlControlFlowGraph sflag_cfg = hooks.RunDoStoreFlag(*lt_expr, two_arg_fn);
  ASSERT_EQ(sflag_cfg.rpo_blocks.size(), 1u);
  EXPECT_GE(sflag_cfg.rpo_blocks[0].instructions.size(), 1u);

  // 7. `RunDoJump`:
  RtlControlFlowGraph jump_cfg = hooks.RunDoJump(*lt_expr, two_arg_fn);
  ASSERT_EQ(jump_cfg.rpo_blocks.size(), 1u);
  ASSERT_FALSE(jump_cfg.rpo_blocks[0].instructions.empty());

  // 8. `RunExpandCall`:
  std::vector<std::unique_ptr<CExpression>> call_args;
  call_args.push_back(CExpression::Identifier("p0"));
  call_args.push_back(CExpression::Identifier("p1"));
  CallExpression call_expr(CExpression::Identifier("callee_fn"), std::move(call_args));
  RtlControlFlowGraph call_cfg =
      hooks.RunExpandCall(call_expr, two_arg_fn, CType::S32(), /*target_pseudo_regno=*/74,
                          /*ignore_result=*/false);
  ASSERT_EQ(call_cfg.rpo_blocks.size(), 1u);
  EXPECT_GE(call_cfg.rpo_blocks[0].instructions.size(), 3u);

  // 9. `RunExpandCond`:
  IfStatement if_stmt(
      CExpression::Binary("<", CExpression::Identifier("p0"), CExpression::Identifier("p1")),
      CStatement::Expression(
          CExpression::Assignment("=", CExpression::Identifier("p0"), CExpression::Integer(1))),
      CStatement::Expression(
          CExpression::Assignment("=", CExpression::Identifier("p0"), CExpression::Integer(2))));
  RtlControlFlowGraph cond_cfg = hooks.RunExpandCond(if_stmt, two_arg_fn);
  EXPECT_GE(cond_cfg.rpo_blocks.size(), 3u);

  // 10. `RunExpandLoop`:
  std::unique_ptr<CStatement> while_stmt = CStatement::While(
      CExpression::Binary("<", CExpression::Identifier("p0"), CExpression::Identifier("p1")),
      CStatement::Expression(CExpression::Assignment(
          "=", CExpression::Identifier("p0"),
          CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Integer(1)))));
  RtlControlFlowGraph loop_cfg = hooks.RunExpandLoop(*while_stmt, two_arg_fn);
  EXPECT_GE(loop_cfg.rpo_blocks.size(), 2u);

  // 11. `RunExpandCase`:
  std::vector<SwitchCase> switch_cases;
  auto case1_body = std::make_unique<CompoundStatement>();
  case1_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("p1"), CExpression::Integer(10))));
  case1_body->AddStatement(CStatement::Break());
  switch_cases.push_back(
      SwitchCase{.case_values = {1}, .is_default = false, .body = std::move(case1_body)});

  auto case2_body = std::make_unique<CompoundStatement>();
  case2_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("p1"), CExpression::Integer(20))));
  case2_body->AddStatement(CStatement::Break());
  switch_cases.push_back(
      SwitchCase{.case_values = {2}, .is_default = false, .body = std::move(case2_body)});

  SwitchStatement switch_stmt(CExpression::Identifier("p0"), std::move(switch_cases));
  RtlControlFlowGraph case_cfg = hooks.RunExpandCase(switch_stmt, two_arg_fn);
  EXPECT_GE(case_cfg.rpo_blocks.size(), 4u);

  // 3. `RunExpandReturn`:
  ReturnStatement ret_stmt(
      CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Identifier("p1")));
  RtlControlFlowGraph ret_cfg = hooks.RunExpandReturn(ret_stmt, two_arg_fn);
  ASSERT_EQ(ret_cfg.rpo_blocks.size(), 1u);
  EXPECT_GE(ret_cfg.rpo_blocks[0].instructions.size(), 3u);

  // 2. `RunExpandFunctionEnd`:
  RtlControlFlowGraph fn_end_cfg = hooks.RunExpandFunctionEnd(ret_cfg, two_arg_fn);
  EXPECT_EQ(fn_end_cfg.rpo_blocks.size(), 2u);

  // 1. `RunInstantiateVirtualRegs`:
  RtlControlFlowGraph pre_virt_cfg;
  pre_virt_cfg.pass = RtlPassKind::kRtl;
  pre_virt_cfg.rpo_blocks.resize(1);
  pre_virt_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 69) (const_int 8))) (reg:SI 72))");
  RtlControlFlowGraph post_virt_cfg = hooks.RunInstantiateVirtualRegs(pre_virt_cfg);
  ASSERT_EQ(post_virt_cfg.rpo_blocks.size(), 1u);
  ASSERT_EQ(post_virt_cfg.rpo_blocks[0].instructions.size(), 1u);
  EXPECT_TRUE(absl::StrContains(post_virt_cfg.rpo_blocks[0].instructions[0], "(reg:SI 30"));
}

TEST(RightInverseVerifierTest, Stage13AllThirteenExpansionSubfunctionsRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  FunctionDeclaration two_arg_fn(CType::S32(), "harness_func",
                                 {
                                     CParameter{CType::S32(), "p0"},
                                     CParameter{CType::S32(), "p1"},
                                 },
                                 std::make_unique<CompoundStatement>());

  // 27. `instantiate_virtual_regs` right-inverse:
  RtlControlFlowGraph pre_virt_cfg;
  pre_virt_cfg.pass = RtlPassKind::kRtl;
  pre_virt_cfg.rpo_blocks.resize(1);
  pre_virt_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 69) (const_int 8))) (reg:SI 72))");
  pre_virt_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 71) (const_int 16))) (reg:SI 73))");
  RtlControlFlowGraph post_virt_cfg = hooks.RunInstantiateVirtualRegs(pre_virt_cfg);
  RightInverseResult virt_res = verifier.VerifyInstantiateVirtualRegsRightInverse(post_virt_cfg);
  EXPECT_TRUE(virt_res.matches) << virt_res.diff_detail;

  // 28. `expand_function_end` right-inverse:
  ReturnStatement ret_stmt(
      CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Identifier("p1")));
  RtlControlFlowGraph ret_cfg = hooks.RunExpandReturn(ret_stmt, two_arg_fn);
  RtlControlFlowGraph post_fn_end_cfg = hooks.RunExpandFunctionEnd(ret_cfg, two_arg_fn);
  RightInverseResult fn_end_res = verifier.VerifyExpandFunctionEndRightInverse(post_fn_end_cfg);
  EXPECT_TRUE(fn_end_res.matches) << fn_end_res.diff_detail;

  // 29. `expand_return` right-inverse (both value return and void return):
  RightInverseResult ret_res = verifier.VerifyExpandReturnRightInverse(ret_cfg);
  EXPECT_TRUE(ret_res.matches) << ret_res.diff_detail;

  FunctionDeclaration void_fn(CType::Void(), "harness_func", {},
                              std::make_unique<CompoundStatement>());
  RtlControlFlowGraph void_ret_cfg = hooks.RunExpandReturn(ReturnStatement(), void_fn);
  RightInverseResult void_ret_res = verifier.VerifyExpandReturnRightInverse(void_ret_cfg);
  EXPECT_TRUE(void_ret_res.matches) << void_ret_res.diff_detail;

  // 30. `expand_expr` right-inverse:
  std::unique_ptr<CExpression> compound_expr = CExpression::Binary(
      "-", CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Identifier("p1")),
      CExpression::Integer(7));
  RtlControlFlowGraph post_expr_cfg = hooks.RunExpandExpr(*compound_expr, two_arg_fn);
  RightInverseResult expr_res = verifier.VerifyExpandExprRightInverse(post_expr_cfg);
  EXPECT_TRUE(expr_res.matches) << expr_res.diff_detail;

  // 31. `expand_assignment` right-inverse (scalar and pointer dereference):
  RtlControlFlowGraph post_assign_cfg =
      hooks.RunExpandAssignment(*CExpression::Identifier("p0"), *compound_expr, two_arg_fn);
  RightInverseResult assign_res = verifier.VerifyExpandAssignmentRightInverse(post_assign_cfg);
  EXPECT_TRUE(assign_res.matches) << assign_res.diff_detail;

  FunctionDeclaration ptr_fn(CType::Void(), "harness_func",
                             {
                                 CParameter{CType::S32().MakePointer(), "p0"},
                                 CParameter{CType::S32(), "p1"},
                             },
                             std::make_unique<CompoundStatement>());
  RtlControlFlowGraph post_ptr_assign_cfg =
      hooks.RunExpandAssignment(*CExpression::Unary("*", CExpression::Identifier("p0")),
                                *CExpression::Identifier("p1"), ptr_fn);
  RightInverseResult ptr_assign_res =
      verifier.VerifyExpandAssignmentRightInverse(post_ptr_assign_cfg);
  EXPECT_TRUE(ptr_assign_res.matches) << ptr_assign_res.diff_detail;

  // 32. `do_store_flag` right-inverse (`<`, `>=`, `==`):
  std::unique_ptr<CExpression> lt_expr =
      CExpression::Binary("<", CExpression::Identifier("p0"), CExpression::Identifier("p1"));
  RtlControlFlowGraph post_sflag_lt = hooks.RunDoStoreFlag(*lt_expr, two_arg_fn);
  RightInverseResult sflag_lt_res = verifier.VerifyDoStoreFlagRightInverse(post_sflag_lt);
  EXPECT_TRUE(sflag_lt_res.matches) << sflag_lt_res.diff_detail;

  std::unique_ptr<CExpression> ge_expr =
      CExpression::Binary(">=", CExpression::Identifier("p0"), CExpression::Identifier("p1"));
  RtlControlFlowGraph post_sflag_ge = hooks.RunDoStoreFlag(*ge_expr, two_arg_fn);
  RightInverseResult sflag_ge_res = verifier.VerifyDoStoreFlagRightInverse(post_sflag_ge);
  EXPECT_TRUE(sflag_ge_res.matches) << sflag_ge_res.diff_detail;

  std::unique_ptr<CExpression> eq_expr =
      CExpression::Binary("==", CExpression::Identifier("p0"), CExpression::Identifier("p1"));
  RtlControlFlowGraph post_sflag_eq = hooks.RunDoStoreFlag(*eq_expr, two_arg_fn);
  RightInverseResult sflag_eq_res = verifier.VerifyDoStoreFlagRightInverse(post_sflag_eq);
  EXPECT_TRUE(sflag_eq_res.matches) << sflag_eq_res.diff_detail;

  // 33. `do_jump` right-inverse (relational and short-circuit `&&` / `||`):
  RtlControlFlowGraph post_jump_lt = hooks.RunDoJump(*lt_expr, two_arg_fn);
  RightInverseResult jump_lt_res = verifier.VerifyDoJumpRightInverse(post_jump_lt);
  EXPECT_TRUE(jump_lt_res.matches) << jump_lt_res.diff_detail;

  std::unique_ptr<CExpression> and_cond = CExpression::Binary(
      "&&", CExpression::Binary(">", CExpression::Identifier("p0"), CExpression::Integer(0)),
      CExpression::Binary(">", CExpression::Identifier("p1"), CExpression::Integer(0)));
  RtlControlFlowGraph post_jump_and = hooks.RunDoJump(*and_cond, two_arg_fn);
  RightInverseResult jump_and_res = verifier.VerifyDoJumpRightInverse(post_jump_and);
  EXPECT_TRUE(jump_and_res.matches) << jump_and_res.diff_detail;

  std::unique_ptr<CExpression> or_cond = CExpression::Binary(
      "||", CExpression::Binary(">", CExpression::Identifier("p0"), CExpression::Integer(0)),
      CExpression::Binary(">", CExpression::Identifier("p1"), CExpression::Integer(0)));
  RtlControlFlowGraph post_jump_or = hooks.RunDoJump(*or_cond, two_arg_fn);
  RightInverseResult jump_or_res = verifier.VerifyDoJumpRightInverse(post_jump_or);
  EXPECT_TRUE(jump_or_res.matches) << jump_or_res.diff_detail;

  // 34. `expand_call` right-inverse:
  std::vector<std::unique_ptr<CExpression>> call_args;
  call_args.push_back(CExpression::Identifier("p0"));
  call_args.push_back(CExpression::Identifier("p1"));
  CallExpression call_expr(CExpression::Identifier("helper_fn"), std::move(call_args));
  RtlControlFlowGraph post_call_cfg =
      hooks.RunExpandCall(call_expr, two_arg_fn, CType::S32(), /*target_pseudo_regno=*/74,
                          /*ignore_result=*/false);
  RightInverseResult call_res = verifier.VerifyExpandCallRightInverse(post_call_cfg);
  EXPECT_TRUE(call_res.matches) << call_res.diff_detail;

  // 35. `expand_cond` right-inverse (`if-then` and `if-then-else`):
  IfStatement if_else_stmt(
      CExpression::Binary("<", CExpression::Identifier("p0"), CExpression::Identifier("p1")),
      CStatement::Expression(
          CExpression::Assignment("=", CExpression::Identifier("p0"), CExpression::Integer(10))),
      CStatement::Expression(
          CExpression::Assignment("=", CExpression::Identifier("p0"), CExpression::Integer(20))));
  RtlControlFlowGraph post_cond_cfg = hooks.RunExpandCond(if_else_stmt, two_arg_fn);
  RightInverseResult cond_res = verifier.VerifyExpandCondRightInverse(post_cond_cfg);
  EXPECT_TRUE(cond_res.matches) << cond_res.diff_detail;

  // 36. `expand_loop` right-inverse:
  std::unique_ptr<CStatement> while_stmt = CStatement::While(
      CExpression::Binary("<", CExpression::Identifier("p0"), CExpression::Identifier("p1")),
      CStatement::Expression(CExpression::Assignment(
          "=", CExpression::Identifier("p0"),
          CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Integer(1)))));
  RtlControlFlowGraph post_loop_cfg = hooks.RunExpandLoop(*while_stmt, two_arg_fn);
  RightInverseResult loop_res = verifier.VerifyExpandLoopRightInverse(post_loop_cfg);
  EXPECT_TRUE(loop_res.matches) << loop_res.diff_detail;

  // 37. `expand_case` right-inverse:
  std::vector<SwitchCase> switch_cases;
  auto case1_body = std::make_unique<CompoundStatement>();
  case1_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("p1"), CExpression::Integer(100))));
  case1_body->AddStatement(CStatement::Break());
  switch_cases.push_back(
      SwitchCase{.case_values = {1}, .is_default = false, .body = std::move(case1_body)});

  auto case2_body = std::make_unique<CompoundStatement>();
  case2_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("p1"), CExpression::Integer(200))));
  case2_body->AddStatement(CStatement::Break());
  switch_cases.push_back(
      SwitchCase{.case_values = {2}, .is_default = false, .body = std::move(case2_body)});

  auto default_body = std::make_unique<CompoundStatement>();
  default_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("p1"), CExpression::Integer(300))));
  default_body->AddStatement(CStatement::Break());
  switch_cases.push_back(
      SwitchCase{.case_values = {}, .is_default = true, .body = std::move(default_body)});

  SwitchStatement switch_stmt(CExpression::Identifier("p0"), std::move(switch_cases));
  RtlControlFlowGraph post_case_cfg = hooks.RunExpandCase(switch_stmt, two_arg_fn);
  RightInverseResult case_res = verifier.VerifyExpandCaseRightInverse(post_case_cfg);
  EXPECT_TRUE(case_res.matches) << case_res.diff_detail;

  // 38. `expand_decl` right-inverse:
  VariableDeclarationStatement local_decl(
      CType::S32(), "local_var",
      CExpression::Binary("+", CExpression::Identifier("p0"), CExpression::Identifier("p1")));
  RtlControlFlowGraph post_decl_cfg = hooks.RunExpandDecl(local_decl, two_arg_fn);
  RightInverseResult decl_res = verifier.VerifyExpandDeclRightInverse(post_decl_cfg);
  EXPECT_TRUE(decl_res.matches) << decl_res.diff_detail;

  // 39. `expand_function_start` + `assign_parms` right-inverse (including promoted narrow args):
  FunctionDeclaration mixed_parm_fn(CType::S32(), "harness_func",
                                    {
                                        CParameter{CType::S32(), "p0"},
                                        CParameter{CType::S16(), "p1"},
                                        CParameter{CType::U8(), "p2"},
                                    },
                                    std::make_unique<CompoundStatement>());
  RtlControlFlowGraph post_start_cfg = hooks.RunExpandFunctionStart(mixed_parm_fn);
  RightInverseResult start_res = verifier.VerifyExpandFunctionStartRightInverse(post_start_cfg);
  EXPECT_TRUE(start_res.matches) << start_res.diff_detail;
}

TEST(RightInverseVerifierTest, SingleMethodSubfunctionInverseProviderSubclassAndBoundaryVerifier) {
  // Verify that a `SubfunctionInverseProvider` subclass can override only a
  // single `Invert*` method under test without stubbing the other 40 methods,
  // and that all unoverridden methods return non-identity sentinels that fail
  // verification rather than vacuously passing.
  class SingleMethodProvider final : public SubfunctionInverseProvider {
   public:
    std::string InvertGasMipsIp(uint32_t word_x) const override {
      if (word_x == 0x00851021u) {
        return "addu $2,$4,$5";
      }
      if (word_x == 0x03e00008u) {
        return "jr $31";
      }
      return "nop";
    }
  };

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  SingleMethodProvider single_provider;
  RightInverseVerifier single_verifier(&ctx, &hooks, &single_provider);

  RightInverseResult mips_ip_res = single_verifier.VerifyGasMipsIpRightInverse(0x00851021u);
  EXPECT_TRUE(mips_ip_res.matches) << mips_ip_res.diff_detail;

  // Default Stage 13 methods return `absl::UnimplementedError`.
  RtlControlFlowGraph dummy_cfg;
  EXPECT_FALSE(single_provider.InvertExpandReturn(dummy_cfg).ok());

  // Verify `VerifySubfunctionBoundaryRightInverse` across all 30 subfunction
  // boundary snapshots of a forward-traced function for both `full_verifier`
  // and `single_verifier` (at both `-O2` and `-O0`).
  RightInverseVerifier full_verifier(&ctx, &hooks);
  const FunctionCompilationTrace trace =
      hooks.TraceForwardCompilation("int add_two(int a, int b) { return a + b; }\n", "add_two",
                                    {"-O2"}, "boundary_right_inverse_add_two");
  ASSERT_EQ(trace.subfunction_snapshots.size(), AllSubfunctionIds().size());
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    SCOPED_TRACE(SubfunctionIdName(snap.subfunction_id));
    RightInverseResult res = full_verifier.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << SubfunctionIdName(snap.subfunction_id) << ": " << res.diff_detail;

    RightInverseResult single_res = single_verifier.VerifySubfunctionBoundaryRightInverse(snap);
    if (snap.subfunction_id == SubfunctionId::kGasMipsIp) {
      EXPECT_TRUE(single_res.matches)
          << SubfunctionIdName(snap.subfunction_id) << ": " << single_res.diff_detail;
    } else {
      EXPECT_FALSE(single_res.matches)
          << "Expected unoverridden " << SubfunctionIdName(snap.subfunction_id)
          << " on SingleMethodProvider to fail verification (-O2)";
    }
  }

  const FunctionCompilationTrace trace_o0 =
      hooks.TraceForwardCompilation("int add_two(int a, int b) { return a + b; }\n", "add_two",
                                    {"-O0"}, "boundary_right_inverse_add_two_O0");
  ASSERT_EQ(trace_o0.subfunction_snapshots.size(), AllSubfunctionIds().size());
  for (const SubfunctionBoundarySnapshot& snap : trace_o0.subfunction_snapshots) {
    SCOPED_TRACE(SubfunctionIdName(snap.subfunction_id));
    if (snap.subfunction_id != SubfunctionId::kGasMipsIp) {
      RightInverseResult single_res = single_verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_FALSE(single_res.matches)
          << "Expected unoverridden " << SubfunctionIdName(snap.subfunction_id)
          << " on SingleMethodProvider to fail verification (-O0)";
    }
  }
}

TEST(RightInverseVerifierTest, Stage0MetadataExtractionAndRightInverseVerification) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // 1. Verify `RunGasMdAssembleWithMetadata` extracts relocations (`kMips26`,
  //    `kMipsHi16`, `kMipsLo16`), `.rdata` jump tables, and `.rdata` float/double
  //    constant pools (`$LC*` and GAS `li.d`).
  constexpr std::string_view kRichAsm = R"(
	.rdata
	.align	3
$LC0:
	.word	0x400921fb
	.word	0x54442d18
	.align	2
$L10:
	.word	$L2
	.word	$L3
	.word	$L4
	.word	$L2
	.word	$L3
	.text
	.align	2
	.ent	rich_stage0
rich_stage0:
	.frame	$sp,24,$31
	.mask	0x80000000,-8
	subu	$sp,$sp,24
	sw	$31,16($sp)
	sltiu	$2,$4,5
	beq	$2,$0,$L4
	sll	$2,$4,2
	lw	$2,$L10($2)
	j	$2
$L2:
	jal	callee_alpha
	lw	$3,g_counter+4
	j	$L5
$L3:
	jal	callee_beta
	l.d	$f0,$LC0
	mfc1	$3,$f0
	j	$L5
$L4:
	li.d	$f2,1.5
	mfc1	$3,$f2
$L5:
	move	$2,$3
	lw	$31,16($sp)
	addu	$sp,$sp,24
	j	$31
	.end	rich_stage0
)";
  const Stage0FunctionInput stage0 = hooks.RunGasMdAssembleWithMetadata(kRichAsm);
  ASSERT_FALSE(stage0.machine_words.empty());
  ASSERT_EQ(stage0.jump_tables.size(), 1u);
  EXPECT_EQ(stage0.jump_tables[0].label_name, "$L10");
  EXPECT_EQ(stage0.jump_tables[0].target_word_indices.size(), 5u);
  EXPECT_GE(stage0.constant_pool.size(), 2u);

  bool saw_callee_alpha = false;
  bool saw_callee_beta = false;
  bool saw_counter_hi = false;
  bool saw_counter_lo = false;
  for (const auto& reloc : stage0.relocations) {
    if (reloc.kind == Stage0RelocKind::kMips26 && reloc.symbol_name == "callee_alpha") {
      saw_callee_alpha = true;
    }
    if (reloc.kind == Stage0RelocKind::kMips26 && reloc.symbol_name == "callee_beta") {
      saw_callee_beta = true;
    }
    if (reloc.kind == Stage0RelocKind::kMipsHi16 && reloc.symbol_name == "g_counter" &&
        reloc.addend == 4) {
      saw_counter_hi = true;
    }
    if (reloc.kind == Stage0RelocKind::kMipsLo16 && reloc.symbol_name == "g_counter" &&
        reloc.addend == 4) {
      saw_counter_lo = true;
    }
  }
  EXPECT_TRUE(saw_callee_alpha);
  EXPECT_TRUE(saw_callee_beta);
  EXPECT_TRUE(saw_counter_hi);
  EXPECT_TRUE(saw_counter_lo);

  // 2. Verify `VerifyFinalAndGasStageRightInverse(const Stage0FunctionInput&)`
  //    accepts an exact inverse and rejects symbol/addend/jump-table/pool mismatches.
  constexpr std::string_view kSource = R"(
extern int callee_one(int);
extern int callee_two(int);
extern int g_table[8];
int dispatch_func(int kind, int x) {
  switch (kind) {
    case 0: return callee_one(x) + g_table[1];
    case 1: return callee_two(x) + g_table[3];
    case 2: return x * 3;
    case 3: return x - 7;
    case 4: return x + 11;
    default: return 0;
  }
}
)";
  const FunctionCompilationTrace trace =
      hooks.TraceForwardCompilation(kSource, "dispatch_func", {"-O2"}, "stage0_verifier_test");
  const RtlControlFlowGraph exact_dbr = trace.pass_snapshots.at(RtlPassKind::kDbr);

  class ConfigurableStage0Provider final : public SubfunctionInverseProvider {
   public:
    explicit ConfigurableStage0Provider(RtlControlFlowGraph cfg) : cfg_(std::move(cfg)) {}
    RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
      return cfg_;
    }

   private:
    RtlControlFlowGraph cfg_;
  };

  ConfigurableStage0Provider exact_provider(exact_dbr);
  RightInverseVerifier exact_verifier(&ctx, &hooks, &exact_provider);
  const RightInverseResult ok_res =
      exact_verifier.VerifyFinalAndGasStageRightInverse(trace.stage0_input);
  EXPECT_TRUE(ok_res.matches) << ok_res.diff_detail;

  // Negative case A: wrong relocation symbol name in expected `Stage0FunctionInput`.
  Stage0FunctionInput bad_sym_input = trace.stage0_input;
  ASSERT_FALSE(bad_sym_input.relocations.empty());
  bad_sym_input.relocations[0].symbol_name = "wrong_symbol_name";
  EXPECT_FALSE(exact_verifier.VerifyFinalAndGasStageRightInverse(bad_sym_input).matches);

  // Negative case B: wrong relocation addend in expected `Stage0FunctionInput`.
  Stage0FunctionInput bad_addend_input = trace.stage0_input;
  bad_addend_input.relocations[0].addend += 16;
  EXPECT_FALSE(exact_verifier.VerifyFinalAndGasStageRightInverse(bad_addend_input).matches);

  // Negative case C: wrong jump-table target index in expected `Stage0FunctionInput`.
  Stage0FunctionInput bad_jt_input = trace.stage0_input;
  ASSERT_FALSE(bad_jt_input.jump_tables.empty());
  ASSERT_FALSE(bad_jt_input.jump_tables[0].target_word_indices.empty());
  bad_jt_input.jump_tables[0].target_word_indices[0] += 1;
  EXPECT_FALSE(exact_verifier.VerifyFinalAndGasStageRightInverse(bad_jt_input).matches);

  // Negative case D: wrong constant-pool word when constant pool is present.
  constexpr std::string_view kFloatSource =
      "double scale_pi(double x) { return x * 3.141592653589793 + 2.718281828459045; }\n";
  const FunctionCompilationTrace fp_trace =
      hooks.TraceForwardCompilation(kFloatSource, "scale_pi", {"-O2"}, "stage0_fp_pool_test");
  ASSERT_FALSE(fp_trace.stage0_input.constant_pool.empty());
  ConfigurableStage0Provider fp_provider(fp_trace.pass_snapshots.at(RtlPassKind::kDbr));
  RightInverseVerifier fp_verifier(&ctx, &hooks, &fp_provider);
  EXPECT_TRUE(fp_verifier.VerifyFinalAndGasStageRightInverse(fp_trace.stage0_input).matches);

  Stage0FunctionInput bad_pool_input = fp_trace.stage0_input;
  bad_pool_input.constant_pool[0].words[0] ^= 0x00000001u;
  EXPECT_FALSE(fp_verifier.VerifyFinalAndGasStageRightInverse(bad_pool_input).matches);
}

TEST(GccHarnessTest, ArgPointerRtxVsHardwareZeroDisambiguationAndReloadElimination) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ctx.BeginFunction("argp_vs_zero_test");

  // 1. Verify `(reg:SI 0 argp)` and `(reg:SI 0 $0)` round-trip distinctly
  //    through `ParseRtx` and `FormatRtx`.
  auto parsed_argp = ctx.ParseRtx("(reg:SI 0 argp)");
  auto parsed_zero = ctx.ParseRtx("(reg:SI 0 $0)");
  ASSERT_NE(parsed_argp, nullptr);
  ASSERT_NE(parsed_zero, nullptr);
  EXPECT_NE(parsed_argp, parsed_zero);
  EXPECT_EQ(ctx.FormatRtx(parsed_argp), "(reg:SI 0 argp)");
  EXPECT_EQ(ctx.FormatRtx(parsed_zero), "(reg:SI 0 $0)");

  // 2. Verify `RunReload` (`eliminate_regs` with `global = 1`) eliminates
  //    `(reg:SI 0 argp)` (`arg_pointer_rtx`) into `$sp`/`$fp` stack addressing
  //    while leaving hardware `$zero` (`(reg:SI 0 $0)`) untouched.
  RtlControlFlowGraph pre_reload;
  pre_reload.pass = RtlPassKind::kLreg;
  pre_reload.sideband.frame_size_bytes = 8;
  RtlBasicBlock blk;
  blk.id = 0;
  blk.instructions = {
      "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 0 argp) (const_int 16))))",
      "(set (mem:SI (reg:SI 5 a1)) (reg:SI 0 $0))",
      "(use (reg/i:SI 2 v0))",
  };
  blk.SyncRtlInsnsFromInstructions();
  pre_reload.rpo_blocks = {blk};

  auto first = ctx.MaterializeFromCfg(pre_reload);
  ASSERT_NE(first, nullptr);
  hooks.RunReload(first, /*global=*/1);
  const RtlControlFlowGraph post_reload = ctx.DematerializeToCfg(first, RtlPassKind::kGreg);
  ASSERT_EQ(post_reload.rpo_blocks.size(), 1u);
  ASSERT_GE(post_reload.rpo_blocks[0].instructions.size(), 2u);

  const std::string& reloaded_arg_load = post_reload.rpo_blocks[0].instructions[0];
  const std::string& untouched_zero_store = post_reload.rpo_blocks[0].instructions[1];
  EXPECT_EQ(reloaded_arg_load.find("argp"), std::string::npos) << reloaded_arg_load;
  EXPECT_NE(reloaded_arg_load.find("(reg:SI 29 sp)"), std::string::npos) << reloaded_arg_load;
  EXPECT_NE(untouched_zero_store.find("(reg:SI 0 $0)"), std::string::npos) << untouched_zero_store;
}

TEST(GccHarnessTest, ConstantPoolMaterializationAndVerifySuffixRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  constexpr std::string_view kPoolSource = R"(
extern int sink_val(double);
int pool_branch(int cond, double x) {
  if (cond > 0) {
    return sink_val(x * 3.141592653589793 + (-0.0));
  }
  return sink_val(x - 2.718281828459045);
}
)";

  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "0", "-Wa,-O2"},
        std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
    const FunctionCompilationTrace trace =
        hooks.TraceForwardCompilation(kPoolSource, "pool_branch", flags, "pool_suffix_test");
    ASSERT_FALSE(trace.stage0_input.constant_pool.empty());
    ASSERT_FALSE(trace.pass_snapshots.at(RtlPassKind::kDbr).sideband.constant_pool.empty());
    EXPECT_EQ(trace.stage0_input.profile.c_flags, flags);

    class TraceSnapshotInverseProvider final : public SubfunctionInverseProvider {
     public:
      explicit TraceSnapshotInverseProvider(const FunctionCompilationTrace* t) : t_(t) {}
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        return t_->pass_snapshots.at(RtlPassKind::kDbr);
      }
      RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph&) const override {
        return t_->pass_snapshots.at(RtlPassKind::kJump2);
      }
      RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph&) const override {
        return t_->pass_snapshots.at(RtlPassKind::kSched2);
      }

     private:
      const FunctionCompilationTrace* t_;
    };

    TraceSnapshotInverseProvider provider(&trace);
    RightInverseVerifier verifier(&ctx, &hooks, &provider);

    for (RtlPassKind pass : {RtlPassKind::kDbr, RtlPassKind::kJump2, RtlPassKind::kSched2}) {
      const RightInverseResult ok = verifier.VerifySuffixRightInverse(pass, trace.stage0_input);
      EXPECT_TRUE(ok.matches) << "pass=" << static_cast<int>(pass) << " diff:\n" << ok.diff_detail;
    }

    // Negative check 1: Dropping `constant_pool` from lifted CFG must fail
    // `VerifySuffixRightInverse` (no `unresolved_symbolic_pool_ref` loophole).
    class DroppedPoolProvider final : public SubfunctionInverseProvider {
     public:
      explicit DroppedPoolProvider(RtlControlFlowGraph cfg) : cfg_(std::move(cfg)) {
        cfg_.sideband.constant_pool.clear();
      }
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        return cfg_;
      }

     private:
      RtlControlFlowGraph cfg_;
    };
    DroppedPoolProvider dropped_pool(trace.pass_snapshots.at(RtlPassKind::kDbr));
    RightInverseVerifier bad_pool_verifier(&ctx, &hooks, &dropped_pool);
    EXPECT_FALSE(
        bad_pool_verifier.VerifySuffixRightInverse(RtlPassKind::kDbr, trace.stage0_input).matches);

    // Negative check 2: Empty Stage 0 CFG must fail at every optimization level,
    // and empty intermediate CFG at `kSched2` must fail when `optimize > 0`.
    class EmptyDbrProvider final : public SubfunctionInverseProvider {
     public:
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        return {};
      }
    };
    EmptyDbrProvider empty_dbr;
    RightInverseVerifier empty_dbr_verifier(&ctx, &hooks, &empty_dbr);
    EXPECT_FALSE(
        empty_dbr_verifier.VerifySuffixRightInverse(RtlPassKind::kDbr, trace.stage0_input).matches);

    if (trace.optimize_level > 0) {
      class EmptySched2Provider final : public SubfunctionInverseProvider {
       public:
        explicit EmptySched2Provider(const FunctionCompilationTrace* t) : t_(t) {}
        RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
          return t_->pass_snapshots.at(RtlPassKind::kDbr);
        }
        RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph&) const override {
          return t_->pass_snapshots.at(RtlPassKind::kJump2);
        }
        RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph&) const override {
          return {};
        }

       private:
        const FunctionCompilationTrace* t_;
      };
      EmptySched2Provider empty_sched2(&trace);
      RightInverseVerifier empty_verifier(&ctx, &hooks, &empty_sched2);
      EXPECT_FALSE(empty_verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, trace.stage0_input)
                       .matches);
    }
  }
}

Stage0FunctionInput LinkStage0InputForTest(const Stage0FunctionInput& reloc_in) {
  Stage0FunctionInput linked;
  linked.machine_words = reloc_in.machine_words;
  linked.base_vram = 0x80020000u;
  linked.gp_vram = 0x80060000u;
  linked.profile = reloc_in.profile;
  linked.profile.gp_symbol_sizes.clear();
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
  uint32_t next_data = 0x80070000u;

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
    const auto gp_sz_it = reloc_in.profile.gp_symbol_sizes.find(rel.symbol_name);
    if (gp_sz_it != reloc_in.profile.gp_symbol_sizes.end() && gp_sz_it->second > 0) {
      linked.vram_to_symbol_size[assigned] = gp_sz_it->second;
    } else if (max_addend > 0) {
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

TEST(RightInverseVerifierTest, LinkedRomStaticRelocationAndAdversarialNegativeChecks) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // 1. Multi-jump-table + global struct/array + `$gp` small-data + constant pool function.
  constexpr std::string_view kMultiSwitchAndGpSrc = R"(
struct ActorState {
  int flags;
  int x;
  int y;
  int z;
};
extern struct ActorState g_actor;
int g_small_counter;
extern double sink_step(double, int);

int multi_switch_gp(int n, const int* ops, const int* modes, double base) {
  int acc = g_actor.z + g_small_counter;
  int i;
  for (i = 0; i < n; ++i) {
    switch (ops[i]) {
      case 0: acc += 3; break;
      case 1: acc -= 5; break;
      case 2: acc ^= 9; break;
      case 3: acc += g_actor.y; break;
      case 4: acc -= g_small_counter; break;
      default: acc += 1; break;
    }
    switch (modes[i]) {
      case 10: base = base * 1.618033988749895 + 0.5772156649015329; break;
      case 11: base = base - 2.718281828459045; break;
      case 12: acc += (int)sink_step(base, acc); break;
      case 13: acc -= 17; break;
      case 14: acc += 23; break;
      default: break;
    }
  }
  g_small_counter = acc;
  return acc;
}
)";
  const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(
      kMultiSwitchAndGpSrc, "multi_switch_gp",
      {"-O2", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"}, "linked_rom_verifier_test");
  ASSERT_GE(trace.stage0_input.jump_tables.size(), 2u);
  ASSERT_FALSE(trace.stage0_input.constant_pool.empty());
  bool saw_gp_reloc_for_small_counter = false;
  for (const Stage0Relocation& rel : trace.stage0_input.relocations) {
    if (rel.symbol_name == "g_small_counter" && rel.kind == Stage0RelocKind::kMipsGpRel16) {
      saw_gp_reloc_for_small_counter = true;
      break;
    }
  }
  ASSERT_TRUE(saw_gp_reloc_for_small_counter);

  class ExactTraceProvider final : public SubfunctionInverseProvider {
   public:
    explicit ExactTraceProvider(const FunctionCompilationTrace* t) : t_(t) {}
    RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
      return t_->pass_snapshots.at(RtlPassKind::kDbr);
    }
    RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph&) const override {
      return t_->pass_snapshots.at(RtlPassKind::kJump2);
    }
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph&) const override {
      return t_->pass_snapshots.at(RtlPassKind::kSched2);
    }

   private:
    const FunctionCompilationTrace* t_;
  };

  ExactTraceProvider exact_provider(&trace);
  RightInverseVerifier verifier(&ctx, &hooks, &exact_provider);

  const Stage0FunctionInput linked_ok = LinkStage0InputForTest(trace.stage0_input);
  ASSERT_TRUE(linked_ok.profile.gp_symbol_sizes.empty());
  EXPECT_TRUE(verifier.VerifyFinalAndGasStageRightInverse(linked_ok).matches);
  EXPECT_TRUE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, linked_ok).matches);

  // Adversarial Negative A0: Omitting or inflating `vram_to_symbol_size` on `-G 8` `$gp` scalar
  // `g_small_counter` causes GAS to emit 2-word `%hi`/`%lo` instead of 1-word `$gp`-relative insns.
  {
    uint32_t small_vram = 0;
    for (const auto& [vram, name] : linked_ok.vram_to_symbol) {
      if (name == "g_small_counter") {
        small_vram = vram;
        break;
      }
    }
    ASSERT_NE(small_vram, 0u);
    ASSERT_TRUE(linked_ok.vram_to_symbol_size.contains(small_vram));
    ASSERT_EQ(linked_ok.vram_to_symbol_size.at(small_vram), 4u);

    Stage0FunctionInput missing_gp_size = linked_ok;
    missing_gp_size.vram_to_symbol_size.erase(small_vram);
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(missing_gp_size).matches);
    EXPECT_FALSE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, missing_gp_size).matches);

    Stage0FunctionInput oversized_gp_sym = linked_ok;
    oversized_gp_sym.vram_to_symbol_size[small_vram] = 16u;
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(oversized_gp_sym).matches);
    EXPECT_FALSE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, oversized_gp_sym).matches);
  }

  // Reversed `.rodata` jump-table layout in `linked_ok` (preserving each table's
  // true `table_vram` and `target_word_indices`) must also pass!
  Stage0FunctionInput linked_rev_jts = linked_ok;
  std::reverse(linked_rev_jts.jump_tables.begin(), linked_rev_jts.jump_tables.end());
  EXPECT_TRUE(verifier.VerifyFinalAndGasStageRightInverse(linked_rev_jts).matches);
  EXPECT_TRUE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, linked_rev_jts).matches);

  // Adversarial Negative A: Wrong global symbol VRAM (`g_actor` shifted by +4).
  {
    Stage0FunctionInput bad_global_vram = linked_ok;
    uint32_t orig_actor_vram = 0;
    for (const auto& [vram, name] : bad_global_vram.vram_to_symbol) {
      if (name == "g_actor") {
        orig_actor_vram = vram;
        break;
      }
    }
    ASSERT_NE(orig_actor_vram, 0u);
    bad_global_vram.vram_to_symbol.erase(orig_actor_vram);
    bad_global_vram.vram_to_symbol[orig_actor_vram + 4u] = "g_actor";
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(bad_global_vram).matches);
    EXPECT_FALSE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, bad_global_vram).matches);
  }

  // Adversarial Negative B: Swapped `table_vram` on the 2-jump-table function.
  {
    Stage0FunctionInput swapped_jt_vram = linked_ok;
    std::swap(swapped_jt_vram.jump_tables[0].table_vram, swapped_jt_vram.jump_tables[1].table_vram);
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(swapped_jt_vram).matches);
    EXPECT_FALSE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, swapped_jt_vram).matches);
  }

  // Adversarial Negative C: Wrong `entry_vram` and wrong constant-pool float/double bits.
  {
    Stage0FunctionInput bad_cp_vram = linked_ok;
    bad_cp_vram.constant_pool[0].entry_vram += 8u;
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(bad_cp_vram).matches);
    EXPECT_FALSE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, bad_cp_vram).matches);

    Stage0FunctionInput bad_cp_bits = linked_ok;
    ASSERT_FALSE(bad_cp_bits.constant_pool[0].words.empty());
    bad_cp_bits.constant_pool[0].words[0] ^= 0x00000001u;
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(bad_cp_bits).matches);
    EXPECT_FALSE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, bad_cp_bits).matches);
  }

  // Adversarial Negative D: Wrong `R_MIPS_26` callee VRAM (`sink_step` shifted by +8 or missing).
  {
    Stage0FunctionInput bad_callee = linked_ok;
    uint32_t orig_callee_vram = 0;
    for (const auto& [vram, name] : bad_callee.vram_to_symbol) {
      if (name == "sink_step") {
        orig_callee_vram = vram;
        break;
      }
    }
    ASSERT_NE(orig_callee_vram, 0u);
    bad_callee.vram_to_symbol.erase(orig_callee_vram);
    bad_callee.vram_to_symbol[orig_callee_vram + 8u] = "sink_step";
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(bad_callee).matches);
    EXPECT_FALSE(verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, bad_callee).matches);

    Stage0FunctionInput missing_callee = linked_ok;
    missing_callee.vram_to_symbol.erase(orig_callee_vram);
    EXPECT_FALSE(verifier.VerifyFinalAndGasStageRightInverse(missing_callee).matches);
  }

  // Adversarial Negative E: Unresolved symbol emitted by a buggy lifter must fail hard.
  {
    class UnresolvedSymProvider final : public SubfunctionInverseProvider {
     public:
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        RtlControlFlowGraph cfg;
        cfg.pass = RtlPassKind::kDbr;
        RtlBasicBlock blk;
        blk.id = 0;
        blk.AddPatternOnly(
            "(set (reg:SI 2 v0) (mem:SI (symbol_ref:SI (\"unresolved_bogus_sym\"))))");
        blk.AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
        cfg.rpo_blocks = {std::move(blk)};
        return cfg;
      }
    };
    UnresolvedSymProvider bad_sym_prov;
    RightInverseVerifier bad_sym_verifier(&ctx, &hooks, &bad_sym_prov);
    Stage0FunctionInput zero_mem_rom;
    zero_mem_rom.base_vram = 0x80020000u;
    zero_mem_rom.machine_words = {0x3c020000u, 0x8c420000u, 0x03e00008u, 0x00000000u};
    zero_mem_rom.profile.optimize_level = 2;
    EXPECT_FALSE(bad_sym_verifier.VerifyFinalAndGasStageRightInverse(zero_mem_rom).matches);
  }

  // Adversarial Negative F: Orphaned `R_MIPS_HI16` without matching `R_MIPS_LO16` must fail hard.
  {
    class OrphanedHiProvider final : public SubfunctionInverseProvider {
     public:
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        RtlControlFlowGraph cfg;
        cfg.pass = RtlPassKind::kDbr;
        RtlBasicBlock blk;
        blk.id = 0;
        blk.AddPatternOnly("(set (reg:SI 2 v0) (high:SI (symbol_ref:SI (\"g_orphan\"))))");
        blk.AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
        cfg.rpo_blocks = {std::move(blk)};
        return cfg;
      }
    };
    OrphanedHiProvider orphan_prov;
    RightInverseVerifier orphan_verifier(&ctx, &hooks, &orphan_prov);
    Stage0FunctionInput orphan_rom;
    orphan_rom.base_vram = 0x80020000u;
    orphan_rom.vram_to_symbol[0x80030000u] = "g_orphan";
    orphan_rom.machine_words = {0x3c028003u, 0x03e00008u, 0x00000000u};
    orphan_rom.profile.optimize_level = 2;
    const RightInverseResult orphan_res =
        orphan_verifier.VerifyFinalAndGasStageRightInverse(orphan_rom);
    EXPECT_FALSE(orphan_res.matches);
    EXPECT_TRUE(absl::StrContains(orphan_res.diff_detail, "Orphaned R_MIPS_HI16"))
        << orphan_res.diff_detail;
  }

  // Adversarial Negative G: Orphaned `R_MIPS_LO16` (`lw $v0, %lo(g_foo)($a0)` with
  // `g_foo == 0x80030010` and zero `lui` against `lw $v0, 16($a0)`) must fail hard!
  {
    class OrphanedLoProvider final : public SubfunctionInverseProvider {
     public:
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        RtlControlFlowGraph cfg;
        cfg.pass = RtlPassKind::kDbr;
        RtlBasicBlock blk;
        blk.id = 0;
        blk.AddPatternOnly(
            "(set (reg:SI 2 v0) (mem:SI (lo_sum:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_foo\")))))");
        blk.AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
        cfg.rpo_blocks = {std::move(blk)};
        return cfg;
      }
    };
    OrphanedLoProvider orphan_lo_prov;
    RightInverseVerifier orphan_lo_verifier(&ctx, &hooks, &orphan_lo_prov);
    Stage0FunctionInput orphan_lo_rom;
    orphan_lo_rom.base_vram = 0x80020000u;
    orphan_lo_rom.vram_to_symbol[0x80030010u] = "g_foo";
    orphan_lo_rom.machine_words = {0x8c820010u, 0x03e00008u, 0x00000000u};
    orphan_lo_rom.profile.optimize_level = 2;
    const RightInverseResult orphan_lo_res =
        orphan_lo_verifier.VerifyFinalAndGasStageRightInverse(orphan_lo_rom);
    EXPECT_FALSE(orphan_lo_res.matches);
    EXPECT_TRUE(absl::StrContains(orphan_lo_res.diff_detail, "Orphaned R_MIPS_LO16"))
        << orphan_lo_res.diff_detail;
  }

  // Adversarial Negative G2: Register- and kill-sensitive `R_MIPS_HI16` 64KB page
  // provenance on a shared page (`g_alpha == 0x80030010`, `g_beta == 0x80030020`, both on
  // page `0x8003`):
  //   (a) Wrong base register: `$s0` legitimately loads `%hi(g_alpha)` (`0x8003`), while
  //       `lw $v0, %lo(g_beta)($a0)` uses incoming pointer `$a0` (`$4`), which never held
  //       `R_MIPS_HI16` `0x8003` — MUST fail hard even though page `0x8003` is backed in `$s0`!
  //   (b) Killed base register: `lui $s0, %hi(g_alpha)` followed by `lw $s0, %lo(g_alpha)($s0)`
  //       (overwriting `$s0` with a loaded value) and then `lw $v0, %lo(g_beta)($s0)` — MUST
  //       fail hard because `$s0` no longer holds `R_MIPS_HI16` `0x8003`!
  {
    class SharedPageWrongBaseProvider final : public SubfunctionInverseProvider {
     public:
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        RtlControlFlowGraph cfg;
        cfg.pass = RtlPassKind::kDbr;
        cfg.sideband.saved_gpr_regnos = {16};
        cfg.sideband.frame_size_bytes = 0;
        cfg.sideband.is_leaf_function = true;
        RtlBasicBlock blk;
        blk.id = 0;
        blk.AddPatternOnly("(set (reg:SI 16 s0) (high:SI (symbol_ref:SI (\"g_alpha\"))))");
        blk.AddPatternOnly(
            "(set (reg:SI 3 v1) (mem:SI (lo_sum:SI (reg:SI 16 s0) (symbol_ref:SI "
            "(\"g_alpha\")))))");
        blk.AddPatternOnly(
            "(set (reg:SI 2 v0) (mem:SI (lo_sum:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_beta\")))))");
        blk.AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
        cfg.rpo_blocks = {std::move(blk)};
        return cfg;
      }
    };
    SharedPageWrongBaseProvider wrong_base_prov;
    RightInverseVerifier wrong_base_verifier(&ctx, &hooks, &wrong_base_prov);
    Stage0FunctionInput wrong_base_rom;
    wrong_base_rom.base_vram = 0x80020000u;
    wrong_base_rom.vram_to_symbol[0x80030010u] = "g_alpha";
    wrong_base_rom.vram_to_symbol[0x80030020u] = "g_beta";
    wrong_base_rom.machine_words = {
        0x27bdfff8u,  // addiu $sp, $sp, -8
        0xafb00000u,  // sw $s0, 0($sp)
        0x3c108003u,  // lui $s0, 0x8003 (%hi(g_alpha))
        0x8e030010u,  // lw $v1, 0x0010($s0) (%lo(g_alpha))
        0x8c820020u,  // lw $v0, 0x0020($a0) (pointer offset off $a0, NOT %lo(g_beta)!)
        0x8fb00000u,  // lw $s0, 0($sp)
        0x27bd0008u,  // addiu $sp, $sp, 8
        0x03e00008u,  // jr $ra
        0x00000000u,  // nop
    };
    wrong_base_rom.profile = {.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
    const RightInverseResult wrong_base_res =
        wrong_base_verifier.VerifyFinalAndGasStageRightInverse(wrong_base_rom);
    EXPECT_FALSE(wrong_base_res.matches);
    EXPECT_TRUE(
        absl::StrContains(wrong_base_res.diff_detail, "base register $4 lacks R_MIPS_HI16 backing"))
        << wrong_base_res.diff_detail;

    class SharedPageKilledBaseProvider final : public SubfunctionInverseProvider {
     public:
      RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
        RtlControlFlowGraph cfg;
        cfg.pass = RtlPassKind::kDbr;
        cfg.sideband.saved_gpr_regnos = {16};
        cfg.sideband.frame_size_bytes = 0;
        cfg.sideband.is_leaf_function = true;
        RtlBasicBlock blk;
        blk.id = 0;
        blk.AddPatternOnly("(set (reg:SI 16 s0) (high:SI (symbol_ref:SI (\"g_alpha\"))))");
        blk.AddPatternOnly(
            "(set (reg:SI 16 s0) (mem:SI (lo_sum:SI (reg:SI 16 s0) "
            "(symbol_ref:SI (\"g_alpha\")))))");
        blk.AddPatternOnly(
            "(set (reg:SI 2 v0) (mem:SI (lo_sum:SI (reg:SI 16 s0) (symbol_ref:SI (\"g_beta\")))))");
        blk.AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
        cfg.rpo_blocks = {std::move(blk)};
        return cfg;
      }
    };
    SharedPageKilledBaseProvider killed_base_prov;
    RightInverseVerifier killed_base_verifier(&ctx, &hooks, &killed_base_prov);
    Stage0FunctionInput killed_base_rom = wrong_base_rom;
    killed_base_rom.machine_words = {
        0x27bdfff8u,  // addiu $sp, $sp, -8
        0xafb00000u,  // sw $s0, 0($sp)
        0x3c108003u,  // lui $s0, 0x8003 (%hi(g_alpha))
        0x8e100010u,  // lw $s0, 0x0010($s0) (overwrites $s0!)
        0x8e020020u,  // lw $v0, 0x0020($s0) (dereferencing loaded pointer $s0, NOT %lo(g_beta)!)
        0x8fb00000u,  // lw $s0, 0($sp)
        0x27bd0008u,  // addiu $sp, $sp, 8
        0x03e00008u,  // jr $ra
        0x00000000u,  // nop
    };
    const RightInverseResult killed_base_res =
        killed_base_verifier.VerifyFinalAndGasStageRightInverse(killed_base_rom);
    EXPECT_FALSE(killed_base_res.matches);
    EXPECT_TRUE(absl::StrContains(killed_base_res.diff_detail,
                                  "base register $16 lacks R_MIPS_HI16 backing"))
        << killed_base_res.diff_detail;
  }

  // Adversarial Negative H: Illegal RTL patterns containing `high` or `lo_sum` must be
  // rejected by `ValidateMaterializedInsnsRecognizable` and `VerifyFinalAndGasStageRightInverse`!
  {
    const std::vector<std::string> invalid_hilo_patterns = {
        "(set (high:SI (symbol_ref:SI (\"g_x\"))) (reg:SI 4 a0))",
        "(set (mem:SI (reg:SI 4 a0)) (mem:SI (lo_sum:SI (reg:SI 5 a1) (symbol_ref:SI (\"g_x\")))))",
        "(set (mem:SI (lo_sum:SI (reg:SI 5 a1) (symbol_ref:SI (\"g_x\")))) "
        "(plus:SI (reg:SI 4 a0) (const_int 1)))",
        "(set (reg:SI 2 v0) (lo_sum:SI (const_int 1) (const_int 2)))",
    };
    for (const std::string& bad_pat : invalid_hilo_patterns) {
      class InvalidHiLoProvider final : public SubfunctionInverseProvider {
       public:
        explicit InvalidHiLoProvider(std::string pat) : pat_(std::move(pat)) {}
        RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput&) const override {
          RtlControlFlowGraph cfg;
          cfg.pass = RtlPassKind::kDbr;
          RtlBasicBlock blk;
          blk.id = 0;
          blk.AddPatternOnly(pat_);
          blk.AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
          cfg.rpo_blocks = {std::move(blk)};
          return cfg;
        }

       private:
        std::string pat_;
      };
      InvalidHiLoProvider bad_prov(bad_pat);
      RightInverseVerifier bad_verifier(&ctx, &hooks, &bad_prov);
      Stage0FunctionInput dummy_rom;
      dummy_rom.base_vram = 0x80020000u;
      dummy_rom.vram_to_symbol[0x80030000u] = "g_x";
      dummy_rom.machine_words = {0x03e00008u, 0x00000000u};
      dummy_rom.profile.optimize_level = 2;
      const RightInverseResult bad_res = bad_verifier.VerifyFinalAndGasStageRightInverse(dummy_rom);
      EXPECT_FALSE(bad_res.matches) << bad_pat;
      EXPECT_TRUE(absl::StrContains(bad_res.diff_detail, "Unrecognizable RTL insn"))
          << bad_pat << ": " << bad_res.diff_detail;
    }
  }
}

TEST(GccHarnessTest, InProcessTargetSwitchingAcrossMips3Mips2R4000AndMips1UnsignedChar) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const std::string c_src = R"(
typedef long long s64;
typedef int s32;
typedef float f32;
s64 TargetSwitchProbe(s64 a, s64 b, s32 scale, f32 f, char* p) {
  s64 prod = (a * b) + ((s64)scale * 13LL);
  s32 trunc_i = (s32)f + (s32)(*p);
  return (prod << (scale & 7)) + (s64)trunc_i;
}
)";

  const FunctionCompilationTrace mips3_trace = hooks.TraceForwardCompilation(
      c_src, "TargetSwitchProbe", {"-O2", "-mips3"}, "TargetSwitchProbe_Mips3");
  EXPECT_TRUE(absl::StrContains(mips3_trace.final_asm_text, "dmult")) << mips3_trace.final_asm_text;
  EXPECT_TRUE(absl::StrContains(mips3_trace.final_asm_text, "lb\t")) << mips3_trace.final_asm_text;
  EXPECT_FALSE(mips3_trace.final_machine_words.empty());

  // Immediately switch in-process to `-O1 -mips2 -mcpu=r4000` (32-bit GPRs/FPRs,
  // `mulsi3_r4000` scratch `hi`/`lo` clobbers + `umulsidi3_internal` `=x` reload).
  const FunctionCompilationTrace mips2_o1_trace = hooks.TraceForwardCompilation(
      c_src, "TargetSwitchProbe", {"-O1", "-mips2", "-mcpu=r4000"}, "TargetSwitchProbe_Mips2O1");
  EXPECT_FALSE(absl::StrContains(mips2_o1_trace.final_asm_text, "dmult"))
      << mips2_o1_trace.final_asm_text;
  EXPECT_TRUE(absl::StrContains(mips2_o1_trace.final_asm_text, "multu\t"))
      << mips2_o1_trace.final_asm_text;
  EXPECT_FALSE(mips2_o1_trace.final_machine_words.empty());

  // Switch in-process to `-O2 -mips1 -funsigned-char -Wa,-O2` (3-operand
  // `trunc.w.s`, unsigned `char` -> `lbu`, and 1-slot COP1 load hazard NOPs).
  const FunctionCompilationTrace mips1_uchar_trace = hooks.TraceForwardCompilation(
      c_src, "TargetSwitchProbe", {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
      "TargetSwitchProbe_Mips1UnsignedChar");
  EXPECT_TRUE(absl::StrContains(mips1_uchar_trace.final_asm_text, "lbu\t"))
      << mips1_uchar_trace.final_asm_text;
  EXPECT_FALSE(mips1_uchar_trace.final_machine_words.empty());

  // Switch back to default signed `char` (`-O2 -mips2 -mcpu=r4000`) and verify
  // `char_type_node` is restored to signed (`lb`).
  const FunctionCompilationTrace mips2_o2_trace = hooks.TraceForwardCompilation(
      c_src, "TargetSwitchProbe", {"-O2", "-mips2", "-mcpu=r4000"}, "TargetSwitchProbe_Mips2O2");
  EXPECT_TRUE(absl::StrContains(mips2_o2_trace.final_asm_text, "lb\t"))
      << mips2_o2_trace.final_asm_text;
  EXPECT_FALSE(mips2_o2_trace.final_machine_words.empty());
}

TEST(GccHarnessTest, GasDirectionalNumericLabelsMatchExternalAssembler) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // 1. Multiple `!TARGET_64BIT` 64-bit variable shifts in one function reuse
  //    GAS directional numeric local labels `1:`, `2:`, `3:` with `1f`, `2f`, `3f`.
  const std::string c_src = R"(
typedef long long s64;
typedef unsigned long long u64;
s64 VarShiftPair(s64 a, u64 b, int n, int m) {
  return (a << (n & 63)) ^ (s64)(b >> (m & 63));
}
)";
  const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(
      c_src, "VarShiftPair", {"-O2", "-mips1"}, "VarShiftPair_O2_Mips1");
  ASSERT_FALSE(trace.stage0_input.machine_words.empty());
  ASSERT_LE(trace.stage0_input.machine_words.size(), trace.final_machine_words.size());
  for (size_t i = 0; i < trace.stage0_input.machine_words.size(); ++i) {
    EXPECT_EQ(trace.stage0_input.machine_words[i], trace.final_machine_words[i])
        << "Mismatch at word " << i << ":\n"
        << trace.final_asm_text;
  }

  // 2. Explicit forward (`1f`) and backward (`1b`) directional numeric labels.
  const std::vector<uint32_t> fb_words = hooks.RunGasMdAssemble(
      "\tbeq\t$4,$0,1f\n"
      "\tnop\n"
      "1:\n"
      "\taddiu\t$4,$4,-1\n"
      "\tbne\t$4,$0,1b\n"
      "\tnop\n"
      "1:\n"
      "\tj\t$31\n"
      "\tnop\n");
  ASSERT_EQ(fb_words.size(), 7u);
  // Word 0 (`beq $4,$0,1f` -> Word 2): offset = 2 - (0 + 1) = +1 (`0x10800001`).
  EXPECT_EQ(fb_words[0], 0x10800001u);
  // Word 3 (`bne $4,$0,1b` -> Word 2): offset = 2 - (3 + 1) = -2 (`0x1480fffe`).
  EXPECT_EQ(fb_words[3], 0x1480fffeu);
}

TEST(GccHarnessTest, RepeatedLit8LiteralPoolAssemblyAcrossFragChunkBoundaries) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  hooks.ApplyCompilationProfile(
      CompilationProfile{.optimize_level = 2, .c_flags = {"-O2", "-G", "8", "-mips2"}});

  constexpr std::string_view kLit8Asm =
      "\t.set\treorder\n"
      "\tli.d\t$f6,4.503599627370496e+15\n"
      "\tj\t$31\n";
  // `4.503599627370496e+15` (`2^52`) in IEEE-754 big-endian double is `0x43300000_00000000`.
  const std::vector<uint32_t> expected_words = {0x43300000u, 0x00000000u};
  for (int iter = 0; iter < 400; ++iter) {
    const Stage0FunctionInput in = hooks.RunGasMdAssembleWithMetadata(kLit8Asm);
    ASSERT_EQ(in.constant_pool.size(), 1u) << "iter=" << iter;
    EXPECT_EQ(in.constant_pool[0].section_name, ".lit8") << "iter=" << iter;
    EXPECT_EQ(in.constant_pool[0].words, expected_words) << "iter=" << iter;
  }
}

TEST(GccHarnessTest, LoopNotesMaterializeRoundTripAndDriveLoopOptimizeHoisting) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ctx.BeginFunction("loop_notes_probe");

  auto make_loop_cfg = [](bool with_loop_notes) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kCse;
    cfg.rpo_blocks.resize(3);

    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg/v:SI 72) (const_int 0))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg/v:SI 73) (reg:SI 4 a0))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg/v:SI 74) (const_int 0))");
    if (with_loop_notes) {
      cfg.rpo_blocks[0].AddLoopNote(RtlLoopNoteKind::kLoopBeg);
    }
    cfg.rpo_blocks[0].successors = {1};

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].has_label = true;
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 75) (plus:SI (reg/v:SI 73) (const_int 100)))");
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg/v:SI 74) (plus:SI (reg/v:SI 74) (reg:SI 75)))");
    if (with_loop_notes) {
      cfg.rpo_blocks[1].AddLoopNote(RtlLoopNoteKind::kLoopCont);
    }
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg/v:SI 72) (plus:SI (reg/v:SI 72) (const_int 1)))");
    if (with_loop_notes) {
      cfg.rpo_blocks[1].AddLoopNote(RtlLoopNoteKind::kLoopVtop);
    }
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (pc) (if_then_else (lt:SI (reg/v:SI 72) (const_int 8)) (label_ref L0) (pc)))");
    cfg.rpo_blocks[1].successors = {1, 2};

    cfg.rpo_blocks[2].id = 2;
    if (with_loop_notes) {
      cfg.rpo_blocks[2].AddLoopNote(RtlLoopNoteKind::kLoopEnd);
    }
    cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg/v:SI 74))");
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  const RtlControlFlowGraph with_notes = make_loop_cfg(/*with_loop_notes=*/true);
  Rtx head = ctx.MaterializeFromCfg(with_notes);
  ASSERT_NE(head, nullptr);
  const RtlControlFlowGraph round_tripped = ctx.DematerializeToCfg(head, RtlPassKind::kCse);
  ASSERT_EQ(round_tripped.rpo_blocks.size(), 3u);
  EXPECT_EQ(round_tripped.rpo_blocks[0].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopBeg}));
  EXPECT_EQ(round_tripped.rpo_blocks[1].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopCont, RtlLoopNoteKind::kLoopVtop}));
  EXPECT_EQ(round_tripped.rpo_blocks[2].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopEnd}));

  const auto scan_with_notes = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kScanLoop, with_notes, {}, /*optimize_level=*/2, {"-O2", "-mips2"});
  bool hoisted_to_preheader = false;
  for (const std::string& pat : scan_with_notes.post_state.rpo_blocks[0].instructions) {
    if (absl::StrContains(pat, "(plus:SI (reg/v:SI 73) (const_int 100))")) {
      hoisted_to_preheader = true;
      break;
    }
  }
  EXPECT_TRUE(hoisted_to_preheader) << scan_with_notes.post_state.FormatSummary();

  // Without `NOTE_INSN_LOOP_*` markers, `loop.c` sees `max_loop_num == 0` and cannot hoist.
  const RtlControlFlowGraph without_notes = make_loop_cfg(/*with_loop_notes=*/false);
  const auto scan_without_notes = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kScanLoop, without_notes, {}, /*optimize_level=*/2, {"-O2", "-mips2"});
  bool hoisted_without_notes = false;
  for (const std::string& pat : scan_without_notes.post_state.rpo_blocks[0].instructions) {
    if (absl::StrContains(pat, "(plus:SI (reg/v:SI 73) (const_int 100))")) {
      hoisted_without_notes = true;
      break;
    }
  }
  EXPECT_FALSE(hoisted_without_notes);
}

TEST(GccHarnessTest, InterBlockCallFusageSynthesisAndExplicitEmptyFusageFidelity) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ctx.BeginFunction("call_fusage_probe");

  // 1. Diamond CFG without explicit `call_fusage`: `$5` (`a1`) defined in `B0`, `$4` (`a0`)
  // defined on both arms `B1` and `B2`, reaching a `call_insn` in `B3`. Inter-block reaching
  // definitions must synthesize `(use (reg:SI 5 a1))` and `(use (reg:SI 4 a0))`.
  RtlControlFlowGraph diamond_cfg;
  diamond_cfg.pass = RtlPassKind::kRtl;
  diamond_cfg.rpo_blocks.resize(4);
  diamond_cfg.rpo_blocks[0].id = 0;
  diamond_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (const_int 7))");
  diamond_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int 0)) (label_ref L0) (pc)))");
  diamond_cfg.rpo_blocks[0].successors = {2, 1};

  diamond_cfg.rpo_blocks[1].id = 1;
  diamond_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 4 a0) (const_int 11))");
  diamond_cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref L1))");
  diamond_cfg.rpo_blocks[1].successors = {3};

  diamond_cfg.rpo_blocks[2].id = 2;
  diamond_cfg.rpo_blocks[2].has_label = true;
  diamond_cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 4 a0) (const_int 22))");
  diamond_cfg.rpo_blocks[2].successors = {3};

  diamond_cfg.rpo_blocks[3].id = 3;
  diamond_cfg.rpo_blocks[3].has_label = true;
  diamond_cfg.rpo_blocks[3].AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"two_arg_callee\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
  diamond_cfg.rpo_blocks[3].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 2 v0))");
  diamond_cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");

  Rtx diamond_head = ctx.MaterializeFromCfg(diamond_cfg);
  ASSERT_NE(diamond_head, nullptr);
  const RtlControlFlowGraph diamond_demat = ctx.DematerializeToCfg(diamond_head, RtlPassKind::kRtl);
  ASSERT_EQ(diamond_demat.rpo_blocks.size(), 4u);
  ASSERT_FALSE(diamond_demat.rpo_blocks[3].rtl_insns.empty());
  const RtlInsn& synth_call = diamond_demat.rpo_blocks[3].rtl_insns[0];
  EXPECT_TRUE(synth_call.has_explicit_call_fusage);
  ASSERT_EQ(synth_call.call_fusage.size(), 2u);
  EXPECT_EQ(synth_call.call_fusage[0].ToGccPrintRtlString(), "(use (reg:SI 5 a1))");
  EXPECT_EQ(synth_call.call_fusage[1].ToGccPrintRtlString(), "(use (reg:SI 4 a0))");

  // 2. Explicit empty `call_fusage` (`has_explicit_call_fusage = true`, `call_fusage = {}`) on a
  // 0-arg call preceded by a scratch `$4` write must NOT synthesize `(use (reg:SI 4 a0))`, so
  // `flow.c` marks the dead `$4` write with `REG_UNUSED` (and deletes it).
  RtlControlFlowGraph zero_arg_cfg;
  zero_arg_cfg.pass = RtlPassKind::kCse2;
  zero_arg_cfg.rpo_blocks.resize(1);
  zero_arg_cfg.rpo_blocks[0].id = 0;
  zero_arg_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 4 a0) (const_int 999))");
  RtlInsn zero_call;
  zero_call.canonical_pattern =
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"zero_arg_callee\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )";
  zero_call.has_explicit_call_fusage = true;
  zero_call.call_fusage.clear();
  zero_arg_cfg.rpo_blocks[0].AddInsn(std::move(zero_call));

  Rtx zero_head = ctx.MaterializeFromCfg(zero_arg_cfg);
  ASSERT_NE(zero_head, nullptr);
  const RtlControlFlowGraph zero_demat = ctx.DematerializeToCfg(zero_head, RtlPassKind::kCse2);
  ASSERT_EQ(zero_demat.rpo_blocks[0].rtl_insns.size(), 2u);
  EXPECT_TRUE(zero_demat.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage);
  EXPECT_TRUE(zero_demat.rpo_blocks[0].rtl_insns[1].call_fusage.empty());

  const auto flow_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kPropagateBlock, zero_arg_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2"});
  ASSERT_EQ(flow_snap.post_state.rpo_blocks.size(), 1u);
  // Because `zero_arg_callee` has empty `CALL_INSN_FUNCTION_USAGE`, `propagate_block` deletes the
  // dead `(set (reg:SI 4 a0) (const_int 999))` instruction completely!
  EXPECT_EQ(flow_snap.post_state.rpo_blocks[0].instructions.size(), 1u)
      << flow_snap.post_state.FormatSummary();
}

TEST(GccHarnessTest, RegPointerUserVarInStructAndUnchangingFlagsRoundTripLosslessly) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const std::string c_src = R"(
struct Pair {
  int a;
  int b;
};
int LoadStructAndScale(struct Pair* p, const int* k_tbl, int idx) {
  int local_acc = p->a + p->b;
  return local_acc + k_tbl[idx];
}
)";
  const RtlControlFlowGraph initial_cfg =
      hooks.CompileCSourceToInitialRtlCfg(c_src, "LoadStructAndScale", {"-O2", "-mips2"});
  EXPECT_FALSE(initial_cfg.sideband.pointer_regnos.empty());

  Rtx head = ctx.MaterializeFromCfg(initial_cfg);
  ASSERT_NE(head, nullptr);
  const RtlControlFlowGraph demat_cfg = ctx.DematerializeToCfg(head, RtlPassKind::kRtl);
  EXPECT_EQ(demat_cfg.sideband.pointer_regnos, initial_cfg.sideband.pointer_regnos);

  bool saw_user_var_reg = false;
  bool saw_in_struct_mem = false;
  for (const auto& blk : demat_cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      if (absl::StrContains(pat, "(reg/v:")) saw_user_var_reg = true;
      if (absl::StrContains(pat, "(mem/s:")) saw_in_struct_mem = true;
    }
  }
  EXPECT_TRUE(saw_user_var_reg) << demat_cfg.FormatSummary();
  EXPECT_TRUE(saw_in_struct_mem) << demat_cfg.FormatSummary();

  const RtlCfgPassComparison cmp = demat_cfg.CompareAgainst(initial_cfg);
  EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
}

TEST(GccHarnessTest, UnbundledAtomicCseMainThreadJumpsAndVerifyThreadJumpsRightInverse) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // Build a 6-block CFG:
  //   B0: redundant `(73 + 4)` twice, sets `reg 72 = reg 5 (a1)`, branches on `reg 72 != 0` to
  //       `L0` (`B2`) or falls through to `B1`.
  //   B1: sets `reg 72 = reg 4 (a0)` and falls through to join block `B2` (`L0`).
  //   B2 (`L0`): branches on `(ne:SI (reg:SI 72) (const_int 0))` to `L1` (`B4`) or falls through
  //       to `B3`.
  //   B3: sets `v0 = reg 74` and jumps to exit `B5` (`L2`).
  //   B4 (`L1`): sets `v0 = reg 74 + reg 75` and falls through to exit `B5` (`L2`).
  //   B5 (`L2`): `(use (reg/i:SI 2 v0))`.
  // Per `toplev.c:2857-2871`, CSE1 runs `thread_jumps(..., 1)` *before* `cse_main(..., 0)` +
  // `delete_dead_from_cse` + `jump_optimize`; per `toplev.c:2915-2936`, CSE2 runs
  // `jump_optimize` -> `cse_main(..., 1)` -> conditional `jump_optimize` *before*
  // `thread_jumps(..., 0)`.
  RtlControlFlowGraph pre_cse_cfg;
  pre_cse_cfg.pass = RtlPassKind::kJump;
  pre_cse_cfg.rpo_blocks.resize(6);

  pre_cse_cfg.rpo_blocks[0].id = 0;
  pre_cse_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (reg:SI 4 a0))");
  pre_cse_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 74) (plus:SI (reg:SI 73) (const_int 4)))");
  pre_cse_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 75) (plus:SI (reg:SI 73) (const_int 4)))");
  pre_cse_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (reg:SI 5 a1))");
  pre_cse_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 72) (const_int 0)) (label_ref L0) (pc)))");
  pre_cse_cfg.rpo_blocks[0].successors = {2, 1};

  pre_cse_cfg.rpo_blocks[1].id = 1;
  pre_cse_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0))");
  pre_cse_cfg.rpo_blocks[1].successors = {2};

  pre_cse_cfg.rpo_blocks[2].id = 2;
  pre_cse_cfg.rpo_blocks[2].has_label = true;
  pre_cse_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 72) (const_int 0)) (label_ref L1) (pc)))");
  pre_cse_cfg.rpo_blocks[2].successors = {4, 3};

  pre_cse_cfg.rpo_blocks[3].id = 3;
  pre_cse_cfg.rpo_blocks[3].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 74))");
  pre_cse_cfg.rpo_blocks[3].AddPatternOnly("(set (pc) (label_ref L2))");
  pre_cse_cfg.rpo_blocks[3].successors = {5};

  pre_cse_cfg.rpo_blocks[4].id = 4;
  pre_cse_cfg.rpo_blocks[4].has_label = true;
  pre_cse_cfg.rpo_blocks[4].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 74) (reg:SI 75)))");
  pre_cse_cfg.rpo_blocks[4].successors = {5};

  pre_cse_cfg.rpo_blocks[5].id = 5;
  pre_cse_cfg.rpo_blocks[5].has_label = true;
  pre_cse_cfg.rpo_blocks[5].AddPatternOnly("(use (reg/i:SI 2 v0))");

  // 1. Atomic `kCseBasicBlockCse1` eliminates the duplicate `(plus:SI (reg:SI 73) (const_int 4))`
  // inside `B0`, WITHOUT running `thread_jumps` (`B0` still branches to `B2`!).
  const auto atomic_cse_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kCseBasicBlockCse1, pre_cse_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2"});
  EXPECT_EQ(atomic_cse_snap.post_state.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  size_t plus4_count = 0;
  for (const std::string& pat : atomic_cse_snap.post_state.rpo_blocks[0].instructions) {
    if (absl::StrContains(pat, "(plus:SI")) {
      ++plus4_count;
    }
  }
  EXPECT_EQ(plus4_count, 1u) << atomic_cse_snap.post_state.FormatSummary();

  // 2. CSE1 pipeline order (`toplev.c:2857-2871`): `kThreadJumpsCse1` BEFORE `kCseBasicBlockCse1`
  // -> `kJumpOptimizeCse1`!
  const auto cse1_thread_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kThreadJumpsCse1, pre_cse_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2"});
  ASSERT_FALSE(
      cse1_thread_snap.post_state.CompareAgainst(cse1_thread_snap.pre_state).MatchesExactly());
  EXPECT_EQ(cse1_thread_snap.post_state.rpo_blocks[0].successors, (std::vector<size_t>{3, 1}));

  const auto cse1_main_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kCseBasicBlockCse1, cse1_thread_snap.post_state, {}, 2, {"-O2", "-mips2"});
  const auto cse1_post_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kJumpOptimizeCse1, cse1_main_snap.post_state, {}, 2, {"-O2", "-mips2"});

  // 3. CSE2 pipeline order (`toplev.c:2915-2936`): `kJumpOptimizeCse2Pre` -> `kCseBasicBlockCse2`
  // -> `kJumpOptimizeCse2` BEFORE `kThreadJumpsCse2`!
  const auto cse2_pre_j =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kJumpOptimizeCse2Pre, pre_cse_cfg, {},
                                       /*optimize_level=*/2, {"-O2", "-mips2"});
  const auto cse2_main = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kCseBasicBlockCse2, cse2_pre_j.post_state, {}, 2, {"-O2", "-mips2"});
  const auto cse2_post_j = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kJumpOptimizeCse2, cse2_main.post_state, {}, 2, {"-O2", "-mips2"});
  const auto cse2_thread_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kThreadJumpsCse2, cse2_post_j.post_state, {}, 2, {"-O2", "-mips2"});
  ASSERT_FALSE(
      cse2_thread_snap.post_state.CompareAgainst(cse2_thread_snap.pre_state).MatchesExactly());

  // 4. `VerifyThreadJumpsRightInverse`, `VerifyCse1StageRightInverse`, and
  // `VerifyCse2StageRightInverse` with `thread_jumps` active (positive + adversarial negative).
  class UnthreadingStageProvider final : public ToySubfunctionInverses {
   public:
    UnthreadingStageProvider(RtlControlFlowGraph cse1_pre_tj, RtlControlFlowGraph cse1_pre_cse,
                             RtlControlFlowGraph cse2_tj_out,
                             RtlControlFlowGraph expected_cse2_pre_tj,
                             RtlControlFlowGraph cse2_pre_cse)
        : cse1_pre_tj_(std::move(cse1_pre_tj)),
          cse1_pre_cse_(std::move(cse1_pre_cse)),
          cse2_tj_out_(std::move(cse2_tj_out)),
          expected_cse2_pre_tj_(std::move(expected_cse2_pre_tj)),
          cse2_pre_cse_(std::move(cse2_pre_cse)) {}
    RtlControlFlowGraph InvertThreadJumps(const RtlControlFlowGraph&,
                                          bool before_loop) const override {
      return before_loop ? cse1_pre_tj_ : cse2_tj_out_;
    }
    RtlControlFlowGraph InvertCseBasicBlock(const RtlControlFlowGraph& x) const override {
      if (x.pass == RtlPassKind::kCse) {
        return cse1_pre_cse_;
      }
      if (x.CompareAgainst(expected_cse2_pre_tj_).MatchesExactly()) {
        return cse2_pre_cse_;
      }
      RtlControlFlowGraph pass_thru = x;
      pass_thru.pass = RtlPassKind::kLoop;
      return pass_thru;
    }
    RtlControlFlowGraph InvertJump1Optimize(const RtlControlFlowGraph& x) const override {
      return x;
    }

   private:
    RtlControlFlowGraph cse1_pre_tj_;
    RtlControlFlowGraph cse1_pre_cse_;
    RtlControlFlowGraph cse2_tj_out_;
    RtlControlFlowGraph expected_cse2_pre_tj_;
    RtlControlFlowGraph cse2_pre_cse_;
  };

  hooks.ApplyCompilationProfile(
      CompilationProfile{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}});
  UnthreadingStageProvider good_prov(cse1_thread_snap.pre_state, cse1_main_snap.pre_state,
                                     cse2_thread_snap.pre_state, cse2_thread_snap.pre_state,
                                     cse2_main.pre_state);
  RightInverseVerifier good_verifier(&ctx, &hooks, &good_prov);
  const RightInverseResult pos_tj_res = good_verifier.VerifyThreadJumpsRightInverse(
      cse1_thread_snap.post_state, /*before_loop=*/true);
  EXPECT_TRUE(pos_tj_res.matches) << pos_tj_res.diff_detail;

  const RightInverseResult pos_cse1_res =
      good_verifier.VerifyCse1StageRightInverse(cse1_post_snap.post_state);
  EXPECT_TRUE(pos_cse1_res.matches) << pos_cse1_res.diff_detail;

  const RightInverseResult pos_cse2_res =
      good_verifier.VerifyCse2StageRightInverse(cse2_thread_snap.post_state);
  EXPECT_TRUE(pos_cse2_res.matches) << pos_cse2_res.diff_detail;

  // Adversarial negative: an `InvertThreadJumps` that corrupts `rpo_blocks[3]` in CSE1/CSE2 must
  // fail `VerifyThreadJumpsRightInverse`, `VerifyCse1StageRightInverse`, and
  // `VerifyCse2StageRightInverse`.
  RtlControlFlowGraph bad_unthreaded = cse1_thread_snap.pre_state;
  bad_unthreaded.rpo_blocks[3].instructions[0] = "(set (reg/i:SI 2 v0) (const_int 12345))";
  bad_unthreaded.rpo_blocks[3].SyncRtlInsnsFromInstructions();
  RtlControlFlowGraph bad_cse2_unthreaded = cse2_thread_snap.pre_state;
  bad_cse2_unthreaded.rpo_blocks[3].instructions[0] = "(set (reg/i:SI 2 v0) (const_int 12345))";
  bad_cse2_unthreaded.rpo_blocks[3].SyncRtlInsnsFromInstructions();

  UnthreadingStageProvider bad_prov(bad_unthreaded, cse1_main_snap.pre_state, bad_cse2_unthreaded,
                                    cse2_thread_snap.pre_state, cse2_main.pre_state);
  RightInverseVerifier bad_verifier(&ctx, &hooks, &bad_prov);
  EXPECT_FALSE(
      bad_verifier.VerifyThreadJumpsRightInverse(cse1_thread_snap.post_state, /*before_loop=*/true)
          .matches);
  EXPECT_FALSE(bad_verifier.VerifyCse1StageRightInverse(cse1_post_snap.post_state).matches);
  EXPECT_FALSE(bad_verifier.VerifyCse2StageRightInverse(cse2_thread_snap.post_state).matches);
}

TEST(GccContextTest, ParseRtxPointerFlagBeforeBeginFunctionAndResetFunctionFalseHygiene) {
  // 1. `ParseRtx` on `(reg/f:SI ...)` BEFORE `BeginFunction` and with high pseudo regno must be
  // completely memory-safe.
  GccContext fresh_ctx;
  Rtx pre_begin_ptr = fresh_ctx.ParseRtx("(reg/f:SI 72)");
  ASSERT_NE(pre_begin_ptr, nullptr);
  EXPECT_EQ(pre_begin_ptr->code, kReg);
  EXPECT_EQ(pre_begin_ptr->fld[0].rtint, 72);

  fresh_ctx.BeginFunction("ptr_hygiene_test");
  Rtx high_pseudo_ptr = fresh_ctx.ParseRtx("(reg/f:SI 512)");
  ASSERT_NE(high_pseudo_ptr, nullptr);
  EXPECT_EQ(high_pseudo_ptr->code, kReg);
  EXPECT_EQ(high_pseudo_ptr->fld[0].rtint, 512);

  // 2. `MaterializeFromCfg(..., /*reset_function=*/false)` must clear stale `regno_pointer_flag`
  // entries from an earlier state in the same `BeginFunction` scope.
  RtlControlFlowGraph ptr_cfg;
  ptr_cfg.pass = RtlPassKind::kRtl;
  ptr_cfg.rpo_blocks.resize(1);
  ptr_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0))");
  ptr_cfg.rpo_blocks[0].AddPatternOnly("(return)");
  ptr_cfg.sideband.pointer_regnos = {72};

  Rtx head1 = fresh_ctx.MaterializeFromCfg(ptr_cfg, /*reset_function=*/true);
  ASSERT_NE(head1, nullptr);
  RtlControlFlowGraph demat1 = fresh_ctx.DematerializeToCfg(head1, RtlPassKind::kRtl);
  EXPECT_EQ(demat1.sideband.pointer_regnos, (std::vector<int>{72}));

  RtlControlFlowGraph non_ptr_cfg = ptr_cfg;
  non_ptr_cfg.sideband.pointer_regnos.clear();
  non_ptr_cfg.rpo_blocks[0].instructions.insert(non_ptr_cfg.rpo_blocks[0].instructions.begin() + 1,
                                                "(set (reg/i:SI 2 v0) (mem:SI (reg:SI 72)))");
  non_ptr_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  Rtx head2 = fresh_ctx.MaterializeFromCfg(non_ptr_cfg, /*reset_function=*/false);
  ASSERT_NE(head2, nullptr);
  RtlControlFlowGraph demat2 = fresh_ctx.DematerializeToCfg(head2, RtlPassKind::kRtl);
  EXPECT_TRUE(demat2.sideband.pointer_regnos.empty());
}

TEST(RightInverseVerifierTest,
     AdversarialNegativeRejectsDroppedLoopNotesCallFusageAndPointerRegnos) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // Build a valid `SubfunctionId::kDoCrossJump` snapshot carrying:
  //   - `NOTE_INSN_LOOP_BEG` / `NOTE_INSN_LOOP_END`
  //   - `CALL_INSN` with explicit 1-arg `call_fusage` (`(use (reg:SI 4 a0))`)
  //   - `sideband.pointer_regnos = {72}`
  RtlControlFlowGraph base_cfg;
  base_cfg.pass = RtlPassKind::kSched2;
  base_cfg.sideband.pointer_regnos = {72};
  base_cfg.rpo_blocks.resize(1);
  base_cfg.rpo_blocks[0].AddLoopNote(RtlLoopNoteKind::kLoopBeg);
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee_one_arg\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  call_insn.call_fusage = {RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4))};
  base_cfg.rpo_blocks[0].AddInsn(std::move(call_insn));
  base_cfg.rpo_blocks[0].AddLoopNote(RtlLoopNoteKind::kLoopEnd);
  base_cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  const SubfunctionBoundarySnapshot snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kDoCrossJump, base_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2"});

  RightInverseVerifier ok_verifier(&ctx, &hooks);
  const RightInverseResult ok_res = ok_verifier.VerifySubfunctionBoundaryRightInverse(snap);
  ASSERT_TRUE(ok_res.matches) << ok_res.diff_detail;

  // (a) Dropping `NOTE_INSN_LOOP_*` must fail `RightInverseVerifier` (`.matches == false`).
  class DropLoopNotesProvider final : public ToySubfunctionInverses {
   public:
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& x) const override {
      RtlControlFlowGraph bad = x;
      auto& insns = bad.rpo_blocks[0].instructions;
      insns.erase(
          std::remove_if(insns.begin(), insns.end(),
                         [](const std::string& p) { return ParseRtlLoopNoteKind(p).has_value(); }),
          insns.end());
      bad.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      return bad;
    }
  };
  DropLoopNotesProvider drop_loop_prov;
  RightInverseVerifier drop_loop_verifier(&ctx, &hooks, &drop_loop_prov);
  EXPECT_FALSE(drop_loop_verifier.VerifySubfunctionBoundaryRightInverse(snap).matches);

  // (b1) Dropping `has_explicit_call_fusage = false` must fail `RightInverseVerifier` even if
  // `MaterializeFromCfg` would have synthesized a fallback.
  class DropExplicitFusageFlagProvider final : public ToySubfunctionInverses {
   public:
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& x) const override {
      RtlControlFlowGraph bad = x;
      for (auto& rinsn : bad.rpo_blocks[0].rtl_insns) {
        rinsn.has_explicit_call_fusage = false;
        rinsn.call_fusage.clear();
      }
      return bad;
    }
  };
  DropExplicitFusageFlagProvider drop_fusage_flag_prov;
  RightInverseVerifier drop_fusage_flag_verifier(&ctx, &hooks, &drop_fusage_flag_prov);
  EXPECT_FALSE(drop_fusage_flag_verifier.VerifySubfunctionBoundaryRightInverse(snap).matches);

  // (b2) Corrupting `call_fusage` (clearing `(use (reg:SI 4 a0))` while keeping
  // `has_explicit_call_fusage = true`) must fail `RightInverseVerifier` via `CompareAgainst`.
  class CorruptCallFusageArgsProvider final : public ToySubfunctionInverses {
   public:
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& x) const override {
      RtlControlFlowGraph bad = x;
      for (auto& rinsn : bad.rpo_blocks[0].rtl_insns) {
        if (rinsn.has_explicit_call_fusage) {
          rinsn.call_fusage.clear();
        }
      }
      return bad;
    }
  };
  CorruptCallFusageArgsProvider corrupt_fusage_prov;
  RightInverseVerifier corrupt_fusage_verifier(&ctx, &hooks, &corrupt_fusage_prov);
  const RightInverseResult corrupt_fusage_res =
      corrupt_fusage_verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_FALSE(corrupt_fusage_res.matches);
  EXPECT_FALSE(corrupt_fusage_res.cfg_comparison.call_fusage_match);

  // (c) Dropping or corrupting `sideband.pointer_regnos` must fail `RightInverseVerifier`.
  class DropPointerRegnosProvider final : public ToySubfunctionInverses {
   public:
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& x) const override {
      RtlControlFlowGraph bad = x;
      bad.sideband.pointer_regnos.clear();
      return bad;
    }
  };
  DropPointerRegnosProvider drop_ptr_prov;
  RightInverseVerifier drop_ptr_verifier(&ctx, &hooks, &drop_ptr_prov);
  const RightInverseResult drop_ptr_res =
      drop_ptr_verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_FALSE(drop_ptr_res.matches);
  EXPECT_FALSE(drop_ptr_res.cfg_comparison.pointer_regnos_match);
}

TEST(RightInverseVerifierTest, AdversarialNegativePostCseJumpAndCseJumpsAlteredVerification) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // 1. `InvertJumpOptimizePostCse` must be invoked without any `snap.pre_state` oracle access and
  // its candidate pre-image must NEVER be silently discarded on `kJumpOptimizeCse1` (`-O1`) or
  // `kJumpOptimizeCse2` (`-O2`), even when `cse_jumps_altered == false`.
  const FunctionCompilationTrace trace_o1 = hooks.TraceForwardCompilation(
      "int add_two(int a, int b) { return a + b; }\n", "add_two", {"-O1"}, "post_cse_jump_neg_o1");
  const FunctionCompilationTrace trace_o2 = hooks.TraceForwardCompilation(
      "int add_two(int a, int b) { return a + b; }\n", "add_two", {"-O2"}, "post_cse_jump_neg_o2");

  const SubfunctionBoundarySnapshot* cse1_j_o1_ptr = nullptr;
  for (const auto& s : trace_o1.subfunction_snapshots) {
    if (s.subfunction_id == SubfunctionId::kJumpOptimizeCse1) {
      cse1_j_o1_ptr = &s;
      break;
    }
  }
  const SubfunctionBoundarySnapshot* cse2_j_o2_ptr = nullptr;
  for (const auto& s : trace_o2.subfunction_snapshots) {
    if (s.subfunction_id == SubfunctionId::kJumpOptimizeCse2) {
      cse2_j_o2_ptr = &s;
      break;
    }
  }
  ASSERT_NE(cse1_j_o1_ptr, nullptr);
  ASSERT_NE(cse2_j_o2_ptr, nullptr);
  ASSERT_FALSE(cse1_j_o1_ptr->pre_state.sideband.cse_jumps_altered);
  ASSERT_FALSE(cse1_j_o1_ptr->post_state.sideband.cse_jumps_altered);
  ASSERT_FALSE(cse2_j_o2_ptr->pre_state.sideband.cse_jumps_altered);
  ASSERT_FALSE(cse2_j_o2_ptr->post_state.sideband.cse_jumps_altered);

  // Strip `pre_state` completely to prove `VerifySubfunctionBoundaryRightInverse` is oracle-free.
  SubfunctionBoundarySnapshot oracle_free_cse1_j = *cse1_j_o1_ptr;
  oracle_free_cse1_j.pre_state = {};
  SubfunctionBoundarySnapshot oracle_free_cse2_j = *cse2_j_o2_ptr;
  oracle_free_cse2_j.pre_state = {};

  RightInverseVerifier ok_verifier(&ctx, &hooks);
  EXPECT_TRUE(ok_verifier.VerifySubfunctionBoundaryRightInverse(oracle_free_cse1_j).matches);
  EXPECT_TRUE(ok_verifier.VerifySubfunctionBoundaryRightInverse(oracle_free_cse2_j).matches);

  class CorruptedJumpOptimizePostCseProvider final : public ToySubfunctionInverses {
   public:
    RtlControlFlowGraph InvertJumpOptimizePostCse(const RtlControlFlowGraph& x,
                                                  bool /*after_loop*/) const override {
      RtlControlFlowGraph bad = x;
      if (!bad.rpo_blocks.empty() && !bad.rpo_blocks[0].instructions.empty()) {
        bad.rpo_blocks[0].instructions[0] = "(set (reg:SI 72) (const_int 99999))";
        bad.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      }
      bad.sideband.cse_jumps_altered = false;
      return bad;
    }
  };
  CorruptedJumpOptimizePostCseProvider bad_post_cse_jump_prov;
  RightInverseVerifier bad_post_cse_jump_verifier(&ctx, &hooks, &bad_post_cse_jump_prov);
  EXPECT_FALSE(
      bad_post_cse_jump_verifier.VerifySubfunctionBoundaryRightInverse(oracle_free_cse1_j).matches);
  EXPECT_FALSE(
      bad_post_cse_jump_verifier.VerifySubfunctionBoundaryRightInverse(oracle_free_cse2_j).matches);

  hooks.ApplyCompilationProfile({.optimize_level = 1, .c_flags = {"-O1"}});
  EXPECT_TRUE(ok_verifier.VerifyCse1StageRightInverse(trace_o1.pass_snapshots.at(RtlPassKind::kCse))
                  .matches);
  EXPECT_FALSE(bad_post_cse_jump_verifier
                   .VerifyCse1StageRightInverse(trace_o1.pass_snapshots.at(RtlPassKind::kCse))
                   .matches);

  hooks.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});
  EXPECT_TRUE(
      ok_verifier.VerifyCse2StageRightInverse(trace_o2.pass_snapshots.at(RtlPassKind::kCse2))
          .matches);
  EXPECT_FALSE(bad_post_cse_jump_verifier
                   .VerifyCse2StageRightInverse(trace_o2.pass_snapshots.at(RtlPassKind::kCse2))
                   .matches);

  // 2. `kCseBasicBlockCse1`, `kCseBasicBlockCse2`, and `VerifyCseBasicBlockRightInverse` must
  // reject any candidate pre-image whose forward replay mismatches `sideband.cse_jumps_altered`.
  RtlControlFlowGraph pre_fold_branch_cfg;
  pre_fold_branch_cfg.pass = RtlPassKind::kJump;
  pre_fold_branch_cfg.rpo_blocks.resize(4);
  pre_fold_branch_cfg.rpo_blocks[0].id = 0;
  pre_fold_branch_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 0))");
  pre_fold_branch_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 72) (const_int 0)) (label_ref L0) (pc)))");
  pre_fold_branch_cfg.rpo_blocks[0].successors = {2, 1};

  pre_fold_branch_cfg.rpo_blocks[1].id = 1;
  pre_fold_branch_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 10))");
  pre_fold_branch_cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref L1))");
  pre_fold_branch_cfg.rpo_blocks[1].successors = {3};

  pre_fold_branch_cfg.rpo_blocks[2].id = 2;
  pre_fold_branch_cfg.rpo_blocks[2].has_label = true;
  pre_fold_branch_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 20))");
  pre_fold_branch_cfg.rpo_blocks[2].successors = {3};

  pre_fold_branch_cfg.rpo_blocks[3].id = 3;
  pre_fold_branch_cfg.rpo_blocks[3].has_label = true;
  pre_fold_branch_cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot cse1_altered_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kCseBasicBlockCse1, pre_fold_branch_cfg, {}, 2, {"-O2", "-mips2"});
  RtlControlFlowGraph pre_fold_cse2_cfg = pre_fold_branch_cfg;
  pre_fold_cse2_cfg.pass = RtlPassKind::kLoop;
  const SubfunctionBoundarySnapshot cse2_altered_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kCseBasicBlockCse2, pre_fold_cse2_cfg, {}, 2, {"-O2", "-mips2"});
  ASSERT_TRUE(cse1_altered_snap.post_state.sideband.cse_jumps_altered);
  ASSERT_TRUE(cse2_altered_snap.post_state.sideband.cse_jumps_altered);

  // A broken `InvertCseBasicBlock` that leaves the branch already folded reproduces the post-CSE
  // instructions structurally, but produces `replayed.post_state.sideband.cse_jumps_altered ==
  // false != true` and must be rejected.
  class IdentityCseBasicBlockProvider final : public ToySubfunctionInverses {
   public:
    RtlControlFlowGraph InvertCseBasicBlock(const RtlControlFlowGraph& x) const override {
      RtlControlFlowGraph out = x;
      out.sideband.cse_jumps_altered = false;
      return out;
    }
  };
  IdentityCseBasicBlockProvider id_cse_prov;
  RightInverseVerifier id_cse_verifier(&ctx, &hooks, &id_cse_prov);

  const RightInverseResult bad_cse1_res =
      id_cse_verifier.VerifySubfunctionBoundaryRightInverse(cse1_altered_snap);
  EXPECT_FALSE(bad_cse1_res.matches);
  EXPECT_TRUE(absl::StrContains(bad_cse1_res.diff_detail, "sideband.cse_jumps_altered"))
      << bad_cse1_res.diff_detail;

  const RightInverseResult bad_cse2_res =
      id_cse_verifier.VerifySubfunctionBoundaryRightInverse(cse2_altered_snap);
  EXPECT_FALSE(bad_cse2_res.matches);
  EXPECT_TRUE(absl::StrContains(bad_cse2_res.diff_detail, "sideband.cse_jumps_altered"))
      << bad_cse2_res.diff_detail;

  const RightInverseResult bad_cse_bb_res =
      id_cse_verifier.VerifyCseBasicBlockRightInverse(cse2_altered_snap.post_state);
  EXPECT_FALSE(bad_cse_bb_res.matches);
  EXPECT_TRUE(absl::StrContains(bad_cse_bb_res.diff_detail, "sideband.cse_jumps_altered"))
      << bad_cse_bb_res.diff_detail;

  // Conversely, an inverse that unfolds the constant-folded branch reproduces both the post-CSE
  // CFG and `sideband.cse_jumps_altered == true`.
  class UnfoldBranchCseProvider final : public ToySubfunctionInverses {
   public:
    UnfoldBranchCseProvider(RtlControlFlowGraph pre_cse1, RtlControlFlowGraph pre_cse2)
        : pre_cse1_(std::move(pre_cse1)), pre_cse2_(std::move(pre_cse2)) {}
    RtlControlFlowGraph InvertCseBasicBlock(const RtlControlFlowGraph& x) const override {
      return x.pass == RtlPassKind::kCse ? pre_cse1_ : pre_cse2_;
    }

   private:
    RtlControlFlowGraph pre_cse1_;
    RtlControlFlowGraph pre_cse2_;
  };
  UnfoldBranchCseProvider unfold_prov(pre_fold_branch_cfg, pre_fold_cse2_cfg);
  RightInverseVerifier unfold_verifier(&ctx, &hooks, &unfold_prov);
  EXPECT_TRUE(unfold_verifier.VerifySubfunctionBoundaryRightInverse(cse1_altered_snap).matches);
  EXPECT_TRUE(unfold_verifier.VerifySubfunctionBoundaryRightInverse(cse2_altered_snap).matches);
}

TEST(GccHarnessTest, PreservesRegnoPointerFlagFidelityAcrossLocalAllocHooksAndVerifiers) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // Pseudo 72 is declared in `pointer_regnos` without a pointer-inducing `PLUS`/`MEM`
  // expression (unguarded `restore_reg_data` + `reg_scan` would clear `regno_pointer_flag[72]`).
  // Pseudo 73 is a non-pointer integer holding `sp + 16` (unguarded `reg_scan` would set
  // `regno_pointer_flag[73] = 1`).
  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kSched;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 29 sp) (const_int 16)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_DEAD (reg:SI 73) (nil)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  pre_cfg.sideband.pointer_regnos = {72};

  // 1. `RunUpdateEquivRegs` preserves `pointer_regnos == {72}`.
  hooks.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});
  ctx.MaterializeFromCfg(pre_cfg);
  hooks.RunUpdateEquivRegs();
  const RtlControlFlowGraph after_equiv =
      ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kLreg);
  EXPECT_EQ(after_equiv.sideband.pointer_regnos, std::vector<int>({72}));

  // 2. `ExecuteSubfunctionBoundary(kBlockAlloc)` preserves `pointer_regnos == {72}` at `-O2` &
  // `-O0`.
  const SubfunctionBoundarySnapshot ba_o2 =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, after_equiv, {}, 2, {"-O2"});
  EXPECT_EQ(ba_o2.post_state.sideband.pointer_regnos, std::vector<int>({72}));

  const SubfunctionBoundarySnapshot ba_o0 =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, pre_cfg, {}, 0, {"-O0"});
  EXPECT_EQ(ba_o0.post_state.sideband.pointer_regnos, std::vector<int>({72}));

  // 3. `RunLocalAlloc` preserves `pointer_regnos == {72}`.
  hooks.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});
  ctx.MaterializeFromCfg(pre_cfg);
  hooks.RunLocalAlloc();
  const RtlControlFlowGraph after_lreg = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kLreg);
  EXPECT_EQ(after_lreg.sideband.pointer_regnos, std::vector<int>({72}));

  // 4. `RightInverseVerifier` catches corrupted `pointer_regnos` without post-pass masking.
  class CorruptedPointerFlagProvider final : public ToySubfunctionInverses {
   public:
    CorruptedPointerFlagProvider(RtlControlFlowGraph pre, RtlControlFlowGraph mid)
        : pre_(std::move(pre)), mid_(std::move(mid)) {}
    RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph&,
                                              const CompilationProfile&) const override {
      RtlControlFlowGraph bad = pre_;
      bad.sideband.pointer_regnos = {73};
      return bad;
    }
    RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph&,
                                         const CompilationProfile&) const override {
      RtlControlFlowGraph bad = mid_;
      bad.sideband.pointer_regnos = {73};
      return bad;
    }

   private:
    RtlControlFlowGraph pre_;
    RtlControlFlowGraph mid_;
  };
  CorruptedPointerFlagProvider bad_prov(pre_cfg, after_equiv);
  RightInverseVerifier bad_verifier(&ctx, &hooks, &bad_prov);
  hooks.ApplyCompilationProfile({.optimize_level = 2, .c_flags = {"-O2"}});
  EXPECT_FALSE(bad_verifier.VerifyUpdateEquivRegsRightInverse(after_equiv).matches);
  EXPECT_FALSE(bad_verifier.VerifyBlockAllocRightInverse(ba_o2.post_state).matches);
  EXPECT_FALSE(bad_verifier.VerifyLocalAllocStageRightInverse(after_lreg).matches);
}

}  // namespace
}  // namespace rom_nom_nom::gcc_harness
