#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/rtl_cfg.h"
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
  cfg.rpo_blocks[2].AddPatternOnly("(return)");
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
  cfg.rpo_blocks[0].AddPatternOnly("(return)");

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

  // Test `find_end_label` + genuine `make_return_insns`:
  ctx.BeginFunction("reorg_return_test");
  ctx.EmitPattern(
      "(sequence [(jump_insn (set (pc) (label_ref L0))) "
      "(insn (set (reg/i:SI 2 v0) (const_int 7)))])");
  ctx.EmitPattern("(barrier)");
  ctx.EmitPattern("(code_label L0)");

  hooks.RunMakeReturnInsns();
  EXPECT_TRUE(absl::StrContains(ctx.DumpInsnChain(), "(return)")) << ctx.DumpInsnChain();

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

  // 6. `reorg.c:make_return_insns` + `find_end_label` right-inverse:
  RtlControlFlowGraph ret_cfg;
  ret_cfg.pass = RtlPassKind::kDbr;
  ret_cfg.rpo_blocks.resize(1);
  ret_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (return)) "
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
  relax_cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  relax_cfg.rpo_blocks[2].AddPatternOnly("(return)");
  relax_cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 2 v0) (const_int 2))");
  relax_cfg.rpo_blocks[3].AddPatternOnly("(return)");
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
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))");
  eager_fallthrough_cfg.rpo_blocks[1].successors = {2};
  eager_fallthrough_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))");
  eager_fallthrough_cfg.rpo_blocks[2].AddPatternOnly("(return)");
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
  eager_target_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (reg:SI 4 a0))");
  eager_target_cfg.rpo_blocks[1].AddPatternOnly("(return)");
  eager_target_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))");
  eager_target_cfg.rpo_blocks[2].AddPatternOnly("(return)");
  RightInverseResult eager_tgt_res =
      verifier.VerifyFillEagerDelaySlotsRightInverse(eager_target_cfg);
  EXPECT_TRUE(eager_tgt_res.matches) << eager_tgt_res.diff_detail;

  // 9. `reorg.c:fill_simple_delay_slots` right-inverse:
  RtlControlFlowGraph simple_cfg;
  simple_cfg.pass = RtlPassKind::kDbr;
  simple_cfg.rpo_blocks.resize(2);
  simple_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (set (pc) (label_ref L0))) "
      "(insn (set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3))))])");
  simple_cfg.rpo_blocks[0].successors = {1};
  simple_cfg.rpo_blocks[1].AddPatternOnly("(return)");
  RightInverseResult simple_res = verifier.VerifyFillSimpleDelaySlotsRightInverse(simple_cfg);
  EXPECT_TRUE(simple_res.matches) << simple_res.diff_detail;

  // 10. `reorg.c:dbr_schedule` right-inverse:
  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks.resize(1);
  dbr_cfg.rpo_blocks[0].AddPatternOnly(
      "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) "
      "(plus:SI (reg:SI 4 a0) (const_int 1))))])");
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
  jump2_cfg.rpo_blocks[3].AddPatternOnly("(return)");
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
  fbb_cfg.rpo_blocks[0].AddPatternOnly("(return)");

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
  flow_cfg.rpo_blocks[0].AddPatternOnly("(return)");

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
  cse_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 72)))");

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
  redirect_cfg.rpo_blocks[1].AddPatternOnly("(return)");
  redirect_cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 2))");
  redirect_cfg.rpo_blocks[2].AddPatternOnly("(return)");

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

}  // namespace
}  // namespace rom_nom_nom::gcc_harness
