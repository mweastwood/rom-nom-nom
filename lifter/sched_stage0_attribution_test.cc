#include "lifter/sched_stage0_attribution.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gtest/gtest.h"
#include "lifter/rtl_liveness.h"
#include "lifter/sched_model.h"
#include "lifter/sched_post_reload_projection.h"

namespace rom_nom_nom::lifter::sched_model {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;

Insn MakeInsn(std::string_view sexpr, int luid, Insn::Kind kind = Insn::Kind::kInsn,
              std::vector<std::string_view> call_fusage_strs = {}, bool setjmp_follows = false,
              std::vector<RtlLoopNoteKind> leading_loop_notes = {}) {
  Insn insn;
  auto parsed = ParsePatternAndNotes(sexpr);
  EXPECT_TRUE(parsed.ok()) << sexpr;
  if (parsed.ok()) {
    insn.pn = std::move(*parsed);
  }
  for (std::string_view fu : call_fusage_strs) {
    auto fe = RtlExpr::Parse(fu);
    EXPECT_TRUE(fe.ok()) << fu;
    if (fe.ok()) {
      insn.pn.has_explicit_call_fusage = true;
      insn.pn.call_fusage.push_back(std::move(*fe));
    }
  }
  insn.luid = luid;
  insn.kind = kind;
  insn.setjmp_follows = setjmp_follows;
  insn.leading_loop_notes = std::move(leading_loop_notes);
  return insn;
}

TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesTransfersStaleDeathUnderO2Only) {
  auto build_cfg = []() {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 4 a0) (ashift:SI (reg:SI 4 a0) (const_int 2))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg/i:SI 2 v0) (mem/s:SI (plus:SI (reg:SI 4 a0) "
                "(symbol_ref:SI (\"g_slot_ids\")))))",
                "(set (mem/s:SI (plus:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_slot_states\")))) "
                "(reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                "(set (mem/s:SI (plus:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_slot_timers\")))) "
                "(const_int 0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(use (reg/i:SI 2 v0))",
                "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
            },
    });
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return cfg;
  };

  // Under `-O2` (`RunsSched1 && RunsSched2`), `sched2` hoisted the load
  // `[a0+g_slot_ids]` ahead of both stores; `REG_DEAD a0` must transfer from
  // insn 3 (`g_slot_timers` store) onto insn 1 (`g_slot_ids` load).
  RtlControlFlowGraph o2_cfg = build_cfg();
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  AttributeSched2PureReaderRegDeadNotes(&o2_cfg, o2_prof);
  EXPECT_NE(o2_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 4 a0)"), std::string::npos)
      << o2_cfg.FormatSummary();
  EXPECT_EQ(o2_cfg.rpo_blocks[0].instructions[3].find("REG_DEAD (reg:SI 4 a0)"), std::string::npos)
      << o2_cfg.FormatSummary();

  // Negative check: under `-O1` (neither `sched1` nor `sched2`), CFG is untouched.
  RtlControlFlowGraph o1_cfg = build_cfg();
  const CompilationProfile o1_prof{1, {"-O1", "-mips2", "-mcpu=r4000"}, {}};
  AttributeSched2PureReaderRegDeadNotes(&o1_cfg, o1_prof);
  EXPECT_EQ(o1_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD"), std::string::npos);
  EXPECT_NE(o1_cfg.rpo_blocks[0].instructions[3].find("REG_DEAD (reg:SI 4 a0)"), std::string::npos);

  // Negative check (`flow.c:2504-2515`, `sched.c:3415-3611`, `function.c:5516`):
  // when an earlier pure reader (`i = 1`) ALREADY carries `REG_DEAD a0` ahead of
  // a later reader (`m = 3`, as from Stage 0 prologue-hoist liveness recovery),
  // a candidate that preserves `i` ahead of `m` must NOT transfer `REG_DEAD`
  // forward from `i` to `m` (`last_reader >= i`).
  RtlControlFlowGraph already_inv_cfg = o2_cfg;
  AttributeSched2PureReaderRegDeadNotes(&already_inv_cfg, o2_prof);
  EXPECT_NE(already_inv_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 4 a0)"),
            std::string::npos)
      << already_inv_cfg.FormatSummary();
  EXPECT_EQ(already_inv_cfg.rpo_blocks[0].instructions[3].find("REG_DEAD (reg:SI 4 a0)"),
            std::string::npos)
      << already_inv_cfg.FormatSummary();
}
TEST(SchedStage0AttributionTest,
     PreservesPostReloadOrderSafetyAllowsMatchingRegEquivRestorationAndCallSeqs) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  const MachineModel mm = MachineModel::FromProfile(prof);
  RegFacts empty_facts;
  empty_facts.max_reg_num = 72;

  // In `base_with_equiv`, insn 0 already has `REG_EQUIV (mem/u (reg a0))` from
  // `local-alloc.c:1049-1055`, but `sched2` hoisted insn 1 (`REG_DEAD a0`) ahead of
  // insn 2 (`REG_DEAD s4`), making `validate_equiv_mem(base) == false`. Moving insn 1
  // after insn 2 (`cand = {0, 2, 1, 3}`) restores `validate_equiv_mem == true`.
  auto build_equiv_block = [](bool attach_reg_equiv) {
    return std::vector<Insn>{
        MakeInsn(attach_reg_equiv ? "(set (reg:SI 20 s4) (mem/u:SI (reg:SI 4 a0))) "
                                    "(expr_list:REG_EQUIV (mem/u:SI (reg:SI 4 a0)) (nil))"
                                  : "(set (reg:SI 20 s4) (mem/u:SI (reg:SI 4 a0)))",
                 0),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 8))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 1),
        MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 20 s4) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 20 s4) (nil))",
                 2),
        MakeInsn("(use (reg/i:SI 2 v0))", 3),
    };
  };
  EXPECT_TRUE(PreservesPostReloadOrderSafety(
      build_equiv_block(/*attach_reg_equiv=*/true), /*cand_in_base=*/{0, 2, 1, 3},
      /*prologue_end=*/0, /*epilogue_begin=*/4, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{},
      empty_facts, mm, /*allow_pure_reader_reg_dead_transfer=*/false));
  // Without an existing matching `REG_EQUIV` note, `0 -> 1` must remain rejected.
  EXPECT_FALSE(PreservesPostReloadOrderSafety(
      build_equiv_block(/*attach_reg_equiv=*/false), /*cand_in_base=*/{0, 2, 1, 3},
      /*prologue_end=*/0, /*epilogue_begin=*/4, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{},
      empty_facts, mm, /*allow_pure_reader_reg_dead_transfer=*/false));
  // Stage 0 discovery (`allow_pure_reader_reg_dead_transfer = true`) defers `mem_equiv_valid`
  // until after the pure-reader `REG_DEAD` transfer is applied.
  EXPECT_TRUE(PreservesPostReloadOrderSafety(
      build_equiv_block(/*attach_reg_equiv=*/false), /*cand_in_base=*/{0, 2, 1, 3},
      /*prologue_end=*/0, /*epilogue_begin=*/4, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{},
      empty_facts, mm, /*allow_pure_reader_reg_dead_transfer=*/true));

  // `AttributeSched2PureReaderRegDeadNotes` must handle a block containing a
  // `call_insn` delay-slot `(sequence [(call_insn ...) (insn ...)])` without bailing out.
  RtlControlFlowGraph call_seq_cfg;
  call_seq_cfg.pass = RtlPassKind::kDbr;
  call_seq_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(sequence [(call_insn (parallel[ (call (mem:SI (symbol_ref:SI "
              "(\"callee\"))) (const_int 16)) (clobber (reg:SI 31 ra)) ] )) "
              "(insn (set (reg:SI 4 a0) (const_int 1)))])",
              "(set (reg:SI 16 s0) (ashift:SI (reg:SI 16 s0) (const_int 2))) "
              "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
              "(set (reg/i:SI 2 v0) (mem/s:SI (plus:SI (reg:SI 16 s0) "
              "(symbol_ref:SI (\"g_slot_ids\")))))",
              "(set (mem/s:SI (plus:SI (reg:SI 16 s0) (symbol_ref:SI (\"g_slot_states\")))) "
              "(reg:SI 17 s1)) (expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
              "(set (mem/s:SI (plus:SI (reg:SI 16 s0) (symbol_ref:SI (\"g_slot_timers\")))) "
              "(const_int 0)) (expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
              "(use (reg/i:SI 2 v0))",
              "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
          },
  });
  call_seq_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  AttributeSched2PureReaderRegDeadNotes(&call_seq_cfg, prof);
  EXPECT_NE(call_seq_cfg.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 16 s0)"),
            std::string::npos)
      << call_seq_cfg.FormatSummary();
  EXPECT_EQ(call_seq_cfg.rpo_blocks[0].instructions[4].find("REG_DEAD (reg:SI 16 s0)"),
            std::string::npos)
      << call_seq_cfg.FormatSummary();
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesRecoversSched2ObstructedStoreAndLoadMemEquiv) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive 1 (store-equiv `local-alloc.c:990-1000`): post-reload `sched2` hoisted
  // `v1 = v1 + 1; v1 = v1 & 15` ahead of `(set (mem (+ a1 buf)) a0)`, obstructing
  // `validate_equiv_mem` for `(set (mem idx) v1) (REG_DEAD v1)`. Stage 0 must
  // synthesize `(REG_EQUIV (mem:SI (symbol_ref:SI ("idx"))))` on `v1 = v1 & 15`
  // so Stage 3 Gate (b) accepts restoring the `sched1`-achievable pre-`sched2` order.
  RtlControlFlowGraph store_cfg;
  store_cfg.pass = RtlPassKind::kDbr;
  store_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& sb = store_cfg.rpo_blocks[0];
  sb.id = 0;
  sb.AddPatternOnly(
      "(set (reg:SI 3 v1) (mem:SI (symbol_ref:SI (\"idx\")))) "
      "(expr_list:REG_EQUIV (mem:SI (symbol_ref:SI (\"idx\"))) (nil))");
  sb.AddPatternOnly(
      "(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int 15))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sb.AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int -1)))");
  sb.AddPatternOnly(
      "(set (reg:SI 2 v0) (and:SI (reg:SI 2 v0) (const_int 15))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  sb.AddPatternOnly(
      "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  sb.AddPatternOnly(
      "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 2 v0) (symbol_ref:SI (\"buf\"))))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  sb.AddPatternOnly("(set (reg:SI 5 a1) (ashift:SI (reg:SI 3 v1) (const_int 2)))");
  sb.AddPatternOnly(
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sb.AddPatternOnly(
      "(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int 15))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sb.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 5 a1) (symbol_ref:SI (\"buf\")))) (reg:SI 4 a0)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  sb.AddPatternOnly(
      "(set (mem:SI (symbol_ref:SI (\"idx\"))) (reg:SI 3 v1)) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  sb.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 2 v0))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");
  sb.AddPatternOnly("(use (reg/i:SI 2 v0))");

  AttributeSched2PureReaderRegDeadNotes(&store_cfg, prof);
  auto parsed_init = ParsePatternAndNotes(store_cfg.rpo_blocks[0].instructions[8]);
  ASSERT_TRUE(parsed_init.ok());
  ASSERT_FALSE(parsed_init->reg_notes.empty());
  EXPECT_EQ(parsed_init->reg_notes.front().kind, RtlMachineMode::kRegEquiv);
  auto expected_idx = ParsePatternAndNotes("(mem:SI (symbol_ref:SI (\"idx\")))");
  ASSERT_TRUE(expected_idx.ok());
  EXPECT_EQ(parsed_init->reg_notes.front().datum, expected_idx->pattern);

  // Gate (b) (`PreservesPostReloadOrderSafety(..., allow_transfer=false)`) now
  // accepts moving `(set (mem (+ a1 buf)) a0)` back before `v1 = v1 + 1; v1 = v1 & 15`.
  const ParsedSchedRegion reg = ParseBlockForSchedModel(store_cfg.rpo_blocks[0], 0, Phase::kSched2);
  const RegFacts facts_s2 = RegFacts::FromCfg(store_cfg, prof, Phase::kSched2);
  EXPECT_TRUE(PreservesPostReloadOrderSafety(
      reg.sched_block, /*cand_in_base=*/{0, 1, 2, 3, 4, 5, 6, 9, 7, 8, 10, 11, 12},
      /*prologue_end=*/0, /*epilogue_begin=*/13, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{},
      facts_s2, mm, /*allow_pure_reader_reg_dead_transfer=*/false));

  // Positive 2 (load-equiv `local-alloc.c:1049-1055` with later hard-reg reuse):
  // under `-mips1` (2-cycle loads), `sched2` hoisted `a0 = sext(mem:HI (a0+8))`
  // ahead of `v0 = t0 + v0 (REG_DEAD v0)`, obstructing `validate_equiv_mem` on
  // `v0 = mem(a0)` even though `v0` is later redefined as the live-out return value.
  const CompilationProfile mips1_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  RtlControlFlowGraph load_cfg;
  load_cfg.pass = RtlPassKind::kDbr;
  load_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& lb = load_cfg.rpo_blocks[0];
  lb.id = 0;
  lb.AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
  lb.AddPatternOnly("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))");
  lb.AddPatternOnly(
      "(set (reg:SI 4 a0) (sign_extend:SI (mem:HI (plus:SI (reg:SI 4 a0) (const_int 8))))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  lb.AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 2 v0))) "
      "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");
  lb.AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
  lb.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))");
  lb.AddPatternOnly("(use (reg/i:SI 2 v0))");

  AttributeSched2PureReaderRegDeadNotes(&load_cfg, mips1_prof);
  auto parsed_load = ParsePatternAndNotes(load_cfg.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(parsed_load.ok());
  ASSERT_FALSE(parsed_load->reg_notes.empty());
  EXPECT_EQ(parsed_load->reg_notes.front().kind, RtlMachineMode::kRegEquiv);
  auto expected_a0 = ParsePatternAndNotes("(mem:SI (reg:SI 4 a0))");
  ASSERT_TRUE(expected_a0.ok());
  EXPECT_EQ(parsed_load->reg_notes.front().datum, expected_a0->pattern);

  // Negative: matched local-frame spill slots (`reload1.c`/`caller-save.c`) must NOT
  // receive synthesized `REG_EQUIV` notes.
  RtlControlFlowGraph spill_cfg;
  spill_cfg.pass = RtlPassKind::kDbr;
  spill_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& pb = spill_cfg.rpo_blocks[0];
  pb.id = 0;
  pb.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -64)))");
  pb.AddPatternOnly("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  pb.AddPatternOnly("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 48))))");
  pb.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:SI 3 v1)) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
  pb.AddPatternOnly("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))))");
  pb.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))");
  pb.AddPatternOnly("(use (reg/i:SI 2 v0))");

  AttributeSched2PureReaderRegDeadNotes(&spill_cfg, prof);
  auto parsed_spill_init = ParsePatternAndNotes(spill_cfg.rpo_blocks[0].instructions[1]);
  ASSERT_TRUE(parsed_spill_init.ok());
  EXPECT_TRUE(parsed_spill_init->reg_notes.empty());
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesRestoresPostCallCombineRegsRmwDeadNote) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000"}};

  auto make_cfg = [](bool read_call_ret) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    RtlBasicBlock& blk = cfg.rpo_blocks[0];
    blk.id = 0;
    blk.AddPatternOnly(
        "(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    blk.AddPatternOnly("(set (reg:SF 54 $f22) (mem:SF (reg:SI 16 s0)))");
    blk.AddPatternOnly("(set (reg:SF 52 $f20) (mem:SF (plus:SI (reg:SI 16 s0) (const_int 4))))");
    blk.AddPatternOnly("(set (reg:SF 44 $f12) (reg:SF 54 $f22))");
    blk.AddPatternOnly(
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"step\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    if (read_call_ret) {
      blk.AddPatternOnly("(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 32 $f0)))");
    } else {
      blk.AddPatternOnly(
          "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22))) "
          "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");
    }
    blk.AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 16 s0) (const_int 8))) (reg:SF 52 $f20)) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SF 52 $f20) (nil)))");
    blk.AddPatternOnly("(use (reg/i:SF 32 $f0))");
    blk.rtl_insns[4].has_explicit_call_fusage = true;
    blk.rtl_insns[4].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSf, 44)),
    };
    return cfg;
  };

  // Positive (`local-alloc.c:1266-1300, 1851-1886, 990-1000`, `sched.c:1732`):
  // at `-O2`, post-call `(set $f20 (plus:SF $f20 $f22))` needs the erased
  // `(REG_DEAD (reg:SF 52 $f20))` note to restore `kSched1` achievability, and
  // also recovers its store-equiv `(REG_EQUIV (mem:SF (plus:SI (reg:SI 16 s0) (const_int 8))))`.
  RtlControlFlowGraph pos_cfg = make_cfg(/*read_call_ret=*/false);
  AttributeSched2PureReaderRegDeadNotes(&pos_cfg, o2_prof);
  EXPECT_NE(pos_cfg.rpo_blocks[0].instructions[5].find("REG_DEAD (reg:SF 52 $f20)"),
            std::string::npos)
      << pos_cfg.rpo_blocks[0].instructions[5];
  EXPECT_NE(pos_cfg.rpo_blocks[0].instructions[5].find(
                "REG_EQUIV (mem:SF (plus:SI (reg:SI 16 s0) (const_int 8)))"),
            std::string::npos)
      << pos_cfg.rpo_blocks[0].instructions[5];

  // Negative 1: when the RMW reads call return `$f0`, the block is already
  // `kSched1`-achievable without splitting, so no `(REG_DEAD $f20)` is added.
  RtlControlFlowGraph neg_ach_cfg = make_cfg(/*read_call_ret=*/true);
  AttributeSched2PureReaderRegDeadNotes(&neg_ach_cfg, o2_prof);
  EXPECT_EQ(neg_ach_cfg.rpo_blocks[0].instructions[5].find("REG_DEAD (reg:SF 52 $f20)"),
            std::string::npos)
      << neg_ach_cfg.rpo_blocks[0].instructions[5];

  // Negative 2: at `-O1` (`!RunsSched1`), Stage 0 attribution is a no-op.
  RtlControlFlowGraph neg_o1_cfg = make_cfg(/*read_call_ret=*/false);
  AttributeSched2PureReaderRegDeadNotes(&neg_o1_cfg, o1_prof);
  EXPECT_EQ(neg_o1_cfg.rpo_blocks[0].instructions[5].find("REG_DEAD (reg:SF 52 $f20)"),
            std::string::npos)
      << neg_o1_cfg.rpo_blocks[0].instructions[5];
}
TEST(SchedStage0AttributionTest,
     AttributeSched2SkipsRaSaveForCallDelaySlotAndNeverTransfersPlaceholderDead) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  // Positive (`reorg.c:2901-2955`): when a `CALL_INSN` delay slot `(set s0 a0)`
  // with placeholder `(REG_DEAD (const_int 0))` was pulled past a prologue
  // `$ra` save `(set (mem sp+20) ra)`, Stage 0 unpacks the delay slot before
  // `sw $ra`, restores `(REG_DEAD a0)` in its read-only view, and recovers the
  // post-call `combine_regs` RMW `(REG_DEAD $f20)` note without mutating the
  // call delay slot's placeholder note.
  RtlControlFlowGraph pos_cfg;
  pos_cfg.pass = RtlPassKind::kDbr;
  pos_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& pblk = pos_cfg.rpo_blocks[0];
  pblk.id = 0;
  pblk.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))");
  pblk.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0)) "
      "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
  pblk.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 31 ra)) "
      "(expr_list:REG_DEAD (reg:SI 31 ra) (nil))");
  pblk.AddPatternOnly(
      "(sequence [(call_insn (parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI "
      "(\"step\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
      "(insn (set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (const_int 0) (nil)))])");
  pblk.AddPatternOnly("(set (reg:SF 54 $f22) (mem:SF (reg:SI 16 s0)))");
  pblk.AddPatternOnly("(set (reg:SF 52 $f20) (mem:SF (plus:SI (reg:SI 16 s0) (const_int 4))))");
  pblk.AddPatternOnly("(set (reg:SF 44 $f12) (reg:SF 54 $f22))");
  pblk.AddPatternOnly(
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"step\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
  pblk.AddPatternOnly(
      "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22))) "
      "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");
  pblk.AddPatternOnly(
      "(set (mem:SF (plus:SI (reg:SI 16 s0) (const_int 8))) (reg:SF 52 $f20)) "
      "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SF 52 $f20) (nil)))");
  pblk.AddPatternOnly("(use (reg/i:SF 32 $f0))");
  pblk.rtl_insns[3].has_explicit_call_fusage = true;
  pblk.rtl_insns[3].call_fusage = {};
  pblk.rtl_insns[7].has_explicit_call_fusage = true;
  pblk.rtl_insns[7].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSf, 44)),
  };
  AttributeSched2PureReaderRegDeadNotes(&pos_cfg, o2_prof);
  EXPECT_NE(pblk.instructions[8].find("REG_DEAD (reg:SF 52 $f20)"), std::string::npos)
      << pblk.instructions[8];
  EXPECT_NE(pblk.instructions[3].find("REG_DEAD (const_int 0)"), std::string::npos)
      << pblk.instructions[3];

  // Negative: a return/jump delay slot carrying a `reorg.c` placeholder
  // `(REG_DEAD (const_int 0))` must never have a provisionally reconstructed
  // `REG_DEAD` transferred onto an earlier store when `a0` is still read in the slot.
  RtlControlFlowGraph neg_cfg;
  neg_cfg.pass = RtlPassKind::kDbr;
  neg_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& nblk = neg_cfg.rpo_blocks[0];
  nblk.id = 0;
  nblk.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 16 s0) (const_int 12))) (reg:SI 4 a0))");
  nblk.AddPatternOnly(
      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
      "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 2 v0))) "
      "(expr_list:REG_DEAD (const_int 0) (nil)))])");
  AttributeSched2PureReaderRegDeadNotes(&neg_cfg, o2_prof);
  EXPECT_EQ(nblk.instructions[0].find("REG_DEAD (reg:SI 4 a0)"), std::string::npos)
      << nblk.instructions[0];

  // Negative 2 (`local-alloc.c:990` `update_equiv_regs`): a narrower-mode
  // store `(set (mem:HI a3) (reg:HI v0))` (pre-reload `(subreg:HI (reg:SI P) 0)`)
  // in an unpacked call delay slot must NEVER attach `REG_EQUIV (mem:HI ...)`
  // onto `(set (reg:SI v0) (const_int 1))`.
  RtlControlFlowGraph subreg_cfg;
  subreg_cfg.pass = RtlPassKind::kDbr;
  subreg_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& sblk = subreg_cfg.rpo_blocks[0];
  sblk.id = 0;
  sblk.AddPatternOnly("(set (reg:SI 4 a0) (reg:SI 16 s0))");
  sblk.AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  sblk.AddPatternOnly("(set (mem:SI (reg:SI 17 s1)) (reg:SI 5 a1))");
  sblk.AddPatternOnly(
      "(sequence [(call_insn (parallel[ (call (mem:SI (symbol_ref:SI (\"init_slot\"))) "
      "(const_int 16)) (clobber (reg:SI 31 ra)) ] )) "
      "(insn (set (mem:HI (reg:SI 7 a3)) (reg:HI 2 v0)) "
      "(expr_list:REG_DEAD (const_int 0) (nil)))])");
  sblk.rtl_insns[3].has_explicit_call_fusage = true;
  sblk.rtl_insns[3].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
  };
  AttributeSched2PureReaderRegDeadNotes(&subreg_cfg, o2_prof);
  EXPECT_EQ(sblk.instructions[1].find("REG_EQUIV"), std::string::npos) << sblk.instructions[1];
}
TEST(SchedStage0AttributionTest, CollapseSched1UnsplitConstDoubleLoadsSiRecoveryAndGuards) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000"}};

  auto make_unhoisted_si_cfg = [](bool bit31_set, bool hoist_const_to_top) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(2);
    RtlBasicBlock& b0 = cfg.rpo_blocks[0];
    b0.id = 0;
    b0.successors = {1};
    const std::string hi_insn = bit31_set ? "(set (reg:SI 3 v1) (const_int -2139095040))"
                                          : "(set (reg:SI 3 v1) (const_int 109903872))";
    const std::string lo_insn =
        bit31_set ? "(set (reg:SI 3 v1) (ior:SI (reg:SI 3 v1) (const_int 32897)))"
                  : "(set (reg:SI 3 v1) (ior:SI (reg:SI 3 v1) (const_int 58287)))";
    b0.AddPatternOnly(
        "(set (reg:SI 3 v1) (zero_extend:SI (mem:QI (reg:SI 4 a0)))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    if (hoist_const_to_top) {
      b0.AddPatternOnly(hi_insn);
      b0.AddPatternOnly(lo_insn);
    }
    b0.AddPatternOnly("(set (reg:SI 2 v0) (ashift:SI (reg:SI 3 v1) (const_int 1)))");
    b0.AddPatternOnly(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
    if (!hoist_const_to_top) {
      b0.AddPatternOnly(hi_insn);
      b0.AddPatternOnly(lo_insn);
    }
    b0.AddPatternOnly(
        "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI "
        "(zero_extend:DI (reg:SI 2 v0)) (zero_extend:DI (reg:SI 3 v1))) (const_int 32)))) "
        "(clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] ) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 3 v1) "
        "(expr_list:REG_UNUSED (reg:SI 65 lo) (expr_list:REG_UNUSED (reg:SI 66 accum) (nil)))))");
    b0.AddPatternOnly(
        "(set (reg:SI 7 a3) (reg:SI 64 hi)) (expr_list:REG_DEAD (reg:SI 64 hi) (nil))");
    b0.AddPatternOnly(
        "(set (reg:SI 2 v0) (lshiftrt:SI (reg:SI 7 a3) (const_int 7))) "
        "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))");
    b0.AddPatternOnly(
        "(sequence [(jump_insn/u (set (pc) (if_then_else (ge:SI (reg:SI 2 v0) (const_int 0)) "
        "(label_ref L1) (pc)))) "
        "(insn/s (set (reg:SI 6 a2) (plus:SI (reg:SI 2 v0) (const_int 1))))])");

    RtlBasicBlock& b1 = cfg.rpo_blocks[1];
    b1.id = 1;
    b1.predecessors = {0};
    b1.AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive (`varasm.c:1857-1895`, `mips.md:3208-3221`, `mips.c:1047-1057`):
  // unhoisted bit-31-set `SI` `lui; ori` in an `-O2` block (even with an
  // annulled tail branch `jump_insn/u`) collapses to `(const_double (cc0) -2139062143 0)`.
  RtlControlFlowGraph pos_cfg = make_unhoisted_si_cfg(/*bit31_set=*/true, /*hoist=*/false);
  CollapseSched1UnsplitConstDoubleLoads(&pos_cfg, o2_prof);
  EXPECT_EQ(pos_cfg.rpo_blocks[0].instructions[3],
            "(set (reg:SI 3 v1) (const_double (cc0) -2139062143 0))");

  // Negative 1: positive `SI` `lui; ori` (`HI > 0`) is never `const_double` in
  // GCC 2.7.2 (`varasm.c:1857-1895`) and must remain uncollapsed.
  RtlControlFlowGraph neg_pos_val = make_unhoisted_si_cfg(/*bit31_set=*/false, /*hoist=*/false);
  const std::vector<std::string> orig_pos_insns = neg_pos_val.rpo_blocks[0].instructions;
  CollapseSched1UnsplitConstDoubleLoads(&neg_pos_val, o2_prof);
  EXPECT_EQ(neg_pos_val.rpo_blocks[0].instructions, orig_pos_insns);

  // Negative 2: already `kSched1`-achievable block (hoisted `lui; ori`) is untouched.
  RtlControlFlowGraph neg_hoisted = make_unhoisted_si_cfg(/*bit31_set=*/true, /*hoist=*/true);
  const std::vector<std::string> orig_hoisted_insns = neg_hoisted.rpo_blocks[0].instructions;
  CollapseSched1UnsplitConstDoubleLoads(&neg_hoisted, o2_prof);
  EXPECT_EQ(neg_hoisted.rpo_blocks[0].instructions, orig_hoisted_insns);

  // Negative 3: `-O1` (`!RunsSched1`) is a no-op.
  RtlControlFlowGraph neg_o1 = make_unhoisted_si_cfg(/*bit31_set=*/true, /*hoist=*/false);
  const std::vector<std::string> orig_o1_insns = neg_o1.rpo_blocks[0].instructions;
  CollapseSched1UnsplitConstDoubleLoads(&neg_o1, o1_prof);
  EXPECT_EQ(neg_o1.rpo_blocks[0].instructions, orig_o1_insns);
}
TEST(SchedStage0AttributionTest, AttributeSched2PureReaderRegDeadNotesAlignsMovedReturnDelaySlot) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};

  // Build a block where `sched2` schedules `(set (mem g_out) v1)` BEFORE
  // `(set v0 (plus v1 t0))` (because `t0` comes from a 2-cycle load), and
  // `reorg.c:fill_simple_delay_slots` moved `(set (mem g_out) v1)` into the
  // `return` delay slot (moving `(REG_DEAD v1)` onto the store).
  auto make_delay_cfg = [](bool moved_to_delay_slot) {
    RtlControlFlowGraph cfg;
    RtlBasicBlock b0;
    if (moved_to_delay_slot) {
      b0.instructions = {
          "(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
          "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 7))) "
          "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
          "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
          "(use (reg/i:SI 2 v0))",
          "(sequence[(jump_insn (return))"
          "(insn (set (mem:SI (symbol_ref:SI (\"g_out\"))) (reg:SI 3 v1)) "
          "(expr_list:REG_DEAD (reg:SI 3 v1) (nil)))])",
      };
    } else {
      b0.instructions = {
          "(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
          "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 7))) "
          "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
          "(set (mem:SI (symbol_ref:SI (\"g_out\"))) (reg:SI 3 v1))",
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
          "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
          "(use (reg/i:SI 2 v0))",
          "(return)",
      };
    }
    b0.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks = {std::move(b0)};
    return cfg;
  };

  // Positive: `AlignStage0DelaySlotsToSched2View` slides the unpacked return
  // delay slot ahead of `addu` to restore `Im(F_sched2)` and commits the
  // pure-reader `(REG_DEAD (reg:SI 3 v1))` transfer onto `addu`.
  RtlControlFlowGraph pos_cfg = make_delay_cfg(/*moved_to_delay_slot=*/true);
  AttributeSched2PureReaderRegDeadNotes(&pos_cfg, o2_prof);
  EXPECT_NE(pos_cfg.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos);
  EXPECT_EQ(pos_cfg.rpo_blocks[0].instructions[4].find("REG_DEAD (reg:SI 3 v1)"),
            std::string::npos);

  // Negative: an already-`Im(F_sched2)` and `kSched1`-achievable block is untouched.
  RtlControlFlowGraph neg_cfg = make_delay_cfg(/*moved_to_delay_slot=*/false);
  const std::vector<std::string> orig_neg = neg_cfg.rpo_blocks[0].instructions;
  AttributeSched2PureReaderRegDeadNotes(&neg_cfg, o2_prof);
  EXPECT_EQ(neg_cfg.rpo_blocks[0].instructions, orig_neg);
}
TEST(SchedStage0AttributionTest,
     AttributeStage0RetractsSpeculativeSelfSetRmwRegDeadWhenSched1Requires) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  // Positive (`flow.c:2506-2515`, `sched.c:2490`, `toplev.c:3028-3053`):
  // when block 1 has `[0] sll $2,$6,5; [1] subu $2,$2,$6 (REG_DEAD $2) (REG_DEAD $6);
  // [2] andi $3,$3,255; [3] addu $6,$2,$3 (REG_DEAD $2); [4] addu $5,$5,1` with
  // `$3` live-out around the loop (`reg_n_sets[$3] >= 2`), the speculative
  // `(REG_DEAD (reg:SI 2 v0))` on `[1]` makes `[0]` a birthing insn (`prio 3 >> 1 = 1`)
  // losing to `[2]` (`prio 2`), rendering the block `kSched1`-unachievable.
  // Pass 4 retracts `(REG_DEAD (reg:SI 2 v0))` from `[1]`, restoring `kSched1`
  // achievability while preserving `kSched2` replay.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (sign_extend:SI (mem:QI (reg:SI 5 a1))))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 6 a2) (const_int 0))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].successors = {1, 2};
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (ashift:SI (reg:SI 6 a2) (const_int 5)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 2 v0) (minus:SI (reg:SI 2 v0) (reg:SI 6 a2))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))");
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int 255)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 6 a2) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 1)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[1].instructions[1].find("REG_DEAD (reg:SI 2 v0)"), std::string::npos)
        << cfg.FormatSummary();
    EXPECT_NE(cfg.rpo_blocks[1].instructions[1].find("REG_DEAD (reg:SI 6 a2)"), std::string::npos)
        << cfg.FormatSummary();
  }

  // Negative contrast: when the block is ALREADY `kSched1`-achievable (`[2]` is
  // placed first so birthing `[0]` does not conflict with `[2]`), Pass 4 does
  // NOT retract `(REG_DEAD (reg:SI 2 v0))` on `[1]`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (sign_extend:SI (mem:QI (reg:SI 5 a1))))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 6 a2) (const_int 0))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].successors = {1, 2};
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int 255)))");
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (ashift:SI (reg:SI 6 a2) (const_int 5)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 2 v0) (minus:SI (reg:SI 2 v0) (reg:SI 6 a2))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 6 a2) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 1)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[1].instructions[2].find("REG_DEAD (reg:SI 2 v0)"), std::string::npos)
        << cfg.FormatSummary();
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesStripsSpuriousStoreEquivOnDelaySlotConstZero) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};

  auto make_cfg = [](const std::string& equiv_sym) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (mem:SI (symbol_ref:SI (\"g\"))))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (symbol_ref:SI (\"h\"))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 4 a0))) "
        "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 4 a0) "
        "(expr_list:REG_EQUIV (mem:SI (symbol_ref:SI (\"" +
        equiv_sym + "\"))) (nil))))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 2))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (mem:SI (symbol_ref:SI (\"h\"))) (reg:SI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 16))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 2 v0) (const_int 16))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (mem:SI (symbol_ref:SI (\"g\"))) (reg:SI 3 v1))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence[ (jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
        "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
        "(expr_list:REG_DEAD (const_int 0) (expr_list:REG_DEAD (const_int 0) (nil)))) ] )");
    return cfg;
  };

  // Positive (`reorg.c:918-920`, `local-alloc.c:540-603, 990-1000`, `sched.c:3415-3611`):
  // `sched2` sank store `[7]` `(mem "g") = $3` after `[9]`'s unpacked return
  // delay-slot death of `$3` (carrying `(REG_DEAD (const_int 0))`). When Stage 0
  // resolves `(const_int 0)` in-place onto `$3` and moves `[7]` back before the
  // delay slot, it must strip the invalidated store-`REG_EQUIV (mem "g")` from `[2]`.
  {
    RtlControlFlowGraph cfg = make_cfg("g");
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[2].find("REG_EQUIV"), std::string::npos)
        << cfg.FormatSummary();
    EXPECT_NE(cfg.rpo_blocks[0].instructions[9].find("REG_DEAD (reg:SI 3 v1)"), std::string::npos)
        << cfg.FormatSummary();
  }

  // Negative contrast: when `[2]` carries a `REG_EQUIV` for a distinct symbol
  // `"other"` unrelated to the moved store `[7]` to `"g"`, Stage 0 preserves
  // `(REG_EQUIV (mem:SI (symbol_ref:SI ("other"))))` on `[2]`.
  {
    RtlControlFlowGraph cfg = make_cfg("other");
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[2].find("REG_EQUIV"), std::string::npos)
        << cfg.FormatSummary();
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesStripsValidateEquivMemOnDeadTransfer) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  // `local-alloc.c:575-600, 1049-1055` (`validate_equiv_mem`): when `sched2`
  // hoists load `[1]` (`[a0+g_slot_ids]`) ahead of stores `[2]` and `[3]`,
  // transferring `(REG_DEAD a0)` from `[3]` onto `[1]` invalidates any
  // speculative `(REG_EQUIV (mem ...))` on `[1]` whose address mentions `a0`,
  // while preserving an unrelated `(REG_EQUIV (mem ...))` mentioning `a1`.
  auto make_cfg = [](const std::string& equiv_base_reg) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 4 a0) (ashift:SI (reg:SI 4 a0) (const_int 2))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg/i:SI 2 v0) (mem/s:SI (plus:SI (reg:SI 4 a0) "
                "(symbol_ref:SI (\"g_slot_ids\"))))) "
                "(expr_list:REG_EQUIV (mem/s:SI (plus:SI " +
                    equiv_base_reg + " (symbol_ref:SI (\"g_slot_ids\")))) (nil))",
                "(set (mem/s:SI (plus:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_slot_states\")))) "
                "(reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                "(set (mem/s:SI (plus:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_slot_timers\")))) "
                "(const_int 0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(use (reg/i:SI 2 v0))",
                "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
            },
    });
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return cfg;
  };

  // Positive: transferring `(REG_DEAD a0)` from `[3]` onto `[1]` must strip
  // the now-invalid `(REG_EQUIV (mem/s:SI (plus:SI (reg:SI 4 a0) ...)))` on `[1]`.
  {
    RtlControlFlowGraph cfg = make_cfg("(reg:SI 4 a0)");
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 4 a0)"), std::string::npos)
        << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV"), std::string::npos)
        << cfg.FormatSummary();
  }

  // Negative contrast: when `[1]`'s `REG_EQUIV` mentions `a1` instead of `a0`,
  // transferring `(REG_DEAD a0)` onto `[1]` preserves the unrelated `REG_EQUIV`.
  {
    RtlControlFlowGraph cfg = make_cfg("(reg:SI 5 a1)");
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 4 a0)"), std::string::npos)
        << cfg.FormatSummary();
    EXPECT_NE(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV"), std::string::npos)
        << cfg.FormatSummary();
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesRetractsSelfSetDeadViaSched2Preimage) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  // `flow.c:2506-2515`, `sched.c:2490` (`birthing_insn_p`): when a block has
  // BOTH a speculative self-set `(REG_DEAD a1)` on an RMW AND a `sched2`
  // reorder so direct `IsAchievable(kSched1)` fails, Stage 0's
  // `TryRetractSpeculativeSelfSetRmwRegDeadNotes` second pass checks
  // `HasSched1AchievablePreimageForSched2Block(..., allow_tie_flips=false)`
  // and retracts `(REG_DEAD a1)` iff retraction unlocks a `kSched1`-achievable
  // `kSched2` preimage.
  auto make_cfg = [](bool include_self_dead_a1, bool swap_load_order) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (mem:SI (reg:SI 4 a0)))");
    if (swap_load_order) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4)))) "
          "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
      cfg.rpo_blocks[0].AddPatternOnly(
          include_self_dead_a1 ? "(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 5 a1) (const_int 31))) "
                                 "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))"
                               : "(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 5 a1) (const_int 31)))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly(
          include_self_dead_a1 ? "(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 5 a1) (const_int 31))) "
                                 "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))"
                               : "(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 5 a1) (const_int 31)))");
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4)))) "
          "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 7)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (ior:SI (reg:SI 5 a1) (reg:SI 6 a2))) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    return cfg;
  };

  // Positive: `HasSched1AchievablePreimageForSched2Block(..., allow_tie_flips=false)`
  // accepts a retracted block that admits a `kSched2`-replayable `kSched1`
  // preimage (used by `TryRetractSpeculativeSelfSetRmwRegDeadNotes`'s second pass).
  const RtlControlFlowGraph clean_cfg = make_cfg(/*include_self_dead_a1=*/false,
                                                 /*swap_load_order=*/true);
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RtlControlFlowGraph live_cfg = clean_cfg;
  ReconstructRtlLiveness(&live_cfg, o2_prof, {.reload_completed = true, .update_reg_notes = false});
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(clean_cfg, live_cfg, mm);
  const ParsedSchedRegion region =
      ParseBlockForSchedModel(clean_cfg.rpo_blocks[0], 0, Phase::kSched2);
  const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, 0);
  const Sched1ProjectionContext proj_ctx = BuildSched1ProjectionContextForBlock(0, cfg_ctx);
  std::vector<Insn> body(region.sched_block.begin() + bounds.prologue_end,
                         region.sched_block.begin() + bounds.epilogue_begin);
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, proj_ctx);
  const RegFacts facts_s2 = RegFacts::FromCfg(clean_cfg, o2_prof, Phase::kSched2);
  EXPECT_TRUE(HasSched1AchievablePreimageForSched2Block(
      region.sched_block, 0, bounds, view, proj_ctx, cfg_ctx, facts_s2, mm,
      /*live_at_end_s2=*/{}, /*allow_pure_reader_reg_dead_transfer=*/false,
      /*allow_tie_flips=*/false));

  // Negative contrast: when the block already satisfies `kSched1` WITH its
  // self-set `(REG_DEAD a1)` intact, `AttributeSched2PureReaderRegDeadNotes`
  // must NOT gratuitously retract `(REG_DEAD a1)`.
  RtlControlFlowGraph keep_cfg = make_cfg(/*include_self_dead_a1=*/true,
                                          /*swap_load_order=*/false);
  AttributeSched2PureReaderRegDeadNotes(&keep_cfg, o2_prof);
  EXPECT_NE(keep_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 5 a1)"),
            std::string::npos)
      << keep_cfg.FormatSummary();
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesResolvesIntegratedReturnDelaySlotZero) {
  // `flow.c:2506-2515` + `reorg.c:918-920` (`emit_delay_sequence`): when a
  // delay-slot return-value `SET` `(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) ...))`
  // carries a zeroed `(REG_DEAD (const_int 0))` placeholder in a block whose
  // `sched2` order triggers `AttributeSched2PureReaderRegDeadNotes`, it resolves
  // `(const_int 0)` in-place onto the integrated return-dest insn as
  // `(REG_DEAD (reg:SI 2 v0))`, whereas a non-integrated RMW
  // `(set (reg:SI 2 v0) ...)` must NOT be falsely resolved.
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RoundTripTestCase tc;
  tc.test_name = "SyntheticIntegratedReturnDelaySlot";
  tc.function_name = "harness_func";
  tc.original_c = R"(
int compute_step(int a, int b);
int harness_func(int* p, int k) {
  int r = compute_step(p[0], k + 1);
  if (r < 0) return -1;
  p[1] = r + p[2];
  p[3] = p[1] ^ k;
  return p[3] + p[4];
}
)";
  tc.options.c_flags = {"-O2", "-mips1", "-funsigned-char"};
  const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(tc);
  const CompilationProfile& prof = trace.stage0_input.profile;
  ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kDbr));
  const RtlControlFlowGraph& dbr_cfg = trace.pass_snapshots.at(RtlPassKind::kDbr);

  // Locate the block and delay-slot instruction containing
  // `(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 2 v0)))` with a zeroed
  // `(REG_DEAD (const_int 0))` placeholder.
  size_t target_blk = dbr_cfg.rpo_blocks.size();
  size_t target_idx = 0;
  for (size_t b = 0; b < dbr_cfg.rpo_blocks.size(); ++b) {
    const auto& insns = dbr_cfg.rpo_blocks[b].instructions;
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].find("(sequence [") != std::string::npos &&
          insns[i].find("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 2 v0)))") !=
              std::string::npos &&
          insns[i].find("REG_DEAD (const_int 0)") != std::string::npos) {
        target_blk = b;
        target_idx = i;
        break;
      }
    }
    if (target_blk < dbr_cfg.rpo_blocks.size()) {
      break;
    }
  }
  ASSERT_LT(target_blk, dbr_cfg.rpo_blocks.size()) << dbr_cfg.FormatSummary();

  // Strip `REG_EQUIV` from the target block to match raw Stage 0 `.11.dbr` input
  // (where `AttributeSched2PureReaderRegDeadNotes` must resolve `(const_int 0)`
  // in-place onto `reg/i:SI 2 v0` before `CollectSched2ObstructedMemRegEquivAdditions`).
  RtlControlFlowGraph stage0_like = dbr_cfg;
  for (std::string& insn_str : stage0_like.rpo_blocks[target_blk].instructions) {
    const size_t eq_pos = insn_str.find("(expr_list:REG_EQUIV ");
    if (eq_pos != std::string::npos) {
      // Remove `(expr_list:REG_EQUIV <datum> <tail>)` wrapper, keeping `<tail>` if non-nil.
      int depth = 0;
      size_t datum_start = eq_pos + std::string("(expr_list:REG_EQUIV ").size();
      size_t p = datum_start;
      for (; p < insn_str.size(); ++p) {
        if (insn_str[p] == '(') {
          ++depth;
        } else if (insn_str[p] == ')') {
          --depth;
          if (depth == 0) {
            ++p;
            break;
          }
        }
      }
      while (p < insn_str.size() && insn_str[p] == ' ') {
        ++p;
      }
      const std::string tail = insn_str.substr(p);
      // Drop the single matching ')' at the end of `tail`.
      if (tail == "(nil))") {
        insn_str.erase(eq_pos > 0 && insn_str[eq_pos - 1] == ' ' ? eq_pos - 1 : eq_pos);
      } else if (!tail.empty() && tail.back() == ')') {
        insn_str.replace(eq_pos, std::string::npos, tail.substr(0, tail.size() - 1));
      }
    }
  }
  stage0_like.rpo_blocks[target_blk].SyncRtlInsnsFromInstructions();

  // Positive: integrated return-dest delay slot `(reg/i:SI 2 v0)` resolves
  // `(const_int 0)` -> `(REG_DEAD (reg:SI 2 v0))`.
  {
    RtlControlFlowGraph pos_cfg = stage0_like;
    AttributeSched2PureReaderRegDeadNotes(&pos_cfg, prof);
    EXPECT_NE(
        pos_cfg.rpo_blocks[target_blk].instructions[target_idx].find("REG_DEAD (reg:SI 2 v0)"),
        std::string::npos)
        << pos_cfg.rpo_blocks[target_blk].instructions[target_idx];
  }

  // Negative: stripping `/i` (making it a non-integrated RMW `(set (reg:SI 2 v0) ...)`)
  // must NOT attach `(REG_DEAD (reg:SI 2 v0))`.
  {
    RtlControlFlowGraph neg_cfg = stage0_like;
    std::string& insn_str = neg_cfg.rpo_blocks[target_blk].instructions[target_idx];
    const size_t pos = insn_str.find("(set (reg/i:SI 2 v0)");
    ASSERT_NE(pos, std::string::npos);
    insn_str.replace(pos, std::string("(set (reg/i:SI 2 v0)").size(), "(set (reg:SI 2 v0)");
    neg_cfg.rpo_blocks[target_blk].SyncRtlInsnsFromInstructions();
    AttributeSched2PureReaderRegDeadNotes(&neg_cfg, prof);
    EXPECT_EQ(
        neg_cfg.rpo_blocks[target_blk].instructions[target_idx].find("REG_DEAD (reg:SI 2 v0)"),
        std::string::npos)
        << neg_cfg.rpo_blocks[target_blk].instructions[target_idx];
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesRepairsCombineRegsPureReaderConflict) {
  const CompilationProfile o2_prof{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
  };
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Build a genuine `kSched1`-achievable pre-`sched2` block where `xor`
  // (`$s2 = $s0 ^ 7`, backward `kSched1` priority `2`) precedes the `$s1` RMW
  // (`$s1 = $s1 + $s0`, backward `kSched1` priority `3`, carrying the true
  // `(REG_DEAD $s0)`). In `kSched2` (which schedules backwards from tail to
  // head, `sched.c:3293-3611`), earlier reuse of hard reg `$s2` (`0 -> 1`)
  // adds a `1 -> xor` WAR dependence that raises `xor`'s backward `kSched2`
  // priority above the `$s1` RMW, so `Schedule(..., kSched2)` picks `xor`
  // earlier in reverse (placing it LATER and hoisting the `$s1` RMW ahead of
  // `xor`). Re-annotating `(REG_DEAD $s0)` on the later post-`sched2` reader
  // (`xor`) creates a provable `local-alloc.c:1266-1300, 1855-1860`
  // (`combine_regs`) contradiction (`dst $s2 != dying src $s0`), which Stage 0
  // repairs by moving `(REG_DEAD $s0)` back to the hoisted `$s1` RMW.
  const std::vector<std::string> pre_sched2_texts = {
      "(parallel[ (set (reg:SI 18 s2) (mult:SI (reg:SI 7 a3) (reg:SI 7 a3))) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
      "(set (mem:SI (reg:SI 6 a2)) (reg:SI 18 s2)) "
      "(expr_list:REG_DEAD (reg:SI 18 s2) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
      "(set (reg:SI 16 s0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 17 s1) (plus:SI (reg:SI 5 a1) (const_int 5))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
      "(set (reg:SI 17 s1) (xor:SI (reg:SI 17 s1) (const_int 3))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
      "(set (reg:SI 18 s2) (xor:SI (reg:SI 16 s0) (const_int 7)))",
      "(set (reg:SI 17 s1) (plus:SI (reg:SI 17 s1) (reg:SI 16 s0))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (expr_list:REG_DEAD (reg:SI 16 s0) (nil)))",
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 17 s1) (reg:SI 18 s2))) "
      "(expr_list:REG_DEAD (reg:SI 17 s1) (expr_list:REG_DEAD (reg:SI 18 s2) (nil)))",
      "(use (reg/i:SI 2 v0))",
  };
  std::vector<std::string> sched1_texts = pre_sched2_texts;
  std::vector<Insn> pre_sched2_insns;
  int xor_s1_idx = 5;
  int rmw_s1_idx = 6;
  for (int round = 0; round < 4; ++round) {
    pre_sched2_insns.clear();
    for (size_t i = 0; i < sched1_texts.size(); ++i) {
      pre_sched2_insns.push_back(MakeInsn(sched1_texts[i], static_cast<int>(i)));
    }
    const Sched1ProjectionView v =
        ProjectPostReloadBlockToSched1View(pre_sched2_insns, mm, {}, Sched1ProjectionContext{});
    if (IsAchievable(v.core_insns, v.facts_s1, mm, Phase::kSched1, v.live_at_end_s1)) {
      break;
    }
    const std::vector<int> s1_ord = ScheduleSched1ViewCoreOrder(v, mm);
    ASSERT_EQ(s1_ord.size(), sched1_texts.size());
    std::vector<std::string> next_texts;
    next_texts.reserve(s1_ord.size());
    int next_xor = -1;
    int next_rmw = -1;
    for (size_t p = 0; p < s1_ord.size(); ++p) {
      const int orig = v.core_to_orig_index[static_cast<size_t>(s1_ord[p])];
      if (orig == xor_s1_idx) next_xor = static_cast<int>(p);
      if (orig == rmw_s1_idx) next_rmw = static_cast<int>(p);
      next_texts.push_back(sched1_texts[static_cast<size_t>(orig)]);
    }
    sched1_texts = std::move(next_texts);
    xor_s1_idx = next_xor;
    rmw_s1_idx = next_rmw;
  }
  ASSERT_GE(xor_s1_idx, 0);
  ASSERT_GT(rmw_s1_idx, xor_s1_idx);
  const Sched1ProjectionView pre_s1_view =
      ProjectPostReloadBlockToSched1View(pre_sched2_insns, mm, {}, Sched1ProjectionContext{});
  ASSERT_TRUE(IsAchievable(pre_s1_view.core_insns, pre_s1_view.facts_s1, mm, Phase::kSched1,
                           pre_s1_view.live_at_end_s1));

  RegFacts facts_s2;
  facts_s2.max_reg_num = 72;
  const DepGraph g_s2 = Analyze(pre_sched2_insns, facts_s2, mm, Phase::kSched2);
  const ScheduleResult s2_res = Schedule(pre_sched2_insns, g_s2, facts_s2, mm, Phase::kSched2, {});
  ASSERT_EQ(s2_res.order.size(), sched1_texts.size());

  // Verify `sched2` genuinely hoisted the `$s1` RMW ahead of `xor`, then
  // re-annotate `(REG_DEAD $s0)` on the later reader (`xor`) as Stage 0 raw
  // liveness produces.
  int rmw_post_idx = -1;
  int xor_post_idx = -1;
  std::vector<std::string> post_sched2_texts;
  post_sched2_texts.reserve(sched1_texts.size() + 1);
  for (size_t p = 0; p < s2_res.order.size(); ++p) {
    const int orig = s2_res.order[p];
    if (orig == rmw_s1_idx) {
      rmw_post_idx = static_cast<int>(p);
      post_sched2_texts.push_back(
          "(set (reg:SI 17 s1) (plus:SI (reg:SI 17 s1) (reg:SI 16 s0))) "
          "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))");
    } else if (orig == xor_s1_idx) {
      xor_post_idx = static_cast<int>(p);
      post_sched2_texts.push_back(
          "(set (reg:SI 18 s2) (xor:SI (reg:SI 16 s0) (const_int 7))) "
          "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))");
    } else {
      post_sched2_texts.push_back(sched1_texts[static_cast<size_t>(orig)]);
    }
  }
  post_sched2_texts.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  ASSERT_GE(rmw_post_idx, 0);
  ASSERT_GT(xor_post_idx, rmw_post_idx);

  // Positive: Stage 0 repairs the `combine_regs` conflict (`dst $s2 != $s0`)
  // and transfers `(REG_DEAD (reg:SI 16 s0))` from `xor` back to the `$s1` RMW.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    RtlBasicBlock blk;
    blk.id = 0;
    blk.instructions = post_sched2_texts;
    blk.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(std::move(blk));
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[static_cast<size_t>(rmw_post_idx)].find(
                  "REG_DEAD (reg:SI 16 s0)"),
              std::string::npos)
        << cfg.rpo_blocks[0].instructions[static_cast<size_t>(rmw_post_idx)];
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[static_cast<size_t>(xor_post_idx)].find(
                  "REG_DEAD (reg:SI 16 s0)"),
              std::string::npos)
        << cfg.rpo_blocks[0].instructions[static_cast<size_t>(xor_post_idx)];
  }

  // Negative contrast: when the later reader writes `$s0` itself (`$s0 = $s0 ^ 7`,
  // no `combine_regs` contradiction and WAR-pinned after the `$s1` RMW),
  // `(REG_DEAD (reg:SI 16 s0))` stays on the later reader.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    RtlBasicBlock blk;
    blk.id = 0;
    blk.instructions = post_sched2_texts;
    blk.instructions[static_cast<size_t>(xor_post_idx)] =
        "(set (reg:SI 16 s0) (xor:SI (reg:SI 16 s0) (const_int 7))) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))";
    for (size_t idx = static_cast<size_t>(xor_post_idx) + 1; idx < blk.instructions.size(); ++idx) {
      if (blk.instructions[idx].find("(reg:SI 18 s2)") != std::string::npos) {
        blk.instructions[idx] =
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 17 s1) (reg:SI 16 s0))) "
            "(expr_list:REG_DEAD (reg:SI 17 s1) (expr_list:REG_DEAD (reg:SI 16 s0) (nil)))";
      }
    }
    blk.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(std::move(blk));
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[static_cast<size_t>(rmw_post_idx)].find(
                  "REG_DEAD (reg:SI 16 s0)"),
              std::string::npos)
        << cfg.rpo_blocks[0].instructions[static_cast<size_t>(rmw_post_idx)];
    EXPECT_NE(cfg.rpo_blocks[0].instructions[static_cast<size_t>(xor_post_idx)].find(
                  "REG_DEAD (reg:SI 16 s0)"),
              std::string::npos)
        << cfg.rpo_blocks[0].instructions[static_cast<size_t>(xor_post_idx)];
  }
}
TEST(SchedStage0AttributionTest,
     PreservesPostReloadOrderSafetyIncomingStackParamAndBaseRegDeathEquivWindow) {
  const CompilationProfile mips1_o2{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const MachineModel mm = MachineModel::FromProfile(mips1_o2);
  const RegFacts facts;

  // 1. Positive + negative (`function.c:3832-3834`, `local-alloc.c:1049-1055`):
  //    an incoming stack-parameter load `[sp + frame_size + 16]` carrying
  //    `REG_EQUIV (mem ...)` was attached by `assign_parms` (bypassing
  //    `validate_equiv_mem`), so moving an unrelated memory store `[2]` into
  //    `[0, 1]` keeps `mem_equiv_valid` unaffected when `stack_layout.valid` is
  //    true (positive), while rejecting the same move when `stack_layout.valid`
  //    is false (negative contrast).
  {
    PostReloadStackLayout layout;
    layout.valid = true;
    layout.frame_size = 32;
    layout.starting_frame_offset = 16;
    const std::vector<Insn> base_block = {
        MakeInsn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 48)))) "
                 "(expr_list:REG_EQUIV (mem:SI (plus:SI (reg:SI 29 sp) (const_int 48))) (nil))",
                 0),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                 1),
        MakeInsn("(set (mem:SI (reg:SI 4 a0)) (reg:SI 5 a1))", 2),
        MakeInsn("(use (reg/i:SI 2 v0))", 3),
    };
    EXPECT_TRUE(PreservesPostReloadOrderSafety(
        base_block, /*cand_in_base=*/{0, 2, 1, 3}, /*prologue_end=*/0, /*epilogue_begin=*/3,
        /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts, mm,
        /*allow_pure_reader_reg_dead_transfer=*/false, layout));
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        base_block, /*cand_in_base=*/{0, 2, 1, 3}, /*prologue_end=*/0, /*epilogue_begin=*/3,
        /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts, mm,
        /*allow_pure_reader_reg_dead_transfer=*/false, PostReloadStackLayout{}));
  }

  // 2. Positive (`local-alloc.c:575, 580, 589-599, 1051-1055`): `sched2` hoisted
  //    `REG_DEAD $s0` (`[1]`) ahead of `REG_DEAD $a0` (`[2]`, base of `[0]`'s
  //    load `(set $s0 (mem $a0))` without `REG_EQUIV`). Restoring `[2]` into
  //    `[load_pos, cand_dst_death_pos)` (`cand = {0, 2, 1, 3}`) moves base
  //    death `d = 2` into `validate_equiv_mem`'s scan window (`bv = 1 -> cv = 0`)
  //    and passes `PreservesPostReloadOrderSafety`.
  {
    const std::vector<Insn> base_block = {
        MakeInsn("(set (reg:SI 16 s0) (mem:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 16 s0) (const_int 5))) "
                 "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                 1),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 4))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
                 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
    EXPECT_TRUE(PreservesPostReloadOrderSafety(
        base_block, /*cand_in_base=*/{0, 2, 1, 3, 4}, /*prologue_end=*/0, /*epilogue_begin=*/4,
        /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts, mm));

    // Negative: when `[0]` actually carries `(REG_EQUIV (mem:SI (reg:SI 4 a0)))`,
    // moving `REG_DEAD $a0` (`[2]`) into `[0, 1]` invalidates the present
    // `REG_EQUIV` and must be rejected.
    std::vector<Insn> with_equiv = base_block;
    with_equiv[0] = MakeInsn(
        "(set (reg:SI 16 s0) (mem:SI (reg:SI 4 a0))) "
        "(expr_list:REG_EQUIV (mem:SI (reg:SI 4 a0)) (nil))",
        0);
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        with_equiv, /*cand_in_base=*/{0, 2, 1, 3, 4}, /*prologue_end=*/0, /*epilogue_begin=*/4,
        /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts, mm));
  }

  // 3. Negative (Inc 34 tightening): `bv = 1 -> cv = 0` caused by a memory store
  //    moving into `[load_pos, cand_dst_death_pos)` while `REG_DEAD $a0` stays
  //    OUTSIDE `[load_pos, cand_dst_death_pos)` (`d_pos >= cand_dst_death_pos`)
  //    must NOT be exempted by `HasBaseRegDeathMovedIntoValidateEquivWindow`.
  {
    const std::vector<Insn> base_block = {
        MakeInsn("(set (reg:SI 16 s0) (mem:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 16 s0) (const_int 5))) "
                 "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                 1),
        MakeInsn("(set (mem:SI (reg:SI 5 a1)) (reg:SI 6 a2))", 2),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 4))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 3),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
                 4),
        MakeInsn("(use (reg/i:SI 2 v0))", 5),
    };
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        base_block, /*cand_in_base=*/{0, 2, 1, 3, 4, 5}, /*prologue_end=*/0, /*epilogue_begin=*/5,
        /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts, mm));
  }

  // 4. Positive + negative Stage 0 (`StripSched2SpuriousLoadMemRegEquivNotes`):
  //    trace a `-O2 -mips1` high-pressure FP leaf function where `sched2` hoists
  //    `REG_DEAD` of a loaded value ahead of `REG_DEAD $a0` (the load's base).
  //    Attaching a spurious Stage-0 `REG_EQUIV` onto that load causes
  //    `AttributeSched2PureReaderRegDeadNotes` to strip it when recovering the
  //    `kSched1`-achievable preimage (positive), while preserving a valid
  //    `REG_EQUIV` on an earlier load whose `REG_DEAD` precedes `REG_DEAD $a0`
  //    even in the `kSched1` preimage (negative contrast).
  {
    const std::string src = R"(
float SpuriousLoadEquivLeaf(const float* f, float* out) {
  float x0 = f[0] * 1.25f + 0.5f;
  float x1 = f[1] * 1.5f - 0.25f;
  float x2 = f[2] * 0.75f + 1.0f;
  float x3 = f[3] * 2.0f - 1.5f;
  float x4 = f[4] + x0 * 0.125f;
  float x5 = f[5] - x1 * 0.125f;
  float x6 = f[6] + x2 * 0.25f;
  float x7 = f[7] - x3 * 0.25f;
  float x8 = f[8] * x4 + x5;
  float x9 = f[9] * x6 - x7;
  float cross = (x0 * x9) + (x1 * x8) + (x2 * x7) + (x3 * x6) + (x4 * x5);
  out[0] = (x0 + x5) * (x1 + x6) + (x2 + x7) * (x3 + x8);
  out[1] = (x4 - x9) + (x0 - x8) + (x1 - x7);
  return cross + x0 + x9;
}
)";
    GccContext ctx;
    FunctionHooks hooks(&ctx);
    const CompilationProfile prof{.optimize_level = 2,
                                  .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
    const FunctionCompilationTrace trace =
        hooks.TraceForwardCompilation(src, "SpuriousLoadEquivLeaf", prof.c_flags, "spurious_equiv");
    bool checked = false;
    for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id != SubfunctionId::kScheduleBlockSched2) {
        continue;
      }
      checked = true;
      RtlControlFlowGraph cfg = snap.post_state;
      ASSERT_FALSE(cfg.rpo_blocks.empty());
      int dead_a0_orig_idx = -1;
      int spurious_idx = -1;
      int last_a0_idx = -1;
      int valid_idx = -1;
      for (size_t i = 0; i < cfg.rpo_blocks[0].instructions.size(); ++i) {
        const std::string& s = cfg.rpo_blocks[0].instructions[i];
        if (s.find("(mem/s/u:SF (plus:SI (reg/v:SI 4 a0) (const_int 32)))") != std::string::npos) {
          dead_a0_orig_idx = static_cast<int>(i);
        } else if (s.find("(mem/s/u:SF (plus:SI (reg/v:SI 4 a0) (const_int 28)))") !=
                   std::string::npos) {
          spurious_idx = static_cast<int>(i);
        } else if (s.find("(mem/s/u:SF (plus:SI (reg/v:SI 4 a0) (const_int 20)))") !=
                   std::string::npos) {
          last_a0_idx = static_cast<int>(i);
        } else if (s.find("REG_EQUIV (mem/u:SF (reg/v:SI 4 a0))") != std::string::npos) {
          valid_idx = static_cast<int>(i);
        }
      }
      ASSERT_GE(dead_a0_orig_idx, 0) << cfg.FormatSummary();
      ASSERT_GE(spurious_idx, 0) << cfg.FormatSummary();
      ASSERT_GE(last_a0_idx, 0) << cfg.FormatSummary();
      // Mirror binary-lifted Stage 0 input: `(REG_DEAD a0)` sits on the
      // post-`sched2` last reader (`const_int 20`, sunk past `const_int 32`)
      // and `const_int 28` carries a spurious Stage-0 `REG_EQUIV`.
      {
        auto pn32 = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[dead_a0_orig_idx]);
        auto pn28 = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[spurious_idx]);
        auto pn20 = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[last_a0_idx]);
        ASSERT_TRUE(pn32.ok() && pn28.ok() && pn20.ok());
        std::vector<RtlRegNote> kept32;
        for (const RtlRegNote& n : pn32->reg_notes) {
          if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() && n.datum.RegNo() == 4) {
            pn20->reg_notes.push_back(n);
          } else {
            kept32.push_back(n);
          }
        }
        pn28->reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegEquiv,
            .list_op = RtlOp::kExprList,
            .datum = pn28->pattern.SetSrc(),
        });
        cfg.rpo_blocks[0].instructions[dead_a0_orig_idx] =
            FormatPatternAndNotes(pn32->pattern, kept32);
        cfg.rpo_blocks[0].instructions[spurious_idx] =
            FormatPatternAndNotes(pn28->pattern, pn28->reg_notes);
        cfg.rpo_blocks[0].instructions[last_a0_idx] =
            FormatPatternAndNotes(pn20->pattern, pn20->reg_notes);
      }
      cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      AttributeSched2PureReaderRegDeadNotes(&cfg, prof);
      EXPECT_EQ(cfg.rpo_blocks[0].instructions[static_cast<size_t>(spurious_idx)].find("REG_EQUIV"),
                std::string::npos)
          << cfg.rpo_blocks[0].instructions[static_cast<size_t>(spurious_idx)];
      if (valid_idx >= 0) {
        EXPECT_NE(cfg.rpo_blocks[0].instructions[static_cast<size_t>(valid_idx)].find("REG_EQUIV"),
                  std::string::npos)
            << cfg.rpo_blocks[0].instructions[static_cast<size_t>(valid_idx)];
      }
    }
    EXPECT_TRUE(checked);
  }
}
TEST(SchedStage0AttributionTest, AttributeSched2SplitRmwLoadRegEquivRecoveryPositiveAndNegative) {
  // `local-alloc.c:1021, 1049-1055`: when `SelectMinimalCombineRegsRmwSplits`
  // attaches `(REG_DEAD r)` to a self-RMW whose producer is a `MEM` load
  // `(set (reg r) (mem/u ...))` without an existing `REG_EQUIV` note (skipped
  // by `SynthesizePostReloadMemRegEquivNotes` as multi-set even though `r`
  // dies later in `base_block`), `AttributeSched2PureReaderRegDeadNotes` must
  // recover the load `REG_EQUIV` note when the RMW is split (positive) and must
  // NOT attach `REG_EQUIV` when no RMW split occurs (negative).
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const CompilationProfile no_s1_prof{
      .optimize_level = 2,
      .c_flags = {"-O2", "-fno-schedule-insns", "-mips1", "-funsigned-char", "-Wa,-O2"}};

  auto make_cfg = []() {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    // Two independent chains: `v1 = mem/u[a0]; v1 = v1 + 1` (unsplit self-RMW
    // without `REG_DEAD v1`, so `v1` is multi-set until split) interleaved with
    // `t0 = mem/u[a1]; t0 = t0 + 2` (single-set with `REG_DEAD t0`).
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (mem/u:SI (reg:SI 5 a1)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (mem/u:SI (reg:SI 4 a0)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 8 t0) (plus:SI (reg:SI 8 t0) (const_int 2))) "
        "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 3 v1))) "
        "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
    return cfg;
  };

  // Positive (`-O2`): splitting the self-RMW on `v1` restores both
  // `(REG_DEAD v1)` on `v1 = v1 + 1` and `(REG_EQUIV (mem/u:SI (reg:SI 4 a0)))`
  // on the `v1` load.
  {
    RtlControlFlowGraph cfg = make_cfg();
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV (mem/u:SI (reg:SI 4 a0))"),
              std::string::npos)
        << cfg.FormatSummary();
    EXPECT_NE(cfg.rpo_blocks[0].instructions[3].find("REG_DEAD (reg:SI 3 v1)"), std::string::npos)
        << cfg.FormatSummary();
  }

  // Negative (`-fno-schedule-insns`): `AttributeSched2PureReaderRegDeadNotes`
  // does not run and neither `REG_DEAD` nor `REG_EQUIV` is attached.
  {
    RtlControlFlowGraph cfg = make_cfg();
    AttributeSched2PureReaderRegDeadNotes(&cfg, no_s1_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV"), std::string::npos)
        << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[3].find("REG_DEAD"), std::string::npos)
        << cfg.FormatSummary();
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesSkipsCrossBankFprReloadDeadOnCallDelaySlot) {
  // `reload1.c:5641, 6102-6130` (`emit_reload_insns`; MIPS does not define
  // `PRESERVE_DEATH_INFO_REGNO_P`): when `BuildStage0UnpackedDelayViewCfg`
  // restores `REG_DEAD` notes from `ReconstructRtlLiveness` onto an unpacked
  // call delay slot that carried a `(REG_DEAD (const_int 0))` placeholder, it
  // must NOT attach `(REG_DEAD fpr)` for a cross-bank `"f"` GPR->FPR (`mtc1`)
  // input reload scratch register (positive), whereas a direct memory load into
  // the same FPR DOES retain `(REG_DEAD fpr)` (negative).
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  auto make_cfg = [](bool use_cross_bank_mtc1_reload) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.basic_block_live_at_start[0] = {4, 5, 6, 29, 44};
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    // Symmetric pair of FP chains (`$f20` already split with `REG_DEAD $f20` vs
    // `$f30` unsplit self-RMW in the call delay slot) so `sched1` achievability
    // requires splitting `$f30` at the delay slot and committing `updated_base`
    // notes back into the `(sequence [(call_insn ...) (insn ...)])`.
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 52 $f20) (mem/u:SF (reg:SI 6 a2)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 62 $f30) (mem/u:SF (reg:SI 4 a0)))");
    if (use_cross_bank_mtc1_reload) {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 40 $f8) (reg:SF 5 a1))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 40 $f8) (mem/u:SF (reg:SI 5 a1)))");
    }
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 44 $f12))) "
        "(expr_list:REG_DEAD (reg:SF 52 $f20) (expr_list:REG_DEAD (reg:SF 44 $f12) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(sequence [(call_insn (parallel[ "
        "(call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
        "(clobber (reg:SI 31 ra)) ] ) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))) "
        "(insn (set (reg:SF 62 $f30) (plus:SF (reg:SF 62 $f30) (reg:SF 40 $f8))) "
        "(expr_list:REG_DEAD (const_int 0) (nil)))])");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 52 $f20) (reg:SF 62 $f30))) "
        "(expr_list:REG_DEAD (reg:SF 52 $f20) (expr_list:REG_DEAD (reg:SF 62 $f30) (nil)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))");
    cfg.rpo_blocks[0].rtl_insns[4].has_explicit_call_fusage = true;
    auto fusage_pn = ParsePatternAndNotes("(use (reg:SI 5 a1))");
    if (fusage_pn.ok()) {
      cfg.rpo_blocks[0].rtl_insns[4].call_fusage = {fusage_pn->pattern};
    }
    return cfg;
  };

  // Positive: cross-bank `mtc1` input reload `(set (reg:SF 40 $f8) (reg:SF 5 a1))`
  // commits `(REG_DEAD (reg:SF 62 $f30))` onto the delay slot WITHOUT injecting
  // a spurious `(REG_DEAD (reg:SF 40 $f8))`.
  {
    RtlControlFlowGraph cfg = make_cfg(/*use_cross_bank_mtc1_reload=*/true);
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    const std::string& seq_insn = cfg.rpo_blocks[0].instructions[4];
    EXPECT_NE(seq_insn.find("REG_DEAD (reg:SF 62 $f30)"), std::string::npos) << seq_insn;
    EXPECT_EQ(seq_insn.find("REG_DEAD (reg:SF 40 $f8)"), std::string::npos) << seq_insn;
  }

  // Negative: a direct memory load `(set (reg:SF 40 $f8) (mem/u:SF (reg:SI 5 a1)))`
  // is NOT a cross-bank `mtc1` input reload, so `(REG_DEAD (reg:SF 40 $f8))`
  // MUST be restored onto the delay slot alongside `(REG_DEAD (reg:SF 62 $f30))`.
  {
    RtlControlFlowGraph cfg = make_cfg(/*use_cross_bank_mtc1_reload=*/false);
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    const std::string& seq_insn = cfg.rpo_blocks[0].instructions[4];
    EXPECT_NE(seq_insn.find("REG_DEAD (reg:SF 62 $f30)"), std::string::npos) << seq_insn;
    EXPECT_NE(seq_insn.find("REG_DEAD (reg:SF 40 $f8)"), std::string::npos) << seq_insn;
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesSynthesizesB0Jump2DeletedPreCallArgCopy) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  auto make_cfg = [](bool clobber_a0_before_call) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    RtlBasicBlock& blk = cfg.rpo_blocks[0];
    blk.id = 0;
    blk.AddPatternOnly("(set (reg:SI 16 s0) (reg:SI 4 a0))");
    blk.AddPatternOnly("(set (reg:SF 54 $f22) (mem:SF (reg:SI 16 s0)))");
    blk.AddPatternOnly("(set (reg:SF 52 $f20) (mem:SF (plus:SI (reg:SI 16 s0) (const_int 4))))");
    if (clobber_a0_before_call) {
      blk.AddPatternOnly("(set (reg:SI 4 a0) (const_int 99))");
    }
    blk.AddPatternOnly(
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"step\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    blk.AddPatternOnly(
        "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22))) "
        "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil))");
    blk.AddPatternOnly(
        "(set (mem:SF (plus:SI (reg:SI 16 s0) (const_int 8))) (reg:SF 52 $f20)) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SF 52 $f20) (nil)))");
    blk.AddPatternOnly("(use (reg/i:SF 32 $f0))");
    const size_t call_idx = clobber_a0_before_call ? 4u : 3u;
    blk.rtl_insns[call_idx].has_explicit_call_fusage = true;
    blk.rtl_insns[call_idx].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
    };
    return cfg;
  };

  // Positive (`jump.c:434-465` + `sched.c:1730-1733`): when `jump2` deleted the
  // redundant pre-call `(set a0 s0)` copy (leaving `(set s0 a0)` without
  // `(REG_DEAD a0)`), `AttributeSched2PureReaderRegDeadNotes` synthesizes the
  // copy only in its temporary view so post-call RMW `(REG_DEAD $f20)` is
  // attributed without leaking the synthetic copy or a spurious `(REG_DEAD a0)`
  // back into `cfg.rpo_blocks[0]`.
  {
    RtlControlFlowGraph pos_cfg = make_cfg(/*clobber_a0_before_call=*/false);
    const size_t orig_len = pos_cfg.rpo_blocks[0].instructions.size();
    AttributeSched2PureReaderRegDeadNotes(&pos_cfg, o2_prof);
    ASSERT_EQ(pos_cfg.rpo_blocks[0].instructions.size(), orig_len);
    EXPECT_EQ(pos_cfg.rpo_blocks[0].instructions[0].find("REG_DEAD (reg:SI 4 a0)"),
              std::string::npos)
        << pos_cfg.rpo_blocks[0].instructions[0];
    EXPECT_NE(pos_cfg.rpo_blocks[0].instructions[4].find("REG_DEAD (reg:SF 52 $f20)"),
              std::string::npos)
        << pos_cfg.rpo_blocks[0].instructions[4];
  }

  // Negative: when `$a0` is overwritten by `(set a0 99)` before the call,
  // `SynthesizeB0Jump2DeletedPreCallArgCopiesInDelayView` does not synthesize
  // a redundant copy and never mutates the instruction count.
  {
    RtlControlFlowGraph neg_cfg = make_cfg(/*clobber_a0_before_call=*/true);
    const size_t orig_len = neg_cfg.rpo_blocks[0].instructions.size();
    AttributeSched2PureReaderRegDeadNotes(&neg_cfg, o2_prof);
    EXPECT_EQ(neg_cfg.rpo_blocks[0].instructions.size(), orig_len);
    EXPECT_EQ(neg_cfg.rpo_blocks[0].instructions[0].find("REG_DEAD (reg:SI 4 a0)"),
              std::string::npos);
  }

  // Positive 2 (delay-slot lower-arg ordering): when `$a1` and `$a2` are saved
  // to callee-saved GPRs before `sw $31` and `$a0 = 1` sits in the `jal` delay
  // slot (after `sw $31`), the synthesized single-cycle GPR->GPR copies must
  // stay after `$a0 = 1` in ascending arg order (`$a1`, `$a2`) so Stage 0
  // attributes `(REG_DEAD s0)` on the post-call `and` RMW without leaking
  // synthetic copies or `(REG_DEAD a1)`/`(REG_DEAD a2)`.
  {
    const CompilationProfile mips2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.frame_size_bytes = 32;
    cfg.rpo_blocks.resize(2);
    RtlBasicBlock& blk = cfg.rpo_blocks[0];
    blk.id = 0;
    blk.successors = {1};
    blk.AddPatternOnly("(set (reg/f:SI 29 sp) (plus:SI (reg/f:SI 29 sp) (const_int -32)))");
    blk.AddPatternOnly("(set (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 20))) (reg:SI 17 s1))");
    blk.AddPatternOnly(
        "(set (reg:SI 17 s1) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    blk.AddPatternOnly("(set (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 24))) (reg:SI 18 s2))");
    blk.AddPatternOnly("(set (reg:SI 18 s2) (reg:SI 5 a1))");
    blk.AddPatternOnly("(set (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 16))) (reg:SI 16 s0))");
    blk.AddPatternOnly("(set (reg:SI 16 s0) (reg:SI 6 a2))");
    blk.AddPatternOnly("(set (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 28))) (reg:SI 31 ra))");
    blk.AddPatternOnly(
        "(sequence[ (call_insn (parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI "
        "(\"step\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
        "(insn (set (reg:SI 4 a0) (const_int 1))) ] )");
    blk.AddPatternOnly("(set (reg:SI 16 s0) (and:SI (reg:SI 16 s0) (const_int 2)))");
    blk.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 17 s1) (const_int 4))) (reg:SI 18 s2))");
    blk.AddPatternOnly(
        "(set (mem:HI (plus:SI (reg:SI 17 s1) (const_int 8))) (subreg:HI (reg:SI 2 v0) 0))");
    blk.AddPatternOnly(
        "(sequence[ (jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 16 s0) (const_int 0)) "
        "(label_ref L1) (pc))) (expr_list:REG_DEAD (reg:SI 16 s0) (nil))) "
        "(insn (set (mem:SI (reg:SI 17 s1)) (reg:SI 2 v0)) "
        "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))) ] )");
    RtlBasicBlock& b1 = cfg.rpo_blocks[1];
    b1.id = 1;
    b1.AddPatternOnly("(set (reg:SI 4 a0) (reg:SI 18 s2))");
    b1.AddPatternOnly("(set (reg:SI 5 a1) (reg:SI 2 v0))");
    b1.AddPatternOnly(
        "(parallel[ (call (mem:SI (symbol_ref:SI (\"notify\"))) (const_int 16)) "
        "(clobber (reg:SI 31 ra)) ] )");
    b1.AddPatternOnly("(set (reg:SI 31 ra) (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 28))))");
    b1.AddPatternOnly("(set (reg:SI 18 s2) (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 24))))");
    b1.AddPatternOnly("(set (reg:SI 17 s1) (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 20))))");
    b1.AddPatternOnly("(set (reg:SI 16 s0) (mem:SI (plus:SI (reg/f:SI 29 sp) (const_int 16))))");
    b1.AddPatternOnly("(set (reg/f:SI 29 sp) (plus:SI (reg/f:SI 29 sp) (const_int 32)))");
    b1.AddPatternOnly("(return)");
    const size_t orig_len = blk.instructions.size();
    AttributeSched2PureReaderRegDeadNotes(&cfg, mips2_prof);
    ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), orig_len);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[4].find("REG_DEAD (reg:SI 5 a1)"), std::string::npos);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[6].find("REG_DEAD (reg:SI 6 a2)"), std::string::npos);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[9].find("REG_DEAD (reg:SI 16 s0)"), std::string::npos)
        << cfg.rpo_blocks[0].instructions[9];
  }
}
TEST(SchedStage0AttributionTest, AttributeSched2ErasedMemAliasFlagsPositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  // Positive (`varasm.c:601-604`, `expr.c:4534, 4571-4586, 4889`, `sched.c:817-882`):
  // Compile a small synthetic function combining a read-only static table
  // (`k_ro_tbl`), a read-only struct pointer (`r`), and a writable pointer
  // (`out`) modified both directly (`*out`) and through a derived index
  // (`out[sel & 3]`). Erase `/s`/`/u` from its `.dbr` CFG (mirroring `final.c`),
  // and verify:
  //   - Negative 1: `-O1` is a complete no-op.
  //   - Positive: `-O2` recovers minimal `/u`/`/s` flags, preserves `kSched2`
  //     achievability, and never marks derived-written `out` (`a1`) as `/u`.
  //   - Negative 2: running a second time on the attributed CFG is a no-op.
  {
    constexpr std::string_view kSrc = R"c(
struct Rec {
  int a;
  int b;
  int c;
  int d;
};
extern const int k_ro_tbl[4];
int SynthAliasSched(const struct Rec* r, int* out, int sel) {
  int t0, t1, t2, t3, prev;
  t0 = r->a + k_ro_tbl[0];
  *out = t0 + 1;
  t1 = r->b + k_ro_tbl[1];
  out[sel & 3] = t1 * 3;
  t2 = r->c + k_ro_tbl[2];
  prev = *out;
  *out = t2 + t1;
  t3 = r->d + k_ro_tbl[3];
  return (t0 + t1) * (t2 + t3) + prev;
}
)c";
    GccContext ctx;
    FunctionHooks hooks(&ctx);
    const FunctionCompilationTrace trace =
        hooks.TraceForwardCompilation(kSrc, "SynthAliasSched", o2_prof.c_flags);
    const CompilationProfile& prof = trace.stage0_input.profile;
    const auto dbr_it = trace.pass_snapshots.find(RtlPassKind::kDbr);
    ASSERT_NE(dbr_it, trace.pass_snapshots.end());
    ASSERT_FALSE(dbr_it->second.rpo_blocks.empty());

    auto strip_alias_flags = [](std::string s) {
      for (const char* tok : {"mem/s/u:", "mem/u:", "mem/s:"}) {
        size_t pos = 0;
        while ((pos = s.find(tok, pos)) != std::string::npos) {
          s.replace(pos, std::strlen(tok), "mem:");
        }
      }
      return s;
    };

    RtlControlFlowGraph cfg = dbr_it->second;
    for (RtlBasicBlock& blk : cfg.rpo_blocks) {
      for (std::string& line : blk.instructions) {
        line = strip_alias_flags(std::move(line));
      }
      blk.SyncRtlInsnsFromInstructions();
    }

    // Negative 1: `-O1` is a complete no-op.
    RtlControlFlowGraph o1_cfg = cfg;
    EXPECT_FALSE(AttributeSched2ErasedMemAliasFlags(&o1_cfg, o1_prof));
    EXPECT_EQ(o1_cfg.rpo_blocks[0].instructions, cfg.rpo_blocks[0].instructions);

    // Positive: `-O2` recovers erased `/u` / `/s` alias flags.
    EXPECT_TRUE(AttributeSched2ErasedMemAliasFlags(&cfg, prof));
    bool saw_recovered_flag = false;
    for (const std::string& line : cfg.rpo_blocks[0].instructions) {
      if (line.find("mem/u:") != std::string::npos || line.find("mem/s:") != std::string::npos ||
          line.find("mem/s/u:") != std::string::npos) {
        saw_recovered_flag = true;
      }
      // Negative 2 (derived-pointer write suppression): `out` (`a1`) is written
      // via derived `(i0 << 2) + a1`, so `a1` must never get `/u`.
      if (line.find("(reg:SI 5 a1)") != std::string::npos &&
          line.find("(mem") != std::string::npos) {
        EXPECT_EQ(line.find("mem/u:"), std::string::npos) << line;
        EXPECT_EQ(line.find("mem/s/u:"), std::string::npos) << line;
      }
    }
    EXPECT_TRUE(saw_recovered_flag);

    // Negative 3: running a second time on the attributed CFG is a strict no-op.
    const std::vector<std::string> after_first = cfg.rpo_blocks[0].instructions;
    EXPECT_FALSE(AttributeSched2ErasedMemAliasFlags(&cfg, prof));
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, after_first);
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesStripsStaleSunkStoreRegEquiv) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  // Positive (`local-alloc.c:540-603, 990-1000`, `sched.c:3764-3830`):
  // Compile a small synthetic function with a local stack array where `sched2`
  // sinks a store `buf[5] = v` past an earlier pure reader of `v`. Simulate
  // Stage 0 liveness placing `(REG_DEAD v)` on the sunk store and synthesizing
  // a spurious Rule-A `REG_EQUIV (mem/s:SI (plus:SI (reg:SI 30 $fp) (const_int ...)))`
  // on `v`'s def, then verify `AttributeSched2PureReaderRegDeadNotes` transfers
  // `(REG_DEAD v)` back to the pure reader AND strips the stale `"$fp"` `REG_EQUIV`.
  {
    constexpr std::string_view kSrc = R"c(
static int g_acc = 7;
int SynthSunkLocalStore(const int* in, int* out, int idx) {
  int buf[6];
  int x0 = in[0];
  int x1 = in[1];
  int x2 = in[2];
  int x3 = in[3];
  buf[0] = x0 * 3 + g_acc;
  buf[1] = x1 ^ (buf[0] + idx);
  buf[2] = x2 + buf[1];
  buf[3] = x3 - buf[0];
  out[idx & 3] = buf[2] ^ buf[3];
  buf[4] = out[0] + buf[1];
  buf[5] = buf[4] + buf[3];
  g_acc = buf[5] ^ x0;
  return buf[0] - buf[5] + buf[2];
}
)c";
    GccContext ctx;
    FunctionHooks hooks(&ctx);
    const FunctionCompilationTrace trace =
        hooks.TraceForwardCompilation(kSrc, "SynthSunkLocalStore", o2_prof.c_flags);
    const auto dbr_it = trace.pass_snapshots.find(RtlPassKind::kDbr);
    ASSERT_NE(dbr_it, trace.pass_snapshots.end());
    ASSERT_FALSE(dbr_it->second.rpo_blocks.empty());

    // Find a local stack store `(set (mem/s:SI (plus (reg 29 sp) off)) (reg r))`
    // that was sunk after a pure reader of `r` (so `r` carries `REG_DEAD` on
    // the earlier reader in `.dbr`, NOT on the store).
    RtlControlFlowGraph cfg = dbr_it->second;
    RtlBasicBlock& blk = cfg.rpo_blocks[0];
    std::vector<RtlPatternAndNotes> pns;
    pns.reserve(blk.instructions.size());
    for (const std::string& line : blk.instructions) {
      auto parsed = ParsePatternAndNotes(line);
      ASSERT_TRUE(parsed.ok()) << line;
      pns.push_back(std::move(*parsed));
    }
    int def_idx = -1;
    int reader_idx = -1;
    int store_idx = -1;
    int target_reg = -1;
    int64_t stack_off = -1;
    for (int j = static_cast<int>(pns.size()) - 1; j >= 1; --j) {
      const RtlExpr& pat = pns[static_cast<size_t>(j)].pattern;
      if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kMem ||
          pat.SetSrc().op != RtlOp::kReg || pat.SetSrc().IsArgPointerReg()) {
        continue;
      }
      const RtlExpr& addr = pat.SetDest().children[0];
      if (addr.op != RtlOp::kPlus || addr.children.size() != 2 ||
          addr.children[0].op != RtlOp::kReg || addr.children[0].RegNo() != 29 ||
          addr.children[1].op != RtlOp::kConstInt) {
        continue;
      }
      const int r = pat.SetSrc().RegNo();
      int found_reader = -1;
      int found_def = -1;
      for (int i = j - 1; i >= 0; --i) {
        const auto& pn = pns[static_cast<size_t>(i)];
        if (pn.pattern.op == RtlOp::kSet && pn.pattern.SetDest().op == RtlOp::kReg &&
            pn.pattern.SetDest().RegNo() == r) {
          found_def = i;
          break;
        }
        for (const RtlRegNote& note : pn.reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
              note.datum.RegNo() == r) {
            found_reader = i;
          }
        }
      }
      if (found_reader >= 0 && found_def >= 0) {
        def_idx = found_def;
        reader_idx = found_reader;
        store_idx = j;
        target_reg = r;
        stack_off = addr.children[1].int_val;
        break;
      }
    }
    ASSERT_GE(store_idx, 0);
    ASSERT_GE(reader_idx, 0);
    ASSERT_GE(def_idx, 0);

    // Move `(REG_DEAD r)` from `reader_idx` to `store_idx` and attach the stale
    // Stage-0 Rule-A `REG_EQUIV (mem/s:SI (plus:SI (reg:SI 30 $fp) (const_int off)))`
    // onto `def_idx` (spelling hard reg 30 as `"$fp"`).
    auto& reader_notes = pns[static_cast<size_t>(reader_idx)].reg_notes;
    RtlRegNote moved_dead;
    for (auto it = reader_notes.begin(); it != reader_notes.end(); ++it) {
      if (it->kind == RtlMachineMode::kRegDead && it->datum.op == RtlOp::kReg &&
          it->datum.RegNo() == target_reg) {
        moved_dead = *it;
        reader_notes.erase(it);
        break;
      }
    }
    pns[static_cast<size_t>(store_idx)].reg_notes.push_back(moved_dead);
    RtlRegNote stale_equiv;
    stale_equiv.kind = RtlMachineMode::kRegEquiv;
    stale_equiv.list_op = RtlOp::kExprList;
    RtlExpr fp_reg = RtlExpr::MakeReg(RtlMachineMode::kSi, 30);
    fp_reg.str_val = "$fp";
    RtlExpr eq_mem =
        RtlExpr::MakeMem(RtlMachineMode::kSi,
                         RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi, std::move(fp_reg),
                                             RtlExpr::MakeConstInt(stack_off)));
    eq_mem.flags.in_struct = true;
    stale_equiv.datum = std::move(eq_mem);
    pns[static_cast<size_t>(def_idx)].reg_notes.push_back(std::move(stale_equiv));
    for (int idx : {def_idx, reader_idx, store_idx}) {
      blk.instructions[static_cast<size_t>(idx)] =
          pns[static_cast<size_t>(idx)].ToGccPrintRtlString();
    }
    blk.SyncRtlInsnsFromInstructions();

    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    const std::string& def_line = cfg.rpo_blocks[0].instructions[static_cast<size_t>(def_idx)];
    const std::string& rdr_line = cfg.rpo_blocks[0].instructions[static_cast<size_t>(reader_idx)];
    const std::string& st_line = cfg.rpo_blocks[0].instructions[static_cast<size_t>(store_idx)];
    const std::string dead_needle = absl::StrCat("REG_DEAD (reg:SI ", target_reg, " ");
    EXPECT_NE(rdr_line.find(dead_needle), std::string::npos) << rdr_line;
    EXPECT_EQ(st_line.find(dead_needle), std::string::npos) << st_line;
    EXPECT_EQ(def_line.find("REG_EQUIV"), std::string::npos) << def_line;
  }

  // Negative: when the store precedes the pure reader (no `sched2` sunk-store
  // `REG_DEAD` transfer), a valid Rule-A `REG_EQUIV` note must be preserved.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.frame_size_bytes = 32;
    cfg.rpo_blocks.resize(1);
    RtlBasicBlock& blk = cfg.rpo_blocks[0];
    blk.id = 0;
    blk.AddPatternOnly("(set (reg/f:SI 29 sp) (plus:SI (reg/f:SI 29 sp) (const_int -32)))");
    blk.AddPatternOnly(
        "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 7))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) "
        "(expr_list:REG_EQUIV (mem/s:SI (plus:SI (reg:SI 30 $fp) (const_int 20))) (nil)))");
    blk.AddPatternOnly("(set (mem/s:SI (plus:SI (reg/f:SI 29 sp) (const_int 20))) (reg:SI 3 v1))");
    blk.AddPatternOnly(
        "(set (reg/i:SI 2 v0) (minus:SI (reg:SI 5 a1) (reg:SI 3 v1))) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
    blk.AddPatternOnly("(set (reg/f:SI 29 sp) (plus:SI (reg/f:SI 29 sp) (const_int 32)))");
    blk.AddPatternOnly("(use (reg/i:SI 2 v0))");
    blk.AddPatternOnly("(return)");

    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV"), std::string::npos)
        << cfg.rpo_blocks[0].instructions[1];
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2ErasedMemAliasFlagsSuppressesDerivedPtrFromWrittenBase) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  // Negative (`sched.c:817-882`): reading via a pointer `q` derived from a
  // written base `a0` (`q = a0 + idx` or `q = a0 - idx`) must NEVER mark
  // loads through `q` as `/u`, whether the base write occurs before OR after
  // the derived read.
  constexpr std::string_view kSrc = R"c(
