#include "lifter/sched_model.h"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gtest/gtest.h"
#include "lifter/rtl_liveness.h"

extern "C" {
extern int rtx_equal_function_value_matters;  // NOLINT(readability-identifier-naming)
}

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

RtlExpr MustParse(std::string_view sexpr) {
  auto res = RtlExpr::Parse(sexpr);
  EXPECT_TRUE(res.ok()) << sexpr;
  return res.ok() ? std::move(*res) : RtlExpr{};
}

RtlPatternAndNotes MustParsePn(std::string_view sexpr) {
  auto res = ParsePatternAndNotes(sexpr);
  EXPECT_TRUE(res.ok()) << sexpr;
  return res.ok() ? std::move(*res) : RtlPatternAndNotes{};
}

const Link* FindDepLink(const DepGraph& g, int insn_idx, int target_idx) {
  for (const Link& l : g.log_links[static_cast<size_t>(insn_idx)]) {
    if (l.target == target_idx) {
      return &l;
    }
  }
  return nullptr;
}

TEST(SchedModelTest, MachineModelProfileUnitsAndBlockageRange) {
  // 1. Default CPU resolution (`elf64.h:26`: `MIPS_ISA_DEFAULT = 3` -> `PROCESSOR_R4000`).
  const MachineModel r4000 = MachineModel::FromProfile({.optimize_level = 2, .c_flags = {"-O2"}});
  EXPECT_EQ(r4000.Cpu(), MipsCpu::kR4000);
  EXPECT_EQ(r4000.MipsIsa(), 3);
  EXPECT_FALSE(r4000.Target64Bit());
  EXPECT_FALSE(r4000.TargetFloat64());

  const MachineModel r3000 =
      MachineModel::FromProfile({.optimize_level = 2, .c_flags = {"-O2", "-mips1"}});
  EXPECT_EQ(r3000.Cpu(), MipsCpu::kR3000);
  EXPECT_EQ(r3000.MipsIsa(), 1);

  const MachineModel r6000 =
      MachineModel::FromProfile({.optimize_level = 2, .c_flags = {"-O2", "-mips2"}});
  EXPECT_EQ(r6000.Cpu(), MipsCpu::kR6000);

  const MachineModel r4650 =
      MachineModel::FromProfile({.optimize_level = 2, .c_flags = {"-O2", "-m4650"}});
  EXPECT_EQ(r4650.Cpu(), MipsCpu::kR4650);

  // 2. Exact `mips.md` `"type"` and `IsRecognized` classification.
  EXPECT_FALSE(r4000.IsRecognized(MustParsePn("(use (reg/i:SI 2 v0))")));
  EXPECT_FALSE(r4000.IsRecognized(MustParsePn("(clobber (reg:SI 72))")));
  EXPECT_FALSE(r4000.IsRecognized(MustParsePn("(asm_input (\"nop\"))")));
  EXPECT_FALSE(r4000.IsRecognized(
      MustParsePn("(set (reg:SI 72) (asm_operands:SI (\"nop\") (\"=r\") 0[ ] [ ] (\"t.c\") 1))")));

  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:SF 72) (const_double:SF (const_int 0) 0 0))"),
                         Phase::kSched1),
            InsnType::kXfer);
  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:DF 72) (const_double:DF (const_int 0) 0 0))"),
                         Phase::kSched1),
            InsnType::kLoad);
  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:SF 4 a0) (reg:SF 72))"), Phase::kSched1),
            InsnType::kXfer);
  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:SF 72) (reg:SF 73))"), Phase::kSched1),
            InsnType::kMove);
  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:SI 72) (reg:SI 65 lo))"), Phase::kSched1),
            InsnType::kHilo);
  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:SI 72) (reg:SI 67 $fcr31))"), Phase::kSched1),
            InsnType::kXfer);
  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:SI 72) (zero_extend:SI (mem:QI (reg:SI 4 a0))))"),
                         Phase::kSched1),
            InsnType::kLoad);
  EXPECT_EQ(r4000.TypeOf(MustParsePn("(set (reg:SF 72) (float:SF (reg:SI 73)))"), Phase::kSched1),
            InsnType::kFcvt);
  EXPECT_EQ(r4000.ResultReadyCost(MustParsePn("(set (reg:SF 72) (float:SF (reg:SI 73)))"),
                                  Phase::kSched1),
            1);
  EXPECT_EQ(
      r4000.TypeOf(MustParsePn("(parallel[ (set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
                               "(use (const_int 2)) ] )"),
                   Phase::kSched1),
      InsnType::kStore);

  // 3. Exact `blockage_range` (`insn-attrtab.c:1249-2055`) & `Blockage(E, C)`
  // (`genattrtab.c:1986`).
  const UnitUse load_r4k = r4000.UnitOf(InsnType::kLoad, RtlMachineMode::kSi);
  const UnitUse load_r3k = r3000.UnitOf(InsnType::kLoad, RtlMachineMode::kSi);
  const UnitUse store_u = r4000.UnitOf(InsnType::kStore, RtlMachineMode::kSi);
  const UnitUse xfer_u = r4000.UnitOf(InsnType::kXfer, RtlMachineMode::kSf);
  EXPECT_EQ(load_r4k.ready, 3);
  EXPECT_EQ(load_r4k.min_blockage, 1);
  EXPECT_EQ(load_r4k.max_blockage, 1);  // `memory_unit_blockage_range`: ready==3 -> `[1,1]`!
  EXPECT_EQ(load_r3k.ready, 2);
  EXPECT_EQ(load_r3k.max_blockage, 2);
  EXPECT_EQ(store_u.min_blockage, 1);
  EXPECT_EQ(store_u.max_blockage, 3);
  EXPECT_EQ(xfer_u.max_blockage, 2);
  EXPECT_EQ(r4000.Blockage(load_r4k, store_u), 3);
  EXPECT_EQ(r4000.Blockage(load_r4k, xfer_u), 2);
  EXPECT_EQ(r4000.Blockage(load_r4k, load_r4k), 1);

  const UnitUse hilo_u = r4000.UnitOf(InsnType::kHilo, RtlMachineMode::kSi);
  EXPECT_EQ(hilo_u.min_blockage, 3);
  EXPECT_EQ(hilo_u.max_blockage, 69);
  EXPECT_EQ(r4000.UnitOf(InsnType::kFadd, RtlMachineMode::kSf).max_blockage, 4);
  EXPECT_EQ(r4000.UnitOf(InsnType::kFmul, RtlMachineMode::kSf).min_blockage, 4);
  EXPECT_EQ(r4000.UnitOf(InsnType::kFmul, RtlMachineMode::kSf).max_blockage, 8);
  EXPECT_EQ(r4000.UnitOf(InsnType::kFdiv, RtlMachineMode::kDf).min_blockage, 12);
  EXPECT_EQ(r4000.UnitOf(InsnType::kFdiv, RtlMachineMode::kDf).max_blockage, 112);

  // 4. Complete MIPS `CALL_USED_REGISTERS` (`mips.h:1203-1210`).
  for (int r : {0, 1, 2, 15, 24, 25, 26, 27, 28, 29, 31, 32, 51, 64, 65, 66, 67}) {
    EXPECT_TRUE(r4000.CallUsed(r)) << "reg " << r;
  }
  for (int r : {16, 23, 30, 52, 63, 68}) {
    EXPECT_FALSE(r4000.CallUsed(r)) << "reg " << r;
  }
}
TEST(SchedModelTest, AliasAnalysisMirrorsSchedCAndRtlanalC) {
  RegFacts facts;
  // 1. `rtx_varies_p` (`rtlanal.c:76-116`): `$sp` IS varying; `$fp` & `argp` are NOT;
  //    `$zero` IS varying; `LO_SUM` inspects operand 1 only.
  EXPECT_TRUE(RtxVariesP(MustParse("(reg:SI 29 sp)"), facts, Phase::kSched1));
  EXPECT_FALSE(RtxVariesP(MustParse("(reg:SI 30 fp)"), facts, Phase::kSched1));
  EXPECT_FALSE(RtxVariesP(MustParse("(reg:SI 0 argp)"), facts, Phase::kSched1));
  EXPECT_TRUE(RtxVariesP(MustParse("(reg:SI 0 $0)"), facts, Phase::kSched1));
  EXPECT_FALSE(RtxVariesP(MustParse("(lo_sum:SI (reg:SI 72) (symbol_ref:SI (\"g\")))"), facts,
                          Phase::kSched1));
  EXPECT_TRUE(RtxAddrVariesP(MustParse("(mem:BLK (reg:SI 30 fp))"), facts, Phase::kSched1));

  // 2. `memrefs_conflict_p` exact sign on swap (`sched.c:636-643`):
  //    Comparing `x = (plus (reg 29 sp) 4)` (size 4) vs `y = (reg 29 sp)` (size 4) with `c = 0`:
  //    Step 1 strips `+4` from `x` -> `c = -4`, `x = sp`, `y = sp` -> `ysize + c = 0` -> no
  //    conflict. Comparing `x = (plus (reg 29 sp) 8)` vs `y = (plus (reg 29 sp) 4)` with `c = 0` ->
  //    no conflict.
  EXPECT_FALSE(MemrefsConflictP(4, MustParse("(plus:SI (reg:SI 29 sp) (const_int 4))"), 4,
                                MustParse("(reg:SI 29 sp)"), 0, facts, Phase::kSched2));
  //    Crucial swap-without-negating-`c` test: `x = (plus sp 4)` (not bare `sp`) vs `y = sp`
  //    with `c = 4` -> swaps `(x, y)` leaving `c = 4` unchanged -> `y = (plus sp 4)` adds `+4` ->
  //    `c = 8` -> false! Whereas negating `c` would give `-4 + 4 = 0` -> true!
  EXPECT_FALSE(MemrefsConflictP(4, MustParse("(plus:SI (reg:SI 29 sp) (const_int 4))"), 4,
                                MustParse("(reg:SI 29 sp)"), 4, facts, Phase::kSched2));

  // 3. No "distinct stack bases => no conflict" rule (`sched.c:645-669`): `$sp` vs `$fp` returns
  // true!
  EXPECT_TRUE(MemrefsConflictP(4, MustParse("(reg:SI 29 sp)"), 4, MustParse("(reg:SI 30 fp)"), 0,
                               facts, Phase::kSched2));
  //    Stack base vs static `SYMBOL_REF` returns false (`sched.c:650-651`).
  EXPECT_FALSE(MemrefsConflictP(4, MustParse("(reg:SI 29 sp)"), 4,
                                MustParse("(symbol_ref:SI (\"g_val\"))"), 0, facts,
                                Phase::kSched2));

  // 4. `MULT` scaling (`sched.c:727-750`): `(mult r72 4)` vs `(mult (plus r72 1) 4)` size 4 -> no
  // conflict!
  EXPECT_FALSE(
      MemrefsConflictP(4, MustParse("(mult:SI (reg:SI 72) (const_int 4))"), 4,
                       MustParse("(mult:SI (plus:SI (reg:SI 72) (const_int 1)) (const_int 4))"), 0,
                       facts, Phase::kSched1));

  // 5. Commutative `rtx_equal_for_memref_p` (`sched.c:487-491`).
  EXPECT_TRUE(RtxEqualForMemrefP(MustParse("(plus:SI (reg:SI 72) (reg:SI 73))"),
                                 MustParse("(plus:SI (reg:SI 73) (reg:SI 72))"), facts));

  // 6. `CONST` unwrapping (`sched.c:753-774`): two non-overlapping offsets of the same symbol.
  EXPECT_FALSE(
      MemrefsConflictP(4, MustParse("(const:SI (plus:SI (symbol_ref:SI (\"arr\")) (const_int 4)))"),
                       4, MustParse("(symbol_ref:SI (\"arr\"))"), 0, facts, Phase::kSched1));

  // 7. `ReadDependence` / `TrueDependence` / `AntiDependence`: volatile & unchanging flags.
  EXPECT_TRUE(
      ReadDependence(MustParse("(mem/v:SI (reg:SI 4 a0))"), MustParse("(mem/v:SI (reg:SI 5 a1))")));
  EXPECT_FALSE(
      ReadDependence(MustParse("(mem/v:SI (reg:SI 4 a0))"), MustParse("(mem:SI (reg:SI 5 a1))")));
  EXPECT_FALSE(TrueDependence(MustParse("(mem:SI (reg:SI 4 a0))"),
                              MustParse("(mem/u:SI (reg:SI 5 a1))"), facts, Phase::kSched1));
  EXPECT_FALSE(AntiDependence(MustParse("(mem/u:SI (reg:SI 4 a0))"),
                              MustParse("(mem:SI (reg:SI 5 a1))"), facts, Phase::kSched1));
}
TEST(SchedModelTest, AnalyzeDependenceGraphEdgeCases) {
  const MachineModel mm = MachineModel::FromProfile({.optimize_level = 2, .c_flags = {"-O2"}});
  RegFacts facts;
  facts.max_reg_num = 80;
  facts.reg_n_calls_crossed[72] = 0;  // Must not cross call (`sched_before_next_call`).
  facts.reg_n_calls_crossed[73] = 1;  // Already crosses call.

  // 1. `sched.c:1704` bug + `$sp` call anti-dep + `CALL_INSN_FUNCTION_USAGE` TRUE upgrade +
  //    `sched_before_next_call` + `SCHED_GROUP_P` (`USE` before `CALL`).
  std::vector<Insn> blk;
  blk.push_back(
      MakeInsn("(set (reg:SI 4 a0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))", 0));
  blk.push_back(MakeInsn("(set (reg:SI 74) (plus:SI (reg:SI 72) (const_int 1)))", 1));
  blk.push_back(MakeInsn("(use (reg:SI 4 a0))", 2));
  blk.push_back(MakeInsn("(call (mem:SI (symbol_ref:SI (\"foo\"))) (const_int 16))", 3,
                         Insn::Kind::kCall, {"(use (reg:SI 4 a0))"}));
  blk.push_back(MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 1)))", 4));
  blk.push_back(MakeInsn("(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22)))", 5));

  const DepGraph g = Analyze(blk, facts, mm, Phase::kSched2);
  // `CALL_INSN_FUNCTION_USAGE` `(use (reg:SI 4 a0))` upgrades call -> insn 0 from ANTI to TRUE!
  const Link* call_to_arg = FindDepLink(g, 3, 0);
  ASSERT_NE(call_to_arg, nullptr);
  EXPECT_EQ(call_to_arg->kind, DepKind::kTrue);
  // `sched_before_next_call` links call -> insn 1 (reads pseudo 72 with 0 calls crossed).
  const Link* call_to_p72 = FindDepLink(g, 3, 1);
  ASSERT_NE(call_to_p72, nullptr);
  EXPECT_EQ(call_to_p72->kind, DepKind::kAnti);
  // `SCHED_GROUP_P` set on call (preceded by `(use (reg:SI 4 a0))`).
  EXPECT_TRUE(g.sched_group_p[3]);
  // Exact `sched.c:1704` bug: BOTH callee-saved GPR `$16` AND callee-saved FPR `$f20` depend on
  // call!
  const Link* s0_to_call = FindDepLink(g, 4, 3);
  ASSERT_NE(s0_to_call, nullptr);
  EXPECT_EQ(s0_to_call->kind, DepKind::kAnti);
  const Link* f20_to_call = FindDepLink(g, 5, 3);
  ASSERT_NE(f20_to_call, nullptr);
  EXPECT_EQ(f20_to_call->kind, DepKind::kAnti);

  // 2. `SETJMP` call adds TRUE dep (`0`) on `reg_last_sets` for pseudos (`sched.c:2211`).
  std::vector<Insn> setjmp_blk;
  setjmp_blk.push_back(MakeInsn("(set (reg:SI 73) (const_int 7))", 0));
  setjmp_blk.push_back(MakeInsn("(call (mem:SI (symbol_ref:SI (\"setjmp\"))) (const_int 16))", 1,
                                Insn::Kind::kCall, {}, /*setjmp_follows=*/true));
  const DepGraph sj = Analyze(setjmp_blk, facts, mm, Phase::kSched1);
  const Link* sj_dep = FindDepLink(sj, 1, 0);
  ASSERT_NE(sj_dep, nullptr);
  EXPECT_EQ(sj_dep->kind, DepKind::kTrue);

  // 3. `pending_lists_length > 32` flush (`sched.c:1740`): 33 non-overlapping stack reads followed
  //    by a non-overlapping stack write flushes all 33 reads via `REG_DEP_ANTI`.
  std::vector<Insn> flush_blk;
  for (int k = 0; k < 33; ++k) {
    flush_blk.push_back(MakeInsn("(set (reg:SI 75) (mem:SI (plus:SI (reg:SI 29 sp) (const_int " +
                                     std::to_string(k * 4) + "))))",
                                 k));
  }
  flush_blk.push_back(
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 200))) (reg:SI 4 a0))", 33));
  const DepGraph fg = Analyze(flush_blk, facts, mm, Phase::kSched2);
  // Without flush, offset 200 does not alias offsets 0..128; WITH `> 32` flush, all 33 link to 33!
  EXPECT_NE(FindDepLink(fg, 33, 0), nullptr);
  EXPECT_NE(FindDepLink(fg, 33, 32), nullptr);
}
TEST(SchedModelTest, ScheduleTracesTwoLoadsBirthingAndImuldivHazard) {
  const CompilationProfile o2{.optimize_level = 2, .c_flags = {"-O2"}};
  // 1. Two-loads + ALU trace matches real GCC `FunctionHooks::RunScheduleInsns` bit-for-bit
  //    (including `birthing_insn_p`, `attach_deaths`, `reg_live_length`, and negative `-1`).
  RtlControlFlowGraph combine_cfg;
  combine_cfg.pass = RtlPassKind::kCombine;
  combine_cfg.rpo_blocks.resize(1);
  combine_cfg.rpo_blocks[0].id = 0;
  combine_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 74) (plus:SI (reg:SI 29 sp) (const_int 16)))");
  combine_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (mem:SI (reg:SI 4 a0)))");
  combine_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  combine_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (reg:SI 74))) "
      "(expr_list:REG_DEAD (reg:SI 73) (expr_list:REG_DEAD (reg:SI 74) (nil)))");
  combine_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  combine_cfg.sideband.basic_block_live_at_start[0] = {4, 29};
  combine_cfg.sideband.reg_live_length[72] = 2;
  combine_cfg.sideband.reg_live_length[73] = -1;
  combine_cfg.sideband.reg_live_length[74] = 4;

  const RtlControlFlowGraph model_sched = RunScheduleInsns(combine_cfg, o2, Phase::kSched1);

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ctx.BeginFunction("sched_model_trace");
  hooks.ApplyCompilationProfile(o2);
  ctx.MaterializeFromCfg(combine_cfg);
  hooks.RunScheduleInsns();
  const RtlControlFlowGraph gcc_sched = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kSched);

  EXPECT_TRUE(model_sched.CompareAgainst(gcc_sched).MatchesExactly())
      << model_sched.CompareAgainst(gcc_sched).mismatch_detail << "\nmodel:\n"
      << model_sched.FormatSummary() << "\ngcc:\n"
      << gcc_sched.FormatSummary();
  EXPECT_EQ(model_sched.sideband.reg_live_length.at(72), 3);
  EXPECT_EQ(model_sched.sideband.reg_live_length.at(73), -1);  // Preserved (`sched.c:4946`).
  EXPECT_EQ(model_sched.sideband.reg_live_length.at(74), 3);
}
TEST(SchedModelTest, DeletesNonFinalNoopMovesPostReloadAndPreservesBlockEndNoop) {
  RtlControlFlowGraph pre;
  pre.pass = RtlPassKind::kGreg;
  pre.sideband.max_regno = 72;
  RtlBasicBlock blk;
  blk.instructions = {
      "(set (reg/v:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
      "(set (reg:SI 3 v1) (plus:SI (reg/v:SI 4 a0) (const_int 1)))",
      "(set (reg/v:SI 5 a1) (reg:SI 5 a1))",  // Final real insn: preserved (`sched.c:4806`).
  };
  blk.SyncRtlInsnsFromInstructions();
  pre.rpo_blocks.push_back(std::move(blk));

  const CompilationProfile profile = {2, {}, {}};
  const RtlControlFlowGraph post = RunScheduleInsns(pre, profile, Phase::kSched2);
  ASSERT_EQ(post.rpo_blocks.size(), 1U);
  ASSERT_EQ(post.rpo_blocks[0].instructions.size(), 2U);
  EXPECT_EQ(post.rpo_blocks[0].instructions[0],
            "(set (reg:SI 3 v1) (plus:SI (reg/v:SI 4 a0) (const_int 1)))");
  EXPECT_EQ(post.rpo_blocks[0].instructions[1], "(set (reg/v:SI 5 a1) (reg:SI 5 a1))");
}
TEST(SchedModelTest, ClassifiesMulHighpartAsImulWithImuldivBlockage) {
  const CompilationProfile r4000_profile = {2, {}, {}};
  const MachineModel mm = MachineModel::FromProfile(r4000_profile);
  auto parsed = ParsePatternAndNotes(
      "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI (zero_extend:DI (reg:SI 2 "
      "v0)) (zero_extend:DI (reg:SI 3 v1))) (const_int 32)))) (clobber (reg:SI 65 lo)) (clobber "
      "(reg:SI 66 accum)) ] )");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(mm.TypeOf(*parsed, Phase::kSched2), InsnType::kImul);
  EXPECT_EQ(mm.ResultReadyCost(*parsed, Phase::kSched2), 10);
  const UnitUse u = mm.UnitOf(mm.TypeOf(*parsed, Phase::kSched2), mm.AttrModeOf(*parsed));
  EXPECT_EQ(u.unit, 1);  // `imuldiv` unit
  EXPECT_EQ(u.min_blockage, 3);
  EXPECT_EQ(u.max_blockage, 69);
}
TEST(SchedModelTest, StopsBlock0ParamPinningAtCombineNarrowArgCopyAndOmitsCallRegNotes) {
  RtlControlFlowGraph pre;
  pre.pass = RtlPassKind::kCombine;
  pre.sideband.max_regno = 77;
  RtlBasicBlock b0;
  b0.successors = {1};
  b0.instructions = {
      "(set (reg/v:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg/v:HI 74) (reg:HI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
      "(set (reg:SI 75) (const_int 0))",
      "(set (reg:SI 4 a0) (reg/v:SI 72))",
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
  };
  b0.SyncRtlInsnsFromInstructions();
  b0.rtl_insns[4].has_explicit_call_fusage = true;
  b0.rtl_insns[4].call_fusage = {MustParse("(use (reg:SI 4 a0))")};

  RtlBasicBlock b1;
  b1.instructions = {
      "(set (reg:SI 75) (plus:SI (reg:SI 75) (zero_extend:SI (reg/v:HI 74))))",
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 75) (reg/v:SI 72)))",
      "(use (reg/i:SI 2 v0))",
  };
  b1.SyncRtlInsnsFromInstructions();
  pre.rpo_blocks = {std::move(b0), std::move(b1)};

  const CompilationProfile profile = {2, {}, {}};
  const RtlControlFlowGraph post = RunScheduleInsns(pre, profile, Phase::kSched1);
  ASSERT_EQ(post.rpo_blocks.size(), 2U);
  // Narrow `HI` arg copy `[1]` was NOT pinned: it passed through `UpdateLife` (refreshing
  // `REG_DEAD` mode from `SI` to `HI`), and `CALL_INSN` omits `REG_NOTES` in serialized text.
  bool saw_hi_dead = false;
  bool saw_call_without_notes = false;
  for (const std::string& insn : post.rpo_blocks[0].instructions) {
    if (insn.find("(set (reg/v:HI 74) (reg:HI 5 a1))") != std::string::npos) {
      saw_hi_dead = (insn.find("(expr_list:REG_DEAD (reg:HI 5 a1) (nil))") != std::string::npos);
    }
    if (insn.find("(call (mem:SI") != std::string::npos) {
      saw_call_without_notes = (insn.find("expr_list:") == std::string::npos);
    }
  }
  EXPECT_TRUE(saw_hi_dead) << post.FormatSummary();
  EXPECT_TRUE(saw_call_without_notes) << post.FormatSummary();
}
TEST(SchedModelTest, SwapSortStalenessPreservesStaleRelativeOrderAndInvertsCleanly) {
  // Construct a block where initial `qsort` sorts `{4, 3, 1, 0}`, then scheduling `4`
  // releases a single newly ready insn `2` (`new_ready - old_ready == 1`, `sched.c:2374`),
  // triggering `swap_sort` instead of `qsort` on `{3, 1, 0, 2}` -> `{3, 2, 1, 0}`:
  // - `0`: arith `$v0 = a0 + 1` (tail, priority 1)
  // - `1`: arith `$v1 = a1 + 2` (tail, priority 1)
  // - `2`: arith `$t0 = a2 + 3` (consumed by `4`, cost 1)
  // - `3`: arith `$t1 = a3 + 4` (tail, priority 1)
  // - `4`: arith `$t2 = t0 + 5` (tail, priority 2)
  const CompilationProfile profile = {2, {}, {}};
  const MachineModel mm = MachineModel::FromProfile(profile);
  std::vector<Insn> pre = {
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2)))", 1),
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (const_int 3)))", 2),
      MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 7 a3) (const_int 4)))", 3),
      MakeInsn("(set (reg:SI 10 t2) (plus:SI (reg:SI 8 t0) (const_int 5)))", 4),
  };
  RegFacts facts;
  facts.max_reg_num = 72;
  const absl::btree_set<int> live_end = {2, 3, 9, 10};
  const DepGraph g = Analyze(pre, facts, mm, Phase::kSched2);
  const ScheduleResult sched = Schedule(pre, g, facts, mm, Phase::kSched2, live_end);
  // Reverse trace:
  // - step 0 (`qsort` on 4 initial tails): `ready = {4, 3, 1, 0}` -> picks `4`, releases `2`.
  // - step 1 (`swap_sort` with `old_ready = 3, new_ready = 4`): inserts `2` -> `{3, 2, 1, 0}` ->
  //   picks `3`.
  // - step 2..4: pops `2`, `1`, `0` -> forward order `{0, 1, 2, 3, 4}`.
  EXPECT_EQ(sched.order, (std::vector<int>{0, 1, 2, 3, 4}));
  ASSERT_EQ(sched.trace.size(), 5U);
  EXPECT_EQ(sched.trace[0].ready_before_select, (std::vector<int>{4, 3, 1, 0}));
  EXPECT_EQ(sched.trace[1].ready_before_select, (std::vector<int>{3, 2, 1, 0}));
  EXPECT_EQ(sched.trace[2].ready_before_select, (std::vector<int>{2, 1, 0}));

  // Build target block in scheduled order and verify `InvertBlockOrder` reproduces it.
  std::vector<Insn> target;
  target.reserve(pre.size());
  for (size_t i = 0; i < sched.order.size(); ++i) {
    Insn insn = pre[static_cast<size_t>(sched.order[i])];
    insn.luid = static_cast<int>(i);
    target.push_back(std::move(insn));
  }
  const std::optional<std::vector<int>> inv =
      InvertBlockOrder(target, facts, mm, Phase::kSched2, live_end);
  ASSERT_TRUE(inv.has_value());
  EXPECT_TRUE(IsAchievable(target, facts, mm, Phase::kSched2, live_end));
}
TEST(SchedModelTest, InvertBlockOrderHonorsPreferredOrderWithoutShortCircuitingToIdentity) {
  // Both `Q_id = {0, 1, 2}` and `Q_pref = {1, 0, 2}` schedule to `P = {0, 1, 2}` because
  // `0 -> 2` has load latency 2, so `1` always fills the load delay slot between `0` and `2`.
  const CompilationProfile profile = {2, {}, {}};
  const MachineModel mm = MachineModel::FromProfile(profile);
  std::vector<Insn> target = {
      MakeInsn("(set (reg:SI 72) (mem:SI (reg:SI 70)))", 0),
      MakeInsn("(set (reg:SI 74) (const_int 1))", 1),
      MakeInsn("(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 2)))", 2),
  };
  RegFacts facts;
  facts.max_reg_num = 75;
  facts.reg_n_sets[72] = 1;
  facts.reg_n_sets[73] = 1;
  facts.reg_n_sets[74] = 1;
  const absl::btree_set<int> live_end = {73, 74};

  const std::vector<int> preferred = {1, 0, 2};
  const std::optional<std::vector<int>> inv_pref =
      InvertBlockOrder(target, facts, mm, Phase::kSched1, live_end, preferred);
  ASSERT_TRUE(inv_pref.has_value());
  EXPECT_EQ(*inv_pref, preferred);

  const std::optional<std::vector<int>> inv_default =
      InvertBlockOrder(target, facts, mm, Phase::kSched1, live_end);
  ASSERT_TRUE(inv_default.has_value());
  EXPECT_EQ(*inv_default, (std::vector<int>{0, 1, 2}));
}
TEST(SchedModelTest, InvertBlockOrderContractsPreferredAdjacentSplitLargeIntPair) {
  // Split `large_int` pair `(0: lui r73, HI; 2: ori r73, LO)` interleaved around an
  // independent load `(1: lw r72, (r70))` feeding `(3: add r74, r72, r73)`.
  // Contracting `AdjacentPair{0, 2}` must reunite `0` and `2` adjacently in `Q` while
  // still scheduling to exact `P = {0, 1, 2, 3}` (or `{1, 0, 2, 3}` -> `P`).
  const CompilationProfile profile = {2, {}, {}};
  const MachineModel mm = MachineModel::FromProfile(profile);
  std::vector<Insn> pre = {
      MakeInsn("(set (reg:SI 72) (mem:SI (reg:SI 70)))", 0),
      MakeInsn("(set (reg:SI 73) (const_int 65536))", 1),
      MakeInsn("(set (reg:SI 73) (ior:SI (reg:SI 73) (const_int 5)))", 2),
      MakeInsn("(set (reg:SI 74) (plus:SI (reg:SI 72) (reg:SI 73)))", 3),
  };
  RegFacts facts;
  facts.max_reg_num = 75;
  facts.reg_n_sets[72] = 1;
  facts.reg_n_sets[73] = 2;
  facts.reg_n_sets[74] = 1;
  const absl::btree_set<int> live_end = {74};

  const DepGraph g = Analyze(pre, facts, mm, Phase::kSched1);
  const ScheduleResult sched = Schedule(pre, g, facts, mm, Phase::kSched1, live_end);
  ASSERT_EQ(sched.order.size(), 4U);

  std::vector<Insn> scheduled;
  int lui_idx = -1;
  int ori_idx = -1;
  for (size_t i = 0; i < sched.order.size(); ++i) {
    Insn insn = pre[static_cast<size_t>(sched.order[i])];
    insn.luid = static_cast<int>(i);
    if (sched.order[i] == 1) lui_idx = static_cast<int>(i);
    if (sched.order[i] == 2) ori_idx = static_cast<int>(i);
    scheduled.push_back(std::move(insn));
  }
  ASSERT_GE(lui_idx, 0);
  ASSERT_GE(ori_idx, 0);

  const std::vector<AdjacentPair> adj = {{.first = lui_idx, .second = ori_idx}};
  const std::optional<std::vector<int>> inv =
      InvertBlockOrder(scheduled, facts, mm, Phase::kSched1, live_end, /*preferred_order=*/{}, adj);
  ASSERT_TRUE(inv.has_value());
  int lui_pos = -1;
  int ori_pos = -1;
  for (size_t i = 0; i < inv->size(); ++i) {
    if ((*inv)[i] == lui_idx) lui_pos = static_cast<int>(i);
    if ((*inv)[i] == ori_idx) ori_pos = static_cast<int>(i);
  }
  EXPECT_EQ(ori_pos, lui_pos + 1);
}
TEST(SchedModelTest, InvertBlockOrderHandlesPendingListsOverflow33MemoryOps) {
  // 34 independent non-aliasing stack loads + 1 store triggers `pending_lists_length > 32`
  // flush (`sched.c:1800, 1844`). Verify forward scheduling and inversion succeed cleanly.
  const CompilationProfile profile = {2, {}, {}};
  const MachineModel mm = MachineModel::FromProfile(profile);
  std::vector<Insn> pre;
  RegFacts facts;
  facts.max_reg_num = 120;
  absl::btree_set<int> live_end;
  for (int i = 0; i < 34; ++i) {
    const int regno = 72 + i;
    pre.push_back(
        MakeInsn(absl::StrCat("(set (reg:SI ", regno,
                              ") (mem:SI (plus:SI (reg:SI 29 sp) (const_int ", i * 4, "))))"),
                 i));
    facts.reg_n_sets[regno] = 1;
    live_end.insert(regno);
  }
  pre.push_back(
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 136))) (reg:SI 72))", 34));

  const DepGraph g = Analyze(pre, facts, mm, Phase::kSched1);
  const ScheduleResult sched = Schedule(pre, g, facts, mm, Phase::kSched1, live_end);
  ASSERT_EQ(sched.order.size(), pre.size());

  std::vector<Insn> target;
  target.reserve(pre.size());
  for (size_t i = 0; i < sched.order.size(); ++i) {
    Insn insn = pre[static_cast<size_t>(sched.order[i])];
    insn.luid = static_cast<int>(i);
    target.push_back(std::move(insn));
  }
  const std::optional<std::vector<int>> inv =
      InvertBlockOrder(target, facts, mm, Phase::kSched1, live_end);
  ASSERT_TRUE(inv.has_value());
}
TEST(SchedModelTest, IsAchievableReturnsFalseForUnachievableTargetOrder) {
  // `[0: const, 1: load, 2: add(load)]` is mathematically unachievable under R4000 `sched2`
  // because `1 -> 2` has load latency 2, forcing independent `0` into the delay slot `[1, 0, 2]`.
  const CompilationProfile profile = {2, {}, {}};
  const MachineModel mm = MachineModel::FromProfile(profile);
  RegFacts facts;
  facts.max_reg_num = 72;
  const absl::btree_set<int> live_end = {2, 3};

  std::vector<Insn> unachievable = {
      MakeInsn("(set (reg:SI 3 v1) (const_int 1))", 0),
      MakeInsn("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))", 1),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 5)))", 2),
  };
  EXPECT_FALSE(IsAchievable(unachievable, facts, mm, Phase::kSched2, live_end));

  std::vector<Insn> achievable = {
      MakeInsn("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SI 3 v1) (const_int 1))", 1),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 5)))", 2),
  };
  EXPECT_TRUE(IsAchievable(achievable, facts, mm, Phase::kSched2, live_end));
}
TEST(SchedModelTest, InvertBlockOrderPrefWorklistHonorsPreferenceOverIdentityFallback) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = kMipsFirstPseudoRegister;

  // Build two independent 2-insn load->use chains plus two independent 1-cycle
  // ALU insns feeding a final return add. In `kSched2`, `preferred_order` asks
  // to invert a pair of independent ties that conflict at depth 0, exercising
  // `pref_worklist` refinement ahead of `identity_rank` fallback.
  std::vector<Insn> sched2_blk = {
      MakeInsn("(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SI 9 t1) (mem:SI (reg:SI 5 a1)))", 1),
      MakeInsn("(set (reg:SI 10 t2) (plus:SI (reg:SI 6 a2) (const_int 1)))", 2),
      MakeInsn("(set (reg:SI 11 t3) (plus:SI (reg:SI 7 a3) (const_int 2)))", 3),
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 8 t0) (reg:SI 10 t2)))", 4),
      MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 9 t1) (reg:SI 11 t3)))", 5),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1)))", 6),
      MakeInsn("(use (reg/i:SI 2 v0))", 7),
  };
  const DepGraph g_id = Analyze(sched2_blk, facts, mm, Phase::kSched2);
  const ScheduleResult sr_id = Schedule(sched2_blk, g_id, facts, mm, Phase::kSched2, {});
  ASSERT_EQ(sr_id.order.size(), sched2_blk.size());
  std::vector<Insn> target;
  target.reserve(sched2_blk.size());
  for (size_t i = 0; i < sr_id.order.size(); ++i) {
    Insn insn = sched2_blk[static_cast<size_t>(sr_id.order[i])];
    insn.luid = static_cast<int>(i);
    target.push_back(std::move(insn));
  }

  // Request a preference that puts the two independent ALU insns after the two
  // loads (reordered relative to `target`) while keeping all chains legal.
  std::vector<int> pref = {0, 1, 3, 2, 4, 5, 6, 7};
  const std::optional<std::vector<int>> inv =
      InvertBlockOrder(target, facts, mm, Phase::kSched2, {}, pref);
  ASSERT_TRUE(inv.has_value());
  ASSERT_EQ(inv->size(), target.size());

  // Verify the returned preimage replays to `target` under `Schedule(kSched2)`.
  std::vector<Insn> cand;
  cand.reserve(target.size());
  for (size_t i = 0; i < inv->size(); ++i) {
    Insn insn = target[static_cast<size_t>((*inv)[i])];
    insn.luid = static_cast<int>(i);
    cand.push_back(std::move(insn));
  }
  const DepGraph g_cand = Analyze(cand, facts, mm, Phase::kSched2);
  const ScheduleResult replay = Schedule(cand, g_cand, facts, mm, Phase::kSched2, {});
  ASSERT_EQ(replay.order.size(), target.size());
  for (size_t i = 0; i < target.size(); ++i) {
    EXPECT_EQ((*inv)[static_cast<size_t>(replay.order[i])], static_cast<int>(i));
  }
}
class SchedModelDifferentialTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    verify_ctx = new GccContext();
    verify_hooks = new FunctionHooks(verify_ctx);
  }

  static void TearDownTestSuite() {
    delete verify_hooks;
    verify_hooks = nullptr;
    delete verify_ctx;
    verify_ctx = nullptr;
  }

  static GccContext* verify_ctx;
  static FunctionHooks* verify_hooks;
};

