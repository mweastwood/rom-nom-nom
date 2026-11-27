#include "lifter/post_reload_noop_moves.h"

#include <string>
#include <utility>
#include <vector>

#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;

RtlControlFlowGraph BuildLoopArgPreferencePostPassCfg() {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.sideband.is_leaf_function = true;
  cfg.sideband.basic_block_live_at_start = {
      {0, {4, 29, 31}}, {1, {3, 4, 29, 31}}, {2, {3, 4, 29, 31}}};
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (const_int 0))");
  cfg.rpo_blocks[0].successors = {1};

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].has_label = true;
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1)))");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (ne:SI (reg:SI 3 v1) (reg:SI 4 a0)) (label_ref L0) (pc)))");
  cfg.rpo_blocks[1].predecessors = {0, 1};
  cfg.rpo_blocks[1].successors = {1, 2};

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
  cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
  cfg.rpo_blocks[2].predecessors = {1};
  return cfg;
}

TEST(PostReloadNoopMovesTest, SeparatesJump2AndSched2StageRestorationAndMatchesForwardPasses) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-Wa,-g"}, {}};
  const CompilationProfile o1_prof{1, {"-O1", "-mips2", "-Wa,-g"}, {}};
  const std::string expected_arg_noop_move =
      "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))";

  // 1. Under `-O2` (`RunsSched2() == true`), `B0` has a trailing active `(set $3 0)` instruction,
  //    so the leading `(set $4 $4)` self-move is non-block-end: `RestoreJump2DeletedNoopMoves` must
  //    leave `B0` untouched, while `RestoreSched2DeletedNoopMoves` restores it (idempotently) and
  //    GCC `sched2` deletes it back.
  RtlControlFlowGraph o2_cfg = BuildLoopArgPreferencePostPassCfg();
  const RtlControlFlowGraph orig_o2_cfg = o2_cfg;
  RestoreJump2DeletedNoopMoves(&o2_cfg, o2_prof);
  EXPECT_EQ(o2_cfg.rpo_blocks[0].instructions.size(), 1u);

  RestoreSched2DeletedNoopMoves(&o2_cfg, o2_prof);
  ASSERT_EQ(o2_cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(o2_cfg.rpo_blocks[0].instructions[0], expected_arg_noop_move);
  RestoreSched2DeletedNoopMoves(&o2_cfg, o2_prof);
  EXPECT_EQ(o2_cfg.rpo_blocks[0].instructions.size(), 2u);

  const SubfunctionBoundarySnapshot sched2_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kScheduleBlockSched2, o2_cfg, {}, o2_prof.optimize_level, o2_prof.c_flags,
      o2_prof.gp_symbol_sizes);
  ASSERT_EQ(sched2_snap.post_state.rpo_blocks.size(), orig_o2_cfg.rpo_blocks.size());
  EXPECT_EQ(sched2_snap.post_state.rpo_blocks[0].instructions,
            orig_o2_cfg.rpo_blocks[0].instructions);

  // 2. Under `-O1` (`RunsSched2() == false`), `sched2` never ran, so `RestoreJump2DeletedNoopMoves`
  //    must restore `(set $4 $4)` directly at the `jump2` boundary, and GCC `jump2` deletes it.
  RtlControlFlowGraph o1_cfg = BuildLoopArgPreferencePostPassCfg();
  o1_cfg.pass = RtlPassKind::kJump2;
  RestoreJump2DeletedNoopMoves(&o1_cfg, o1_prof);
  ASSERT_EQ(o1_cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(o1_cfg.rpo_blocks[0].instructions[0], expected_arg_noop_move);

  const SubfunctionBoundarySnapshot jump2_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kDoCrossJump, o1_cfg, {}, o1_prof.optimize_level, o1_prof.c_flags,
      o1_prof.gp_symbol_sizes);
  EXPECT_EQ(jump2_snap.post_state.rpo_blocks[0].instructions.size(), 1u);
}