struct Rec {
  const int a;
  const int b;
  const int c;
  const int d;
};
extern int g0;
int SynthDerivedStruct(const struct Rec* r, int k, int idx, int j) {
  int m, t_indep, t0;
  const struct Rec* q;
  m = idx * idx;
  t_indep = ((j ^ 7) + 9) ^ 3;
  g0 = k;
  q = r + (m & 3);
  t0 = q->a;
  t0 = ((t0 + 1) ^ 5) + 3;
  return t0 + t_indep + (r != 0);
}
)c";
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const FunctionCompilationTrace trace =
      hooks.TraceForwardCompilation(kSrc, "SynthDerivedStruct", o2_prof.c_flags);
  const CompilationProfile& prof = trace.stage0_input.profile;
  const auto dbr_it = trace.pass_snapshots.find(RtlPassKind::kDbr);
  ASSERT_NE(dbr_it, trace.pass_snapshots.end());
  ASSERT_FALSE(dbr_it->second.rpo_blocks.empty());

  auto strip_alias_flags = [](std::string s) {
    for (const char* tok : {"mem/s/u:", "mem/u:", "mem/s:"}) {
      size_t pos = 0;
      while ((pos = s.find(tok, pos)) != std::string::npos) {
        s.replace(pos, std::strlen(tok), "mem:");
      }
    }
    return s;
  };

  RtlControlFlowGraph base_cfg = dbr_it->second;
  for (RtlBasicBlock& blk : base_cfg.rpo_blocks) {
    for (std::string& line : blk.instructions) line = strip_alias_flags(std::move(line));
    blk.SyncRtlInsnsFromInstructions();
  }

  // Locate the register `q_reg` derived from `a0` (`(plus:SI ... (reg:SI 4 a0))`).
  int q_reg = -1;
  size_t derive_idx = 0;
  for (size_t i = 0; i < base_cfg.rpo_blocks[0].instructions.size(); ++i) {
    auto pn = ParsePatternAndNotes(base_cfg.rpo_blocks[0].instructions[i]);
    ASSERT_TRUE(pn.ok());
    if (pn->pattern.op == RtlOp::kSet && pn->pattern.children.size() == 2 &&
        pn->pattern.SetDest().op == RtlOp::kReg && pn->pattern.SetSrc().op == RtlOp::kPlus &&
        pn->pattern.SetSrc().children.size() == 2 &&
        ((pn->pattern.SetSrc().children[0].op == RtlOp::kReg &&
          pn->pattern.SetSrc().children[0].RegNo() == 4) ||
         (pn->pattern.SetSrc().children[1].op == RtlOp::kReg &&
          pn->pattern.SetSrc().children[1].RegNo() == 4))) {
      q_reg = pn->pattern.SetDest().RegNo();
      derive_idx = i;
      break;
    }
  }
  ASSERT_GE(q_reg, 2);

  // Positive control: with `a0` unwritten, `AttributeSched2ErasedMemAliasFlags`
  // attributes `/u` on `q_reg` (`q->a` at offset 0).
  const std::string q_needle = absl::StrCat(":SI ", q_reg, " ");
  {
    RtlControlFlowGraph pos_cfg = base_cfg;
    EXPECT_TRUE(AttributeSched2ErasedMemAliasFlags(&pos_cfg, prof));
    bool saw_u_on_q = false;
    for (const std::string& line : pos_cfg.rpo_blocks[0].instructions) {
      if (line.find(q_needle) != std::string::npos &&
          (line.find("mem/u:") != std::string::npos ||
           line.find("mem/s/u:") != std::string::npos)) {
        saw_u_on_q = true;
      }
    }
    EXPECT_TRUE(saw_u_on_q);
  }

  auto assert_no_u_on_q = [&](const RtlControlFlowGraph& cfg) {
    for (const std::string& line : cfg.rpo_blocks[0].instructions) {
      if (line.find(q_needle) != std::string::npos && line.find("(mem") != std::string::npos) {
        EXPECT_EQ(line.find("mem/u:"), std::string::npos) << line;
        EXPECT_EQ(line.find("mem/s/u:"), std::string::npos) << line;
      }
    }
  };

  // Negative 1 (write to base `a0` BEFORE derived read, `kPlus`):
  // Insert a store `(set (mem:SI (reg:SI 4 a0)) (reg:SI 7 a3))` before
  // `derive_idx` so `a0` is written before `q_reg` is derived and read;
  // `q_reg` must NEVER get `/u`.
  {
    RtlControlFlowGraph cfg = base_cfg;
    auto& insns = cfg.rpo_blocks[0].instructions;
    insns.insert(insns.begin() + static_cast<ptrdiff_t>(derive_idx),
                 "(set (mem:SI (reg:SI 4 a0)) (reg:SI 7 a3))");
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    AttributeSched2ErasedMemAliasFlags(&cfg, prof);
    assert_no_u_on_q(cfg);
  }

  // Negative 2 (write to base `a0` AFTER derived read `q->a`, `kPlus`):
  // Insert a store `(set (mem:SI (reg:SI 4 a0)) (reg:SI 7 a3))` right after the
  // `q_reg` read (while `a0` still holds `r`, before `(r != 0)` clobbers `a0`);
  // `q_reg` must NEVER get `/u`.
  {
    RtlControlFlowGraph cfg = base_cfg;
    auto& insns = cfg.rpo_blocks[0].instructions;
    ASSERT_GT(insns.size(), derive_idx + 2);
    insns.insert(insns.begin() + static_cast<ptrdiff_t>(derive_idx + 2),
                 "(set (mem:SI (reg:SI 4 a0)) (reg:SI 7 a3))");
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    AttributeSched2ErasedMemAliasFlags(&cfg, prof);
    assert_no_u_on_q(cfg);
  }

  // Negative 3 (`kMinus` derivation `q = a0 - idx` with write to `a0` after read):
  // Rewrite `derive_idx` to `(minus:SI (reg:SI 4 a0) <idx>)` + post-read write to `a0`.
  {
    RtlControlFlowGraph cfg = base_cfg;
    auto pn = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[derive_idx]);
    ASSERT_TRUE(pn.ok());
    RtlExpr& src = pn->pattern.children[1];
    src.op = RtlOp::kMinus;
    if (src.children[0].RegNo() != 4) std::swap(src.children[0], src.children[1]);
    cfg.rpo_blocks[0].instructions[derive_idx] = pn->ToGccPrintRtlString();
    auto& insns = cfg.rpo_blocks[0].instructions;
    insns.insert(insns.begin() + static_cast<ptrdiff_t>(derive_idx + 2),
                 "(set (mem:SI (reg:SI 4 a0)) (reg:SI 7 a3))");
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    AttributeSched2ErasedMemAliasFlags(&cfg, prof);
    assert_no_u_on_q(cfg);
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2ErasedMemAliasFlagsSuppressesCrossBlockWrittenOrEscapedSymbol) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  // Negative (`varasm.c:601-604`): a static symbol `k_ro_tbl` whose address is
  // hoisted into a register in B0 and written (or returned via `(use (reg v0))`)
  // in successor B1 must NEVER receive `/u` in B0.
  constexpr std::string_view kSrc = R"c(