GccContext* SchedModelDifferentialTest::verify_ctx = nullptr;
FunctionHooks* SchedModelDifferentialTest::verify_hooks = nullptr;

// Extracts the schedulable instruction sequence of block `b` from a scheduled CFG
// (skipping boundary notes and block-0 pinned param copies in `kSched1`) so
// `InvertBlockOrder` can be round-trip verified across all 4,172 corpus functions.
std::vector<Insn> ExtractSchedulableBlockForTest(const RtlControlFlowGraph& cfg, size_t b,
                                                 Phase phase) {
  const RtlBasicBlock& blk = cfg.rpo_blocks[b];
  struct Item {
    bool is_note = false;
    RtlLoopNoteKind note_kind = RtlLoopNoteKind::kLoopBeg;
    Insn insn;
    bool valid = false;
  };
  std::vector<Item> items;
  items.reserve(blk.instructions.size());
  for (size_t i = 0; i < blk.instructions.size(); ++i) {
    Item it;
    if (auto nk = ParseRtlLoopNoteKind(blk.instructions[i]); nk.has_value()) {
      it.is_note = true;
      it.note_kind = *nk;
      items.push_back(std::move(it));
      continue;
    }
    auto parsed = ParsePatternAndNotes(blk.instructions[i]);
    if (!parsed.ok()) {
      continue;
    }
    if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
        !parsed->has_explicit_call_fusage) {
      parsed->has_explicit_call_fusage = true;
      parsed->call_fusage = blk.rtl_insns[i].call_fusage;
    }
    it.valid = true;
    it.insn.pn = std::move(*parsed);
    const RtlExpr& pat = it.insn.pn.pattern;
    bool is_call =
        (pat.op == RtlOp::kCall || (pat.op == RtlOp::kSet && pat.SetSrc().op == RtlOp::kCall));
    bool is_jump = (pat.op == RtlOp::kSet && pat.SetDest().op == RtlOp::kPc);
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        if (ch.op == RtlOp::kCall || (ch.op == RtlOp::kSet && ch.SetSrc().op == RtlOp::kCall)) {
          is_call = true;
        }
        if (ch.op == RtlOp::kSet && ch.SetDest().op == RtlOp::kPc) {
          is_jump = true;
        }
      }
    }
    it.insn.kind = is_call ? Insn::Kind::kCall : (is_jump ? Insn::Kind::kJump : Insn::Kind::kInsn);
    items.push_back(std::move(it));
  }

  size_t head = 0;
  size_t tail = items.size();
  while (tail > head && items[tail - 1].is_note) --tail;
  if (phase == Phase::kSched1 && b == 0) {
    absl::btree_set<int> word_copied_args;
    while (head + 1 < tail && !items[head].is_note && items[head].valid &&
           items[head].insn.kind == Insn::Kind::kInsn &&
           items[head].insn.pn.pattern.op == RtlOp::kSet) {
      const RtlExpr* src = &items[head].insn.pn.pattern.SetSrc();
      while ((src->op == RtlOp::kSubreg || src->op == RtlOp::kSignExtend ||
              src->op == RtlOp::kZeroExtend || src->op == RtlOp::kSignExtract ||
              src->op == RtlOp::kZeroExtract) &&
             !src->children.empty()) {
        src = &src->children[0];
      }
      if (src->op != RtlOp::kReg || src->RegNo() >= kMipsFirstPseudoRegister) {
        break;
      }
      const RtlExpr& pat = items[head].insn.pn.pattern;
      if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kReg) {
        const RtlExpr& dst = pat.SetDest();
        const RtlExpr& s = pat.SetSrc();
        const int sr = s.RegNo();
        if ((s.mode == RtlMachineMode::kSi || s.mode == RtlMachineMode::kDi) && sr >= 4 &&
            sr <= 7) {
          word_copied_args.insert(sr);
        } else if ((dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi) &&
                   (s.mode == RtlMachineMode::kQi || s.mode == RtlMachineMode::kHi) && sr >= 4 &&
                   sr <= 7 && !word_copied_args.contains(sr)) {
          break;
        }
      }
      ++head;
    }
  }
  while (head < tail && items[head].is_note) ++head;

  std::vector<Insn> out;
  std::vector<RtlLoopNoteKind> pending_barriers;
  int luid = 0;
  for (size_t i = head; i < tail; ++i) {
    if (items[i].is_note) {
      if (items[i].note_kind == RtlLoopNoteKind::kLoopBeg ||
          items[i].note_kind == RtlLoopNoteKind::kLoopEnd) {
        pending_barriers.push_back(items[i].note_kind);
      }
      ++luid;
      continue;
    }
    Insn cur = std::move(items[i].insn);
    cur.luid = luid++;
    cur.leading_loop_notes = std::move(pending_barriers);
    pending_barriers.clear();
    out.push_back(std::move(cur));
  }
  return out;
}