TEST(PostReloadNoopMovesTest, RestoresPreScheduleNonEntryCallArgSelfMovesAndMatchesSched2) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-Wa,-g"}, {}};

  // Build a 2-block non-leaf CFG where `$4 a0` is computed in `B0` (non-entry-unmodified), read in
  // `B1` before a `CALL_INSN`, and passed in `CALL_INSN_FUNCTION_USAGE` without being rewritten in
  // `B1` prior to the call.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.sideband.is_leaf_function = false;
  cfg.sideband.saved_gpr_regnos = {31};
  cfg.sideband.frame_size_bytes = 24;
  cfg.sideband.outgoing_args_size = 16;
  cfg.sideband.basic_block_live_at_start[0] = {5, 29, 31};
  cfg.sideband.basic_block_live_at_start[1] = {4, 29, 31};

  RtlBasicBlock b0;
  b0.id = 0;
  b0.AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  b0.successors = {1};
  cfg.rpo_blocks.push_back(std::move(b0));

  RtlBasicBlock b1;
  b1.id = 1;
  b1.has_label = true;
  b1.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 4 a0))");
  b1.AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI \"Callee\")) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] ) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)) "
      "(expr_list (use (reg:SI 4 a0)) (nil))");
  b1.AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 2 v0))");
  b1.AddPatternOnly("(use (reg/i:SI 2 v0))");
  b1.predecessors = {0};
  cfg.rpo_blocks.push_back(std::move(b1));

  RestoreSched2PreScheduleDeletedNoopMoves(&cfg, o2_prof);
  ASSERT_EQ(cfg.rpo_blocks[1].instructions.size(), 5u);
  EXPECT_EQ(cfg.rpo_blocks[1].instructions[1],
            "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const SubfunctionBoundarySnapshot sched2_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kScheduleBlockSched2, cfg, {}, o2_prof.optimize_level, o2_prof.c_flags,
      o2_prof.gp_symbol_sizes);
  bool has_self_move_after_sched2 = false;
  for (const std::string& insn_s : sched2_snap.post_state.rpo_blocks[1].instructions) {
    if (insn_s.find("(set (reg:SI 4 a0) (reg:SI 4 a0))") != std::string::npos) {
      has_self_move_after_sched2 = true;
    }
  }
  EXPECT_FALSE(has_self_move_after_sched2) << sched2_snap.post_state.FormatSummary();
}

TEST(PostReloadNoopMovesTest,
     RestoresMutatedLoopArgSelfMoveCaseGAndGuardedAgainstHigherLocalAllocReg) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  const CompilationProfile no_s1_prof{
      2, {"-O2", "-fno-schedule-insns", "-mips2", "-mcpu=r4000"}, {}};

  // 1. Positive Case G (`function.c:3629-3681` + `sched.c:2469-2501`): multi-block loop where
  //    `$4 a0` is RMW-mutated (`a0 += 4`) while the only block-local register in `B1` is
  //    `$2 v0` (lower in `REG_ALLOC_ORDER` than `$4 a0`).
  RtlControlFlowGraph safe_cfg;
  safe_cfg.pass = RtlPassKind::kSched2;
  safe_cfg.sideband.is_leaf_function = true;
  safe_cfg.sideband.basic_block_live_at_start = {
      {0, {4, 5, 29, 31}}, {1, {3, 4, 5, 29, 31}}, {2, {3, 29, 31}}};
  safe_cfg.rpo_blocks.resize(3);
  safe_cfg.rpo_blocks[0].id = 0;
  safe_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (const_int 0))");
  safe_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (le:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");
  safe_cfg.rpo_blocks[0].successors = {2, 1};

  safe_cfg.rpo_blocks[1].id = 1;
  safe_cfg.rpo_blocks[1].has_label = true;
  safe_cfg.rpo_blocks[1].predecessors = {0, 1};
  safe_cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
  safe_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 4)))");
  safe_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  safe_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int -1)))");
  safe_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (gt:SI (reg:SI 5 a1) (const_int 0)) (label_ref L0) (pc)))");
  safe_cfg.rpo_blocks[1].successors = {1, 2};

  safe_cfg.rpo_blocks[2].id = 2;
  safe_cfg.rpo_blocks[2].has_label = true;
  safe_cfg.rpo_blocks[2].predecessors = {0, 1};
  safe_cfg.rpo_blocks[2].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (reg:SI 3 v1)) (expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  safe_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  EXPECT_EQ(PredictRestoredEntryArgSelfMoveRegs(safe_cfg, o2_prof), (std::vector<int>{4, 5}));
  EXPECT_TRUE(PredictRestoredEntryArgSelfMoveRegs(safe_cfg, no_s1_prof).empty());

  RtlControlFlowGraph restored = safe_cfg;
  RestoreSched2DeletedNoopMoves(&restored, o2_prof);
  ASSERT_EQ(restored.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(restored.rpo_blocks[0].instructions[0],
            "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  EXPECT_EQ(restored.rpo_blocks[0].instructions[1],
            "(set (reg:SI 5 a1) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");

  const SubfunctionBoundarySnapshot sched2_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kScheduleBlockSched2, restored, {}, o2_prof.optimize_level, o2_prof.c_flags,
      o2_prof.gp_symbol_sizes);
  EXPECT_EQ(sched2_snap.post_state.rpo_blocks[0].instructions, safe_cfg.rpo_blocks[0].instructions);

  // 2. Negative `local-alloc.c:2158-2210` guard: a block-local pseudo in `B1` uses `$8 t0`
  //    (higher in `REG_ALLOC_ORDER` than `$4 a0` / `$5 a1`) while `$4 a0` and `$5 a1` are live,
  //    so Case G must NOT restore entry self-moves that would free `$4`/`$5` during `local-alloc`.
  RtlControlFlowGraph guarded_cfg = safe_cfg;
  guarded_cfg.rpo_blocks[1].instructions[0] = "(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))";
  guarded_cfg.rpo_blocks[1].instructions[2] =
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))";
  guarded_cfg.rpo_blocks[1].SyncRtlInsnsFromInstructions();
  EXPECT_TRUE(PredictRestoredEntryArgSelfMoveRegs(guarded_cfg, o2_prof).empty());
  RestoreSched2DeletedNoopMoves(&guarded_cfg, o2_prof);
  EXPECT_EQ(guarded_cfg.rpo_blocks[0].instructions.size(), 2u);
}

