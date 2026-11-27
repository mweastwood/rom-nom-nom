#include "lifter/reorg_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

RtlExpr MustParse(std::string_view sexpr) {
  auto parsed = ParsePatternAndNotes(sexpr);
  EXPECT_TRUE(parsed.ok()) << sexpr;
  return parsed.ok() ? parsed->pattern : RtlExpr::MakeNil();
}

RtlBasicBlock MakeBlock(uint32_t id, bool has_label, std::vector<std::string> insns,
                        std::vector<size_t> succs = {}, bool has_trailing_barrier = false) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.has_label = has_label;
  blk.instructions = std::move(insns);
  blk.successors = std::move(succs);
  blk.has_trailing_barrier = has_trailing_barrier;
  return blk;
}

TEST(ReorgModelTest, MipsDelaySlotEligibilityAcrossIsaKindsAndAnnulModes) {
  const RtlExpr add_si = MustParse("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");

  // MIPS1 vs MIPS2 annul modes (`mips.md:125-138`):
  // - kNormal: eligible for branch, jump, non-abicalls call; ineligible for abicalls call.
  // - kAnnulTrue: always `(nil)` on MIPS.
  // - kAnnulFalse: eligible ONLY for MIPS2+ branches (`branch_likely == "yes"`).
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 1,
                                   .kind = MipsDelaySlotKind::kBranch,
                                   .annul_mode = MipsDelaySlotAnnulMode::kNormal}));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 1,
                                   .kind = MipsDelaySlotKind::kJump,
                                   .annul_mode = MipsDelaySlotAnnulMode::kNormal}));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 1,
                                   .abicalls = false,
                                   .kind = MipsDelaySlotKind::kCall,
                                   .annul_mode = MipsDelaySlotAnnulMode::kNormal}));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 1,
                                   .abicalls = true,
                                   .kind = MipsDelaySlotKind::kCall,
                                   .annul_mode = MipsDelaySlotAnnulMode::kNormal}));

  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 2,
                                   .kind = MipsDelaySlotKind::kBranch,
                                   .annul_mode = MipsDelaySlotAnnulMode::kAnnulTrue}));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 1,
                                   .kind = MipsDelaySlotKind::kBranch,
                                   .annul_mode = MipsDelaySlotAnnulMode::kAnnulFalse}));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 2,
                                   .kind = MipsDelaySlotKind::kBranch,
                                   .annul_mode = MipsDelaySlotAnnulMode::kAnnulFalse}));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 2,
                                   .kind = MipsDelaySlotKind::kJump,
                                   .annul_mode = MipsDelaySlotAnnulMode::kAnnulFalse}));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      add_si, MipsDelaySlotOptions{.mips_isa = 2,
                                   .kind = MipsDelaySlotKind::kCall,
                                   .annul_mode = MipsDelaySlotAnnulMode::kAnnulFalse}));
}

