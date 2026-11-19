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

}  // namespace
}  // namespace rom_nom_nom::lifter