TEST(PostReloadNoopMovesTest, CaseESingleBlockDiPairSelfMoveOmitsSelfDeadNote) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  // Single-block `!gp64` function where `a0`/`a1` are read strictly in `SImode`,
  // `a1` dies early on an instruction defining `$8 t0` (later in
  // `REG_ALLOC_ORDER` than `a1`), and `a0` dies later: Case E restores
  // `(set (reg:DI 4 a0) (reg:DI 4 a0))` WITHOUT self-dead `REG_DEAD`
  // (`flow.c:2920` / `sched.c:2872`), since `InvertAlterReg` keeps it as a
  // hard-reg self-set.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 29, 31}}};
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 8 t0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  RtlControlFlowGraph restored = cfg;
  RestoreSched2DeletedNoopMoves(&restored, o2_prof);
  ASSERT_EQ(restored.rpo_blocks[0].instructions.size(), cfg.rpo_blocks[0].instructions.size() + 1);
  EXPECT_EQ(restored.rpo_blocks[0].instructions[0], "(set (reg:DI 4 a0) (reg:DI 4 a0))");

  // Re-running restoration must be strictly idempotent even without `REG_DEAD`.
  RtlControlFlowGraph restored_twice = restored;
  RestoreSched2DeletedNoopMoves(&restored_twice, o2_prof);
  EXPECT_EQ(restored_twice.rpo_blocks[0].instructions, restored.rpo_blocks[0].instructions);
}