TEST(ReorgModelTest, MipsDelaySlotEligibilityPositiveAndNegativePatterns) {
  const MipsDelaySlotOptions mips1_o32{.gp64 = false, .fp64 = false, .mips_isa = 1};
  const MipsDelaySlotOptions mips3_64{.gp64 = true, .fp64 = true, .mips_isa = 2};

  // Positive 1-insn `dslot == no && length == 1` patterns.
  EXPECT_TRUE(
      IsEligibleForMipsDelaySlot(MustParse("(set (reg:SI 2 v0) (const_int 65535))"), mips1_o32));
  EXPECT_TRUE(
      IsEligibleForMipsDelaySlot(MustParse("(set (reg:SI 2 v0) (const_int 65536))"), mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:SI 2 v0) (high:SI (symbol_ref:SI (\"g_var\"))))"), mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:SI 2 v0) (lo_sum:SI (reg:SI 2 v0) (symbol_ref:SI (\"g_var\"))))"),
      mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      MustParse("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 4 a0))"),
      mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:SF 32 $f0) (float_truncate:SF (reg:DF 34 $f2)))"), mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:SI 2 v0) (le:SI (reg:SI 4 a0) (const_int 10)))"), mips1_o32));

  // Small-data symbolic store depends on `-G N` (`allow_sdata_symbolic_store`).
  const RtlExpr sym_store = MustParse("(set (mem:SI (symbol_ref:SI (\"g_sdata\"))) (reg:SI 4 a0))");
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(sym_store, mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(sym_store,
                                         MipsDelaySlotOptions{.allow_sdata_symbolic_store = true}));

  // Negative cases: loads (`dslot == yes`), HI/LO/FPSW, xfer, multi-insn templates,
  // calls, branches, parallels, unspecs, 2-word immediates, DI/DF on 32-bit.
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"),
      mips1_o32));
  EXPECT_FALSE(
      IsEligibleForMipsDelaySlot(MustParse("(set (reg:SI 2 v0) (reg:SI 65 lo))"), mips1_o32));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:CC 67 $fcc0) (lt:CC (reg:SF 32 $f0) (reg:SF 34 $f2)))"), mips1_o32));
  EXPECT_FALSE(
      IsEligibleForMipsDelaySlot(MustParse("(set (reg:SI 2 v0) (reg:SF 32 $f0))"), mips1_o32));
  EXPECT_FALSE(
      IsEligibleForMipsDelaySlot(MustParse("(set (reg:SF 32 $f0) (reg:SI 2 v0))"), mips1_o32));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:DF 32 $f0) (float:DF (reg:SI 4 a0)))"), mips1_o32));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(MustParse("(set (reg:SI 2 v0) (const_int 305419896))"),
                                          mips1_o32));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:SI 2 v0) (symbol_ref:SI (\"g_var\")))"), mips1_o32));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      MustParse("(set (reg:SI 2 v0) (ge:SI (reg:SI 4 a0) (reg:SI 5 a1)))"), mips1_o32));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(MustParse("(parallel[ (return) (use (reg:SI 31 ra)) ] )"),
                                          mips1_o32));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(
      MustParse("(call (mem:QI (symbol_ref:SI (\"foo\"))) (const_int 16))"), mips1_o32));
  EXPECT_FALSE(
      IsEligibleForMipsDelaySlot(MustParse("(unspec_volatile [(const_int 0)] 0)"), mips1_o32));

  // `DImode` / `DFmode` store eligibility respects `gp64` / `fp64`.
  const RtlExpr di_move = MustParse("(set (reg:DI 2 v0) (reg:DI 4 a0))");
  const RtlExpr df_store = MustParse("(set (mem:DF (reg:SI 4 a0)) (reg:DF 32 $f0))");
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(di_move, mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(di_move, mips3_64));
  EXPECT_FALSE(IsEligibleForMipsDelaySlot(df_store, mips1_o32));
  EXPECT_TRUE(IsEligibleForMipsDelaySlot(df_store, mips3_64));
}