struct Rec {
  int a;
  int b;
  int c;
  int d;
};
extern const int k_ro_tbl[4];
int SynthCrossBlockRo(const struct Rec* r, int* out, int sel) {
  int t0, t1, t2, t3;
  const int* p = k_ro_tbl;
  t0 = r->a + p[0];
  *out = t0 + 1;
  t1 = r->b + p[1];
  *out = t1 + t0;
  t2 = r->c + p[2];
  *out = t2 + t1;
  t3 = r->d + p[3];
  if (sel != 0) {
    return (t0 + t1) * (t2 + t3) + p[sel & 3];
  }
  return (t0 - t1) + (t2 - t3);
}
)c";
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  const FunctionCompilationTrace trace =
      hooks.TraceForwardCompilation(kSrc, "SynthCrossBlockRo", o2_prof.c_flags);
  const CompilationProfile& prof = trace.stage0_input.profile;
  const auto dbr_it = trace.pass_snapshots.find(RtlPassKind::kDbr);
  ASSERT_NE(dbr_it, trace.pass_snapshots.end());
  ASSERT_GE(dbr_it->second.rpo_blocks.size(), 2u);

  auto strip_alias_flags = [](std::string s) {
    for (const char* tok : {"mem/s/u:", "mem/u:", "mem/s:"}) {
      size_t pos = 0;
      while ((pos = s.find(tok, pos)) != std::string::npos) {
        s.replace(pos, std::strlen(tok), "mem:");
      }
    }
    return s;
  };

  RtlControlFlowGraph base_cfg = dbr_it->second;
  for (RtlBasicBlock& blk : base_cfg.rpo_blocks) {
    for (std::string& line : blk.instructions) line = strip_alias_flags(std::move(line));
    blk.SyncRtlInsnsFromInstructions();
  }

  // Find the register holding `&k_ro_tbl` in B0 (`symbol_ref` or `lo_sum`).
  int sym_reg = -1;
  for (const std::string& line : base_cfg.rpo_blocks[0].instructions) {
    if (line.find("k_ro_tbl") != std::string::npos && line.find("(mem") == std::string::npos) {
      auto pn = ParsePatternAndNotes(line);
      if (pn.ok() && pn->pattern.op == RtlOp::kSet && pn->pattern.SetDest().op == RtlOp::kReg) {
        sym_reg = pn->pattern.SetDest().RegNo();
        break;
      }
    }
  }
  ASSERT_GE(sym_reg, 2);

  // Positive control: with `k_ro_tbl` only read across blocks, B0 recovers `/u`
  // on `k_ro_tbl` accesses.
  auto has_u_on_sym = [&](const RtlControlFlowGraph& cfg) {
    const std::string reg_needle = absl::StrCat("(reg:SI ", sym_reg, " ");
    for (const std::string& line : cfg.rpo_blocks[0].instructions) {
      if ((line.find("k_ro_tbl") != std::string::npos ||
           line.find(reg_needle) != std::string::npos) &&
          (line.find("mem/u:") != std::string::npos ||
           line.find("mem/s/u:") != std::string::npos)) {
        return true;
      }
    }
    return false;
  };
  {
    RtlControlFlowGraph pos_cfg = base_cfg;
    EXPECT_TRUE(AttributeSched2ErasedMemAliasFlags(&pos_cfg, prof));
    EXPECT_TRUE(has_u_on_sym(pos_cfg));
  }

  // Find a successor block reachable from B0 where `sym_reg` is used (or any
  // non-entry successor block of B0).
  ASSERT_FALSE(base_cfg.rpo_blocks[0].successors.empty());
  const size_t succ_b = static_cast<size_t>(base_cfg.rpo_blocks[0].successors[0]);
  ASSERT_LT(succ_b, base_cfg.rpo_blocks.size());

  // Negative 1: cross-block write via hoisted `sym_reg` in successor B1
  // suppresses `/u` on `k_ro_tbl` in B0.
  {
    RtlControlFlowGraph write_cfg = base_cfg;
    write_cfg.rpo_blocks[succ_b].instructions.insert(
        write_cfg.rpo_blocks[succ_b].instructions.begin(),
        absl::StrCat("(set (mem:SI (reg:SI ", sym_reg, " t0)) (reg:SI 2 v0))"));
    write_cfg.rpo_blocks[succ_b].SyncRtlInsnsFromInstructions();
    AttributeSched2ErasedMemAliasFlags(&write_cfg, prof);
    EXPECT_FALSE(has_u_on_sym(write_cfg));
  }

  // Negative 2: cross-block address escape via `(use (reg/i:SI 2 v0))` in B1
  // suppresses `/u` on `k_ro_tbl` in B0.
  {
    RtlControlFlowGraph esc_cfg = base_cfg;
    auto& sinsns = esc_cfg.rpo_blocks[succ_b].instructions;
    sinsns.insert(sinsns.begin(), absl::StrCat("(set (reg/i:SI 2 v0) (reg:SI ", sym_reg, " t0))"));
    sinsns.insert(sinsns.begin() + 1, "(use (reg/i:SI 2 v0))");
    esc_cfg.rpo_blocks[succ_b].SyncRtlInsnsFromInstructions();
    AttributeSched2ErasedMemAliasFlags(&esc_cfg, prof);
    EXPECT_FALSE(has_u_on_sym(esc_cfg));
  }
}
TEST(SchedStage0AttributionTest,
     AttributeSched2PureReaderRegDeadNotesRepairsRegEquivFreshDestCombineConflict) {
  const CompilationProfile o2_prof{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
  };
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Build a genuine `kSched1`-achievable pre-`sched2` block with a cross-call
  // reload spill slot `[sp+28]` (`reload1.c:2309-2400`) where post-call
  // `$t0 = $a2 + $t0` precedes `$s0 = $s0 + $a2 (REG_DEAD $a2)`. At `kSched2`,
  // the reload load `$t0 = mem[sp+28]` (load latency > 1) raises `$t0 = $a2 + $t0`'s
  // backward `kSched2` priority above `$s0 = $s0 + $a2`, hoisting `$s0 = $s0 + $a2`
  // ahead of `$t0 = $a2 + $t0`. At `kSched1` (where the spill load folds away),
  // BOTH orders tie in priority and are `kSched1`-achievable, so Stage 0 enters
  // `TryRepairCombineRegsPureReaderRegDeadConflicts` directly and repairs the
  // inverted `(REG_DEAD $a2)` iff `REG_EQUIV` proves `$t0` is a fresh single-set
  // pseudo (`flow.c:2506-2515`, `local-alloc.c:1021, 1266-1300, 1851`).
  auto build_post_sched2_cfg = [&](bool attach_reg_equiv_on_t0_add) {
    const std::string t0_add_pre =
        attach_reg_equiv_on_t0_add ? "(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (reg:SI 8 t0))) "
                                     "(expr_list:REG_EQUIV (mem/u:SI (reg:SI 18 s2)) (nil))"
                                   : "(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (reg:SI 8 t0)))";
    const std::string t0_add_post =
        attach_reg_equiv_on_t0_add ? "(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (reg:SI 8 t0))) "
                                     "(expr_list:REG_EQUIV (mem/u:SI (reg:SI 18 s2)) "
                                     "(expr_list:REG_DEAD (reg:SI 6 a2) (nil)))"
                                   : "(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (reg:SI 8 t0))) "
                                     "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))";
    const std::vector<std::string> pre_body_texts = {
        "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 32)))",
        "(set (reg:SI 10 t2) (xor:SI (reg:SI 4 a0) (const_int 7))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 10 t2)) "
        "(expr_list:REG_DEAD (reg:SI 10 t2) (nil))",
        "(call (mem:SI (symbol_ref:SI (\"helper_fn\"))) (const_int 16))",
        "(set (reg:SI 6 a2) (plus:SI (reg:SI 17 s1) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
        "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))",
        t0_add_pre,
        "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 6 a2))) "
        "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 8 t0))) "
        "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
        "(use (reg/i:SI 2 v0))",
    };
    std::vector<Insn> pre_sched2_insns;
    pre_sched2_insns.reserve(pre_body_texts.size());
    for (size_t i = 0; i < pre_body_texts.size(); ++i) {
      pre_sched2_insns.push_back(MakeInsn(pre_body_texts[i], static_cast<int>(i),
                                          (i == 3) ? Insn::Kind::kCall : Insn::Kind::kInsn));
    }

    RegFacts facts_s2;
    facts_s2.max_reg_num = 72;
    const DepGraph g_s2 = Analyze(pre_sched2_insns, facts_s2, mm, Phase::kSched2);
    const ScheduleResult s2_res =
        Schedule(pre_sched2_insns, g_s2, facts_s2, mm, Phase::kSched2, {});
    EXPECT_EQ(s2_res.order.size(), pre_body_texts.size());

    int rmw_post_idx = -1;
    int t0_add_post_idx = -1;
    std::vector<std::string> post_sched2_texts;
    post_sched2_texts.reserve(pre_body_texts.size() + 1);
    for (size_t p = 0; p < s2_res.order.size(); ++p) {
      const int orig = s2_res.order[p];
      if (orig == 7) {
        rmw_post_idx = static_cast<int>(post_sched2_texts.size());
        post_sched2_texts.push_back("(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 6 a2)))");
      } else if (orig == 6) {
        t0_add_post_idx = static_cast<int>(post_sched2_texts.size());
        post_sched2_texts.push_back(t0_add_post);
      } else {
        post_sched2_texts.push_back(pre_body_texts[static_cast<size_t>(orig)]);
      }
    }
    post_sched2_texts.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    EXPECT_GE(rmw_post_idx, 0);
    EXPECT_GT(t0_add_post_idx, rmw_post_idx);

    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 32;
    RtlBasicBlock blk;
    blk.id = 0;
    blk.instructions = std::move(post_sched2_texts);
    blk.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(std::move(blk));
    return std::tuple<RtlControlFlowGraph, int, int>{std::move(cfg), rmw_post_idx, t0_add_post_idx};
  };

  // Positive (`flow.c:2506-2515`, `local-alloc.c:1021, 1266-1300, 1851`): when
  // `(set t0 (plus a2 t0))` carries `REG_EQUIV`, `t0` is a fresh single-set
  // pseudo whose dying input `t0` had its `REG_DEAD` erased, so `(REG_DEAD a2)`
  // on operand 0 causes a Conflict Kind 1 false `combine_regs` merge. Stage 0
  // repairs the conflict by transferring `(REG_DEAD a2)` back to `rmw_post_idx`.
  {
    auto [cfg, rmw_post_idx, t0_add_post_idx] =
        build_post_sched2_cfg(/*attach_reg_equiv_on_t0_add=*/true);
    ASSERT_GE(rmw_post_idx, 0);
    ASSERT_GE(t0_add_post_idx, 0);
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[static_cast<size_t>(rmw_post_idx)].find(
                  "REG_DEAD (reg:SI 6 a2)"),
              std::string::npos)
        << cfg.FormatSummary();
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[static_cast<size_t>(t0_add_post_idx)].find(
                  "REG_DEAD (reg:SI 6 a2)"),
              std::string::npos)
        << cfg.FormatSummary();
  }

  // Negative: without `REG_EQUIV`, `(set t0 (plus a2 t0))` without `(REG_DEAD t0)`
  // is a multi-set RMW (`reg_qty != -1`, skipping `combine_regs`), so Stage 0
  // leaves `(REG_DEAD a2)` on `t0_add_post_idx`.
  {
    auto [cfg, rmw_post_idx, t0_add_post_idx] =
        build_post_sched2_cfg(/*attach_reg_equiv_on_t0_add=*/false);
    ASSERT_GE(rmw_post_idx, 0);
    ASSERT_GE(t0_add_post_idx, 0);
    AttributeSched2PureReaderRegDeadNotes(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[static_cast<size_t>(rmw_post_idx)].find(
                  "REG_DEAD (reg:SI 6 a2)"),
              std::string::npos)
        << cfg.FormatSummary();
    EXPECT_NE(cfg.rpo_blocks[0].instructions[static_cast<size_t>(t0_add_post_idx)].find(
                  "REG_DEAD (reg:SI 6 a2)"),
              std::string::npos)
        << cfg.FormatSummary();
  }
}
}  // namespace
}  // namespace rom_nom_nom::lifter::sched_model