TEST(PostReloadNoopMovesTest, RestoreSched2PreScheduleCallReturnNoopMoves) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // 1. Positive Pattern 1 (`calls.c:479`, `sched.c:4803-4821`): post-call RMW
  //    chain in `$v0` ending in `(set (reg/i:SI 2 v0) ...)` with `REG_DEAD $v0`
  //    restores `(set (reg:SI 2 v0) (reg:SI 2 v0)) (REG_DEAD (reg:SI 2 v0))`
  //    immediately after the call, and `sched2` deletes it cleanly on replay.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start = {{0, {16, 17, 29, 31}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"helper\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (minus:SI (reg:SI 2 v0) (reg:SI 17 s1))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 17 s1) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

    RtlControlFlowGraph restored = cfg;
    RestoreSched2PreScheduleDeletedNoopMoves(&restored, o2_prof);
    ASSERT_EQ(restored.rpo_blocks[0].instructions.size(), 5u);
    EXPECT_EQ(restored.rpo_blocks[0].instructions[1],
              "(set (reg:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))");

    const SubfunctionBoundarySnapshot sched2_snap = hooks.ExecuteSubfunctionBoundary(
        SubfunctionId::kScheduleBlockSched2, restored, {}, o2_prof.optimize_level, o2_prof.c_flags,
        o2_prof.gp_symbol_sizes);
    bool has_self_move = false;
    for (const std::string& insn_s : sched2_snap.post_state.rpo_blocks[0].instructions) {
      if (insn_s.find("(set (reg:SI 2 v0) (reg:SI 2 v0))") != std::string::npos) {
        has_self_move = true;
      }
    }
    EXPECT_FALSE(has_self_move) << sched2_snap.post_state.FormatSummary();
  }

  // 2. Positive Pattern 2 (`optabs.c:2554`, `mips.h:2946`): post-libcall `DI`
  //    `"=&d"` earlyclobber output reload into `(reg/i:DI 2 v0)` restores
  //    `(set (reg:DI 2 v0) (reg:DI 2 v0)) (REG_DEAD (reg:DI 2 v0))` after an
  //    independent non-memory `ashift:DI` that `sched1` schedules ahead of the
  //    1-cost `REG_DEP_TRUE` return copy.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start = {{0, {16, 17, 29, 31}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:DI 2 v0) (call (mem:SI (symbol_ref:SI (\"__moddi3\"))) "
        "(const_int 0))) (clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:DI 16 s0) (ashift:DI (reg:DI 16 s0) (const_int 2))) "
        "(clobber (reg:SI 4 a0)) ] ) (expr_list:REG_UNUSED (reg:SI 4 a0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:DI 8 t0) (plus:DI (reg:DI 16 s0) (reg:DI 2 v0))) "
        "(clobber (reg:SI 4 a0)) ] ) (expr_list:REG_DEAD (reg:DI 16 s0) "
        "(expr_list:REG_DEAD (reg:DI 2 v0) (expr_list:REG_UNUSED (reg:SI 4 a0) (nil))))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:DI 2 v0) (reg:DI 8 t0))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:DI 2 v0))");

    RtlControlFlowGraph restored = cfg;
    RestoreSched2PreScheduleDeletedNoopMoves(&restored, o2_prof);
    ASSERT_EQ(restored.rpo_blocks[0].instructions.size(), 6u);
    EXPECT_NE(restored.rpo_blocks[0].instructions[1].find("(ashift:DI"), std::string::npos);
    EXPECT_EQ(restored.rpo_blocks[0].instructions[2],
              "(set (reg:DI 2 v0) (reg:DI 2 v0)) (expr_list:REG_DEAD (reg:DI 2 v0) (nil))");
  }

  // 3. Negative: a plain post-call copy into a callee-saved register does NOT
  //    trigger synthetic return self-move insertion.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start = {{0, {29, 31}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"helper\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 16 s0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (reg:SI 16 s0)) (expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

    RtlControlFlowGraph restored = cfg;
    RestoreSched2PreScheduleDeletedNoopMoves(&restored, o2_prof);
    EXPECT_EQ(restored.rpo_blocks[0].instructions, cfg.rpo_blocks[0].instructions);
  }
}