TEST(ReorgModelTest, MostlyTrueJumpScoreMatchesReorgHeuristics) {
  // Condition heuristics (`reorg.c:1401-1429`):
  const RtlExpr beq_reg =
      MustParse("(set (pc) (if_then_else (eq (reg:SI 4 a0) (reg:SI 5 a1)) (label_ref L1) (pc)))");
  const RtlExpr bne_reg =
      MustParse("(set (pc) (if_then_else (ne (reg:SI 4 a0) (reg:SI 5 a1)) (label_ref L1) (pc)))");
  const RtlExpr bltz =
      MustParse("(set (pc) (if_then_else (lt (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))");
  const RtlExpr bgez =
      MustParse("(set (pc) (if_then_else (ge (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))");
  const RtlExpr blt_reg =
      MustParse("(set (pc) (if_then_else (lt (reg:SI 4 a0) (reg:SI 5 a1)) (label_ref L1) (pc)))");
  const RtlExpr flipped_eq =
      MustParse("(set (pc) (if_then_else (eq (reg:SI 4 a0) (const_int 0)) (pc) (label_ref L1)))");

  // `EQ` -> 0 even on backward branch; `NE` -> 1 even on forward branch.
  EXPECT_EQ(MostlyTrueJumpScore(beq_reg, /*block_b=*/3, /*target_b=*/1), 0);
  EXPECT_EQ(MostlyTrueJumpScore(bne_reg, /*block_b=*/1, /*target_b=*/3), 1);
  // Flipped `(if_then_else (eq ...) (pc) (label_ref))` reverses to `NE` -> 1.
  EXPECT_EQ(MostlyTrueJumpScore(flipped_eq, /*block_b=*/1, /*target_b=*/3), 1);
  // Signed vs 0: `LT 0` -> 0, `GE 0` -> 1.
  EXPECT_EQ(MostlyTrueJumpScore(bltz, /*block_b=*/3, /*target_b=*/1), 0);
  EXPECT_EQ(MostlyTrueJumpScore(bgez, /*block_b=*/1, /*target_b=*/3), 1);
  // Non-zero `LT` falls through to backward (1) vs forward (0).
  EXPECT_EQ(MostlyTrueJumpScore(blt_reg, /*block_b=*/3, /*target_b=*/1), 1);
  EXPECT_EQ(MostlyTrueJumpScore(blt_reg, /*block_b=*/1, /*target_b=*/3), 0);

  // CFG-aware loop notes (`reorg.c:1359-1373`):
  // `NOTE_INSN_LOOP_BEG` before target -> 2; `NOTE_INSN_LOOP_VTOP` at target -> 1.
  RtlControlFlowGraph loop_cfg;
  loop_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(note NOTE_INSN_LOOP_BEG)"}, {1}),
      MakeBlock(1, true, {"(set (reg:SI 2 v0) (const_int 0))", beq_reg.ToGccPrintRtlString()},
                {1, 2}),
      MakeBlock(2, true,
                {"(note NOTE_INSN_LOOP_VTOP)", "(set (reg:SI 3 v1) (const_int 1))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };
  EXPECT_EQ(MostlyTrueJumpScore(loop_cfg, /*block_b=*/1, beq_reg, /*target_b=*/1), 2);
  EXPECT_EQ(MostlyTrueJumpScore(loop_cfg, /*block_b=*/1, beq_reg, /*target_b=*/2), 1);

  // `reorg.c:1344-1351` (`LABEL_OUTSIDE_LOOP_P`, `(label_ref/s ...)`): returns
  // `-1` on either `if_then_else` arm before loop notes, `rare_destination`, or
  // `NE` condition heuristics.
  const RtlExpr bne_loop_exit =
      MustParse("(set (pc) (if_then_else (ne (reg:SI 4 a0) (reg:SI 5 a1)) (label_ref/s L1) (pc)))");
  const RtlExpr beq_flipped_exit =
      MustParse("(set (pc) (if_then_else (eq (reg:SI 4 a0) (const_int 0)) (pc) (label_ref/s L1)))");
  EXPECT_EQ(MostlyTrueJumpScore(bne_loop_exit, /*block_b=*/1, /*target_b=*/3), -1);
  EXPECT_EQ(MostlyTrueJumpScore(beq_flipped_exit, /*block_b=*/1, /*target_b=*/3), -1);
  EXPECT_EQ(MostlyTrueJumpScore(loop_cfg, /*block_b=*/1, bne_loop_exit, /*target_b=*/1), -1);
  EXPECT_EQ(MostlyTrueJumpScore(loop_cfg, /*block_b=*/1, bne_loop_exit, /*target_b=*/2), -1);

  // `reorg.c:1444-1448` + `jump.c:2983`: bare `(return)` scores 1, while MIPS
  // `return_internal` `PARALLEL` is not unwrapped by `condjump_in_parallel_p`
  // and returns 0.
  EXPECT_EQ(MostlyTrueJumpScore(MustParse("(return)"), /*block_b=*/1, /*target_b=*/3), 1);
  EXPECT_EQ(MostlyTrueJumpScore(MustParse("(parallel[ (return) (use (reg:SI 31 ra)) ] )"),
                                /*block_b=*/1, /*target_b=*/3),
            0);

  // CFG-aware `rare_destination` (`reorg.c:1300-1325, 1379-1391`):
  // 1. Non-simple epilogue falling off end of function (`successors = {}`, no
  //    `return_internal`) -> `rare_fallthrough = 1` vs `rare_dest = 0` => 1.
  RtlControlFlowGraph falloff_cfg;
  falloff_cfg.rpo_blocks = {
      MakeBlock(0, false, {beq_reg.ToGccPrintRtlString()}, {2, 1}),
      MakeBlock(1, false, {"(use (reg/i:SI 2 v0))"}, {}),
      MakeBlock(2, true, {"(set (reg:SI 2 v0) (const_int 1))", "(set (pc) (label_ref L1))"}, {3}),
      MakeBlock(3, true, {"(set (reg:SI 2 v0) (const_int 2))"}, {}),
  };
  EXPECT_EQ(MostlyTrueJumpScore(falloff_cfg, /*block_b=*/0, beq_reg, /*target_b=*/2), 1);

  // 2. Post-call noreturn barrier (`has_trailing_barrier = true`, `successors = {}`)
  //    -> `rare_fallthrough = 2` (`reorg.c:1304-1309`) vs `rare_dest = 0` => 2.
  RtlControlFlowGraph barrier_cfg = falloff_cfg;
  barrier_cfg.rpo_blocks[1].instructions = {
      "(call (mem:QI (symbol_ref:SI (\"abort\"))) (const_int 0))"};
  barrier_cfg.rpo_blocks[1].has_trailing_barrier = true;
  EXPECT_EQ(MostlyTrueJumpScore(barrier_cfg, /*block_b=*/0, beq_reg, /*target_b=*/2), 2);

  // 3. Negative case: MIPS `return_internal` `PARALLEL` fallthrough
  //    (`config/mips/mips.md:5934-5941`) -> `rare_fallthrough = 0`
  //    (`reorg.c:1311, 1317`), so diff is 0 and `EQ` scores 0.
  RtlControlFlowGraph mips_ret_cfg = falloff_cfg;
  mips_ret_cfg.rpo_blocks[1].instructions = {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"};
  EXPECT_EQ(MostlyTrueJumpScore(mips_ret_cfg, /*block_b=*/0, beq_reg, /*target_b=*/2), 0);
}

TEST(ReorgModelTest, PatternMayTrapForReorgMatchesRtlanalMayTrapP) {
  // Non-trapping patterns: integer ALU, FP register move, stack/symbol MEM.
  EXPECT_FALSE(PatternMayTrapForReorg(
      MustParse("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 4)))")));
  EXPECT_FALSE(PatternMayTrapForReorg(MustParse("(set (reg:SF 32 $f0) (reg:SF 34 $f2))")));
  EXPECT_FALSE(PatternMayTrapForReorg(
      MustParse("(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))")));
  EXPECT_FALSE(PatternMayTrapForReorg(
      MustParse("(set (reg:SI 2 v0) (mem:SI (symbol_ref:SI (\"g_val\"))))")));

  // Trapping patterns: general pointer MEM, div/mod, FP arithmetic, unspec_volatile.
  EXPECT_TRUE(PatternMayTrapForReorg(MustParse("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))")));
  EXPECT_TRUE(PatternMayTrapForReorg(
      MustParse("(set (reg:SI 2 v0) (div:SI (reg:SI 4 a0) (reg:SI 5 a1)))")));
  EXPECT_TRUE(PatternMayTrapForReorg(
      MustParse("(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2)))")));
  EXPECT_TRUE(PatternMayTrapForReorg(MustParse("(set (reg:DF 32 $f0) (sqrt:DF (reg:DF 34 $f2)))")));
  EXPECT_TRUE(PatternMayTrapForReorg(MustParse("(unspec_volatile [(const_int 0)] 0)")));
}

TEST(ReorgModelTest, RegDeadConstZeroAndUseClobberBlockHelpers) {
  auto zero_dead = ParsePatternAndNotes(
      "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (const_int 0) (nil))");
  ASSERT_TRUE(zero_dead.ok());
  EXPECT_TRUE(HasRegDeadConstZeroNote(*zero_dead));

  auto reg_dead = ParsePatternAndNotes(
      "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  ASSERT_TRUE(reg_dead.ok());
  EXPECT_FALSE(HasRegDeadConstZeroNote(*reg_dead));

  const RtlBasicBlock use_only =
      MakeBlock(0, false, {"(note NOTE_INSN_LOOP_END)", "(use (reg/i:SI 2 v0))"});
  EXPECT_TRUE(BlockHasOnlyUseOrClobberInsns(use_only, /*skip_notes=*/true));
  EXPECT_FALSE(BlockHasOnlyUseOrClobberInsns(use_only, /*skip_notes=*/false));
  EXPECT_TRUE(BlockStartsWithUseOrClobberInsn(use_only));

  const RtlBasicBlock mixed =
      MakeBlock(1, false, {"(use (reg/i:SI 2 v0))", "(set (reg:SI 3 v1) (const_int 1))"});
  EXPECT_FALSE(BlockHasOnlyUseOrClobberInsns(mixed));
  EXPECT_TRUE(BlockStartsWithUseOrClobberInsn(mixed));

  const RtlBasicBlock empty = MakeBlock(2, false, {});
  EXPECT_FALSE(BlockHasOnlyUseOrClobberInsns(empty));
  EXPECT_FALSE(BlockStartsWithUseOrClobberInsn(empty));
}

}  // namespace
}  // namespace rom_nom_nom::lifter