void VerifyCorpusCfgInvertibility(const RtlControlFlowGraph& scheduled_cfg,
                                  const CompilationProfile& profile, Phase phase,
                                  const std::string& test_name) {
  const MachineModel mm = MachineModel::FromProfile(profile);
  const RegFacts facts = RegFacts::FromCfg(scheduled_cfg, profile, phase);
  RtlControlFlowGraph live_cfg = scheduled_cfg;
  ReconstructRtlLiveness(&live_cfg, profile,
                         {.reload_completed = (phase == Phase::kSched2),
                          .update_reg_notes = false,
                          .preserve_phantom_pseudos = true});
  for (size_t b = 0; b < scheduled_cfg.rpo_blocks.size(); ++b) {
    const std::vector<Insn> block = ExtractSchedulableBlockForTest(scheduled_cfg, b, phase);
    if (block.size() <= 1) {
      continue;
    }
    absl::btree_set<int> live_at_end;
    if (phase == Phase::kSched1) {
      for (int succ : scheduled_cfg.rpo_blocks[b].successors) {
        if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
            it != live_cfg.sideband.basic_block_live_at_start.end()) {
          live_at_end.insert(it->second.begin(), it->second.end());
        }
      }
    }
    const std::optional<std::vector<int>> inv =
        InvertBlockOrder(block, facts, mm, phase, live_at_end);
    if (!inv.has_value()) {
      const DepGraph g = Analyze(block, facts, mm, phase);
      const ScheduleResult sr = Schedule(block, g, facts, mm, phase, live_at_end);
      std::string order_str;
      for (int idx : sr.order) absl::StrAppend(&order_str, idx, ",");
      EXPECT_TRUE(inv.has_value())
          << (phase == Phase::kSched1 ? "kSched1" : "kSched2") << " InvertBlockOrder failed on "
          << test_name << " block " << b << " (n=" << block.size() << ", Schedule(P)=" << order_str
          << ")\n"
          << scheduled_cfg.FormatSummary();
    }
  }
}
TEST_P(SchedModelDifferentialTest, MatchesRealGccRunScheduleInsnsOnSched1AndSched2) {
  const RoundTripTestCase& tc = GetParam();
  const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const FunctionCompilationTrace& trace = **cached_trace;
  const CompilationProfile& profile = trace.stage0_input.profile;

  verify_ctx->BeginFunction("harness_func");
  verify_hooks->ApplyCompilationProfile(profile);
  regclass_init();

  std::vector<std::string> eff_flags;
  eff_flags.push_back("-O" + std::to_string(profile.optimize_level));
  eff_flags.insert(eff_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(eff_flags);

  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kScheduleBlockSched1 && flags.RunsSched1()) {
      verify_ctx->BeginFunction("diff_sched1");
      verify_hooks->ApplyCompilationProfile(profile);
      regclass_init();
      verify_ctx->MaterializeFromCfg(snap.pre_state);
      // Mirrors `toplev.c:2779` & `right_inverse_verifier.cc:341`: after RTL generation,
      // `rtx_equal_function_value_matters` is 0.
      rtx_equal_function_value_matters = 0;
      verify_hooks->RunScheduleInsns();
      const RtlControlFlowGraph gcc_out =
          verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched);

      const RtlControlFlowGraph model_out =
          RunScheduleInsns(snap.pre_state, profile, Phase::kSched1);
      const RtlCfgPassComparison cmp = model_out.CompareAgainst(gcc_out);
      EXPECT_TRUE(cmp.MatchesExactly())
          << "kSched1 differential mismatch on " << tc.test_name << ":\n"
          << cmp.mismatch_detail << "\npre:\n"
          << snap.pre_state.FormatSummary() << "\nmodel:\n"
          << model_out.FormatSummary() << "\ngcc:\n"
          << gcc_out.FormatSummary();

      // Verify `kSched1` sideband write-back agreement (`sched.c:4911-4966`,
      // `gcc_context.cc:3933-3958` only emits positive `reg_live_length` / `reg_n_calls_crossed`).
      auto filter_positive_pseudos = [](const auto& m) {
        absl::btree_map<int, int> out;
        for (const auto& [r, v] : m) {
          if (r >= kMipsFirstPseudoRegister && v > 0) {
            out[r] = v;
          }
        }
        return out;
      };
      EXPECT_EQ(filter_positive_pseudos(model_out.sideband.reg_n_sets),
                filter_positive_pseudos(gcc_out.sideband.reg_n_sets))
          << "kSched1 reg_n_sets sideband mismatch on " << tc.test_name << "\ngcc_out:\n"
          << gcc_out.FormatSummary();
      EXPECT_EQ(filter_positive_pseudos(model_out.sideband.reg_live_length),
                filter_positive_pseudos(gcc_out.sideband.reg_live_length))
          << "kSched1 reg_live_length sideband mismatch on " << tc.test_name << "\ngcc_out:\n"
          << gcc_out.FormatSummary();
      EXPECT_EQ(filter_positive_pseudos(model_out.sideband.reg_n_calls_crossed),
                filter_positive_pseudos(gcc_out.sideband.reg_n_calls_crossed))
          << "kSched1 reg_n_calls_crossed sideband mismatch on " << tc.test_name;

      VerifyCorpusCfgInvertibility(gcc_out, profile, Phase::kSched1, tc.test_name);
    } else if (snap.subfunction_id == SubfunctionId::kScheduleBlockSched2 && flags.RunsSched2()) {
      verify_ctx->BeginFunction("diff_sched2");
      verify_hooks->ApplyCompilationProfile(profile);
      regclass_init();
      verify_ctx->MaterializeFromCfg(snap.pre_state);
      rtx_equal_function_value_matters = 0;
      verify_hooks->RunScheduleInsns();
      const RtlControlFlowGraph gcc_out =
          verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched2);

      const RtlControlFlowGraph model_out =
          RunScheduleInsns(snap.pre_state, profile, Phase::kSched2);
      const RtlCfgPassComparison cmp = model_out.CompareAgainst(gcc_out);
      EXPECT_TRUE(cmp.MatchesExactly())
          << "kSched2 differential mismatch on " << tc.test_name << ":\n"
          << cmp.mismatch_detail << "\npre:\n"
          << snap.pre_state.FormatSummary() << "\nmodel:\n"
          << model_out.FormatSummary() << "\ngcc:\n"
          << gcc_out.FormatSummary();

      VerifyCorpusCfgInvertibility(gcc_out, profile, Phase::kSched2, tc.test_name);
    }
  }
}
TEST(SchedModelTest, InvertBlockOrderPreferredAdjacentAndPendingFlushDissolution) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = 72;
  auto run_sched2 = [&](absl::Span<const Insn> blk,
                        const absl::btree_set<int>& live_at_end) -> std::vector<int> {
    std::vector<Insn> numbered(blk.begin(), blk.end());
    for (size_t i = 0; i < numbered.size(); ++i) {
      numbered[i].luid = static_cast<int>(i);
    }
    const DepGraph g = Analyze(numbered, facts, mm, Phase::kSched2);
    return Schedule(numbered, g, facts, mm, Phase::kSched2, live_at_end).order;
  };

  // Part 1: `InvertBlockOrder` prioritizes a verified candidate satisfying
  // `preferred_adjacent` (`use_soft_adj = true`) over an uncontracted fallback.
  {
    const std::vector<Insn> pre_block = {
        MakeInsn("(set (reg:SI 8 t0) (mem/u:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 5 a1) (const_int 1)))", 1),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 9 t1))", 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1)))", 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
    const std::vector<int> sched_perm = run_sched2(pre_block, /*live_at_end=*/{2});
    std::vector<Insn> sched_block;
    sched_block.reserve(pre_block.size());
    std::vector<int> orig_to_sched(pre_block.size(), -1);
    for (size_t pos = 0; pos < sched_perm.size(); ++pos) {
      orig_to_sched[static_cast<size_t>(sched_perm[pos])] = static_cast<int>(pos);
      Insn insn = pre_block[static_cast<size_t>(sched_perm[pos])];
      insn.luid = static_cast<int>(pos);
      sched_block.push_back(std::move(insn));
    }
    // Positive: request `(1, 2)` adjacent (in `sched_block` indices) with a
    // preferred order that places 1 and 2 apart; `InvertBlockOrder` must still
    // return a verified preimage with `1` immediately before `2`.
    const int s1 = orig_to_sched[1];
    const int s2 = orig_to_sched[2];
    std::vector<int> pref(sched_block.size());
    for (size_t i = 0; i < pref.size(); ++i) {
      pref[i] = static_cast<int>(i);
    }
    const std::optional<std::vector<int>> inv = InvertBlockOrder(
        sched_block, facts, mm, Phase::kSched2, /*live_at_end=*/{2}, pref, {{s1, s2}});
    ASSERT_TRUE(inv.has_value());
    int pos_s1 = -1;
    int pos_s2 = -1;
    for (size_t p = 0; p < inv->size(); ++p) {
      if ((*inv)[p] == s1) pos_s1 = static_cast<int>(p);
      if ((*inv)[p] == s2) pos_s2 = static_cast<int>(p);
    }
    EXPECT_EQ(pos_s2, pos_s1 + 1);
    // Negative contrast: an impossible adjacency (`(4, 0)` where 4 depends on 0)
    // falls back cleanly and still returns a verified preimage.
    const std::optional<std::vector<int>> fallback =
        InvertBlockOrder(sched_block, facts, mm, Phase::kSched2, /*live_at_end=*/{2}, pref,
                         {{orig_to_sched[4], orig_to_sched[0]}});
    ASSERT_TRUE(fallback.has_value());
    std::vector<Insn> cand_seq;
    cand_seq.reserve(sched_block.size());
    for (int idx : *fallback) {
      cand_seq.push_back(sched_block[static_cast<size_t>(idx)]);
    }
    const std::vector<int> replay = run_sched2(cand_seq, /*live_at_end=*/{2});
    for (size_t p = 0; p < replay.size(); ++p) {
      EXPECT_EQ((*fallback)[static_cast<size_t>(replay[p])], static_cast<int>(p));
    }
  }

  // Part 2 (`sched.c:1740, 1748` `flush_pending_lists` dissolution):
  // Build a block with 34 `mem/u` loads before a stack spill store (tripping
  // `pending_lists_length > 32` at the store hub in `sched_block`). Request a
  // `preferred_order` that moves independent pre-hub loads past the hub (dissolving
  // the flush cap). `InvertBlockOrder` must succeed and verify under `Schedule`.
  {
    std::vector<Insn> pre_block;
    int id = 0;
    // 31 independent `mem/u` loads before the store, 1 store at hub, 2 loads after.
    for (int k = 0; k < 31; ++k) {
      const int reg = 8 + (k % 8);
      pre_block.push_back(
          MakeInsn(absl::StrCat("(set (reg:SI ", reg, " t", (k % 8),
                                ") (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int ", k * 4, "))))"),
                   id++));
    }
    // Hub store (when <= 32 pending MEM ops precede it, no flush occurs in `pre_block`).
    pre_block.push_back(
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 5 a1))", id++));
    // Two independent `mem/u` loads after the hub store.
    pre_block.push_back(
        MakeInsn("(set (reg:SI 16 s0) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 200))))", id++));
    pre_block.push_back(
        MakeInsn("(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 204))))", id++));
    pre_block.push_back(
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 17 s1)))", id++));
    pre_block.push_back(MakeInsn("(use (reg/i:SI 2 v0))", id++));

    // Construct a post-`sched2` order where the 2 loads were hoisted ahead of the
    // store (so 33 MEM loads precede the store, tripping `pending_lists_length > 32`).
    std::vector<Insn> sched_block;
    const std::vector<int> sched_perm = run_sched2(pre_block, /*live_at_end=*/{2});
    sched_block.reserve(pre_block.size());
    std::vector<int> orig_to_sched(pre_block.size(), -1);
    for (size_t pos = 0; pos < sched_perm.size(); ++pos) {
      orig_to_sched[static_cast<size_t>(sched_perm[pos])] = static_cast<int>(pos);
      Insn insn = pre_block[static_cast<size_t>(sched_perm[pos])];
      insn.luid = static_cast<int>(pos);
      sched_block.push_back(std::move(insn));
    }
    // Preferred order is the original `pre_block` order (mapped into `sched_block` indices).
    std::vector<int> pref_in_sched;
    pref_in_sched.reserve(pre_block.size());
    for (size_t orig = 0; orig < pre_block.size(); ++orig) {
      pref_in_sched.push_back(orig_to_sched[orig]);
    }
    const std::optional<std::vector<int>> inv = InvertBlockOrder(
        sched_block, facts, mm, Phase::kSched2, /*live_at_end=*/{2}, pref_in_sched);
    ASSERT_TRUE(inv.has_value());
    std::vector<Insn> cand_seq;
    cand_seq.reserve(sched_block.size());
    for (int idx : *inv) {
      cand_seq.push_back(sched_block[static_cast<size_t>(idx)]);
    }
    const std::vector<int> replay = run_sched2(cand_seq, /*live_at_end=*/{2});
    ASSERT_EQ(replay.size(), sched_block.size());
    for (size_t p = 0; p < replay.size(); ++p) {
      EXPECT_EQ((*inv)[static_cast<size_t>(replay[p])], static_cast<int>(p));
    }
  }
}
TEST(SchedModelTest, Sched1StandaloneClobberPreservesRegNsetsUnlikeDeadPlainSet) {
  // Mirrors `flow.c:1792-1795` (`insn_dead_p`): a standalone `(clobber (reg:M p))`
  // is never dead, so `RunScheduleInsns(..., Phase::kSched1)` preserves its
  // `reg_n_sets` / `reg_live_length` entry even when `p` is never read; by
  // contrast, a dead plain `(set (reg:SI q) (const_int 7))` whose destination
  // is never read is omitted from `reg_n_sets`.
  RtlControlFlowGraph pre;
  pre.pass = RtlPassKind::kCombine;
  pre.sideband.reg_n_sets = {{80, 1}, {81, 1}, {82, 1}};
  RtlBasicBlock b0;
  b0.id = 0;
  b0.instructions = {
      "(clobber (reg:DI 80))",
      "(set (reg:SI 81) (const_int 7))",
      "(set (reg:SI 82) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg/i:SI 2 v0) (reg:SI 82)) (expr_list:REG_DEAD (reg:SI 82) (nil))",
      "(use (reg/i:SI 2 v0))",
  };
  b0.SyncRtlInsnsFromInstructions();
  pre.rpo_blocks = {std::move(b0)};

  const CompilationProfile profile = {2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  const RtlControlFlowGraph post = RunScheduleInsns(pre, profile, Phase::kSched1);
  ASSERT_EQ(post.rpo_blocks.size(), 1U);
  // Positive: standalone `(clobber (reg:DI 80))` is retained in `reg_n_sets`.
  ASSERT_TRUE(post.sideband.reg_n_sets.contains(80));
  EXPECT_GT(post.sideband.reg_n_sets.at(80), 0);
  // Negative: dead plain `(set (reg:SI 81) (const_int 7))` is omitted.
  EXPECT_FALSE(post.sideband.reg_n_sets.contains(81));
}
TEST(SchedModelTest, IsAchievableRepositionsSyntheticNoConflictRetvalPositiveAndNegative) {
  // Mirrors `sched.c:schedule_block` + `reload1.c`: when an unpaired
  // `REG_LIBCALL`/`REG_NO_CONFLICT` `zero_extend:DI` sequence has an independent
  // load scheduled between its last `REG_NO_CONFLICT` subword set and its
  // `reload`-deleted `REG_RETVAL` no-op, `IsAchievable` accepts the block by
  // verifying the `Schedule(expanded, kSched1)` placement of the synthetic
  // `REG_RETVAL` (positive), while still rejecting unachievable real-insn orders
  // (negative).
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = 82;
  facts.reg_n_sets[72] = 1;
  facts.reg_n_sets[73] = 2;
  facts.reg_n_sets[74] = 1;
  facts.reg_n_sets[75] = 1;
  facts.reg_n_sets[76] = 1;
  facts.reg_n_sets[78] = 1;

  // Build the genuine pre-sched1 block (including `REG_RETVAL`) and run
  // GCC's backward `Schedule(kSched1)`: `r74 = mem(r76)` has 3-cycle load
  // latency to `r75`, matching the 3-cycle chain `nc -> REG_RETVAL -> r78 -> r75`.
  // Backward scheduling places `r78`, then `REG_RETVAL`, then `r74` (higher
  // backward priority from `r76 = mem(a1)`) strictly before `REG_RETVAL` and
  // strictly after both `REG_NO_CONFLICT` subword sets. Strip `REG_RETVAL`.
  {
    RegFacts exp_facts = facts;
    exp_facts.reg_n_sets[73] = 3;
    const std::vector<Insn> pre_sched_expanded = {
        MakeInsn(
            "(set (reg:SI 76) (mem/s:SI (reg:SI 5 a1))) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
            0),
        MakeInsn("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 1),
        MakeInsn("(clobber (reg:DI 73)) (insn_list:REG_LIBCALL (nil))", 2),
        MakeInsn("(set (subreg:SI (reg:DI 73) 0) (const_int 0)) "
                 "(expr_list:REG_NO_CONFLICT (reg:SI 72) (nil))",
                 3),
        MakeInsn("(set (subreg:SI (reg:DI 73) 1) (reg:SI 72)) "
                 "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_NO_CONFLICT (reg:SI 72) (nil)))",
                 4),
        MakeInsn("(set (reg:DI 73) (reg:DI 73)) (insn_list:REG_RETVAL (nil))", 5),
        MakeInsn("(set (reg:SI 78) (plus:SI (subreg:SI (reg:DI 73) 1) (const_int 7))) "
                 "(expr_list:REG_DEAD (reg:DI 73) (nil))",
                 6),
        MakeInsn("(set (reg:SI 74) (mem:SI (reg:SI 76))) (expr_list:REG_DEAD (reg:SI 76) (nil))",
                 7),
        MakeInsn("(set (reg:SI 75) (plus:SI (reg:SI 78) (reg:SI 74))) "
                 "(expr_list:REG_DEAD (reg:SI 78) (expr_list:REG_DEAD (reg:SI 74) (nil)))",
                 8),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 75)) (expr_list:REG_DEAD (reg:SI 75) (nil))", 9),
        MakeInsn("(use (reg/i:SI 2 v0))", 10),
    };
    const DepGraph g = Analyze(pre_sched_expanded, exp_facts, mm, Phase::kSched1);
    const ScheduleResult sr =
        Schedule(pre_sched_expanded, g, exp_facts, mm, Phase::kSched1, /*live_at_end=*/{2});
    ASSERT_EQ(sr.order.size(), pre_sched_expanded.size());
    int pos_nc0 = -1;
    int pos_nc1 = -1;
    int pos_rv = -1;
    int pos_r74 = -1;
    std::vector<Insn> post_reload_blk;
    for (int p = 0; p < static_cast<int>(sr.order.size()); ++p) {
      const int orig = sr.order[static_cast<size_t>(p)];
      if (orig == 3) pos_nc0 = p;
      if (orig == 4) pos_nc1 = p;
      if (orig == 5) {
        pos_rv = p;
        continue;
      }
      if (orig == 7) pos_r74 = p;
      Insn copy = pre_sched_expanded[static_cast<size_t>(orig)];
      copy.luid = static_cast<int>(post_reload_blk.size());
      post_reload_blk.push_back(std::move(copy));
    }
    ASSERT_GT(pos_r74, std::max(pos_nc0, pos_nc1));
    ASSERT_LT(pos_r74, pos_rv);
    EXPECT_TRUE(IsAchievable(post_reload_blk, facts, mm, Phase::kSched1, /*live_at_end=*/{2}));
  }

  // Negative: moving `r74 = mem(r76)` immediately after `r76 = mem/s(a1)`
  // violates load-use hazard / backward priority and stays unachievable even
  // with synthetic `REG_RETVAL` repositioning.
  {
    const std::vector<Insn> bad_order = {
        MakeInsn(
            "(set (reg:SI 76) (mem/s:SI (reg:SI 5 a1))) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
            0),
        MakeInsn("(set (reg:SI 74) (mem:SI (reg:SI 76))) (expr_list:REG_DEAD (reg:SI 76) (nil))",
                 1),
        MakeInsn("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 2),
        MakeInsn("(clobber (reg:DI 73)) (insn_list:REG_LIBCALL (nil))", 3),
        MakeInsn("(set (subreg:SI (reg:DI 73) 0) (const_int 0)) "
                 "(expr_list:REG_NO_CONFLICT (reg:SI 72) (nil))",
                 4),
        MakeInsn("(set (subreg:SI (reg:DI 73) 1) (reg:SI 72)) "
                 "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_NO_CONFLICT (reg:SI 72) (nil)))",
                 5),
        MakeInsn("(set (reg:SI 78) (plus:SI (subreg:SI (reg:DI 73) 1) (const_int 7))) "
                 "(expr_list:REG_DEAD (reg:DI 73) (nil))",
                 6),
        MakeInsn("(set (reg:SI 75) (plus:SI (reg:SI 78) (reg:SI 74))) "
                 "(expr_list:REG_DEAD (reg:SI 78) (expr_list:REG_DEAD (reg:SI 74) (nil)))",
                 7),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 75)) (expr_list:REG_DEAD (reg:SI 75) (nil))", 8),
        MakeInsn("(use (reg/i:SI 2 v0))", 9),
    };
    EXPECT_FALSE(IsAchievable(bad_order, facts, mm, Phase::kSched1, /*live_at_end=*/{2}));
  }
}
TEST(SchedModelTest, InvertBlockOrderUnpinsAccidentalSchedGroupBeforeCall) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // `sched.c:2125-2157`: in the pre-`sched2` block, an independent load sits
  // between `(use (reg:SI 4 a0))` and `CALL_INSN`, so `SCHED_GROUP_P` is NOT
  // set on the call. `schedule_block` hoists the independent load ahead of
  // `(use (reg:SI 4 a0))`, accidentally making `(use ...)` adjacent to
  // `CALL_INSN` in the post-schedule block. `InvertBlockOrder` must unpin that
  // artificial `SCHED_GROUP_P` and find a valid preimage whose forward schedule
  // reproduces `scheduled` bit-for-bit.
  const std::vector<Insn> pre = {
      MakeInsn("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(use (reg:SI 4 a0))", 1),
      MakeInsn("(set (reg:SI 16 s0) (mem/u:SI (symbol_ref:SI (\"g_const\"))))", 2),
      MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
               "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
               3, Insn::Kind::kCall),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
               "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
               4),
      MakeInsn("(use (reg/i:SI 2 v0))", 5),
  };
  const RegFacts facts;
  const DepGraph pre_graph = Analyze(pre, facts, mm, Phase::kSched2);
  const std::vector<int> fwd_order =
      Schedule(pre, pre_graph, facts, mm, Phase::kSched2, /*live_at_end=*/{2}).order;
  ASSERT_EQ(fwd_order.size(), pre.size());
  std::vector<Insn> scheduled;
  scheduled.reserve(pre.size());
  for (size_t i = 0; i < fwd_order.size(); ++i) {
    Insn insn = pre[static_cast<size_t>(fwd_order[i])];
    insn.luid = static_cast<int>(i);
    scheduled.push_back(std::move(insn));
  }
  // Confirm `schedule_block` actually made `(use (reg:SI 4 a0))` immediately
  // precede `CALL_INSN` (triggering artificial `SCHED_GROUP_P` on `scheduled`).
  const DepGraph post_graph = Analyze(scheduled, facts, mm, Phase::kSched2);
  bool has_post_sched_group = false;
  for (bool sg : post_graph.sched_group_p) {
    has_post_sched_group |= sg;
  }
  ASSERT_TRUE(has_post_sched_group);

  const std::optional<std::vector<int>> inv =
      InvertBlockOrder(scheduled, facts, mm, Phase::kSched2, /*live_at_end=*/{2});
  ASSERT_TRUE(inv.has_value());
  std::vector<Insn> preimage;
  preimage.reserve(scheduled.size());
  for (size_t i = 0; i < inv->size(); ++i) {
    Insn insn = scheduled[static_cast<size_t>((*inv)[i])];
    insn.luid = static_cast<int>(i);
    preimage.push_back(std::move(insn));
  }
  const DepGraph preimage_graph = Analyze(preimage, facts, mm, Phase::kSched2);
  const std::vector<int> replayed =
      Schedule(preimage, preimage_graph, facts, mm, Phase::kSched2, /*live_at_end=*/{2}).order;
  ASSERT_EQ(replayed.size(), scheduled.size());
  for (size_t i = 0; i < replayed.size(); ++i) {
    EXPECT_EQ(preimage[static_cast<size_t>(replayed[i])].pn.pattern.ToGccPrintRtlString(),
              scheduled[i].pn.pattern.ToGccPrintRtlString())
        << "Mismatch at index " << i;
  }
}
TEST(SchedModelTest, InvertBlockOrderDissolvesPendingListFlushCapAcrossMultiStoreAndRegDeadMem) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // 1. `kSched1` multi-store cascade (`sched.c:1740, 1748`): 3 independent
  //    stores (including volatile `mem/v`, which does NOT flush pending lists)
  //    precede a 35-load `mem/u` chain + sink store, followed by a post-sink
  //    store. Forward `kSched1` hoists `mem/u` loads ahead of the 3 stores,
  //    creating an artificial `pending_lists_length > 32` flush hub.
  //    `InvertBlockOrder` must dissolve the cascade via prefix batch-hoist
  //    (leaving the post-sink store after the sink) and replay bit-exactly.
  {
    RtlControlFlowGraph pre_cfg;
    pre_cfg.pass = RtlPassKind::kCombine;
    pre_cfg.rpo_blocks.resize(1);
    pre_cfg.rpo_blocks[0].id = 0;
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg/v:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg/v:SI 72) (const_int 1)))");
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem/v:SI (symbol_ref:SI (\"g_vol1\"))) (reg:SI 73)) "
        "(expr_list:REG_DEAD (reg:SI 73) (nil))");
    pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 74) (plus:SI (reg/v:SI 72) (const_int 2)))");
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem/v:SI (symbol_ref:SI (\"g_vol2\"))) (reg:SI 74)) "
        "(expr_list:REG_DEAD (reg:SI 74) (nil))");
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 75) (plus:SI (reg/v:SI 72) (const_int 3))) "
        "(expr_list:REG_DEAD (reg/v:SI 72) (nil))");
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SI (symbol_ref:SI (\"g_dst3\"))) (reg:SI 75)) "
        "(expr_list:REG_DEAD (reg:SI 75) (nil))");
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 80) (mem/u:SI (symbol_ref:SI (\"g_arr\"))))");
    for (int k = 1; k < 35; ++k) {
      pre_cfg.rpo_blocks[0].AddPatternOnly(
          absl::StrCat("(set (reg:SI 81) (mem/u:SI (const:SI (plus:SI (symbol_ref:SI (\"g_arr\")) "
                       "(const_int ",
                       k * 4, ")))))"));
      pre_cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 80) (plus:SI (reg:SI 80) (reg:SI 81))) "
          "(expr_list:REG_DEAD (reg:SI 81) (nil))");
    }
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SI (symbol_ref:SI (\"g_sink\"))) (reg:SI 80)) "
        "(expr_list:REG_DEAD (reg:SI 80) (nil))");
    pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 82) (const_int 7))");
    pre_cfg.rpo_blocks[0].AddPatternOnly(
        "(set (mem:SI (symbol_ref:SI (\"g_post_sink\"))) (reg:SI 82)) "
        "(expr_list:REG_DEAD (reg:SI 82) (nil))");

    const ParsedSchedRegion pre_reg =
        ParseBlockForSchedModel(pre_cfg.rpo_blocks[0], /*block_index=*/0, Phase::kSched1);
    ASSERT_TRUE(pre_reg.valid);
    const RegFacts facts = RegFacts::FromCfg(pre_cfg, o2_prof, Phase::kSched1);
    const DepGraph pre_graph = Analyze(pre_reg.sched_block, facts, mm, Phase::kSched1);
    const std::vector<int> fwd_order =
        Schedule(pre_reg.sched_block, pre_graph, facts, mm, Phase::kSched1, {}).order;
    ASSERT_EQ(fwd_order.size(), pre_reg.sched_block.size());
    std::vector<Insn> scheduled;
    scheduled.reserve(pre_reg.sched_block.size());
    for (size_t i = 0; i < fwd_order.size(); ++i) {
      Insn insn = pre_reg.sched_block[static_cast<size_t>(fwd_order[i])];
      insn.luid = static_cast<int>(i);
      scheduled.push_back(std::move(insn));
    }
    const std::optional<std::vector<int>> inv =
        InvertBlockOrder(scheduled, facts, mm, Phase::kSched1, {});
    ASSERT_TRUE(inv.has_value());
    std::vector<Insn> preimage;
    preimage.reserve(scheduled.size());
    for (size_t i = 0; i < inv->size(); ++i) {
      Insn insn = scheduled[static_cast<size_t>((*inv)[i])];
      insn.luid = static_cast<int>(i);
      preimage.push_back(std::move(insn));
    }
    const DepGraph preimage_graph = Analyze(preimage, facts, mm, Phase::kSched1);
    const std::vector<int> replayed =
        Schedule(preimage, preimage_graph, facts, mm, Phase::kSched1, {}).order;
    ASSERT_EQ(replayed.size(), scheduled.size());
    for (size_t i = 0; i < replayed.size(); ++i) {
      EXPECT_EQ(preimage[static_cast<size_t>(replayed[i])].pn.pattern.ToGccPrintRtlString(),
                scheduled[i].pn.pattern.ToGccPrintRtlString())
          << "kSched1 mismatch at index " << i;
    }
  }

  // 2. `kSched2` with `REG_DEAD (mem ...)` (`sched.c:2095-2102`): post-reload
  //    `REG_DEAD (mem ...)` notes increment `pending_lists_length`, pushing
  //    a block with `<= 32` memory instructions (24 total: 3 independent
  //    stores + 19 loads each carrying `REG_DEAD (mem ...)` + sink + post-sink
  //    store) over the `> 32` pending-op threshold (43 ops). Assert that the
  //    scheduled block is NOT an identity fixed point and has pure flush-cap
  //    artifact edges (`Analyze` vs `AnalyzeWithoutPendingListFlushCap`), so
  //    `InvertBlockOrder` must gate on `total_pending_mem_ops > 32` (not
  //    `mem_insns.size() > 32`) and dissolve the flush cap to replay exactly.
  {
    std::vector<Insn> pre;
    int idx = 0;
    pre.push_back(MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))", idx++));
    pre.push_back(
        MakeInsn("(set (mem/v:SI (symbol_ref:SI (\"g_s2_vol1\"))) (reg:SI 8 t0)) "
                 "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                 idx++));
    pre.push_back(MakeInsn("(set (reg:SI 11 t3) (plus:SI (reg:SI 4 a0) (const_int 2)))", idx++));
    pre.push_back(
        MakeInsn("(set (mem/v:SI (symbol_ref:SI (\"g_s2_vol2\"))) (reg:SI 11 t3)) "
                 "(expr_list:REG_DEAD (reg:SI 11 t3) (nil))",
                 idx++));
    pre.push_back(
        MakeInsn("(set (reg:SI 12 t4) (plus:SI (reg:SI 4 a0) (const_int 3))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 idx++));
    pre.push_back(
        MakeInsn("(set (mem:SI (symbol_ref:SI (\"g_s2_dst3\"))) (reg:SI 12 t4)) "
                 "(expr_list:REG_DEAD (reg:SI 12 t4) (nil))",
                 idx++));
    pre.push_back(
        MakeInsn("(set (reg:SI 9 t1) (mem/u:SI (symbol_ref:SI (\"g_arr\")))) "
                 "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) "
                 "(nil))",
                 idx++));
    for (int k = 1; k < 19; ++k) {
      pre.push_back(MakeInsn(
          absl::StrCat("(set (reg:SI 10 t2) (mem/u:SI (const:SI (plus:SI (symbol_ref:SI "
                       "(\"g_arr\")) (const_int ",
                       k * 4,
                       "))))) (expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int ",
                       16 + k * 4, "))) (nil))"),
          idx++));
      pre.push_back(
          MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 9 t1) (reg:SI 10 t2))) "
                   "(expr_list:REG_DEAD (reg:SI 10 t2) (nil))",
                   idx++));
    }
    pre.push_back(
        MakeInsn("(set (mem:SI (symbol_ref:SI (\"g_s2_sink\"))) (reg:SI 9 t1)) "
                 "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
                 idx++));
    pre.push_back(MakeInsn("(set (reg:SI 13 t5) (const_int 7))", idx++));
    pre.push_back(
        MakeInsn("(set (mem:SI (symbol_ref:SI (\"g_s2_post_sink\"))) (reg:SI 13 t5)) "
                 "(expr_list:REG_DEAD (reg:SI 13 t5) (nil))",
                 idx++));

    int mem_insn_count = 0;
    for (const Insn& insn : pre) {
      const std::string pat_str = insn.pn.pattern.ToGccPrintRtlString();
      if (pat_str.find("(mem") != std::string::npos) {
        ++mem_insn_count;
      }
    }
    ASSERT_LE(mem_insn_count, 32);

    const RegFacts facts;
    const DepGraph pre_graph = Analyze(pre, facts, mm, Phase::kSched2);
    const std::vector<int> fwd_order =
        Schedule(pre, pre_graph, facts, mm, Phase::kSched2, {}).order;
    ASSERT_EQ(fwd_order.size(), pre.size());
    std::vector<Insn> scheduled;
    scheduled.reserve(pre.size());
    for (size_t i = 0; i < fwd_order.size(); ++i) {
      Insn insn = pre[static_cast<size_t>(fwd_order[i])];
      insn.luid = static_cast<int>(i);
      scheduled.push_back(std::move(insn));
    }

    // Verify `scheduled` genuinely hits the `pending_lists_length > 32` flush
    // cap (via `REG_DEAD (mem ...)`) and is NOT an identity fixed point.
    const DepGraph sched_graph = Analyze(scheduled, facts, mm, Phase::kSched2);
    size_t max_store_in_degree = 0;
    for (size_t i = 0; i < scheduled.size(); ++i) {
      const std::string pat_str = scheduled[i].pn.pattern.ToGccPrintRtlString();
      if (pat_str.find("g_s2_vol") != std::string::npos ||
          pat_str.find("g_s2_dst3") != std::string::npos) {
        max_store_in_degree = std::max(max_store_in_degree, sched_graph.log_links[i].size());
      }
    }
    // Semantically each independent store depends only on its single register
    // producer (and prior volatile store); > 16 incoming edges proves the
    // `pending_lists_length > 32` flush fired across <= 32 memory instructions.
    ASSERT_GT(max_store_in_degree, 16u);
    std::vector<int> identity(scheduled.size());
    std::iota(identity.begin(), identity.end(), 0);
    const std::vector<int> id_replay =
        Schedule(scheduled, sched_graph, facts, mm, Phase::kSched2, {}).order;
    ASSERT_NE(id_replay, identity);

    const std::optional<std::vector<int>> inv =
        InvertBlockOrder(scheduled, facts, mm, Phase::kSched2, {});
    ASSERT_TRUE(inv.has_value());
    std::vector<Insn> preimage;
    preimage.reserve(scheduled.size());
    for (size_t i = 0; i < inv->size(); ++i) {
      Insn insn = scheduled[static_cast<size_t>((*inv)[i])];
      insn.luid = static_cast<int>(i);
      preimage.push_back(std::move(insn));
    }
    const DepGraph preimage_graph = Analyze(preimage, facts, mm, Phase::kSched2);
    const std::vector<int> replayed =
        Schedule(preimage, preimage_graph, facts, mm, Phase::kSched2, {}).order;
    ASSERT_EQ(replayed.size(), scheduled.size());
    for (size_t i = 0; i < replayed.size(); ++i) {
      EXPECT_EQ(preimage[static_cast<size_t>(replayed[i])].pn.pattern.ToGccPrintRtlString(),
                scheduled[i].pn.pattern.ToGccPrintRtlString())
          << "kSched2 mismatch at index " << i;
    }
  }
}
INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, SchedModelDifferentialTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });
}  // namespace
}  // namespace rom_nom_nom::lifter::sched_model