TEST(PostReloadNoopMovesTest, RestoresNoConflictSignExtendRetvalAndEntrySelfMove) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};
  hooks.ApplyCompilationProfile(o2_prof);

  auto make_no_conflict_cfg = []() {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.basic_block_live_at_start = {{0, {4, 6, 29, 31}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
    cfg.rpo_blocks[0].AddPatternOnly("(clobber (reg:DI 4 a0)) (insn_list:REG_LIBCALL (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (subreg:SI (reg:DI 4 a0) 1) (reg:SI 4 a0)) "
        "(expr_list:REG_NO_CONFLICT (reg:SI 4 a0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (subreg:SI (reg:DI 4 a0) 0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_NO_CONFLICT (reg:SI 4 a0) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:DI (reg:SI 6 a2)) (reg:DI 4 a0)) "
        "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_DEAD (reg:DI 4 a0) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive (`sched.c:4803-4818`, `optabs.c:2507-2523`): restores both the
  // block-0 incoming ABI arg self-move `(set (reg:SI 4 a0) (reg:SI 4 a0))` and
  // the closing `REG_RETVAL` + `REG_EQUAL (sign_extend:DI (reg:SI 4 a0))`
  // self-move, which `sched2` cleanly deletes on replay.
  {
    RtlControlFlowGraph restored = make_no_conflict_cfg();
    RestoreSched2PreScheduleDeletedNoopMoves(&restored, o2_prof);
    ASSERT_EQ(restored.rpo_blocks[0].instructions.size(), 9u) << restored.FormatSummary();
    EXPECT_EQ(restored.rpo_blocks[0].instructions[0],
              "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    EXPECT_EQ(restored.rpo_blocks[0].instructions[5],
              "(set (reg:DI 4 a0) (reg:DI 4 a0)) "
              "(insn_list:REG_RETVAL "
              "(expr_list:REG_EQUAL (sign_extend:DI (reg:SI 4 a0)) (nil)))");

    const SubfunctionBoundarySnapshot sched2_snap = hooks.ExecuteSubfunctionBoundary(
        SubfunctionId::kScheduleBlockSched2, restored, {}, o2_prof.optimize_level, o2_prof.c_flags,
        o2_prof.gp_symbol_sizes);
    bool has_retval_self_move = false;
    for (const std::string& insn_s : sched2_snap.post_state.rpo_blocks[0].instructions) {
      if (insn_s.find("(set (reg:DI 4 a0) (reg:DI 4 a0))") != std::string::npos) {
        has_retval_self_move = true;
      }
    }
    EXPECT_FALSE(has_retval_self_move) << sched2_snap.post_state.FormatSummary();
  }

  // Negative 1: idempotent when `REG_RETVAL` is already present.
  {
    RtlControlFlowGraph cfg = make_no_conflict_cfg();
    RestoreSched2PreScheduleDeletedNoopMoves(&cfg, o2_prof);
    const std::vector<std::string> once = cfg.rpo_blocks[0].instructions;
    RestoreSched2PreScheduleDeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, once);
  }

  // Negative 2: `-mgp64` skips 32-bit `DImode` subword no-conflict restoration.
  {
    const CompilationProfile gp64_prof{.optimize_level = 2,
                                       .c_flags = {"-O2", "-mips3", "-mgp64", "-Wa,-g"}};
    RtlControlFlowGraph cfg = make_no_conflict_cfg();
    const std::vector<std::string> orig = cfg.rpo_blocks[0].instructions;
    RestoreSched2PreScheduleDeletedNoopMoves(&cfg, gp64_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, orig);
  }
}

TEST(PostReloadNoopMovesTest, RestoresDeletedNoopEarlyReturnValueMovesBeforeConditionalJump) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1", "-Wa,-O2"}};
  const CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips1", "-Wa,-O2"}};

  auto make_early_ret_cfg = [](bool other_reads_ret_hr, bool jump_reads_ret_hr) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {2, 1};
    if (jump_reads_ret_hr) {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 1)))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref L2) (pc)))");
      cfg.rpo_blocks[1].id = 1;
      cfg.rpo_blocks[1].successors = {2};
      cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 3 v1))");
      cfg.rpo_blocks[2].id = 2;
      cfg.rpo_blocks[2].has_label = true;
      cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
      cfg.sideband.basic_block_live_at_start = {{0, {4}}, {1, {3}}, {2, {2}}};
    } else {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 32 $f0) (mem:SF (reg:SI 4 a0)))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:CC_FP 67 $fcr31) (eq:CC_FP (reg:SF 32 $f0) (reg:SF 44 $f12)))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (pc) (if_then_else (eq:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0)) "
          "(label_ref L2) (pc)))");
      cfg.rpo_blocks[1].id = 1;
      cfg.rpo_blocks[1].successors = {2};
      if (other_reads_ret_hr) {
        cfg.rpo_blocks[1].AddPatternOnly(
            "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 44 $f12)))");
        cfg.sideband.basic_block_live_at_start = {{0, {4, 44}}, {1, {32, 44}}, {2, {32}}};
      } else {
        cfg.rpo_blocks[1].AddPatternOnly(
            "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))");
        cfg.sideband.basic_block_live_at_start = {{0, {4, 44, 46}}, {1, {44, 46}}, {2, {32}}};
      }
      cfg.rpo_blocks[2].id = 2;
      cfg.rpo_blocks[2].has_label = true;
      cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SF 32 $f0))");
    }
    return cfg;
  };

  // Positive (`sched.c:4803-4818`, `jump.c:308, 427-433`): restores
  // `(set (reg/i:SF 32 $f0) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))`
  // immediately before the conditional jump in both `sched2` (`-O2`) and
  // `!RunsSched2()` (`-O1`) modes, and is idempotent.
  {
    RtlControlFlowGraph cfg =
        make_early_ret_cfg(/*other_reads_ret_hr=*/false, /*jump_reads_ret_hr=*/false);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[2],
              "(set (reg/i:SF 32 $f0) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))");
    const std::vector<std::string> once = cfg.rpo_blocks[0].instructions;
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, once);
  }
  {
    RtlControlFlowGraph cfg =
        make_early_ret_cfg(/*other_reads_ret_hr=*/false, /*jump_reads_ret_hr=*/false);
    RestoreJump2DeletedNoopMoves(&cfg, o1_prof);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[2],
              "(set (reg/i:SF 32 $f0) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))");
  }

  // Negative 1: when `$f0` is ALSO live-in on the non-exit successor `B1`,
  // no early-return noop self-move is restored.
  {
    RtlControlFlowGraph cfg =
        make_early_ret_cfg(/*other_reads_ret_hr=*/true, /*jump_reads_ret_hr=*/false);
    const std::vector<std::string> orig = cfg.rpo_blocks[0].instructions;
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, orig);
  }

  // Negative 2: when the conditional jump itself reads `ret_hr` (`$v0`), no
  // early-return noop self-move is restored.
  {
    RtlControlFlowGraph cfg =
        make_early_ret_cfg(/*other_reads_ret_hr=*/false, /*jump_reads_ret_hr=*/true);
    const std::vector<std::string> orig = cfg.rpo_blocks[0].instructions;
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, orig);
  }
}

TEST(PostReloadNoopMovesTest, RestoresO1ZeroExtendDiNoConflictRetvalNoopMove) {
  // Mirrors `jump.c:delete_noop_moves` + `optabs.c:2485-2525`: at `-O1`
  // (`!RunsSched2()`), `RestoreJump2DeletedNoopMoves` restores the deleted
  // `(set (reg:DI dh) (reg:DI dh)) (REG_RETVAL) (REG_EQUAL (zero_extend:DI src))`
  // after `(clobber (reg:DI dh)) (REG_LIBCALL)`, `(set (subreg dh 1) src)`,
  // `(set (subreg dh 0) (const_int 0))`, but does NOT fire for non-zero high word.
  const CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips2", "-Wa,-g"}};
  auto make_cfg = [](const std::string& hi_rhs) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].instructions = {
        "(clobber (reg:DI 2 v0)) (insn_list:REG_LIBCALL (nil))",
        "(set (subreg:SI (reg:DI 2 v0) 1) (reg:SI 2 v0)) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_NO_CONFLICT (reg:SI 2 v0) (nil)))",
        "(set (subreg:SI (reg:DI 2 v0) 0) " + hi_rhs +
            ") (expr_list:REG_NO_CONFLICT (reg:SI 2 v0) (nil))",
        "(use (reg/i:DI 2 v0))",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return cfg;
  };

  {
    RtlControlFlowGraph pos = make_cfg("(const_int 0)");
    RestoreJump2DeletedNoopMoves(&pos, o1_prof);
    ASSERT_EQ(pos.rpo_blocks[0].instructions.size(), 5u) << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[3].find("REG_RETVAL"), std::string::npos)
        << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[3].find("(zero_extend:DI (reg:SI 2 v0))"),
              std::string::npos)
        << pos.FormatSummary();
  }
  {
    RtlControlFlowGraph neg = make_cfg("(const_int 1)");
    RestoreJump2DeletedNoopMoves(&neg, o1_prof);
    EXPECT_EQ(neg.rpo_blocks[0].instructions.size(), 4u) << neg.FormatSummary();
  }
}

TEST(PostReloadNoopMovesTest, CaseBSequentialLoopPreheaderArgCopyPositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};

  auto make_case_b_cfg = [](bool add_multicycle_load_in_preheader, bool arg_live_after_loop) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.is_leaf_function = true;
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 5, 29, 31}},    {1, {4, 5, 8, 29, 31}}, {2, {4, 5, 8, 29, 31}},
        {3, {4, 5, 8, 29, 31}}, {4, {2, 29, 31}},
    };
    if (arg_live_after_loop) {
      cfg.sideband.basic_block_live_at_start[4].insert(4);
    }
    cfg.rpo_blocks.resize(5);

    // B0: initializes `$8 t0` (higher caller reg witness), falls into earlier loop `B1`.
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (const_int 0))");
    cfg.rpo_blocks[0].successors = {1};

    // B1 (earlier loop): `$4 a0` is live-in & unmodified, `$2 v0` (`pref_hr`) is completely
    // unused/free, while higher caller reg `$8 t0` is live-in & written.
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].has_label = true;
    cfg.rpo_blocks[1].predecessors = {0, 1};
    cfg.rpo_blocks[1].successors = {1, 2};
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 8 t0) (plus:SI (reg:SI 8 t0) (const_int 1)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (pc) (if_then_else (lt:SI (reg:SI 8 t0) (reg:SI 5 a1)) (label_ref L0) (pc)))");

    // B2 (unique non-loop preheader of writer loop `B3`).
    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].predecessors = {1};
    cfg.rpo_blocks[2].successors = {3};
    if (add_multicycle_load_in_preheader) {
      cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 3 v1) (mem:SI (reg:SI 5 a1)))");
    } else {
      cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 3 v1) (const_int 0))");
    }

    // B3 (writer loop): modifies `$4 a0` and sets `pref_hr = $2 v0` from `$4 a0`.
    cfg.rpo_blocks[3].id = 3;
    cfg.rpo_blocks[3].has_label = true;
    cfg.rpo_blocks[3].predecessors = {2, 3};
    cfg.rpo_blocks[3].successors = {3, 4};
    cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 4)))");
    cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
    cfg.rpo_blocks[3].AddPatternOnly(
        "(set (pc) (if_then_else (lt:SI (reg:SI 4 a0) (reg:SI 8 t0)) (label_ref L1) (pc)))");

    // B4 (exit).
    cfg.rpo_blocks[4].id = 4;
    cfg.rpo_blocks[4].predecessors = {3};
    cfg.rpo_blocks[4].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 2 v0))");
    cfg.rpo_blocks[4].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: restores `(set (reg:SI 4 a0) (reg:SI 4 a0))` in BOTH `B0` and preheader `B2`.
  {
    RtlControlFlowGraph cfg =
        make_case_b_cfg(/*add_multicycle_load_in_preheader=*/false, /*arg_live_after_loop=*/false);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 2u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[0],
              "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    ASSERT_EQ(cfg.rpo_blocks[2].instructions.size(), 2u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[2].instructions[0],
              "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  }

  // Negative 1: multi-cycle load in `B2` ahead of `$4 a0` suppresses Case B under `sched2`.
  {
    RtlControlFlowGraph cfg =
        make_case_b_cfg(/*add_multicycle_load_in_preheader=*/true, /*arg_live_after_loop=*/false);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 1u) << cfg.FormatSummary();
  }

  // Negative 2: when `$4 a0` remains live-out to `B4` after the writer loop, Case B does not fire.
  {
    RtlControlFlowGraph cfg =
        make_case_b_cfg(/*add_multicycle_load_in_preheader=*/false, /*arg_live_after_loop=*/true);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[2].instructions.size(), 1u) << cfg.FormatSummary();
  }
}

TEST(PostReloadNoopMovesTest, CaseCMultiBlockLocalAllocArgSuggPositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};

  auto make_case_c_cfg = [](bool insert_call_before_dst_death) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.is_leaf_function = !insert_call_before_dst_death;
    cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 29, 31}}, {1, {4, 29, 31}}, {2, {29, 31}}};
    cfg.rpo_blocks.resize(3);

    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {2, 1};
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc))) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");

    // B1: `$4 a0` dies on `(set $8 t0 (plus $4 a0 7))`, then `$8 t0` dies locally at `j`.
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].predecessors = {0};
    cfg.rpo_blocks[1].successors = {2};
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 7))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    if (insert_call_before_dst_death) {
      cfg.rpo_blocks[1].AddPatternOnly(
          "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 0)) "
          "(clobber (reg:SI 31 ra)) ] )");
    }
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (mem:SI (symbol_ref:SI (\"g_out\"))) (reg:SI 8 t0)) "
        "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].has_label = true;
    cfg.rpo_blocks[2].predecessors = {0, 1};
    cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: Case C restores `(set (reg:SI 4 a0) (reg:SI 4 a0))` at `B0`.
  {
    RtlControlFlowGraph cfg = make_case_c_cfg(/*insert_call_before_dst_death=*/false);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 2u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[0],
              "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  }

  // Negative: intervening call between `i` and `dst_hr` death blocks Case C.
  {
    RtlControlFlowGraph cfg = make_case_c_cfg(/*insert_call_before_dst_death=*/true);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 1u) << cfg.FormatSummary();
  }
}

TEST(PostReloadNoopMovesTest, CaseDSingleBlockEntryCopySuggPositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};

  auto make_case_d_cfg = [](bool use_higher_early_reg) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.is_leaf_function = true;
    cfg.sideband.basic_block_live_at_start = {{0, {4, 29, 31}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    const std::string early = use_higher_early_reg ? "(reg:SI 8 t0)" : "(reg:SI 2 v0)";
    const std::string early_i = use_higher_early_reg ? "(reg/i:SI 8 t0)" : "(reg/i:SI 2 v0)";
    cfg.rpo_blocks[0].AddPatternOnly("(set " + early + " (plus:SI (reg:SI 4 a0) (const_int 1)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 4)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set " + early_i + " (plus:SI " + early +
                                     " (reg:SI 4 a0))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: `early_hr = $2 v0` (`< $4 a0` in natural register order) + `$4 a0` RMW + final
  // integrated combine triggers Case D entry self-move restoration.
  {
    RtlControlFlowGraph cfg = make_case_d_cfg(/*use_higher_early_reg=*/false);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 5u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[0],
              "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  }

  // Negative: when `early_hr = $8 t0` (`> $4 a0` in natural register order), Case D does not fire.
  {
    RtlControlFlowGraph cfg = make_case_d_cfg(/*use_higher_early_reg=*/true);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u) << cfg.FormatSummary();
  }
}

TEST(PostReloadNoopMovesTest, CaseFSingleBlockEntryFirstInsnMovstrsiPositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};

  auto make_case_f_cfg = [](bool read_arg_after_movstrsi) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.is_leaf_function = true;
    cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 29, 31}}};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
        "(clobber (reg:SI 8 t0)) (clobber (reg:SI 9 t1)) (clobber (reg:SI 10 t2)) "
        "(clobber (reg:SI 11 t3)) (use (const_int 16)) (use (const_int 4)) (use (const_int 0)) ] ) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    if (read_arg_after_movstrsi) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg/i:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: first-insn `movstrsi_internal` kills sibling live-in `$5 a1` while `$4 a0` survives
  // to a later read -> Case F restores `(set (reg:SI 4 a0) (reg:SI 4 a0))`.
  {
    RtlControlFlowGraph cfg = make_case_f_cfg(/*read_arg_after_movstrsi=*/true);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[0],
              "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  }

  // Negative: when `$4 a0` is never read again after `movstrsi_internal`, Case F does not fire.
  {
    RtlControlFlowGraph cfg = make_case_f_cfg(/*read_arg_after_movstrsi=*/false);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 3u) << cfg.FormatSummary();
  }
}

TEST(PostReloadNoopMovesTest, RestoresDeletedNoopSharedReturnMovePositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-Wa,-g"}};

  auto make_shared_ret_cfg = [](bool pred_already_integrated) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.is_leaf_function = true;
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 5, 29, 31}}, {1, {3, 4, 29, 31}}, {2, {3, 5, 29, 31}}, {3, {2, 29, 31}}};
    cfg.rpo_blocks.resize(4);

    // B0 defines multi-block witness `$3 v1` (disjoint from `$2 v0`) and branches to `B1`/`B2`.
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {2, 1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (const_int 10))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))");

    // B1 and B2 are both single-successor predecessors of `B3` whose last active insn sets `$2 v0`.
    const std::string ret_dst = pred_already_integrated ? "(reg/i:SI 2 v0)" : "(reg:SI 2 v0)";
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].predecessors = {0};
    cfg.rpo_blocks[1].successors = {3};
    cfg.rpo_blocks[1].AddPatternOnly("(set " + ret_dst + " (plus:SI (reg:SI 3 v1) (reg:SI 4 a0)))");
    cfg.rpo_blocks[1].AddPatternOnly("(set (pc) (label_ref L2))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].has_label = true;
    cfg.rpo_blocks[2].predecessors = {0};
    cfg.rpo_blocks[2].successors = {3};
    cfg.rpo_blocks[2].AddPatternOnly("(set " + ret_dst + " (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))");

    // B3: shared exit block containing only `(use (reg/i:SI 2 v0))`.
    cfg.rpo_blocks[3].id = 3;
    cfg.rpo_blocks[3].has_label = true;
    cfg.rpo_blocks[3].predecessors = {1, 2};
    cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: restores `(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (REG_DEAD (reg:SI 2 v0))` in `B3`.
  {
    RtlControlFlowGraph cfg = make_shared_ret_cfg(/*pred_already_integrated=*/false);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    ASSERT_EQ(cfg.rpo_blocks[3].instructions.size(), 2u) << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[3].instructions[0],
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  }

  // Negative: when predecessors already write `(reg/i:SI 2 v0)`, no shared return move is added.
  {
    RtlControlFlowGraph cfg = make_shared_ret_cfg(/*pred_already_integrated=*/true);
    RestoreSched2DeletedNoopMoves(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[3].instructions.size(), 1u) << cfg.FormatSummary();
  }
}

}  // namespace
}  // namespace rom_nom_nom::lifter
