#include "lifter/sched_model.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewSplitsHardRegLiveRangesAndPreservesAbi) {
  const CompilationProfile profile = {2, {}, {}};
  const MachineModel mm = MachineModel::FromProfile(profile);
  std::vector<Insn> post_reload = {
      // Two disjoint `$v0` (`2`) defs/uses (reused hard reg), plus caller-save spill of `$t0` (`8`)
      // across `CALL_INSN`, ABI arg setup `$a0` (`4`), and live-out `$s0` (`16`).
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 5 a1) (const_int 7)))", 0),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 8 t0))", 1),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 8 t0) (const_int 1)))", 2),
      MakeInsn("(set (reg:SI 4 a0) (reg:SI 2 v0))", 3),
      MakeInsn("(parallel[ (call (mem:SI (symbol_ref:SI (\"helper\"))) (const_int 16)) "
               "(clobber (reg:SI 31 ra)) ] )",
               4, Insn::Kind::kCall, {"(use (reg:SI 4 a0))"}),
      MakeInsn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))", 5),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 8 t0) (const_int 2)))", 6),
      MakeInsn("(set (reg:SI 16 s0) (reg:SI 2 v0))", 7),
  };
  const absl::btree_set<int> live_end_s2 = {16};
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(post_reload, mm, live_end_s2);

  // Matched caller-save spill store (idx 1) and reload (idx 5) are excluded -> 6 core insns.
  ASSERT_EQ(view.core_insns.size(), 6U);
  EXPECT_EQ(view.core_to_orig_index, (std::vector<int>{0, 2, 3, 4, 6, 7}));

  // ABI arg setup `$a0` (`4`) before call stays unrenamed (hard reg 4).
  const RtlExpr& arg_set = view.core_insns[2].pn.pattern;
  EXPECT_EQ(arg_set.SetDest().RegNo(), 4);
  // First `$v0` (`2`) live range (core idx 1) and second `$v0` (`2`) live range (core idx 4)
  // are renamed to DISTINCT pseudos >= 72.
  const int v0_first_pseudo = view.core_insns[1].pn.pattern.SetDest().RegNo();
  const int v0_second_pseudo = view.core_insns[4].pn.pattern.SetDest().RegNo();
  EXPECT_GE(v0_first_pseudo, kMipsFirstPseudoRegister);
  EXPECT_GE(v0_second_pseudo, kMipsFirstPseudoRegister);
  EXPECT_NE(v0_first_pseudo, v0_second_pseudo);

  // `$t0` (`8`) was preserved across the call by the excluded caller-save pair, so its
  // renamed pseudo crosses the call (`reg_n_calls_crossed >= 1`) and is read at core idx 4.
  const int t0_pseudo = view.core_insns[0].pn.pattern.SetDest().RegNo();
  EXPECT_GE(t0_pseudo, kMipsFirstPseudoRegister);
  EXPECT_EQ(view.core_insns[4].pn.pattern.SetSrc().children[0].RegNo(), t0_pseudo);
  ASSERT_TRUE(view.facts_s1.reg_n_calls_crossed.contains(t0_pseudo));
  EXPECT_EQ(view.facts_s1.reg_n_calls_crossed.at(t0_pseudo), 1);

  // Live-out `$s0` (`16`) maps to its final renamed pseudo in `live_at_end_s1`.
  const int s0_pseudo = view.core_insns[5].pn.pattern.SetDest().RegNo();
  EXPECT_GE(s0_pseudo, kMipsFirstPseudoRegister);
  EXPECT_TRUE(view.live_at_end_s1.contains(s0_pseudo));
  EXPECT_EQ(view.facts_s1.reg_n_sets.at(s0_pseudo), 1);
}

TEST(SchedModelTest, ProjectPostReloadBlockSplitsRegDeadReuseAndUsesWholeFunctionDefCounts) {
  const MachineModel mm = MachineModel::FromProfile({2, {"-O2", "-mips2", "-mcpu=r4000"}, {}});

  // 1. `flow.c:2506-2515`: `(set r (op r ...))` WITH `REG_DEAD r` came from two distinct
  //    single-set pseudos sharing `r`, whereas WITHOUT `REG_DEAD r` it is one multi-set pseudo.
  std::vector<Insn> reuse_blk = {
      MakeInsn("(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 8 t0) (const_int 1))) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
               1),
      MakeInsn("(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))", 2),
      MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 9 t1) (const_int 2)))", 3),
  };
  const Sched1ProjectionView reuse_view =
      ProjectPostReloadBlockToSched1View(reuse_blk, mm, /*live_at_end_s2=*/{8, 9});
  ASSERT_EQ(reuse_view.core_insns.size(), 4u);
  const int t0_p1 = reuse_view.core_insns[0].pn.pattern.SetDest().RegNo();
  const int t0_p2 = reuse_view.core_insns[1].pn.pattern.SetDest().RegNo();
  EXPECT_NE(t0_p1, t0_p2);
  EXPECT_EQ(reuse_view.facts_s1.reg_n_sets.at(t0_p1), 1);
  EXPECT_EQ(reuse_view.facts_s1.reg_n_sets.at(t0_p2), 1);
  const int t1_p1 = reuse_view.core_insns[2].pn.pattern.SetDest().RegNo();
  const int t1_p2 = reuse_view.core_insns[3].pn.pattern.SetDest().RegNo();
  EXPECT_EQ(t1_p1, t1_p2);
  EXPECT_EQ(reuse_view.facts_s1.reg_n_sets.at(t1_p1), 2);

  // 2. Whole-function `Sched1ProjectionContext` (`sched.c:2469-2501`): incoming param `$a0`
  //    with 0 defs in other blocks keeps `reg_n_sets == 1`, while loop-carried `$v0` (`2`)
  //    initialized in a predecessor gets `reg_n_sets == 2`.
  std::vector<Insn> loop_blk = {
      MakeInsn("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -2)))", 0),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))", 1),
      MakeInsn("(set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))",
               2, Insn::Kind::kJump),
  };
  Sched1ProjectionContext ctx;
  ctx.has_cfg_context = true;
  ctx.cross_block_reaching_defs[2] = 1;
  ctx.other_block_total_defs[2] = 1;
  const Sched1ProjectionView loop_view =
      ProjectPostReloadBlockToSched1View(loop_blk, mm, /*live_at_end_s2=*/{2, 4}, ctx);
  ASSERT_EQ(loop_view.core_insns.size(), 3u);
  EXPECT_EQ(loop_view.facts_s1.reg_n_sets.at(4), 1);
  EXPECT_EQ(loop_view.facts_s1.reg_n_sets.at(2), 2);
  // Because `a0` has `reg_n_sets == 1` (`birthing_insn_p == true`) and `v0` has `2` (`false`),
  // `[a0 -= 2; v0 += 1; bgtz]` is NOT in `Im(F_sched1)`, whereas `[v0 += 1; a0 -= 2; bgtz]` IS.
  EXPECT_FALSE(IsAchievable(loop_view.core_insns, loop_view.facts_s1, mm, Phase::kSched1,
                            loop_view.live_at_end_s1));
}

TEST(SchedModelTest, ComputeCfgSched1ContextCountsExtraB0AssignParmsDefsIdempotently) {
  const MachineModel mm = MachineModel::FromProfile({2, {"-O2", "-mips2", "-mcpu=r4000"}, {}});

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.sideband.basic_block_live_at_start = {
      {0, {4, 29, 31}}, {1, {2, 4, 29, 31}}, {2, {2, 29, 31}}};
  cfg.rpo_blocks.resize(3);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  cfg.rpo_blocks[0].successors = {1};

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].has_label = true;
  cfg.rpo_blocks[1].predecessors = {0, 1};
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -2)))");
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  cfg.rpo_blocks[1].successors = {1, 2};

  cfg.rpo_blocks[2].id = 2;
  cfg.rpo_blocks[2].predecessors = {1};
  cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  // Without `extra_b0_defs`, `$4 a0` has only 1 def (in B1).
  const CfgSched1Context plain_ctx = ComputeCfgSched1Context(cfg, cfg, mm);
  const Sched1ProjectionContext plain_b1 = BuildSched1ProjectionContextForBlock(1, plain_ctx);
  EXPECT_FALSE(plain_b1.cross_block_reaching_defs.contains(4));

  // With `extra_b0_defs = {4}`, `$4 a0` gets 1 reaching entry def from B0 -> `reg_n_sets == 2`.
  const std::vector<int> extra_b0 = {4};
  const CfgSched1Context arg_ctx = ComputeCfgSched1Context(cfg, cfg, mm, extra_b0);
  const Sched1ProjectionContext arg_b1 = BuildSched1ProjectionContextForBlock(1, arg_ctx);
  ASSERT_TRUE(arg_b1.cross_block_reaching_defs.contains(4));
  EXPECT_EQ(arg_b1.cross_block_reaching_defs.at(4), 1);

  const ParsedSchedRegion b1_reg = ParseBlockForSchedModel(cfg.rpo_blocks[1], 1, Phase::kSched2);
  const Sched1ProjectionView b1_view =
      ProjectPostReloadBlockToSched1View(b1_reg.sched_block, mm, /*live_at_end_s2=*/{2, 4}, arg_b1);
  EXPECT_EQ(b1_view.facts_s1.reg_n_sets.at(4), 2);
  EXPECT_EQ(b1_view.facts_s1.reg_n_sets.at(2), 2);

  // Idempotency: if B0 already explicitly defines `$4 a0`, `extra_b0_defs` must not double-count.
  cfg.rpo_blocks[0].instructions.insert(
      cfg.rpo_blocks[0].instructions.begin(),
      "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  const CfgSched1Context idemp_ctx = ComputeCfgSched1Context(cfg, cfg, mm, extra_b0);
  const Sched1ProjectionContext idemp_b1 = BuildSched1ProjectionContextForBlock(1, idemp_ctx);
  ASSERT_TRUE(idemp_b1.cross_block_reaching_defs.contains(4));
  EXPECT_EQ(idemp_b1.cross_block_reaching_defs.at(4), 1);
}

TEST(SchedModelTest, ProjectPostReloadBlockFoldsAdjacentMdOutputReloadAndAnchorsAfterProducer) {
  const MachineModel mm = MachineModel::FromProfile({2, {"-O2", "-mips2", "-mcpu=r4000"}, {}});
  std::vector<Insn> blk = {
      MakeInsn("(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI "
               "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1))) "
               "(const_int 32)))) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] )",
               0),
      MakeInsn("(set (reg:SI 8 t0) (reg:SI 64 hi))", 1),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (const_int 1))) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
               2),
      MakeInsn("(use (reg/i:SI 2 v0))", 3),
  };
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{});
  ASSERT_EQ(view.core_insns.size(), 3u);
  EXPECT_EQ(view.core_to_orig_index, (std::vector<int>{0, 2, 3}));
  ASSERT_TRUE(view.output_reload_after_core.contains(1));
  EXPECT_EQ(view.output_reload_after_core.at(1), 0);
  // Folded `PARALLEL` writes the same fresh pseudo that core insn 1 reads.
  const int mul_pseudo = view.core_insns[0].pn.pattern.children[0].SetDest().RegNo();
  EXPECT_GE(mul_pseudo, kMipsFirstPseudoRegister);
  EXPECT_EQ(view.core_insns[1].pn.pattern.SetSrc().children[0].RegNo(), mul_pseudo);
}

TEST(SchedModelTest, PreservesPostReloadOrderSafetyPureReaderRegDeadRestorationAndGuards) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  const RegFacts empty_facts;

  // (a) Pure-reader stale `REG_DEAD` restoration (`sched.c:3415-3611`):
  //     insn 1 (`v0 = [a0+ids]`, carrying `REG_DEAD a0`) precedes insn 2
  //     (`[a0+states] = a1`) and insn 3 (`[a0+timers] = 0`). Moving insn 1
  //     after insn 3 (`{0, 2, 3, 1, 4}`) restores `REG_DEAD a0` to the true
  //     last reader of `a0` -> allowed even with `allow_transfer = false`.
  std::vector<Insn> stale_block = {
      MakeInsn("(set (reg:SI 4 a0) (ashift:SI (reg:SI 4 a0) (const_int 2))) "
               "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
               0),
      MakeInsn("(set (reg/i:SI 2 v0) (mem/s:SI (plus:SI (reg:SI 4 a0) "
               "(symbol_ref:SI (\"g_ids\"))))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
               1),
      MakeInsn("(set (mem/s:SI (plus:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_states\")))) "
               "(reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
               2),
      MakeInsn("(set (mem/s:SI (plus:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_timers\")))) "
               "(const_int 0))",
               3),
      MakeInsn("(use (reg/i:SI 2 v0))", 4),
  };
  EXPECT_TRUE(PreservesPostReloadOrderSafety(
      stale_block, /*cand_in_base=*/{0, 2, 3, 1, 4}, /*prologue_end=*/0, /*epilogue_begin=*/5,
      /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, empty_facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/false));
  // Moving insn 1 only past insn 2 (leaving reader insn 3 after insn 1) must
  // still be rejected when `allow_pure_reader_reg_dead_transfer == false`.
  EXPECT_FALSE(PreservesPostReloadOrderSafety(
      stale_block, /*cand_in_base=*/{0, 2, 1, 3, 4}, /*prologue_end=*/0, /*epilogue_begin=*/5,
      /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, empty_facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/false));

  // (b) Reverse direction (Stage 0 default `REG_DEAD a0` on insn 3): rejected
  //     with `allow = false`, accepted with `allow = true` for Stage 0 transfer.
  std::vector<Insn> default_dead_block = stale_block;
  default_dead_block[1] = MakeInsn(
      "(set (reg/i:SI 2 v0) (mem/s:SI (plus:SI (reg:SI 4 a0) "
      "(symbol_ref:SI (\"g_ids\")))))",
      1);
  default_dead_block[3] = MakeInsn(
      "(set (mem/s:SI (plus:SI (reg:SI 4 a0) (symbol_ref:SI (\"g_timers\")))) "
      "(const_int 0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      3);
  EXPECT_FALSE(PreservesPostReloadOrderSafety(
      default_dead_block, /*cand_in_base=*/{0, 2, 3, 1, 4}, /*prologue_end=*/0,
      /*epilogue_begin=*/5, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, empty_facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/false));
  EXPECT_TRUE(PreservesPostReloadOrderSafety(
      default_dead_block, /*cand_in_base=*/{0, 2, 3, 1, 4}, /*prologue_end=*/0,
      /*epilogue_begin=*/5, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, empty_facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/true));

  // (c) Negative guard: dying-input RMW (`i` defines `a0` with `REG_DEAD a0`)
  //     must NEVER flip past a reader of `a0`, even with `allow = true`.
  std::vector<Insn> rmw_block = {
      MakeInsn("(set (reg:SI 6 a2) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 4))) "
               "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
               1),
      MakeInsn("(use (reg/i:SI 2 v0))", 2),
  };
  EXPECT_FALSE(PreservesPostReloadOrderSafety(
      rmw_block, /*cand_in_base=*/{1, 0, 2}, /*prologue_end=*/0, /*epilogue_begin=*/3,
      /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, empty_facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/true));
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesTransfersStaleDeathUnderO2Only) {
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

TEST(SchedModelTest, FindInterleavedPrologueSaveBodyIndicesExcludesOnlyPreWriteSavesInBlock0) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  const MachineModel mm = MachineModel::FromProfile(prof);
  std::vector<Insn> blk = {
      MakeInsn("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))", 0),
      MakeInsn("(set (reg:SI 3 v1) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))", 1),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 31 ra))", 2),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 16 s0))", 3),
      MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 3 v1) (const_int 4)))", 4),
      // Post-write store of `$s0` (already modified by body insn 4): NOT a prologue save!
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0))", 5),
      MakeInsn("(use (reg/i:SI 2 v0))", 6),
  };
  const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(blk, /*block_idx=*/0);
  ASSERT_EQ(bounds.prologue_end, 1);
  ASSERT_EQ(bounds.epilogue_begin, 7);

  const absl::btree_set<int> excluded_b0 =
      FindInterleavedPrologueSaveBodyIndices(blk, bounds, /*block_idx=*/0, mm);
  // Body-relative indices: 0=move v1,a0; 1=sw $ra; 2=sw $s0 (pre-write); 3=s0=...; 4=sw $s0 (post)
  EXPECT_EQ(excluded_b0, (absl::btree_set<int>{1, 2}));

  // Non-entry block (`block_idx != 0`) must never exclude interleaved prologue saves.
  const absl::btree_set<int> excluded_b1 =
      FindInterleavedPrologueSaveBodyIndices(blk, bounds, /*block_idx=*/1, mm);
  EXPECT_TRUE(excluded_b1.empty());

  // `ProjectPostReloadBlockToSched1View` must drop excluded prologue saves from `core_insns`.
  std::vector<Insn> body(blk.begin() + 1, blk.end());
  for (size_t i = 0; i < body.size(); ++i) {
    body[i].luid = static_cast<int>(i);
  }
  Sched1ProjectionContext ctx;
  ctx.excluded_prologue_body_indices = excluded_b0;
  const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(body, mm, {}, ctx);
  EXPECT_EQ(view.core_to_orig_index, (std::vector<int>{0, 3, 4, 5}));
}

TEST(SchedModelTest, PreservesPostReloadOrderSafetyAllowsMatchingRegEquivRestorationAndCallSeqs) {
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

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewPinsLeadingEntryArgCopiesOnlyInEntryBlock) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);
  const std::vector<Insn> body = {
      MakeInsn("(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))", 0),
      MakeInsn("(set (reg:SI 17 s1) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))", 1),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 17 s1))) "
               "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SI 17 s1) (nil)))",
               2),
      MakeInsn("(use (reg/i:SI 2 v0))", 3),
  };

  // Positive: in entry block (`is_entry_block = true`), `sched.c:3191-3215` pins
  // leading hard-reg arg copies outside `core_insns` while preserving their `reg_n_sets`.
  Sched1ProjectionContext entry_ctx;
  entry_ctx.is_entry_block = true;
  const Sched1ProjectionView entry_view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, entry_ctx);
  EXPECT_EQ(entry_view.pinned_prefix_orig_indices, (std::vector<int>{0, 1}));
  EXPECT_EQ(entry_view.core_to_orig_index, (std::vector<int>{2, 3}));
  ASSERT_EQ(entry_view.core_insns.size(), 2u);
  const RtlExpr& plus_src = entry_view.core_insns[0].pn.pattern.SetSrc();
  ASSERT_EQ(plus_src.children.size(), 2u);
  const int p_s0 = plus_src.children[0].RegNo();
  const int p_s1 = plus_src.children[1].RegNo();
  EXPECT_GE(p_s0, kMipsFirstPseudoRegister);
  EXPECT_GE(p_s1, kMipsFirstPseudoRegister);
  EXPECT_EQ(entry_view.facts_s1.reg_n_sets.at(p_s0), 1);
  EXPECT_EQ(entry_view.facts_s1.reg_n_sets.at(p_s1), 1);

  // Negative: in a non-entry block (`is_entry_block = false`), leading copies are
  // ordinary schedulable instructions inside `core_insns`.
  Sched1ProjectionContext non_entry_ctx;
  non_entry_ctx.is_entry_block = false;
  const Sched1ProjectionView non_entry_view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, non_entry_ctx);
  EXPECT_TRUE(non_entry_view.pinned_prefix_orig_indices.empty());
  EXPECT_EQ(non_entry_view.core_to_orig_index, (std::vector<int>{0, 1, 2, 3}));
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewAddsOtherBlockDefsForAllUnrenamedHardRegs) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);
  Insn call_insn = MakeInsn(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
      1, Insn::Kind::kCall);
  auto use_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  ASSERT_TRUE(use_a0.ok());
  call_insn.pn.has_explicit_call_fusage = true;
  call_insn.pn.call_fusage = {std::move(use_a0->pattern)};
  const std::vector<Insn> body = {
      MakeInsn("(set (reg:SI 4 a0) (plus:SI (reg:SI 16 s0) (const_int 1)))", 0),
      std::move(call_insn),
  };

  // Positive: `flow.c:2047` / `sched.c:2469-2501` whole-function `reg_n_sets`
  // includes `other_block_total_defs` for unrenamed ABI-pinned hard regs (`$a0`).
  Sched1ProjectionContext multi_ctx;
  multi_ctx.has_cfg_context = true;
  multi_ctx.other_block_total_defs[4] = 1;
  const Sched1ProjectionView multi_view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, multi_ctx);
  ASSERT_TRUE(multi_view.facts_s1.reg_n_sets.contains(4));
  EXPECT_EQ(multi_view.facts_s1.reg_n_sets.at(4), 2);

  // Negative: with no definitions of `$a0` in other blocks, `reg_n_sets[4]` stays 1.
  Sched1ProjectionContext single_ctx;
  single_ctx.has_cfg_context = true;
  const Sched1ProjectionView single_view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, single_ctx);
  ASSERT_TRUE(single_view.facts_s1.reg_n_sets.contains(4));
  EXPECT_EQ(single_view.facts_s1.reg_n_sets.at(4), 1);
}

TEST(SchedModelTest,
     PreservesPostReloadOrderSafetyUsesPostReloadStackLayoutForPreReloadStackAliasing) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);
  const RegFacts empty_facts;

  // Layout of a non-FP frame (`frame_size = 64`, `starting_frame_offset = 24`):
  //   - `$sp + 16`, `$sp + 20`: outgoing call arguments (`< 24`, pre-reload `$sp`);
  //   - `$sp + 32`: local stack buffer (`>= 24`, pre-reload `$fp`).
  const PostReloadStackLayout layout{
      .valid = true,
      .frame_pointer_needed = false,
      .starting_frame_offset = 24,
      .frame_size = 64,
  };

  // Positive (no false aliasing): disjoint outgoing-argument `$sp` slots
  // (`$sp+16` vs `$sp+20`) share base `$sp` pre-reload (`sched.c:636-669`), so
  // moving `(set (mem (+ sp 20)) a2)` into the lifetime of `(set v1 (mem (+ sp 16)))`
  // does NOT invalidate `validate_equiv_mem`.
  const std::vector<Insn> disjoint_outgoing = {
      MakeInsn("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))", 0),
      MakeInsn("(set (reg:SI 8 t0) (const_int 5))", 1),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
               "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
               2),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 6 a2)) "
               "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
               3),
      MakeInsn("(use (reg/i:SI 2 v0))", 4),
  };
  EXPECT_TRUE(PreservesPostReloadOrderSafety(
      disjoint_outgoing, /*cand_in_base=*/{0, 1, 3, 2, 4}, /*prologue_end=*/0, /*epilogue_begin=*/5,
      /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, empty_facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/false, layout));

  // Negative (true pre-reload aliasing): outgoing-argument store `$sp+16`
  // (pre-reload `$sp`) vs local stack load `$sp+32` (pre-reload `$fp`) had
  // distinct stack bases pre-reload (`config/mips/mips.h:1649-1651, 1756-1764`,
  // `sched.c:817-840`), so moving the `$sp+16` store into `$v1`'s live range
  // flips `validate_equiv_mem` from `true` to `false` and must be rejected.
  const std::vector<Insn> cross_base_stack = {
      MakeInsn("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))))", 0),
      MakeInsn("(set (reg:SI 8 t0) (const_int 5))", 1),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
               "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
               2),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 6 a2)) "
               "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
               3),
      MakeInsn("(use (reg/i:SI 2 v0))", 4),
  };
  EXPECT_FALSE(PreservesPostReloadOrderSafety(
      cross_base_stack, /*cand_in_base=*/{0, 1, 3, 2, 4}, /*prologue_end=*/0, /*epilogue_begin=*/5,
      /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, empty_facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/false, layout));
}

TEST(SchedModelTest, FindPostReloadBodyBoundsExcludesTrailingReturnInFramelessLeafBlocks) {
  // Positive: frameless leaf block ending in `(use (reg/i:SI 2 v0))` + `(return)`
  // excludes only `(return)` (`function.c:5516`, `mips.c:mips_expand_epilogue`),
  // keeping `(use (reg/i:SI 2 v0))` inside `[prologue_end, epilogue_begin)`.
  const std::vector<Insn> leaf_return_block = {
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(use (reg/i:SI 2 v0))", 1),
      MakeInsn("(parallel[ (return) (use (reg:SI 31 ra)) ] )", 2, Insn::Kind::kJump),
  };
  const PostReloadBodyBounds ret_bounds = FindPostReloadBodyBounds(leaf_return_block, 0);
  EXPECT_EQ(ret_bounds.prologue_end, 0);
  EXPECT_EQ(ret_bounds.epilogue_begin, 2);

  // Negative: a block ending in an ordinary branch/jump retains `epilogue_begin == n`.
  const std::vector<Insn> jump_block = {
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(set (pc) (label_ref L1))", 1, Insn::Kind::kJump),
  };
  const PostReloadBodyBounds jmp_bounds = FindPostReloadBodyBounds(jump_block, 1);
  EXPECT_EQ(jmp_bounds.prologue_end, 0);
  EXPECT_EQ(jmp_bounds.epilogue_begin, 2);
}

TEST(SchedModelTest,
     CollectDisplacedBlock0EntryArgCopyBodyIndicesDistinguishesEntryCopiesFromReloadsAndCalls) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Block-0 body with:
  //   0: leading pinned `(set s1 a0)` (`sched.c:3191-3215`)
  //   1: early body load `(set v0 (mem (plus s1 12)))`
  //   2: displaced GPR entry-arg copy `(set s2 a1)` -> POSITIVE
  //   3: displaced callee-saved FPR entry-arg copy `(set $f22 a2)` -> POSITIVE
  //   4: caller-saved scratch `mtc1` input reload `(set $f2 a3)` before `mult:SF` -> NEGATIVE
  //   5: `(set $f0 (mult:SF $f0 $f2))`
  //   6: overwrite of `a1` `(set a1 (const_int 1))`
  //   7: copy from overwritten `a1` `(set s3 a1)` -> NEGATIVE (source clobbered)
  //   8: outgoing call-arg move `(set a0 a3)` feeding `call_fusage` -> NEGATIVE
  //   9: `CALL_INSN` using `a0`
  //  10: post-call copy `(set s4 a0)` -> NEGATIVE (after call)
  Insn call_insn = MakeInsn(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"helper\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
      9, Insn::Kind::kCall);
  call_insn.pn.has_explicit_call_fusage = true;
  auto fusage_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  ASSERT_TRUE(fusage_a0.ok());
  call_insn.pn.call_fusage = {fusage_a0->pattern};

  const std::vector<Insn> body = {
      MakeInsn("(set (reg:SI 17 s1) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))", 0),
      MakeInsn("(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 17 s1) (const_int 12))))", 1),
      MakeInsn("(set (reg:SI 18 s2) (reg:SI 5 a1))", 2),
      MakeInsn("(set (reg:SF 54 $f22) (reg:SF 6 a2)) (expr_list:REG_DEAD (reg:SF 6 a2) (nil))", 3),
      MakeInsn("(set (reg:SF 34 $f2) (reg:SF 7 a3))", 4),
      MakeInsn("(set (reg:SF 32 $f0) (mult:SF (reg:SF 32 $f0) (reg:SF 34 $f2)))", 5),
      MakeInsn("(set (reg:SI 5 a1) (const_int 1))", 6),
      MakeInsn("(set (reg:SI 19 s3) (reg:SI 5 a1))", 7),
      MakeInsn("(set (reg:SI 4 a0) (reg:SI 7 a3))", 8),
      std::move(call_insn),
      MakeInsn("(set (reg:SI 20 s4) (reg:SI 4 a0))", 10),
  };
  Sched1ProjectionContext ctx;
  ctx.is_entry_block = true;
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, ctx);
  const std::vector<int> displaced =
      CollectDisplacedBlock0EntryArgCopyBodyIndices(body, view, /*excluded_prologue=*/{}, mm);
  EXPECT_EQ(displaced, (std::vector<int>{2, 3}));
}

TEST(SchedModelTest,
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

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewFoldsAdjacentFprInputReloads) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  auto make_insn = [](int luid, absl::string_view text) {
    Insn insn;
    insn.luid = luid;
    insn.kind = Insn::Kind::kInsn;
    auto parsed = ParsePatternAndNotes(text);
    EXPECT_TRUE(parsed.ok()) << text;
    if (parsed.ok()) {
      insn.pn = std::move(*parsed);
    }
    return insn;
  };

  // Positive: `reload1.c:5641, 5849-5871, 6118-6125` cross-bank `"f"` `mtc1`
  // input reload `(set $f2 a1)` (no `REG_DEAD a1`, `$f2` dead after `mult:SF`)
  // folds onto `mult:SF`, substituting `$f2 -> a1` in read positions so `sched1`
  // sees `(mult:SF (reg:SF 72) (reg:SF 5 a1))` and `IsAchievable(kSched1)` passes.
  std::vector<Insn> pos_block = {
      make_insn(0, "(set (reg:SF 32 $f0) (float:SF (reg:SI 4 a0)))"),
      make_insn(1, "(set (reg:SF 34 $f2) (reg:SF 5 a1))"),
      make_insn(2,
                "(set (reg:SF 32 $f0) (mult:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
                "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 5 a1) (nil)))"),
      make_insn(3,
                "(set (mem:SF (reg:SI 7 a3)) (reg:SF 32 $f0)) "
                "(expr_list:REG_DEAD (reg:SI 7 a3) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil)))"),
  };
  const Sched1ProjectionView pos_view =
      ProjectPostReloadBlockToSched1View(pos_block, mm, /*live_at_end_s2=*/{});
  ASSERT_EQ(pos_view.core_to_orig_index, (std::vector<int>{0, 2, 3}));
  ASSERT_EQ(pos_view.core_insns.size(), 3u);
  EXPECT_EQ(pos_view.core_insns[1].pn.pattern.SetSrc().op, RtlOp::kMult);
  EXPECT_EQ(pos_view.core_insns[1].pn.pattern.SetSrc().children[1].op, RtlOp::kReg);
  EXPECT_EQ(pos_view.core_insns[1].pn.pattern.SetSrc().children[1].RegNo(), 5);
  EXPECT_TRUE(IsAchievable(pos_view.core_insns, pos_view.facts_s1, mm, Phase::kSched1,
                           pos_view.live_at_end_s1));

  // Negative 1: an entry-parameter copy `(set $f2 a1) (REG_DEAD a1)` carries
  // `REG_DEAD gpr` from `flow.c` and must NOT be folded as an input reload.
  std::vector<Insn> dead_copy_block = pos_block;
  dead_copy_block[1] =
      make_insn(1, "(set (reg:SF 34 $f2) (reg:SF 5 a1)) (expr_list:REG_DEAD (reg:SF 5 a1) (nil))");
  const Sched1ProjectionView dead_copy_view =
      ProjectPostReloadBlockToSched1View(dead_copy_block, mm, /*live_at_end_s2=*/{});
  EXPECT_EQ(dead_copy_view.core_to_orig_index, (std::vector<int>{0, 1, 2, 3}));

  // Negative 2: when `$f2` remains live after `mult:SF` (`live_at_end_s2` contains
  // 34), `(set $f2 a1)` is NOT a dead single-use input reload and must NOT fold.
  const Sched1ProjectionView live_after_view =
      ProjectPostReloadBlockToSched1View(pos_block, mm, /*live_at_end_s2=*/{34});
  EXPECT_EQ(live_after_view.core_to_orig_index, (std::vector<int>{0, 1, 2, 3}));
}

TEST(SchedModelTest, IsAdjacentReloadContiguityPairDistinguishesReloadsFromLocalStackAndEntryCopy) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  auto make_insn = [](int luid, absl::string_view text) {
    Insn insn;
    insn.luid = luid;
    insn.kind = Insn::Kind::kInsn;
    auto parsed = ParsePatternAndNotes(text);
    EXPECT_TRUE(parsed.ok()) << text;
    if (parsed.ok()) {
      insn.pn = std::move(*parsed);
    }
    return insn;
  };

  PostReloadStackLayout layout;
  layout.valid = true;
  layout.starting_frame_offset = 16;
  layout.frame_size = 64;
  layout.has_stack_slot_access_info = true;
  layout.stack_slot_store_offsets = {24, 32};
  layout.stack_slot_load_offsets = {32};  // Only offset 32 is both stored and reloaded.

  const Insn add_f2 =
      make_insn(0, "(set (reg:SF 34 $f2) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22)))");
  const Insn store_spill_32 =
      make_insn(1, "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:SF 34 $f2))");
  const Insn store_local_24 =
      make_insn(1, "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SF 34 $f2))");
  const Insn store_out_arg_8 =
      make_insn(1, "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 8))) (reg:SF 34 $f2))");

  // Positive: true spill slot (in local frame and loaded elsewhere) stays paired.
  EXPECT_TRUE(IsAdjacentReloadContiguityPair(add_f2, store_spill_32, mm, layout));
  // Negative: local stack-array store never reloaded elsewhere, or outgoing arg slot,
  // must NOT be glued as a reload spill.
  EXPECT_FALSE(IsAdjacentReloadContiguityPair(add_f2, store_local_24, mm, layout));
  EXPECT_FALSE(IsAdjacentReloadContiguityPair(add_f2, store_out_arg_8, mm, layout));

  // Cross-bank `mtc1`: reload (no `REG_DEAD gpr`) pairs; entry-arg copy (`REG_DEAD a1`) does not.
  const Insn mtc1_reload = make_insn(0, "(set (reg:SF 34 $f2) (reg:SF 5 a1))");
  const Insn mtc1_entry_copy =
      make_insn(0, "(set (reg:SF 52 $f20) (reg:SF 5 a1)) (expr_list:REG_DEAD (reg:SF 5 a1) (nil))");
  const Insn fp_use_f2 =
      make_insn(1, "(set (reg:SF 32 $f0) (mult:SF (reg:SF 32 $f0) (reg:SF 34 $f2)))");
  const Insn fp_use_f20 =
      make_insn(1, "(set (reg:SF 44 $f12) (plus:SF (reg:SF 52 $f20) (reg:SF 52 $f20)))");
  EXPECT_TRUE(IsAdjacentReloadContiguityPair(mtc1_reload, fp_use_f2, mm, layout));
  EXPECT_FALSE(IsAdjacentReloadContiguityPair(mtc1_entry_copy, fp_use_f20, mm, layout));

  // Positive (inherited spill slot, `reload1.c:6134-6140`): a stack spill written
  // to `(mem:DI (reg:SI 29 sp))` with no explicit reload load, only a consumer
  // `(REG_DEAD (mem:DI (reg:SI 29 sp)))`, is recorded in `stack_slot_load_offsets`
  // by `ComputeCfgSched1Context` so its `(def, spill_store)` pair stays contiguous.
  RtlControlFlowGraph inh_cfg;
  inh_cfg.pass = RtlPassKind::kSched2;
  inh_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& ib = inh_cfg.rpo_blocks[0];
  ib.id = 0;
  ib.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -16)))");
  ib.AddPatternOnly("(set (reg:DI 10 t2) (reg:DI 64 hi))");
  ib.AddPatternOnly("(set (mem:DI (reg:SI 29 sp)) (reg:DI 10 t2))");
  ib.AddPatternOnly(
      "(set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 10 t2))) "
      "(expr_list:REG_DEAD (mem:DI (reg:SI 29 sp)) (nil))");
  ib.AddPatternOnly("(use (reg/i:DI 2 v0))");
  const CfgSched1Context inh_ctx = ComputeCfgSched1Context(inh_cfg, inh_cfg, mm);
  EXPECT_TRUE(inh_ctx.stack_layout.stack_slot_load_offsets.contains(0));
  const ParsedSchedRegion inh_reg = ParseBlockForSchedModel(ib, 0, Phase::kSched2);
  ASSERT_TRUE(inh_reg.valid);
  EXPECT_TRUE(IsAdjacentReloadContiguityPair(inh_reg.sched_block[1], inh_reg.sched_block[2], mm,
                                             inh_ctx.stack_layout));
}

TEST(SchedModelTest, HasSched1AchievablePreimageForSched2BlockFindsPostSched2Preimage) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Pre-`sched2` block 0 whose `Schedule(kSched2)` interleaves callee-save store
  // `s.d $f20` and displaces entry-arg copy `$f20 = a1` past `a0 = sp + 16`:
  // the resulting post-`sched2` block is in `Im(F_sched2)`, NOT directly in
  // `Im(F_sched1)`, yet admits a `kSched2`-verified preimage in `Im(F_sched1)`.
  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kGreg;
  pre_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& pre_blk = pre_cfg.rpo_blocks[0];
  pre_blk.id = 0;
  pre_blk.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -48)))");
  pre_blk.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))) (reg:SI 16 s0))");
  pre_blk.AddPatternOnly("(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:DF 52 $f20))");
  pre_blk.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 40))) (reg:SI 31 ra))");
  pre_blk.AddPatternOnly(
      "(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pre_blk.AddPatternOnly(
      "(set (reg:SF 52 $f20) (reg:SF 5 a1)) (expr_list:REG_DEAD (reg:SF 5 a1) (nil))");
  pre_blk.AddPatternOnly("(set (reg:SI 4 a0) (plus:SI (reg:SI 29 sp) (const_int 16)))");
  pre_blk.AddPatternOnly("(set (reg:SI 5 a1) (const_int 1))");
  pre_blk.AddPatternOnly(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )");
  auto u_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  auto u_a1 = ParsePatternAndNotes("(use (reg:SI 5 a1))");
  ASSERT_TRUE(u_a0.ok() && u_a1.ok());
  pre_blk.rtl_insns.back().has_explicit_call_fusage = true;
  pre_blk.rtl_insns.back().call_fusage = {u_a0->pattern, u_a1->pattern};

  const ParsedSchedRegion pre_reg = ParseBlockForSchedModel(pre_blk, 0, Phase::kSched2);
  ASSERT_TRUE(pre_reg.valid);
  const RegFacts pre_facts = RegFacts::FromCfg(pre_cfg, prof, Phase::kSched2);
  const DepGraph pre_g = Analyze(pre_reg.sched_block, pre_facts, mm, Phase::kSched2);
  const ScheduleResult s2_res =
      Schedule(pre_reg.sched_block, pre_g, pre_facts, mm, Phase::kSched2, /*live_at_end=*/{});
  ASSERT_EQ(s2_res.order.size(), pre_reg.sched_block.size());

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(1);
  RtlBasicBlock& blk = cfg.rpo_blocks[0];
  blk.id = 0;
  for (int idx : s2_res.order) {
    blk.instructions.push_back(pre_blk.instructions[static_cast<size_t>(idx)]);
    blk.rtl_insns.push_back(pre_blk.rtl_insns[static_cast<size_t>(idx)]);
  }

  const ParsedSchedRegion region = ParseBlockForSchedModel(blk, 0, Phase::kSched2);
  ASSERT_TRUE(region.valid);
  const RegFacts facts_s2 = RegFacts::FromCfg(cfg, prof, Phase::kSched2);
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, cfg, mm);
  const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, 0);
  Sched1ProjectionContext proj_ctx = BuildSched1ProjectionContextForBlock(0, cfg_ctx);
  proj_ctx.excluded_prologue_body_indices =
      FindInterleavedPrologueSaveBodyIndices(region.sched_block, bounds, 0, mm);
  std::vector<Insn> body;
  for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
    Insn insn = region.sched_block[static_cast<size_t>(i)];
    insn.luid = i - bounds.prologue_end;
    body.push_back(std::move(insn));
  }
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, proj_ctx);

  // Positive: in `Im(F_sched2)`, not directly in `Im(F_sched1)`, yet hoisting
  // displaced entry-arg copy `$f20 = a1` recovers a `kSched2`-verified preimage in `Im(F_sched1)`.
  EXPECT_TRUE(IsAchievable(region.sched_block, facts_s2, mm, Phase::kSched2, /*live_at_end=*/{}));
  EXPECT_FALSE(
      IsAchievable(view.core_insns, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1));
  EXPECT_TRUE(HasSched1AchievablePreimageForSched2Block(region.sched_block, /*block_idx=*/0, bounds,
                                                        view, proj_ctx, cfg_ctx, facts_s2, mm,
                                                        /*live_at_end_s2=*/{}));

  // Negative: a post-`sched2` block outside `Im(F_sched2)` and `Im(F_sched1)` (short ALU
  // chain placed ahead of an independent high-latency FP divide chain) has NO
  // `kSched2`-verified preimage.
  RtlControlFlowGraph bad_cfg;
  bad_cfg.pass = RtlPassKind::kSched2;
  bad_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& bblk = bad_cfg.rpo_blocks[0];
  bblk.id = 0;
  bblk.AddPatternOnly("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 44 $f12)))");
  bblk.AddPatternOnly(
      "(set (reg:SF 34 $f2) (div:SF (reg:SF 46 $f14) (reg:SF 44 $f12))) "
      "(expr_list:REG_DEAD (reg:SF 46 $f14) (expr_list:REG_DEAD (reg:SF 44 $f12) (nil)))");
  bblk.AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
      "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 34 $f2) (nil)))");
  bblk.AddPatternOnly("(use (reg/i:SF 32 $f0))");
  const ParsedSchedRegion breg = ParseBlockForSchedModel(bblk, 0, Phase::kSched2);
  ASSERT_TRUE(breg.valid);
  const RegFacts bfacts = RegFacts::FromCfg(bad_cfg, prof, Phase::kSched2);
  const CfgSched1Context bctx = ComputeCfgSched1Context(bad_cfg, bad_cfg, mm);
  const PostReloadBodyBounds bbounds = FindPostReloadBodyBounds(breg.sched_block, 0);
  const Sched1ProjectionContext bpctx = BuildSched1ProjectionContextForBlock(0, bctx);
  const Sched1ProjectionView bview =
      ProjectPostReloadBlockToSched1View(breg.sched_block, mm, /*live_at_end_s2=*/{}, bpctx);
  EXPECT_FALSE(IsAchievable(breg.sched_block, bfacts, mm, Phase::kSched2, /*live_at_end=*/{}));
  EXPECT_FALSE(
      IsAchievable(bview.core_insns, bview.facts_s1, mm, Phase::kSched1, bview.live_at_end_s1));
  EXPECT_FALSE(HasSched1AchievablePreimageForSched2Block(breg.sched_block, /*block_idx=*/0, bbounds,
                                                         bview, bpctx, bctx, bfacts, mm,
                                                         /*live_at_end_s2=*/{}));
}

TEST(SchedModelTest, Sched1704PostCallCalleeSavedHoistRelaxationPositiveAndNegative) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  auto make_block_with_call = [](absl::Span<const std::string> insns, size_t call_idx) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(1);
    RtlBasicBlock& blk = cfg.rpo_blocks[0];
    blk.id = 0;
    for (const std::string& s : insns) {
      blk.AddPatternOnly(s);
    }
    auto u_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
    EXPECT_TRUE(u_a0.ok());
    blk.rtl_insns[call_idx].has_explicit_call_fusage = true;
    blk.rtl_insns[call_idx].call_fusage = {u_a0->pattern};
    return cfg;
  };

  // Positive (`sched.c:1704`): start from a pre-`sched2` block where pure
  // callee-saved `$f20 = $f22 + $f24` waits on a high-latency `div:DF $f22`
  // before `call`; `Schedule(kSched2)` sinks `$f20 = $f22 + $f24` after `call`
  // (where `Analyze(post_block)` adds the asymmetric `sched.c:1704` anti-edge),
  // and `InvertBlockOrder(kSched2)` + `PreservesPostReloadOrderSafety` hoist it
  // back ahead of `call`.
  RtlControlFlowGraph pre_cfg = make_block_with_call(
      {
          "(set (reg:DF 54 $f22) (div:DF (reg:DF 54 $f22) (reg:DF 56 $f24)))",
          "(set (reg:DF 52 $f20) (plus:DF (reg:DF 52 $f20) (reg:DF 54 $f22)))",
          "(set (reg:SI 4 a0) (const_int 1))",
          "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
          "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
      },
      /*call_idx=*/3);
  const ParsedSchedRegion pre_reg =
      ParseBlockForSchedModel(pre_cfg.rpo_blocks[0], 0, Phase::kSched2);
  ASSERT_TRUE(pre_reg.valid);
  const RegFacts pre_facts = RegFacts::FromCfg(pre_cfg, prof, Phase::kSched2);
  const DepGraph pre_g = Analyze(pre_reg.sched_block, pre_facts, mm, Phase::kSched2);
  const absl::btree_set<int> live_out = {2, 52, 53, 54, 55};
  const ScheduleResult s2_res =
      Schedule(pre_reg.sched_block, pre_g, pre_facts, mm, Phase::kSched2, live_out);
  ASSERT_EQ(s2_res.order.size(), 5u);
  const int post_f20_pos = static_cast<int>(std::find(s2_res.order.begin(), s2_res.order.end(), 1) -
                                            s2_res.order.begin());
  const int post_call_pos = static_cast<int>(
      std::find(s2_res.order.begin(), s2_res.order.end(), 3) - s2_res.order.begin());
  ASSERT_GT(post_f20_pos, post_call_pos);

  std::vector<Insn> post_block;
  std::vector<int> pre_in_post(5, -1);
  post_block.reserve(5);
  for (int p = 0; p < 5; ++p) {
    Insn insn = pre_reg.sched_block[static_cast<size_t>(s2_res.order[static_cast<size_t>(p)])];
    insn.luid = p;
    post_block.push_back(std::move(insn));
    pre_in_post[static_cast<size_t>(s2_res.order[static_cast<size_t>(p)])] = p;
  }
  const std::optional<std::vector<int>> pos_inv =
      InvertBlockOrder(post_block, pre_facts, mm, Phase::kSched2, live_out, pre_in_post);
  ASSERT_TRUE(pos_inv.has_value());
  EXPECT_LT(std::find(pos_inv->begin(), pos_inv->end(), post_f20_pos) - pos_inv->begin(),
            std::find(pos_inv->begin(), pos_inv->end(), post_call_pos) - pos_inv->begin());
  EXPECT_TRUE(PreservesPostReloadOrderSafety(post_block, *pos_inv, /*prologue_end=*/0,
                                             /*epilogue_begin=*/5, live_out, {}, pre_facts, mm));

  // Negative: post-call insn writing a call-used reg (`$t0`) MUST NOT be hoisted
  // ahead of the call (rejected by `PreservesPostReloadOrderSafety`).
  RtlControlFlowGraph neg_call_used = make_block_with_call(
      {
          "(set (reg:SI 4 a0) (const_int 1))",
          "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
          "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
          "(set (reg:SI 8 t0) (plus:SI (reg:SI 17 s1) (const_int 5)))",
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
      },
      /*call_idx=*/1);
  const ParsedSchedRegion neg_reg =
      ParseBlockForSchedModel(neg_call_used.rpo_blocks[0], 0, Phase::kSched2);
  ASSERT_TRUE(neg_reg.valid);
  const RegFacts neg_facts = RegFacts::FromCfg(neg_call_used, prof, Phase::kSched2);
  const std::vector<int> bad_hoist = {2, 0, 1, 3};
  EXPECT_FALSE(PreservesPostReloadOrderSafety(neg_reg.sched_block, bad_hoist, /*prologue_end=*/0,
                                              /*epilogue_begin=*/4, /*live_at_end_s2=*/{2, 8}, {},
                                              neg_facts, mm));
}

TEST(SchedModelTest, FindInterleavedPrologueSaveBodyIndicesDetectsSched2InterleavedLeafSpDec) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive: leaf block where `sched2` hoisted loads ahead of `sp -= 16` and
  // `s.d $f20, 0($sp)` (`bounds.prologue_end == 0`). Both the interleaved
  // `sp -= 16` (idx 1) and `s.d $f20` (idx 2) must be excluded.
  RtlControlFlowGraph pos_cfg;
  pos_cfg.pass = RtlPassKind::kSched2;
  pos_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& pb = pos_cfg.rpo_blocks[0];
  pb.id = 0;
  pb.AddPatternOnly("(set (reg:DF 32 $f0) (mem/u:DF (reg:SI 4 a0)))");
  pb.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -16)))");
  pb.AddPatternOnly("(set (mem:DF (reg:SI 29 sp)) (reg:DF 52 $f20))");
  pb.AddPatternOnly("(set (reg:DF 52 $f20) (mem/u:DF (plus:SI (reg:SI 4 a0) (const_int 8))))");
  pb.AddPatternOnly("(set (reg/i:DF 32 $f0) (plus:DF (reg:DF 32 $f0) (reg:DF 52 $f20)))");
  pb.AddPatternOnly("(set (reg:DF 52 $f20) (mem:DF (reg:SI 29 sp)))");
  pb.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 16)))");
  pb.AddPatternOnly("(use (reg/i:DF 32 $f0))");
  const ParsedSchedRegion preg = ParseBlockForSchedModel(pb, 0, Phase::kSched2);
  ASSERT_TRUE(preg.valid);
  const PostReloadBodyBounds pbounds = FindPostReloadBodyBounds(preg.sched_block, 0);
  EXPECT_EQ(pbounds.prologue_end, 0);
  const absl::btree_set<int> excluded =
      FindInterleavedPrologueSaveBodyIndices(preg.sched_block, pbounds, 0, mm);
  EXPECT_TRUE(excluded.contains(1));
  EXPECT_TRUE(excluded.contains(2));
  EXPECT_FALSE(excluded.contains(0));
  EXPECT_FALSE(excluded.contains(3));

  // Negative: if `$sp` was already read/modified before `sp -= 16` (not a
  // prologue decrement), do NOT exclude it as an interleaved prologue.
  RtlControlFlowGraph neg_cfg;
  neg_cfg.pass = RtlPassKind::kSched2;
  neg_cfg.rpo_blocks.resize(1);
  RtlBasicBlock& nb = neg_cfg.rpo_blocks[0];
  nb.id = 0;
  nb.AddPatternOnly("(set (reg:SI 8 t0) (mem:SI (reg:SI 29 sp)))");
  nb.AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -16)))");
  nb.AddPatternOnly("(set (mem:DF (reg:SI 29 sp)) (reg:DF 52 $f20))");
  nb.AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 8 t0))");
  nb.AddPatternOnly("(use (reg/i:SI 2 v0))");
  const ParsedSchedRegion nreg = ParseBlockForSchedModel(nb, 0, Phase::kSched2);
  ASSERT_TRUE(nreg.valid);
  const PostReloadBodyBounds nbounds = FindPostReloadBodyBounds(nreg.sched_block, 0);
  EXPECT_TRUE(FindInterleavedPrologueSaveBodyIndices(nreg.sched_block, nbounds, 0, mm).empty());
}

TEST(SchedModelTest, ComputeCfgSched1ContextExcludesCrossBankCopyPairs) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive + Negative (`global.c:1010-1014`, `local-alloc.c:2148`):
  // Same-bank GPR copy (`$s0 = $a0`) and same-bank FPR copy (`$f20 = $f12`) are
  // recorded in `cfg_copy_pairs`, whereas cross-bank `mtc1` (`$f6 = $s0`) and
  // `mfc1` (`$v0 = $f20`) moves between disjoint register files are excluded.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 16 s0) (reg:SI 4 a0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 52 $f20) (reg:SF 44 $f12))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SF 38 $f6) (reg:SF 16 s0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (reg:SI 52 $f20))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const CfgSched1Context ctx = ComputeCfgSched1Context(cfg, cfg, mm);
  EXPECT_TRUE(ctx.cfg_copy_pairs.contains({4, 16}));
  EXPECT_TRUE(ctx.cfg_copy_pairs.contains({44, 52}));
  EXPECT_FALSE(ctx.cfg_copy_pairs.contains({16, 38}));
  EXPECT_FALSE(ctx.cfg_copy_pairs.contains({2, 52}));
}

TEST(SchedModelTest, ProjectPostReloadBlockSplitsPostCallCombineRegsRmwPseudo) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(1);
  RtlBasicBlock& blk = cfg.rpo_blocks[0];
  blk.id = 0;
  blk.AddPatternOnly("(set (reg:SF 52 $f20) (reg:SF 44 $f12))");
  blk.AddPatternOnly("(set (reg:SF 54 $f22) (reg:SF 46 $f14))");
  blk.AddPatternOnly(
      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"step\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
  blk.AddPatternOnly("(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22)))");
  blk.AddPatternOnly("(set (mem:SF (reg:SI 16 s0)) (reg:SF 52 $f20))");
  const ParsedSchedRegion reg = ParseBlockForSchedModel(blk, 0, Phase::kSched2);
  ASSERT_TRUE(reg.valid);

  // Positive: `$f20` dies inside block after post-call RMW -> split into a fresh
  // `reg_n_calls_crossed == 0` pseudo (`local-alloc.c:1266-1300, 1855`, `sched.c:1732`).
  const Sched1ProjectionView split_view =
      ProjectPostReloadBlockToSched1View(reg.sched_block, mm, /*live_at_end_s2=*/{});
  ASSERT_EQ(split_view.core_insns.size(), 5u);
  const int p_old = split_view.core_insns[0].pn.pattern.SetDest().RegNo();
  const int p_new = split_view.core_insns[3].pn.pattern.SetDest().RegNo();
  EXPECT_NE(p_old, p_new);
  EXPECT_FALSE(split_view.facts_s1.reg_n_calls_crossed.contains(p_new));

  // Negative: if `$f20` (52) is live at end of block (not block-local), do NOT split.
  const Sched1ProjectionView live_out_view =
      ProjectPostReloadBlockToSched1View(reg.sched_block, mm, /*live_at_end_s2=*/{52});
  ASSERT_EQ(live_out_view.core_insns.size(), 5u);
  EXPECT_EQ(live_out_view.core_insns[0].pn.pattern.SetDest().RegNo(),
            live_out_view.core_insns[3].pn.pattern.SetDest().RegNo());
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesRestoresPostCallCombineRegsRmwDeadNote) {
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

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewGeneralCombineRegsRmwSplitsAndGuards) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive: leaf block-local RMW `(set v0 (plus v0 a1))` splits into a fresh
  // `reg_n_sets == 1` pseudo under `allow_general_combine_regs_split = true`,
  // and stays unsplit when `false`.
  RtlBasicBlock leaf_blk;
  leaf_blk.id = 0;
  leaf_blk.AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
  leaf_blk.AddPatternOnly(
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  leaf_blk.AddPatternOnly(
      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");
  const ParsedSchedRegion leaf_reg = ParseBlockForSchedModel(leaf_blk, 0, Phase::kSched2);
  ASSERT_TRUE(leaf_reg.valid);

  Sched1ProjectionContext unsplit_ctx;
  unsplit_ctx.allow_post_call_combine_regs_split = false;
  unsplit_ctx.allow_general_combine_regs_split = false;
  const Sched1ProjectionView unsplit_view =
      ProjectPostReloadBlockToSched1View(leaf_reg.sched_block, mm, {}, unsplit_ctx);
  ASSERT_EQ(unsplit_view.core_insns.size(), 3u);
  EXPECT_EQ(unsplit_view.core_insns[0].pn.pattern.SetDest().RegNo(),
            unsplit_view.core_insns[1].pn.pattern.SetDest().RegNo());

  Sched1ProjectionContext gen_ctx;
  gen_ctx.allow_general_combine_regs_split = true;
  const Sched1ProjectionView gen_view =
      ProjectPostReloadBlockToSched1View(leaf_reg.sched_block, mm, {}, gen_ctx);
  ASSERT_EQ(gen_view.core_insns.size(), 3u);
  EXPECT_NE(gen_view.core_insns[0].pn.pattern.SetDest().RegNo(),
            gen_view.core_insns[1].pn.pattern.SetDest().RegNo());
  EXPECT_EQ(gen_view.implicit_combine_regs_split_orig_indices, std::vector<int>({1}));

  // Negative 1: MIPS `lui + ori` 32-bit constant materialization stays unsplit.
  RtlBasicBlock lui_ori_blk;
  lui_ori_blk.id = 0;
  lui_ori_blk.AddPatternOnly("(set (reg:SI 2 v0) (const_int 65536))");
  lui_ori_blk.AddPatternOnly("(set (reg:SI 2 v0) (ior:SI (reg:SI 2 v0) (const_int 4660)))");
  lui_ori_blk.AddPatternOnly(
      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");
  const ParsedSchedRegion lui_reg = ParseBlockForSchedModel(lui_ori_blk, 0, Phase::kSched2);
  ASSERT_TRUE(lui_reg.valid);
  const Sched1ProjectionView lui_view =
      ProjectPostReloadBlockToSched1View(lui_reg.sched_block, mm, {}, gen_ctx);
  EXPECT_TRUE(lui_view.implicit_combine_regs_split_orig_indices.empty());

  // Negative 2 (`local-alloc.c:892-920` `update_equiv_regs`): when `birth_insn`
  // is a same-mode copy `(set v0 a1)` with live `a1`, do NOT split the RMW.
  RtlBasicBlock copy_blk;
  copy_blk.id = 0;
  copy_blk.AddPatternOnly("(set (reg:SI 2 v0) (reg:SI 5 a1))");
  copy_blk.AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
  copy_blk.AddPatternOnly(
      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");
  const ParsedSchedRegion copy_reg = ParseBlockForSchedModel(copy_blk, 0, Phase::kSched2);
  ASSERT_TRUE(copy_reg.valid);
  const Sched1ProjectionView copy_view =
      ProjectPostReloadBlockToSched1View(copy_reg.sched_block, mm, /*live_at_end_s2=*/{5}, gen_ctx);
  EXPECT_TRUE(copy_view.implicit_combine_regs_split_orig_indices.empty());

  // Negative 3 (`expmed.c:693-707` `store_fixed_bit_field`): multi-step
  // intra-block `and`/`ior` bitfield RMW chains reuse a single pseudo and
  // must NOT be split by `IsGeneralCombineRegsRmwSplitCandidate`.
  RtlBasicBlock bf_blk;
  bf_blk.id = 0;
  bf_blk.AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
  bf_blk.AddPatternOnly("(set (reg:SI 2 v0) (and:SI (reg:SI 2 v0) (const_int 65535)))");
  bf_blk.AddPatternOnly(
      "(set (reg:SI 2 v0) (ior:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  bf_blk.AddPatternOnly(
      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");
  const ParsedSchedRegion bf_reg = ParseBlockForSchedModel(bf_blk, 0, Phase::kSched2);
  ASSERT_TRUE(bf_reg.valid);
  const Sched1ProjectionView bf_view =
      ProjectPostReloadBlockToSched1View(bf_reg.sched_block, mm, {}, gen_ctx);
  EXPECT_TRUE(bf_view.implicit_combine_regs_split_orig_indices.empty());
}

TEST(SchedModelTest, IsAdjacentMdOutputReloadPairAndNonAdjacentProjectionFold) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  RtlBasicBlock blk;
  blk.id = 0;
  blk.AddPatternOnly(
      "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI "
      "(sign_extend:DI (reg:SI 3 v1)) (sign_extend:DI (reg:SI 2 v0))) (const_int 32)))) "
      "(clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  blk.AddPatternOnly("(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 3 v1) (const_int 31)))");
  blk.AddPatternOnly("(set (reg:SI 6 a2) (reg:SI 64 hi))");
  blk.AddPatternOnly("(set (reg:SI 7 a3) (reg:SI 65 lo))");
  blk.AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 6 a2) (reg:SI 3 v1))) "
      "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  const ParsedSchedRegion reg = ParseBlockForSchedModel(blk, 0, Phase::kSched2);
  ASSERT_TRUE(reg.valid);
  ASSERT_EQ(reg.sched_block.size(), 5u);

  // Positive: `(mult -> hi, mfhi)` and `(mfhi, mflo)` are adjacent MD reload pairs.
  EXPECT_TRUE(IsAdjacentMdOutputReloadPair(reg.sched_block[0], reg.sched_block[2]));
  EXPECT_TRUE(IsAdjacentMdOutputReloadPair(reg.sched_block[2], reg.sched_block[3]));
  // Negative: an interleaved ALU insn (`sra v0, v1, 31`) before `mfhi` is NOT a pair,
  // and a following consumer without `(REG_DEAD lo)` is NOT a pair.
  EXPECT_FALSE(IsAdjacentMdOutputReloadPair(reg.sched_block[1], reg.sched_block[2]));
  EXPECT_FALSE(IsAdjacentMdOutputReloadPair(reg.sched_block[3], reg.sched_block[4]));

  // Positive: `(mflo, consumer carrying REG_DEAD lo)` IS an adjacent MD reload pair
  // so Stage 3 preserves `REG_DEAD lo` for `InvertReload`.
  RtlBasicBlock md_dead_blk;
  md_dead_blk.id = 0;
  md_dead_blk.AddPatternOnly("(set (reg:SI 7 a3) (reg:SI 65 lo))");
  md_dead_blk.AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 7 a3) (const_int 12345))) "
      "(expr_list:REG_DEAD (reg:SI 65 lo) (nil))");
  const ParsedSchedRegion md_dead_reg = ParseBlockForSchedModel(md_dead_blk, 0, Phase::kSched2);
  ASSERT_TRUE(md_dead_reg.valid);
  ASSERT_EQ(md_dead_reg.sched_block.size(), 2u);
  EXPECT_TRUE(IsAdjacentMdOutputReloadPair(md_dead_reg.sched_block[0], md_dead_reg.sched_block[1]));

  // Positive projection: `mfhi` at index 2 folds onto `mult` at index 0 across
  // the interleaved `sra` at index 1.
  RtlBasicBlock fold_blk;
  fold_blk.id = 0;
  fold_blk.AddPatternOnly(blk.instructions[0]);
  fold_blk.AddPatternOnly(blk.instructions[1]);
  fold_blk.AddPatternOnly(blk.instructions[2]);
  fold_blk.AddPatternOnly(blk.instructions[4]);
  const ParsedSchedRegion fold_reg = ParseBlockForSchedModel(fold_blk, 0, Phase::kSched2);
  ASSERT_TRUE(fold_reg.valid);
  const Sched1ProjectionView fold_view =
      ProjectPostReloadBlockToSched1View(fold_reg.sched_block, mm, {});
  EXPECT_EQ(fold_view.core_insns.size(), 3u);
  ASSERT_TRUE(fold_view.output_reload_after_core.contains(2));
  EXPECT_EQ(fold_view.output_reload_after_core.at(2), 0);

  // Negative projection: if an intervening insn writes `a2` (`6`), do NOT fold `mfhi`.
  RtlBasicBlock clobber_blk;
  clobber_blk.id = 0;
  clobber_blk.AddPatternOnly(blk.instructions[0]);
  clobber_blk.AddPatternOnly("(set (reg:SI 6 a2) (const_int 7))");
  clobber_blk.AddPatternOnly(blk.instructions[2]);
  clobber_blk.AddPatternOnly(blk.instructions[4]);
  const ParsedSchedRegion clobber_reg = ParseBlockForSchedModel(clobber_blk, 0, Phase::kSched2);
  ASSERT_TRUE(clobber_reg.valid);
  const Sched1ProjectionView clobber_view =
      ProjectPostReloadBlockToSched1View(clobber_reg.sched_block, mm, {});
  EXPECT_EQ(clobber_view.core_insns.size(), 4u);
  EXPECT_FALSE(clobber_view.output_reload_after_core.contains(2));
}

TEST(SchedModelTest, ComputeCfgSched1ContextExcludesCallerSaveRestoresAndQualifiesModes) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // 2-block CFG:
  //  B0 defines loop-invariant `$t7` (15, SI), `$f0` (32, DF), and `$v0` (2, SI),
  //     all live into B1.
  //  B1 caller-save stores & restores `$t7` across a call (NOT a real def),
  //     defines `$f0` ONLY in `SF` mode (disjoint from B0's `DF` pseudo),
  //     and redefines `$v0` in `SI` mode (same-mode cross-block join).
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(2);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 15 t7) (mem/u:SI (reg:SI 4 a0)))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:DF 32 $f0) (mem/u:DF (reg:SI 5 a1)))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].successors = {1};
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:SI 15 t7))");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"probe\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 15 t7) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))))");
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SF 32 $f0) (float_truncate:SF (reg:DF 32 $f0))) "
      "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))");
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 15 t7)))");

  RtlControlFlowGraph live_cfg = cfg;
  live_cfg.sideband.basic_block_live_at_start[1] = {2, 15, 32, 33};

  const CfgSched1Context ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
  const Sched1ProjectionContext b0_ctx = BuildSched1ProjectionContextForBlock(0, ctx);

  // Positive 1: caller-save restore of `$t7` (15) in B1 is excluded from B0's cross-block defs.
  EXPECT_FALSE(b0_ctx.cross_block_reaching_defs.contains(15));
  EXPECT_FALSE(b0_ctx.cross_block_reaching_defs_by_mode.contains({15, RtlMachineMode::kSi}));

  // Positive 2: B1's `SF` def of `$f0` (32) does NOT add a cross-block def for B0's `DF` pseudo.
  EXPECT_FALSE(b0_ctx.cross_block_reaching_defs_by_mode.contains({32, RtlMachineMode::kDf}));
  EXPECT_EQ(b0_ctx.cross_block_reaching_defs_by_mode.at({32, RtlMachineMode::kSf}), 1);

  // Negative (same-mode join): B1's `SI` def of `$v0` (2) DOES increment B0's `SI` pseudo.
  ASSERT_TRUE(b0_ctx.cross_block_reaching_defs_by_mode.contains({2, RtlMachineMode::kSi}));
  EXPECT_EQ(b0_ctx.cross_block_reaching_defs_by_mode.at({2, RtlMachineMode::kSi}), 1);

  const ParsedSchedRegion b0_reg = ParseBlockForSchedModel(cfg.rpo_blocks[0], 0, Phase::kSched2);
  ASSERT_TRUE(b0_reg.valid);
  const Sched1ProjectionView b0_view =
      ProjectPostReloadBlockToSched1View(b0_reg.sched_block, mm, {2, 15, 32, 33}, b0_ctx);
  ASSERT_EQ(b0_view.core_insns.size(), 3u);
  const int t7_pseudo = b0_view.core_insns[0].pn.pattern.SetDest().RegNo();
  const int f0_pseudo = b0_view.core_insns[1].pn.pattern.SetDest().RegNo();
  const int v0_pseudo = b0_view.core_insns[2].pn.pattern.SetDest().RegNo();
  EXPECT_EQ(b0_view.facts_s1.reg_n_sets.at(t7_pseudo), 1);
  EXPECT_EQ(b0_view.facts_s1.reg_n_sets.at(f0_pseudo), 1);
  EXPECT_EQ(b0_view.facts_s1.reg_n_sets.at(v0_pseudo), 2);
}

TEST(SchedModelTest, AttributeSched2SkipsRaSaveForCallDelaySlotAndNeverTransfersPlaceholderDead) {
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

TEST(SchedModelTest, ComputeCfgSched1ContextExactDuWebsAndIncomingAbiArgRmw) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // 1. Positive (disjoint DU-webs): two blocks define `$v0` (`reg 2`) on
  // disjoint webs (each killed in its own block, never reaching a shared use) ->
  // `block_exit_web_other_defs` is 0 (`reg_n_sets == 1`, `birthing_insn_p == true`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1, 2};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref L2) (pc))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].predecessors = {0};
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (const_int 11))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].predecessors = {0};
    cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 2 v0) (const_int 22))");
    cfg.rpo_blocks[2].AddPatternOnly(
        "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))");

    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, o2_prof,
                           {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    const Sched1ProjectionContext ctx_b0 = BuildSched1ProjectionContextForBlock(0, cfg_ctx);
    EXPECT_TRUE(ctx_b0.has_per_endpoint_web_defs);
    EXPECT_EQ(ctx_b0.exit_web_other_defs.count({2, RtlMachineMode::kSi}), 0u);
  }

  // 2. Negative (shared join web): diamond arms both define `$v0` reaching a
  // shared join-block use -> `exit_web_other_defs[2] >= 1` (`reg_n_sets >= 2`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(4);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1, 2};
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L2) (pc))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].predecessors = {0};
    cfg.rpo_blocks[1].successors = {3};
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].predecessors = {0};
    cfg.rpo_blocks[2].successors = {3};
    cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 2 v0) (const_int 2))");

    cfg.rpo_blocks[3].id = 3;
    cfg.rpo_blocks[3].predecessors = {1, 2};
    cfg.rpo_blocks[3].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[3].AddPatternOnly("(use (reg/i:SI 2 v0))");

    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, o2_prof,
                           {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    const Sched1ProjectionContext ctx_b1 = BuildSched1ProjectionContextForBlock(1, cfg_ctx);
    ASSERT_TRUE(ctx_b1.has_per_endpoint_web_defs);
    EXPECT_GE(ctx_b1.exit_web_other_defs.at({2, RtlMachineMode::kSi}), 1);
  }

  // 3. Negative (cross-block `call_fusage` ABI arg, `calls.c:emit_call_1`,
  // `flow.c:2065`): an ABI arg `$a1` (`reg 5`) defined in block 1 and consumed
  // only by a downstream `CALL_INSN` `call_fusage` in block 2 remains an
  // unrenamed hard reg whose `reg_n_sets` counts all explicit sets in other blocks.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1, 2};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (const_int 7))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L2) (pc)))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].predecessors = {0};
    cfg.rpo_blocks[1].successors = {2};
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 5 a1) (const_int 9))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].predecessors = {0, 1};
    cfg.rpo_blocks[2].AddPatternOnly(
        "(parallel[ (call (mem:SI (symbol_ref:SI (\"sink\"))) (const_int 16)) "
        "(clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[2].rtl_insns[0].has_explicit_call_fusage = true;
    cfg.rpo_blocks[2].rtl_insns[0].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
    };

    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, o2_prof,
                           {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    const Sched1ProjectionContext ctx_b1 = BuildSched1ProjectionContextForBlock(1, cfg_ctx);
    ASSERT_TRUE(ctx_b1.has_per_endpoint_web_defs);
    EXPECT_GE(ctx_b1.exit_web_other_defs.at({5, RtlMachineMode::kSi}), 1);
  }

  // 4. Positive + Negative (`function.c:assign_parms`): an entry-block RMW of an
  // incoming ABI arg `$a0` allocates a fresh single-set pseudo, whereas the same
  // RMW in a non-entry block (`is_entry_block == false`) stays unsplit.
  {
    RtlBasicBlock blk;
    blk.id = 0;
    blk.AddPatternOnly("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 4)))");
    blk.AddPatternOnly(
        "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    const ParsedSchedRegion reg = ParseBlockForSchedModel(blk, 0, Phase::kSched2);
    ASSERT_TRUE(reg.valid);

    Sched1ProjectionContext entry_ctx;
    entry_ctx.is_entry_block = true;
    entry_ctx.allow_post_call_combine_regs_split = false;
    entry_ctx.allow_general_combine_regs_split = false;
    const Sched1ProjectionView entry_view =
        ProjectPostReloadBlockToSched1View(reg.sched_block, mm, {}, entry_ctx);
    ASSERT_EQ(entry_view.core_insns.size(), 2u);
    const int entry_dst = entry_view.core_insns[0].pn.pattern.SetDest().RegNo();
    EXPECT_GE(entry_dst, kMipsFirstPseudoRegister);
    EXPECT_EQ(entry_view.facts_s1.reg_n_sets.at(entry_dst), 1);

    Sched1ProjectionContext non_entry_ctx = entry_ctx;
    non_entry_ctx.is_entry_block = false;
    const Sched1ProjectionView non_entry_view =
        ProjectPostReloadBlockToSched1View(reg.sched_block, mm, {}, non_entry_ctx);
    ASSERT_EQ(non_entry_view.core_insns.size(), 2u);
    EXPECT_EQ(non_entry_view.core_insns[0].pn.pattern.SetDest().RegNo(), 4);
  }
}

TEST(SchedModelTest, CollapseSched1UnsplitConstDoubleLoadsSiRecoveryAndGuards) {
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

TEST(SchedModelTest, ComputeCfgSched1ContextExtractsPositiveFrameSizeAndEscapeAndSpillCounts) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  auto make_two_block_cfg = [](const std::string& sp_dec, bool escape_sp) {
    RtlControlFlowGraph cfg;
    RtlBasicBlock b0;
    b0.successors = {1};
    b0.instructions = {
        sp_dec,
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 5)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 2 v0))",
        "(set (reg:SI 2 v0) (const_int 0))",
    };
    if (escape_sp) {
      b0.instructions.push_back("(set (reg:SI 5 a1) (plus:SI (reg:SI 29 sp) (const_int 16)))");
    }
    b0.SyncRtlInsnsFromInstructions();

    RtlBasicBlock b1;
    b1.instructions = {
        "(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))",
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 112)))",
        "(use (reg/i:SI 2 v0))",
    };
    b1.SyncRtlInsnsFromInstructions();

    cfg.rpo_blocks = {std::move(b0), std::move(b1)};
    return cfg;
  };

  // Positive 1: `kMinus` prologue decrement yields positive `frame_size == 112`
  // (`mips.c:expand_prologue`), no stack escape, and cross-block spill counts.
  const RtlControlFlowGraph minus_cfg =
      make_two_block_cfg("(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 112)))",
                         /*escape_sp=*/false);
  RtlControlFlowGraph minus_live = minus_cfg;
  ReconstructRtlLiveness(
      &minus_live, o2_prof,
      {.reload_completed = true, .update_reg_notes = false, .preserve_phantom_pseudos = false});
  const CfgSched1Context minus_ctx = ComputeCfgSched1Context(minus_cfg, minus_live, mm);
  EXPECT_EQ(minus_ctx.stack_layout.frame_size, 112);
  EXPECT_FALSE(minus_ctx.has_stack_addr_escape);
  const Sched1ProjectionContext b0_ctx = BuildSched1ProjectionContextForBlock(0, minus_ctx);
  const Sched1ProjectionContext b1_ctx = BuildSched1ProjectionContextForBlock(1, minus_ctx);
  EXPECT_TRUE(b0_ctx.other_block_spill_load_offsets.contains(24));
  EXPECT_EQ(b1_ctx.other_block_spill_store_counts.at(24), 1);

  // Positive 2: `kPlus` negative-immediate prologue decrement yields positive `frame_size == 64`.
  const RtlControlFlowGraph plus_cfg =
      make_two_block_cfg("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -64)))",
                         /*escape_sp=*/false);
  RtlControlFlowGraph plus_live = plus_cfg;
  ReconstructRtlLiveness(
      &plus_live, o2_prof,
      {.reload_completed = true, .update_reg_notes = false, .preserve_phantom_pseudos = false});
  const CfgSched1Context plus_ctx = ComputeCfgSched1Context(plus_cfg, plus_live, mm);
  EXPECT_EQ(plus_ctx.stack_layout.frame_size, 64);
  EXPECT_FALSE(plus_ctx.has_stack_addr_escape);

  // Negative: taking the address of a stack slot sets `has_stack_addr_escape == true`.
  const RtlControlFlowGraph esc_cfg =
      make_two_block_cfg("(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 112)))",
                         /*escape_sp=*/true);
  RtlControlFlowGraph esc_live = esc_cfg;
  ReconstructRtlLiveness(
      &esc_live, o2_prof,
      {.reload_completed = true, .update_reg_notes = false, .preserve_phantom_pseudos = false});
  const CfgSched1Context esc_ctx = ComputeCfgSched1Context(esc_cfg, esc_live, mm);
  EXPECT_TRUE(esc_ctx.has_stack_addr_escape);
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewFoldsCrossBlockAndAcrossCallSpills) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // 1. Positive (cross-block `reload1.c:2309-2400, 5641-6160` spill store in B0
  //    and spill load in B1): B0 folds the spill store onto its producer (marked
  //    live-out in `live_at_end_s1`), and B1 folds the spill load onto its
  //    consumer (recording `input_reload_before_core`).
  Sched1ProjectionContext b0_ctx;
  b0_ctx.has_cfg_context = true;
  b0_ctx.stack_layout.valid = true;
  b0_ctx.stack_layout.frame_size = 64;
  b0_ctx.stack_layout.starting_frame_offset = 16;
  b0_ctx.other_block_spill_load_offsets.insert(24);
  const std::vector<Insn> b0_body = {
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 5)))", 0),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 2 v0))", 1),
      MakeInsn("(set (reg:SI 2 v0) (const_int 99))", 2),
  };
  const Sched1ProjectionView b0_view =
      ProjectPostReloadBlockToSched1View(b0_body, mm, /*live_at_end_s2=*/{2}, b0_ctx);
  ASSERT_EQ(b0_view.core_insns.size(), 2u);
  EXPECT_EQ(b0_view.output_reload_after_core.at(1), 0);
  const int spilled_pseudo_b0 = b0_view.core_insns[0].pn.pattern.SetDest().RegNo();
  EXPECT_GE(spilled_pseudo_b0, kMipsFirstPseudoRegister);
  EXPECT_TRUE(b0_view.live_at_end_s1.contains(spilled_pseudo_b0));

  Sched1ProjectionContext b1_ctx;
  b1_ctx.has_cfg_context = true;
  b1_ctx.stack_layout.valid = true;
  b1_ctx.stack_layout.frame_size = 64;
  b1_ctx.stack_layout.starting_frame_offset = 16;
  b1_ctx.other_block_spill_store_counts[24] = 1;
  const std::vector<Insn> b1_body = {
      MakeInsn("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))", 0),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1))) "
               "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
               1),
      MakeInsn("(use (reg/i:SI 2 v0))", 2),
  };
  const Sched1ProjectionView b1_view =
      ProjectPostReloadBlockToSched1View(b1_body, mm, /*live_at_end_s2=*/{}, b1_ctx);
  ASSERT_EQ(b1_view.core_insns.size(), 2u);
  EXPECT_EQ(b1_view.input_reload_before_core.at(0), 0);
  const RtlExpr& b1_add_src = b1_view.core_insns[0].pn.pattern.SetSrc();
  ASSERT_EQ(b1_add_src.op, RtlOp::kPlus);
  EXPECT_GE(b1_add_src.children[0].RegNo(), kMipsFirstPseudoRegister);

  // 2. Positive (intra-block across-call reload spill reusing scratch `$v0`
  //    after an intervening overwrite before `CALL_INSN`, distinguishing it
  //    from `caller-save.c:480-580`): producer and consumer share the same
  //    unified spilled pseudo across the call.
  const std::vector<Insn> across_call_body = {
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3)))", 0),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 2 v0))", 1),
      MakeInsn("(set (reg:SI 2 v0) (const_int 1))", 2),
      MakeInsn("(set (reg:SI 4 a0) (reg:SI 2 v0))", 3),
      MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) "
               "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
               4, Insn::Kind::kCall, {"(use (reg:SI 4 a0))"}),
      MakeInsn("(set (reg:SI 3 v1) (reg:SI 2 v0))", 5),
      MakeInsn("(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))", 6),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
               "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
               7),
      MakeInsn("(use (reg/i:SI 2 v0))", 8),
  };
  const Sched1ProjectionView ac_view =
      ProjectPostReloadBlockToSched1View(across_call_body, mm, /*live_at_end_s2=*/{}, {});
  EXPECT_EQ(ac_view.output_reload_after_core.at(1), 0);
  EXPECT_TRUE(ac_view.input_reload_before_core.contains(6));
  const int prod_pseudo = ac_view.core_insns[0].pn.pattern.SetDest().RegNo();
  const int cons_core_idx = ac_view.input_reload_before_core.at(6);
  const RtlExpr& cons_rhs =
      ac_view.core_insns[static_cast<size_t>(cons_core_idx)].pn.pattern.SetSrc();
  ASSERT_EQ(cons_rhs.op, RtlOp::kPlus);
  EXPECT_EQ(cons_rhs.children[1].RegNo(), prod_pseudo);

  // 3. Negative (low-pressure leaf stack-struct copy `sw 0($sp); lw 0($sp)`
  //    with no call and `< 8` intervening defs): must NOT fold onto a spilled
  //    pseudo via `FoldReloadSpillsForSched1View`.
  const std::vector<Insn> leaf_copy_body = {
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(set (mem:SI (reg:SI 29 sp)) (reg:SI 8 t0))", 1),
      MakeInsn("(set (reg:SI 9 t1) (mem:SI (reg:SI 29 sp)))", 2),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (const_int 2))) "
               "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
               3),
      MakeInsn("(use (reg/i:SI 2 v0))", 4),
  };
  const Sched1ProjectionView leaf_view =
      ProjectPostReloadBlockToSched1View(leaf_copy_body, mm, /*live_at_end_s2=*/{}, {});
  EXPECT_FALSE(leaf_view.input_reload_before_core.contains(2));
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesAlignsMovedReturnDelaySlot) {
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

TEST(SchedModelTest, PreservesPostReloadOrderSafetyPureMdOutputAndCallCrossingExemptions) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  const RegFacts facts_s2;

  // 1. `reload1.c:3619-3663` (`order_regs_for_reload`): swapping a pure `mflo`
  //    output-reload pair (`hi = $t1`, `hard_reg_n_uses == 0`) ahead of a body insn
  //    defining already-used `lo = $a2` drops `(a2, t1)` interference safely;
  //    if `$t1` is ALSO defined by a non-`md` body insn (`uses > 0`), it is rejected.
  auto make_md_disjoint_block = [](bool t1_also_used_in_body) {
    std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 6 a2) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
        MakeInsn("(parallel[ (set (reg:SI 65 lo) (div:SI (reg:SI 5 a1) (reg:SI 7 a3))) "
                 "(set (reg:SI 64 hi) (mod:SI (reg:SI 5 a1) (reg:SI 7 a3))) ] )",
                 1),
        MakeInsn("(set (reg:SI 9 t1) (reg:SI 65 lo))", 2),
        MakeInsn("(set (mem:SI (symbol_ref:SI (\"g_out\"))) (reg:SI 9 t1)) "
                 "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
                 3),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 6 a2) (const_int 2))) "
                 "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                 4),
    };
    if (t1_also_used_in_body) {
      blk.push_back(MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 2 v0) (const_int 5)))", 5));
      blk.push_back(
          MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 9 t1)) "
                   "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
                   6));
      blk.push_back(MakeInsn("(use (reg/i:SI 2 v0))", 7));
    } else {
      blk.push_back(MakeInsn("(use (reg/i:SI 2 v0))", 5));
    }
    return blk;
  };
  {
    const std::vector<Insn> pos_blk = make_md_disjoint_block(/*t1_also_used_in_body=*/false);
    // Base has `(6 a2, 9 t1)` interference; `cand = [1, 2, 3, 0, 4, 5]` drops `(6, 9)`.
    const std::vector<int> pos_cand = {1, 2, 3, 0, 4, 5};
    EXPECT_TRUE(PreservesPostReloadOrderSafety(pos_blk, pos_cand, /*prologue_end=*/0,
                                               /*epilogue_begin=*/5, /*live_at_end_s2=*/{},
                                               /*cfg_copy_pairs=*/{}, facts_s2, mm));

    const std::vector<Insn> neg_blk = make_md_disjoint_block(/*t1_also_used_in_body=*/true);
    const std::vector<int> neg_cand = {1, 2, 3, 0, 4, 5, 6, 7};
    EXPECT_FALSE(PreservesPostReloadOrderSafety(neg_blk, neg_cand, /*prologue_end=*/0,
                                                /*epilogue_begin=*/7, /*live_at_end_s2=*/{},
                                                /*cfg_copy_pairs=*/{}, facts_s2, mm));
  }

  // 2. `local-alloc.c:2106` / `global.c:927` (`COPY_HARD_REG_SET (..., call_used_reg_set)`):
  //    dropping an interference `(lo, hi)` between a call-used reg `lo` (`$v1`)
  //    and a callee-saved `mflo` output reg `hi` (`$s0`, also defined in the body
  //    so `md_blocked` applies) is safe when `hi` crosses a call (`call_crossing_regs`
  //    excludes `call_used_reg_set`), and rejected when `hi` does NOT cross a call.
  auto make_call_crossing_md_block = [](bool with_call) {
    std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 0),
        MakeInsn("(parallel[ (set (reg:SI 65 lo) (div:SI (reg:SI 5 a1) (reg:SI 7 a3))) "
                 "(set (reg:SI 64 hi) (mod:SI (reg:SI 5 a1) (reg:SI 7 a3))) ] )",
                 1),
        MakeInsn("(set (reg:SI 16 s0) (reg:SI 65 lo))", 2),
        MakeInsn("(set (mem:SI (symbol_ref:SI (\"g_out\"))) (reg:SI 3 v1)) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                 3),
    };
    if (with_call) {
      Insn call = MakeInsn(
          "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
          "(clobber (reg:SI 31 ra)) ] )",
          4);
      call.kind = Insn::Kind::kCall;
      blk.push_back(std::move(call));
      blk.push_back(MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 2)))", 5));
      blk.push_back(
          MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 16 s0)) "
                   "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                   6));
      blk.push_back(MakeInsn("(use (reg/i:SI 2 v0))", 7));
    } else {
      blk.push_back(MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 2)))", 4));
      blk.push_back(
          MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 16 s0)) "
                   "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                   5));
      blk.push_back(MakeInsn("(use (reg/i:SI 2 v0))", 6));
    }
    return blk;
  };
  {
    // Moving insn 3 (`sw g_out, v1 (REG_DEAD v1)`) ahead of `div; mflo s0` (1, 2)
    // drops `(3 v1, 16 s0)` interference while keeping `div; mflo s0` contiguous.
    EXPECT_TRUE(PreservesPostReloadOrderSafety(
        make_call_crossing_md_block(/*with_call=*/true), /*cand_in_base=*/{0, 3, 1, 2, 4, 5, 6, 7},
        /*prologue_end=*/0, /*epilogue_begin=*/7, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{},
        facts_s2, mm));
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        make_call_crossing_md_block(/*with_call=*/false), /*cand_in_base=*/{0, 3, 1, 2, 4, 5, 6},
        /*prologue_end=*/0, /*epilogue_begin=*/6, /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{},
        facts_s2, mm));
  }
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewUneliminatesStackMemsForPreReloadAliasing) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // `reload1.c:2641` (`eliminate_regs`) + `mips.h:1649-1651, 1756-1764`: incoming stack-arg
  // `MEM` at `($sp + frame_size + 16)` is un-eliminated to `(plus argp 16)`,
  // frame-local `MEM` in `[starting_frame_offset, frame_size)` is un-eliminated
  // to `(plus $fp off)`, and outgoing-arg `MEM` below `starting_frame_offset`
  // retains `$sp` — exposing exact pre-reload `sched.c:true_dependence` bases.
  Sched1ProjectionContext ctx;
  ctx.stack_layout.valid = true;
  ctx.stack_layout.frame_size = 32;
  ctx.stack_layout.starting_frame_offset = 16;
  ctx.stack_layout.frame_pointer_needed = false;
  const std::vector<Insn> mem_body = {
      MakeInsn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 48))))", 0),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 4 a0))", 1),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 8))) (reg:SI 5 a1))", 2),
      MakeInsn("(set (reg:SI 9 t1) (mem:SI (reg:SI 4 a0)))", 3),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1))) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))",
               4),
      MakeInsn("(use (reg/i:SI 2 v0))", 5),
  };
  const Sched1ProjectionView mem_view =
      ProjectPostReloadBlockToSched1View(mem_body, mm, /*live_at_end_s2=*/{}, ctx);
  ASSERT_EQ(mem_view.core_insns.size(), 6u);
  const std::string s0 = FormatPatternAndNotes(mem_view.core_insns[0].pn.pattern, {});
  const std::string s1 = FormatPatternAndNotes(mem_view.core_insns[1].pn.pattern, {});
  const std::string s2 = FormatPatternAndNotes(mem_view.core_insns[2].pn.pattern, {});
  const std::string s3 = FormatPatternAndNotes(mem_view.core_insns[3].pn.pattern, {});
  // Positive: incoming arg (`$sp + 48`) -> `argp + 16`; frame local (`$sp + 20`) -> `$fp + 20`.
  EXPECT_NE(s0.find("(plus:SI (reg:SI 0 argp) (const_int 16))"), std::string::npos) << s0;
  EXPECT_NE(s1.find("(plus:SI (reg:SI 30 fp) (const_int 20))"), std::string::npos) << s1;
  // Negative: outgoing arg (`$sp + 8` < `starting_frame_offset`) stays `$sp + 8`;
  // non-stack `$a0` load is untouched.
  EXPECT_NE(s2.find("(plus:SI (reg:SI 29 sp) (const_int 8))"), std::string::npos) << s2;
  EXPECT_EQ(s3.find("argp"), std::string::npos) << s3;
  EXPECT_EQ(s3.find("fp"), std::string::npos) << s3;

  // Negative (`stack_layout.valid == false`): no `$sp` rewriting occurs.
  const Sched1ProjectionView no_layout_view =
      ProjectPostReloadBlockToSched1View(mem_body, mm, /*live_at_end_s2=*/{}, {});
  ASSERT_EQ(no_layout_view.core_insns.size(), 6u);
  const std::string raw0 = FormatPatternAndNotes(no_layout_view.core_insns[0].pn.pattern, {});
  EXPECT_NE(raw0.find("(plus:SI (reg:SI 29 sp) (const_int 48))"), std::string::npos) << raw0;
}

TEST(SchedModelTest, CollectSched1TieFlipCoreOrdersExploresMultiDepthEqualPriorityTies) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`sched.c:2428` `rank_for_schedule`): three independent equal-priority
  // stores before a return form equal-priority LUID tie-breaks; bounded multi-depth
  // BFS discovers both depth-1 and depth-2 reordered core sequences (including moving
  // index 2 ahead of both 0 and 1).
  const std::vector<Insn> indep_body = {
      MakeInsn("(set (mem:SI (reg:SI 4 a0)) (reg:SI 5 a1))", 0),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SI 6 a2))", 1),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:SI 7 a3))", 2),
  };
  const Sched1ProjectionView indep_view =
      ProjectPostReloadBlockToSched1View(indep_body, mm, /*live_at_end_s2=*/{}, {});
  const std::vector<std::vector<int>> indep_orders =
      CollectSched1TieFlipCoreOrders(indep_view, mm, /*max_orders=*/16);
  EXPECT_FALSE(indep_orders.empty());
  bool saw_depth2_move_2_first = false;
  for (const std::vector<int>& ord : indep_orders) {
    if (ord == std::vector<int>({2, 0, 1})) {
      saw_depth2_move_2_first = true;
    }
  }
  EXPECT_TRUE(saw_depth2_move_2_first);

  // Negative: a strictly dependent chain has no equal-priority ready ties, so
  // `CollectSched1TieFlipCoreOrders` returns empty.
  const std::vector<Insn> dep_body = {
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 8 t0) (const_int 2))) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
               1),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (const_int 3))) "
               "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
               2),
      MakeInsn("(use (reg/i:SI 2 v0))", 3),
  };
  const Sched1ProjectionView dep_view =
      ProjectPostReloadBlockToSched1View(dep_body, mm, /*live_at_end_s2=*/{}, {});
  EXPECT_TRUE(CollectSched1TieFlipCoreOrders(dep_view, mm, /*max_orders=*/16).empty());
}

TEST(SchedModelTest, CalleeSavedSaturationAndCallerSaveSlotExclusionForReloadContiguity) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // 1. `caller-save.c:248-340, 575-630`: a stack store/load targeting a caller-save
  //    slot offset in `stack_layout.caller_save_slot_offsets` is NOT treated as an
  //    adjacent `reload1.c:emit_reload_insns` spill contiguity pair, whereas an
  //    identical offset outside `caller_save_slot_offsets` IS a contiguity pair.
  const Insn prod = MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0);
  const Insn spill =
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 8 t0))", 1);
  PostReloadStackLayout spill_layout;
  spill_layout.valid = true;
  spill_layout.frame_size = 40;
  spill_layout.starting_frame_offset = 16;
  EXPECT_TRUE(IsAdjacentReloadContiguityPair(prod, spill, mm, spill_layout));

  PostReloadStackLayout caller_save_layout = spill_layout;
  caller_save_layout.caller_save_slot_offsets.insert(24);
  EXPECT_FALSE(IsAdjacentReloadContiguityPair(prod, spill, mm, caller_save_layout));

  // 2. `mips.h:1203` + `local-alloc.c:2140-2181` + `global.c:345-360`: when callee-saved
  //    GPRs (`$s0-$s7`) are NOT saturated across a call, a stack slot saved before
  //    and reloaded after the call is a `caller-save` slot (NOT a reload spill slot),
  //    so `ProjectPostReloadBlockToSched1View` does NOT fold it as a spilled pseudo.
  Insn call = MakeInsn(
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
      2);
  call.kind = Insn::Kind::kCall;
  const std::vector<Insn> unsaturated_call_body = {
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 7)))", 0),
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 8 t0)) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
               1),
      call,
      MakeInsn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))", 3),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (const_int 1))) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
               4),
      MakeInsn("(use (reg/i:SI 2 v0))", 5),
  };
  Sched1ProjectionContext sat_ctx;
  sat_ctx.stack_layout = spill_layout;
  // Positive (unsaturated): `$s0-$s7` are free, so `24($sp)` is NOT folded as a spill.
  const Sched1ProjectionView unsat_view =
      ProjectPostReloadBlockToSched1View(unsaturated_call_body, mm, /*live_at_end_s2=*/{}, sat_ctx);
  EXPECT_FALSE(unsat_view.input_reload_before_core.contains(3));
}

TEST(SchedModelTest, NoConflictSignExtendDiViewAndScheduleSched1CoreOrder) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`optabs.c:2485-2525`, `sched.c:4803-4818`): `REG_LIBCALL`
  // `(clobber (reg:DI 4 a0))` followed by `REG_NO_CONFLICT (reg:SI 4 a0)` subword
  // sets allocates a distinct `DImode` pseudo without clobbering `4 a0`'s active
  // pseudo, and `ScheduleSched1ViewCoreOrder` expands the unpaired `REG_RETVAL`
  // dependence chain cleanly.
  const std::vector<Insn> body = {
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(clobber (reg:DI 4 a0)) (insn_list:REG_LIBCALL (nil))", 1),
      MakeInsn("(set (subreg:SI (reg:DI 4 a0) 1) (reg:SI 4 a0)) "
               "(expr_list:REG_NO_CONFLICT (reg:SI 4 a0) (nil))",
               2),
      MakeInsn("(set (subreg:SI (reg:DI 4 a0) 0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31))) "
               "(expr_list:REG_DEAD (reg:SI 4 a0) "
               "(expr_list:REG_NO_CONFLICT (reg:SI 4 a0) (nil)))",
               3),
      MakeInsn("(set (mem:DI (reg:SI 6 a2)) (reg:DI 4 a0)) "
               "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_DEAD (reg:DI 4 a0) (nil)))",
               4),
  };
  Sched1ProjectionContext ctx;
  ctx.is_entry_block = true;
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{2}, ctx);
  ASSERT_EQ(view.core_insns.size(), 5u);
  ASSERT_EQ(view.core_insns[1].pn.pattern.op, RtlOp::kClobber);
  const int clob_p = view.core_insns[1].pn.pattern.children[0].RegNo();
  EXPECT_GE(clob_p, kMipsFirstPseudoRegister);
  // Both subword writes target the `DImode` pseudo while still reading hard `4 a0`.
  EXPECT_EQ(view.core_insns[2].pn.pattern.SetDest().children[0].RegNo(), clob_p);
  EXPECT_EQ(view.core_insns[2].pn.pattern.SetSrc().RegNo(), 4);
  EXPECT_EQ(view.core_insns[3].pn.pattern.SetDest().children[0].RegNo(), clob_p);
  EXPECT_EQ(view.core_insns[4].pn.pattern.SetSrc().RegNo(), clob_p);

  const std::vector<int> sched_order = ScheduleSched1ViewCoreOrder(view, mm);
  EXPECT_EQ(sched_order.size(), view.core_insns.size());
  EXPECT_TRUE(
      IsAchievable(view.core_insns, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1));

  // Negative contrast: without `REG_LIBCALL` / `REG_NO_CONFLICT`, a plain
  // `(clobber (reg:DI 4 a0))` does NOT use the no-conflict side table.
  const std::vector<Insn> plain_body = {
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(clobber (reg:DI 4 a0))", 1),
      MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 2)))", 2),
  };
  const Sched1ProjectionView plain_view =
      ProjectPostReloadBlockToSched1View(plain_body, mm, /*live_at_end_s2=*/{3}, ctx);
  ASSERT_EQ(plain_view.core_insns.size(), 3u);
  EXPECT_EQ(ScheduleSched1ViewCoreOrder(plain_view, mm).size(), 3u);
}

TEST(SchedModelTest, CoalescedGlobalCarrierHardRegKeepsMultiDefPseudoInSched1View) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`flow.c:2065`, `sched.c:2469-2501`, `global.c:find_reg`): in a
  // non-entry block with CFG context, a hard register (`6 a2`) that is both
  // live-in and live-out and redefined after an intermediate `REG_DEAD` maps to
  // a single multi-def pseudo (`reg_n_sets >= 2`, `!birthing_insn_p`) rather than
  // splitting into a fresh single-set pseudo at `[1]`.
  const std::vector<Insn> body = {
      MakeInsn("(set (reg:SI 2 v0) (mem:SI (reg:SI 6 a2))) "
               "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
               0),
      MakeInsn("(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 4)))", 1),
      MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
               "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
               2),
  };
  Sched1ProjectionContext cfg_ctx;
  cfg_ctx.has_cfg_context = true;
  cfg_ctx.is_entry_block = false;
  cfg_ctx.cross_block_reaching_defs[6] = 1;
  const Sched1ProjectionView coalesced =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{3, 6}, cfg_ctx);
  ASSERT_EQ(coalesced.core_insns.size(), 3u);
  const int p_in = coalesced.core_insns[0].pn.pattern.SetSrc().children[0].RegNo();
  const int p_redef = coalesced.core_insns[1].pn.pattern.SetDest().RegNo();
  EXPECT_GE(p_in, kMipsFirstPseudoRegister);
  EXPECT_EQ(p_redef, p_in);
  ASSERT_TRUE(coalesced.facts_s1.reg_n_sets.contains(p_in));
  EXPECT_GE(coalesced.facts_s1.reg_n_sets.at(p_in), 2);

  // Negative 1: when `6 a2` is NOT live-out (`live_at_end_s2` omits `6`), it is
  // NOT coalesced as a global carrier pseudo (`6 a2` remains an unrenamed ABI
  // hard register rather than a multi-def pseudo).
  const Sched1ProjectionView not_live_out =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{3}, cfg_ctx);
  ASSERT_EQ(not_live_out.core_insns.size(), 3u);
  EXPECT_EQ(not_live_out.core_insns[0].pn.pattern.SetSrc().children[0].RegNo(), 6);
  EXPECT_EQ(not_live_out.core_insns[1].pn.pattern.SetDest().RegNo(), 6);

  // Negative 2: in block 0 (`is_entry_block = true`), an incoming ABI parameter
  // `6 a2` is NEVER coalesced into a single multi-def pseudo.
  Sched1ProjectionContext entry_ctx = cfg_ctx;
  entry_ctx.is_entry_block = true;
  const Sched1ProjectionView entry_view =
      ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{3, 6}, entry_ctx);
  ASSERT_EQ(entry_view.core_insns.size(), 3u);
  EXPECT_EQ(entry_view.core_insns[0].pn.pattern.SetSrc().children[0].RegNo(), 6);
  EXPECT_GE(entry_view.core_insns[1].pn.pattern.SetDest().RegNo(), kMipsFirstPseudoRegister);
}

TEST(SchedModelTest, AttributeStage0RetractsSpeculativeSelfSetRmwRegDeadWhenSched1Requires) {
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

TEST(SchedModelTest, PreservesPostReloadOrderSafetyAllowsSymmetricStoreMemEquivInvalidation) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;

  // Positive (`local-alloc.c:540-603, 990-1000`, `sched.c:3415-3611`):
  // `sched2` moved non-dying store `[2]` (`mem[a0] = v0`) past `[1]` (which
  // carries `REG_DEAD $2` for `[0] $2 = a1 + 1` WITHOUT `REG_EQUIV`). Because
  // `validate_equiv_mem` starts at `[0]` and stops scanning at `REG_DEAD $2`
  // (`[1]`), `base_block` spuriously sees `mem_equiv_valid[2] == true`; moving
  // `[2]` back ahead of `[1]` restores `mem_equiv_valid[2] == false`, matching
  // the absent `REG_EQUIV`.
  {
    const std::vector<Insn> base_block = {
        MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1)))", 0),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 2))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 1),
        MakeInsn("(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0))", 2),
    };
    EXPECT_TRUE(PreservesPostReloadOrderSafety(
        base_block, /*cand_in_base=*/{0, 2, 1}, /*body_begin=*/0, /*body_end=*/3,
        /*live_at_end_s2=*/{3}, /*cfg_copy_pairs=*/{}, facts, mm));
  }

  // Negative 1: when `[0]` actually carries `(REG_EQUIV (mem:SI (reg:SI 4 a0)))`,
  // moving store `[2]` back ahead of `[1]` invalidates a present `REG_EQUIV`
  // (`validate_equiv_mem` would drop it in `local-alloc`) and must be rejected.
  {
    const std::vector<Insn> base_block = {
        MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                 "(expr_list:REG_EQUIV (mem:SI (reg:SI 4 a0)) (nil))",
                 0),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 2))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 1),
        MakeInsn("(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0))", 2),
    };
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        base_block, /*cand_in_base=*/{0, 2, 1}, /*body_begin=*/0, /*body_end=*/3,
        /*live_at_end_s2=*/{3}, /*cfg_copy_pairs=*/{}, facts, mm));
  }

  // Negative 2: when the store `[2]` itself carries `(REG_DEAD (reg:SI 2 v0))`
  // (rather than crossing an earlier death `d < 2`), moving `[2]` before `[1]`
  // must be rejected.
  {
    const std::vector<Insn> base_block = {
        MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1)))", 0),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 2)))", 1),
        MakeInsn("(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 2),
    };
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        base_block, /*cand_in_base=*/{0, 2, 1}, /*body_begin=*/0, /*body_end=*/3,
        /*live_at_end_s2=*/{3}, /*cfg_copy_pairs=*/{}, facts, mm));
  }
}

TEST(SchedModelTest,
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

TEST(SchedModelTest, ComputeCfgSched1ContextDisjointDagWebDoesNotInflateExitBirthingOrCoalesce) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`flow.c:2065, 2337, 2505-2520`, `sched.c:2469-2501`): in a 3-block DAG
  // `B0 -> B1 -> B2`, `B0` initializes `$a1 (5)`, `B1` kills the incoming `$a1`
  // via `REG_DEAD` on a pure reader before defining a fresh `$a1` live-out to
  // `B2`. Because `B0 -> B1` and `B1 -> B2` are disjoint DU webs, `B1` must NOT
  // put `5` in `block_entry_exit_same_web_regs[1]` or `block_exit_web_other_defs[1]`,
  // and `ProjectPostReloadBlockToSched1View` must give the fresh `$a1` definition
  // `reg_n_sets == 1` (`birthing_insn_p == true`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (mem:SI (reg:SI 4 a0)))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].successors = {2};
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 2 v0) (and:SI (reg:SI 5 a1) (const_int 7))) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 5 a1) (and:SI (reg:SI 2 v0) (const_int 15))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, o2_prof,
                           {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    ASSERT_EQ(cfg_ctx.block_entry_exit_same_web_regs.size(), 3u);
    EXPECT_FALSE(cfg_ctx.block_entry_exit_same_web_regs[1].contains(5));
    EXPECT_FALSE(cfg_ctx.block_exit_web_other_defs[1].contains({5, RtlMachineMode::kSi}));

    const ParsedSchedRegion r1 = ParseBlockForSchedModel(cfg.rpo_blocks[1], 1, Phase::kSched2);
    const Sched1ProjectionContext p1 = BuildSched1ProjectionContextForBlock(1, cfg_ctx);
    const Sched1ProjectionView v1 =
        ProjectPostReloadBlockToSched1View(r1.sched_block, mm, /*live_at_end_s2=*/{5}, p1);
    ASSERT_EQ(v1.core_insns.size(), 2u);
    const int p_in = v1.core_insns[0].pn.pattern.SetSrc().children[0].RegNo();
    const int p_out = v1.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_NE(p_in, p_out);
    ASSERT_TRUE(v1.facts_s1.reg_n_sets.contains(p_out));
    EXPECT_EQ(v1.facts_s1.reg_n_sets.at(p_out), 1);
  }

  // Negative 1: when `B1` performs a true live-in RMW on `$a1` (without an
  // earlier `REG_DEAD` killing `$a1`), `B0` and `B1` share the same web.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (mem:SI (reg:SI 4 a0)))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].successors = {2};
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 5 a1) (ior:SI (reg:SI 5 a1) (const_int 1)))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, o2_prof,
                           {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    ASSERT_EQ(cfg_ctx.block_entry_exit_same_web_regs.size(), 3u);
    EXPECT_TRUE(cfg_ctx.block_entry_exit_same_web_regs[1].contains(5));
    ASSERT_TRUE(cfg_ctx.block_exit_web_other_defs[1].contains({5, RtlMachineMode::kSi}));
    EXPECT_EQ(cfg_ctx.block_exit_web_other_defs[1].at({5, RtlMachineMode::kSi}), 1);
  }

  // Negative 2: in a loop `B0 -> B1 -> {B1, B2}` where `B1` reads `$a1` with
  // `REG_DEAD` and redefines `$a1` along the back-edge, both `B0` and `B1` reach
  // `B1`'s upward-exposed use, so `B1` keeps `5` in `block_entry_exit_same_web_regs[1]`
  // and coalesces `$a1` into a multi-def carrier (`reg_n_sets >= 2`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 5 a1) (mem:SI (reg:SI 4 a0)))");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].successors = {1, 2};
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 2 v0) (and:SI (reg:SI 5 a1) (const_int 7))) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (reg:SI 5 a1) (plus:SI (reg:SI 2 v0) (const_int 1))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].AddPatternOnly(
        "(set (reg/i:SI 2 v0) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, o2_prof,
                           {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    ASSERT_EQ(cfg_ctx.block_entry_exit_same_web_regs.size(), 3u);
    EXPECT_TRUE(cfg_ctx.block_entry_exit_same_web_regs[1].contains(5));

    const ParsedSchedRegion r1 = ParseBlockForSchedModel(cfg.rpo_blocks[1], 1, Phase::kSched2);
    const Sched1ProjectionContext p1 = BuildSched1ProjectionContextForBlock(1, cfg_ctx);
    const Sched1ProjectionView v1 =
        ProjectPostReloadBlockToSched1View(r1.sched_block, mm, /*live_at_end_s2=*/{5}, p1);
    ASSERT_GE(v1.core_insns.size(), 2u);
    const int p_in = v1.core_insns[0].pn.pattern.SetSrc().children[0].RegNo();
    const int p_out = v1.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_EQ(p_in, p_out);
    ASSERT_TRUE(v1.facts_s1.reg_n_sets.contains(p_out));
    EXPECT_GE(v1.facts_s1.reg_n_sets.at(p_out), 2);
  }
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewHandlesOutputReloadCopyAndDeadTrailingRmw) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // (a) `local-alloc.c:892-925, 1004-1007` (`optimize_reg_copy_1`): when
  // `birth_insn` is a same-mode copy `(set v0 t2)` omitting `REG_DEAD t2`
  // (like a `reload1.c` output-reload move), splitting a subsequent RMW on `v0`
  // is allowed iff `t2` is actually dead after `birth_insn`.
  auto run_copy_case = [&](bool t2_used_later) {
    RtlBasicBlock blk;
    blk.id = 0;
    blk.AddPatternOnly("(set (reg:SI 2 v0) (reg:SI 10 t2))");
    blk.AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))");
    if (t2_used_later) {
      blk.AddPatternOnly(
          "(set (mem:SI (reg:SI 5 a1)) (reg:SI 10 t2)) "
          "(expr_list:REG_DEAD (reg:SI 10 t2) (nil))");
    }
    blk.AddPatternOnly(
        "(set (mem:SI (reg:SI 6 a2)) (reg:SI 2 v0)) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    const ParsedSchedRegion r = ParseBlockForSchedModel(blk, 0, Phase::kSched2);
    Sched1ProjectionContext ctx;
    ctx.allow_general_combine_regs_split = true;
    return ProjectPostReloadBlockToSched1View(r.sched_block, mm, /*live_at_end_s2=*/{}, ctx);
  };

  // Positive: `t2` is dead after the copy, so the RMW on `v0` splits into a fresh pseudo.
  {
    const Sched1ProjectionView v = run_copy_case(/*t2_used_later=*/false);
    ASSERT_GE(v.core_insns.size(), 3u);
    const int p_birth = v.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p_rmw = v.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_NE(p_birth, p_rmw);
  }
  // Negative: `t2` has a later read, so `v0` must NOT split across the RMW.
  {
    const Sched1ProjectionView v = run_copy_case(/*t2_used_later=*/true);
    ASSERT_GE(v.core_insns.size(), 4u);
    const int p_birth = v.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p_rmw = v.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_EQ(p_birth, p_rmw);
  }

  // (b) `flow.c:2505-2520`, `local-alloc.c:1266-1300`: a trailing return-value
  // RMW carrying `(REG_DEAD v0)` terminates the local live range of `v0` even
  // though `v0` is live at exit, allowing an earlier RMW on `v0` to split.
  auto run_trailing_rmw_case = [&](bool trailing_has_dead_v0) {
    RtlBasicBlock blk;
    blk.id = 0;
    blk.AddPatternOnly(
        "(set (reg:SI 2 v0) (ashift:SI (reg:SI 4 a0) (const_int 2))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    blk.AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))");
    blk.AddPatternOnly(trailing_has_dead_v0
                           ? "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                             "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))"
                           : "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1)))");
    const ParsedSchedRegion r = ParseBlockForSchedModel(blk, 0, Phase::kSched2);
    Sched1ProjectionContext ctx;
    ctx.allow_general_combine_regs_split = true;
    return ProjectPostReloadBlockToSched1View(r.sched_block, mm, /*live_at_end_s2=*/{2}, ctx);
  };

  // Positive: trailing RMW kills local `v0` via `REG_DEAD`, so `[1]` splits.
  {
    const Sched1ProjectionView v = run_trailing_rmw_case(/*trailing_has_dead_v0=*/true);
    ASSERT_EQ(v.core_insns.size(), 3u);
    const int p_birth = v.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p_rmw = v.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_NE(p_birth, p_rmw);
  }
  // Negative: without `REG_DEAD v0` on the trailing live-out RMW, `[1]` must NOT split.
  {
    const Sched1ProjectionView v = run_trailing_rmw_case(/*trailing_has_dead_v0=*/false);
    ASSERT_EQ(v.core_insns.size(), 3u);
    const int p_birth = v.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p_rmw = v.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_EQ(p_birth, p_rmw);
  }
}

TEST(SchedModelTest, CollectSched1TieFlipCoreOrdersEmitsCanonicalSeedsAndEmptyOnChain) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`expr.c:4027`, `sched.c:2428`): two symmetric FP load->add pairs
  // where `sched2` sank `p82 = p80 + p80` after `p83 = p81 + p81`. Default-LUID
  // `Schedule(..., kSched1)` preserves the stale post-`sched2` LUID tie-break,
  // while `CollectSched1TieFlipCoreOrders` returns a distinct canonical
  // `Im(F_sched1)` permutation escaping the tie-break trap.
  {
    Sched1ProjectionView view;
    view.core_insns = {
        MakeInsn("(set (reg:DF 80) (mem/u:DF (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:DF 81) (mem/u:DF (plus:SI (reg:SI 4 a0) (const_int 8)))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 1),
        MakeInsn("(set (reg:DF 83) (plus:DF (reg:DF 81) (reg:DF 81))) "
                 "(expr_list:REG_DEAD (reg:DF 81) (nil))",
                 2),
        MakeInsn("(set (reg:DF 82) (plus:DF (reg:DF 80) (reg:DF 80))) "
                 "(expr_list:REG_DEAD (reg:DF 80) (nil))",
                 3),
        MakeInsn("(set (reg:DF 84) (plus:DF (reg:DF 82) (reg:DF 83))) "
                 "(expr_list:REG_DEAD (reg:DF 82) (expr_list:REG_DEAD (reg:DF 83) (nil)))",
                 4),
    };
    view.facts_s1.max_reg_num = 85;
    for (int r = 80; r <= 84; ++r) {
      view.facts_s1.reg_n_sets[r] = 1;
    }
    view.live_at_end_s1 = {84};

    const DepGraph g_s1 = Analyze(view.core_insns, view.facts_s1, mm, Phase::kSched1);
    const ScheduleResult base_res =
        Schedule(view.core_insns, g_s1, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
    const std::vector<std::vector<int>> alt_orders =
        CollectSched1TieFlipCoreOrders(view, mm, /*max_orders=*/16);
    ASSERT_FALSE(alt_orders.empty());
    bool found_unsunk = false;
    for (const std::vector<int>& ord : alt_orders) {
      ASSERT_EQ(ord.size(), view.core_insns.size());
      EXPECT_NE(ord, base_res.order);
      auto pos_of = [&](int idx) {
        return static_cast<int>(std::find(ord.begin(), ord.end(), idx) - ord.begin());
      };
      if (pos_of(3) < pos_of(2)) {
        found_unsunk = true;
      }
    }
    EXPECT_TRUE(found_unsunk);
  }

  // Negative: a fully serialized 3-insn chain admits only a single topological
  // order, so `CollectSched1TieFlipCoreOrders` returns empty.
  {
    Sched1ProjectionView view;
    view.core_insns = {
        MakeInsn("(set (reg:SI 80) (mem:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 81) (plus:SI (reg:SI 80) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 80) (nil))",
                 1),
        MakeInsn("(set (reg:SI 82) (plus:SI (reg:SI 81) (const_int 2))) "
                 "(expr_list:REG_DEAD (reg:SI 81) (nil))",
                 2),
    };
    view.facts_s1.max_reg_num = 83;
    view.facts_s1.reg_n_sets[80] = 1;
    view.facts_s1.reg_n_sets[81] = 1;
    view.facts_s1.reg_n_sets[82] = 1;
    view.live_at_end_s1 = {82};
    EXPECT_TRUE(CollectSched1TieFlipCoreOrders(view, mm, /*max_orders=*/16).empty());
  }
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesStripsValidateEquivMemOnDeadTransfer) {
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

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesRetractsSelfSetDeadViaSched2Preimage) {
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

TEST(SchedModelTest, BuildInvertedRegDeadSeedOrderInBaseHoistsTransitiveSched2Ancestors) {
  // `sched.c:2872-2906, 3415-3611`: when `sched2` sinks a pure reader `j` of
  // `a0` past insn `i` carrying `(REG_DEAD a0)` AND an intermediate insn `a`
  // between `i` and `j` computes an operand feeding `j`,
  // `BuildInvertedRegDeadSeedOrderInBase` must hoist both `a` and `j` ahead of
  // `i` in topological order. Conversely, if `j` depends on `i` itself (or no
  // later reader of `a0` exists), it must return `std::nullopt`.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts_s2;
  facts_s2.max_reg_num = kMipsFirstPseudoRegister;

  // Positive: insn `0` carries `(REG_DEAD a0)`, insn `1` computes `t0` from
  // `a1`, and insn `2` reads both `a0` and `t0`. Both `1` and `2` must move
  // ahead of `0` with `1` before `2`.
  {
    const std::vector<Insn> block = {
        MakeInsn("(set (reg:SI 9 t1) (mem:SI (reg:SI 4 a0))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 0),
        MakeInsn("(set (reg:SI 8 t0) (ashift:SI (reg:SI 5 a1) (const_int 2))) "
                 "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                 1),
        MakeInsn("(set (reg:SI 10 t2) (plus:SI (reg:SI 4 a0) (reg:SI 8 t0))) "
                 "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (reg:SI 10 t2))) "
                 "(expr_list:REG_DEAD (reg:SI 9 t1) (expr_list:REG_DEAD (reg:SI 10 t2) (nil)))",
                 3),
    };
    const std::optional<std::vector<int>> seed = BuildInvertedRegDeadSeedOrderInBase(
        block, /*prologue_end=*/0, /*epilogue_begin=*/4, facts_s2, mm);
    ASSERT_TRUE(seed.has_value());
    auto pos_of = [&](int id) {
      return static_cast<int>(std::find(seed->begin(), seed->end(), id) - seed->begin());
    };
    EXPECT_LT(pos_of(1), pos_of(2));
    EXPECT_LT(pos_of(2), pos_of(0));
  }

  // Negative: `2` depends directly on `0` (`t1`), so hoisting `2` ahead of `0`
  // would violate `base_graph`; `BuildInvertedRegDeadSeedOrderInBase` returns
  // `std::nullopt`.
  {
    const std::vector<Insn> block = {
        MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 0),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 9 t1))) "
                 "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
                 1),
    };
    EXPECT_FALSE(BuildInvertedRegDeadSeedOrderInBase(block, /*prologue_end=*/0,
                                                     /*epilogue_begin=*/2, facts_s2, mm)
                     .has_value());
  }
}

TEST(SchedModelTest, ExtractCoreSched2EdgesAndConflictDrivenTieFlipsResolveSched2Violations) {
  // `sched.c:2428` (`rank_for_schedule` LUID tie-break) + `toplev.c:3028-3034`:
  // `ExtractCoreSched2Edges` projects hard-register anti/output dependencies
  // from post-reload `kSched2` back onto core indices, and passing them to
  // `CollectSched1TieFlipCoreOrders` steers conflict-driven LUID tie flips
  // toward a `kSched1`-achievable order that respects every `core_s2_edges` edge.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Two independent post-reload loads that share hard reg `$t0` via an
  // anti-dependence (`0`: reads `$t0`, `1`: overwrites `$t0`), projected into
  // independent pseudos in `kSched1` view.
  const std::vector<Insn> post_block = {
      MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 8 t0) (const_int 1))) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
               0),
      MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 2))) "
               "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
               1),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (reg:SI 8 t0))) "
               "(expr_list:REG_DEAD (reg:SI 9 t1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
               2),
  };
  RegFacts facts_s2;
  facts_s2.max_reg_num = kMipsFirstPseudoRegister;
  Sched1ProjectionContext ctx;
  ctx.is_entry_block = true;
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(post_block, mm, /*live_at_end_s2=*/{}, ctx);
  ASSERT_EQ(view.core_insns.size(), 3u);

  const std::vector<std::pair<int, int>> core_s2_edges = ExtractCoreSched2Edges(
      post_block, /*prologue_end=*/0, /*epilogue_begin=*/3, view, facts_s2, mm);
  // Anti-dependence `0 -> 1` (reading `t0` before overwriting `t0`) must be
  // captured in `core_s2_edges`.
  EXPECT_NE(std::find(core_s2_edges.begin(), core_s2_edges.end(), std::make_pair(0, 1)),
            core_s2_edges.end());

  // Positive: on a 5-insn symmetric FP view with a `core_s2_edges` constraint
  // `(3, 2)` that `base_res` violates (`2` before `3`), passing `core_s2_edges`
  // produces an order placing `3` before `2`.
  Sched1ProjectionView fp_view;
  fp_view.core_insns = {
      MakeInsn("(set (reg:DF 80) (mem/u:DF (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:DF 81) (mem/u:DF (plus:SI (reg:SI 4 a0) (const_int 8)))) "
               "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
               1),
      MakeInsn("(set (reg:DF 83) (plus:DF (reg:DF 81) (reg:DF 81))) "
               "(expr_list:REG_DEAD (reg:DF 81) (nil))",
               2),
      MakeInsn("(set (reg:DF 82) (plus:DF (reg:DF 80) (reg:DF 80))) "
               "(expr_list:REG_DEAD (reg:DF 80) (nil))",
               3),
      MakeInsn("(set (reg:DF 84) (plus:DF (reg:DF 82) (reg:DF 83))) "
               "(expr_list:REG_DEAD (reg:DF 82) (expr_list:REG_DEAD (reg:DF 83) (nil)))",
               4),
  };
  fp_view.facts_s1.max_reg_num = 85;
  for (int r = 80; r <= 84; ++r) {
    fp_view.facts_s1.reg_n_sets[r] = 1;
  }
  fp_view.live_at_end_s1 = {84};
  const std::vector<std::pair<int, int>> required_s2 = {{3, 2}};
  const std::vector<std::vector<int>> orders =
      CollectSched1TieFlipCoreOrders(fp_view, mm, /*max_orders=*/16, required_s2);
  ASSERT_FALSE(orders.empty());
  const std::vector<int>& best = orders.front();
  auto pos_of = [&](int idx) {
    return static_cast<int>(std::find(best.begin(), best.end(), idx) - best.begin());
  };
  EXPECT_LT(pos_of(3), pos_of(2));

  // Negative: empty region bounds yield empty `ExtractCoreSched2Edges`.
  EXPECT_TRUE(ExtractCoreSched2Edges(post_block, /*prologue_end=*/0, /*epilogue_begin=*/0, view,
                                     facts_s2, mm)
                  .empty());
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesResolvesIntegratedReturnDelaySlotZero) {
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

TEST(SchedModelTest,
     ProjectPostReloadBlockToSched1ViewKeepsLocalDyingHardRegSuggestionConflictsUnrenamed) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive (`local-alloc.c:1266-1297, 1765-1820, 2145-2182`): in the entry
  // block, a non-leading insn consuming dying physical arg `$a0` into `$v0`
  // (where `$a0` stays free until `$v0`'s death) keeps `$v0` unrenamed and
  // binds subsequent reads of `$v0` to physical `$v0`, chaining to `$v1`.
  const std::vector<Insn> body = {
      MakeInsn("(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
               "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
               1),
      MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 2))) "
               "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
               2),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 3 v1))) "
               "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))",
               3),
      MakeInsn("(use (reg/i:SI 2 v0))", 4),
  };
  {
    Sched1ProjectionContext entry_ctx;
    entry_ctx.is_entry_block = true;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(body, mm, {}, entry_ctx);
    ASSERT_EQ(view.core_insns.size(), 5u);
    EXPECT_EQ(view.core_insns[1].pn.pattern.SetDest().RegNo(), 2);
    EXPECT_EQ(view.core_insns[2].pn.pattern.SetDest().RegNo(), 3);
    EXPECT_EQ(view.core_insns[2].pn.pattern.SetSrc().children[0].RegNo(), 2);
  }

  // Negative 1: in a non-entry block (`is_entry_block == false`), live-in `$a0`
  // is a cross-block global pseudo rather than a physical hard reg, so `$v0`
  // is lifted to a fresh single-set pseudo instead of staying hard reg `2`.
  {
    Sched1ProjectionContext non_entry_ctx;
    non_entry_ctx.is_entry_block = false;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, {}, non_entry_ctx);
    ASSERT_EQ(view.core_insns.size(), 5u);
    EXPECT_GE(view.core_insns[1].pn.pattern.SetDest().RegNo(), kMipsFirstPseudoRegister);
  }

  // Negative 2 (`local-alloc.c:2148`, `config/mips/mips.h:1280`): cross-bank
  // `mtc1` `(set (reg:SF 34 $f2) (reg:SF 5 a1)) (REG_DEAD a1)` has disjoint
  // `FP_REGS` vs `GR_REGS` classes, so `$f2` must be lifted to a pseudo.
  {
    const std::vector<Insn> cross_bank = {
        MakeInsn("(set (reg:SF 54 $f22) (mem/u:SF (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SF 34 $f2) (reg:SF 5 a1)) (expr_list:REG_DEAD (reg:SF 5 a1) (nil))", 1),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 54 $f22) (reg:SF 34 $f2))) "
                 "(expr_list:REG_DEAD (reg:SF 54 $f22) (expr_list:REG_DEAD (reg:SF 34 $f2) (nil)))",
                 2),
        MakeInsn("(use (reg/i:SF 32 $f0))", 3),
    };
    Sched1ProjectionContext entry_ctx;
    entry_ctx.is_entry_block = true;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(cross_bank, mm, {}, entry_ctx);
    ASSERT_EQ(view.core_insns.size(), 4u);
    EXPECT_GE(view.core_insns[1].pn.pattern.SetDest().RegNo(), kMipsFirstPseudoRegister);
  }
}

TEST(SchedModelTest, ComputeCfgSched1ContextCountsCallFusageDefsPerReachingWeb) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive: `B0` sets `$a0` and branches to `B1` (redefines `$a0` before a
  // call reading `$a0`) vs `B2` (returns without calling). Because `B0`'s def
  // does NOT reach `B1`'s `CALL_INSN_FUNCTION_USAGE`, `B1`'s `$a0` must NOT be
  // inflated by `B0`'s def (`block_unrenamed_hard_defs[0]` excludes `$a0`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:SI 4 a0) (const_int 1))",
                    "(set (pc) (if_then_else (eq (reg:SI 5 a1) (const_int 0)) (label_ref L2) "
                    "(pc)))",
                },
            .successors = {1, 2},
        },
        RtlBasicBlock{
            .id = 1,
            .instructions =
                {
                    "(set (reg:SI 4 a0) (const_int 2))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Sink\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
                    "(use (reg/i:SI 2 v0))",
                },
            .successors = {},
        },
        RtlBasicBlock{
            .id = 2,
            .instructions =
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3)))",
                    "(use (reg/i:SI 2 v0))",
                },
            .successors = {},
        },
    };
    for (auto& blk : cfg.rpo_blocks) {
      blk.SyncRtlInsnsFromInstructions();
    }
    cfg.rpo_blocks[1].rtl_insns[1].has_explicit_call_fusage = true;
    cfg.rpo_blocks[1].rtl_insns[1].call_fusage = {
        ParsePatternAndNotes("(use (reg:SI 4 a0))")->pattern,
    };
    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, prof, {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    const auto b0_it = ctx.block_unrenamed_hard_defs[0].find(4);
    EXPECT_TRUE(b0_it == ctx.block_unrenamed_hard_defs[0].end() || b0_it->second == 0);
    const auto b1_it = ctx.block_unrenamed_hard_defs[1].find(4);
    ASSERT_NE(b1_it, ctx.block_unrenamed_hard_defs[1].end());
    EXPECT_EQ(b1_it->second, 1);
  }

  // Negative contrast: when BOTH `B0` (`$a3 = 0`) and `B1` (`$a3 = $v0`) reach
  // `B2`'s `CALL_INSN_FUNCTION_USAGE (use $a3)` in the same web, both blocks
  // must see `other_cnt >= 1` (`block_exit_web_other_defs[0][7] >= 1`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:SI 7 a3) (const_int 0))",
                    "(set (pc) (if_then_else (eq (reg:SI 5 a1) (const_int 0)) (label_ref L2) "
                    "(pc)))",
                },
            .successors = {1, 2},
        },
        RtlBasicBlock{
            .id = 1,
            .instructions =
                {
                    "(set (reg:SI 7 a3) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                },
            .successors = {2},
        },
        RtlBasicBlock{
            .id = 2,
            .instructions =
                {
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Sink\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(use (reg/i:SI 2 v0))",
                },
            .successors = {},
        },
    };
    for (auto& blk : cfg.rpo_blocks) {
      blk.SyncRtlInsnsFromInstructions();
    }
    cfg.rpo_blocks[2].rtl_insns[0].has_explicit_call_fusage = true;
    cfg.rpo_blocks[2].rtl_insns[0].call_fusage = {
        ParsePatternAndNotes("(use (reg:SI 7 a3))")->pattern,
    };
    RtlControlFlowGraph live_cfg = cfg;
    ReconstructRtlLiveness(&live_cfg, prof, {.reload_completed = true, .update_reg_notes = false});
    const CfgSched1Context ctx = ComputeCfgSched1Context(cfg, live_cfg, mm);
    const auto b0_it = ctx.block_exit_web_other_defs[0].find({7, RtlMachineMode::kSi});
    ASSERT_NE(b0_it, ctx.block_exit_web_other_defs[0].end());
    EXPECT_GE(b0_it->second, 1);
  }
}

TEST(SchedModelTest,
     ProjectPostReloadBlockToSched1ViewFoldsNonAdjacentFprOutputAndInputReloadsWithInheritance) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive (`reload1.c:5186-5249, 5641`): folds both pre-`mv` and post-`mv`
  // reload-inheriting FP readers onto `s1`, AND folds non-adjacent `mtc1`
  // input reloads across an interleaved independent FP op into `mult.s`.
  for (bool mv_before_reader : {false, true}) {
    std::vector<Insn> body;
    body.push_back(
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30))) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 0));
    if (mv_before_reader) {
      body.push_back(MakeInsn("(set (reg:SF 17 s1) (reg:SF 32 $f0))", 1));
      body.push_back(
          MakeInsn("(set (reg:SF 44 $f12) (plus:SF (reg:SF 54 $f22) (reg:SF 32 $f0)))", 2));
    } else {
      body.push_back(
          MakeInsn("(set (reg:SF 44 $f12) (plus:SF (reg:SF 54 $f22) (reg:SF 32 $f0)))", 1));
      body.push_back(MakeInsn("(set (reg:SF 17 s1) (reg:SF 32 $f0))", 2));
    }
    body.push_back(MakeInsn("(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1071644672))", 3));
    body.push_back(MakeInsn("(set (reg:SF 42 $f10) (reg:SF 18 s2))", 4));
    body.push_back(MakeInsn("(set (reg:SF 40 $f8) (reg:SF 17 s1))", 5));
    body.push_back(
        MakeInsn("(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 32 $f0))) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 6));
    body.push_back(
        MakeInsn("(set (reg/i:SF 32 $f0) (mult:SF (reg:SF 42 $f10) (reg:SF 40 $f8))) "
                 "(expr_list:REG_DEAD (reg:SF 18 s2) "
                 "(expr_list:REG_DEAD (reg:SF 17 s1) (nil)))",
                 7));
    body.push_back(MakeInsn("(use (reg/i:SF 32 $f0))", 8));

    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(body, mm, {}, {});
    // `mfc1 s1,$f0` + `mtc1 $f10,s2` + `mtc1 $f8,s1` are all folded out: 9 - 3 = 6 core insns.
    EXPECT_EQ(view.core_insns.size(), 6u);
    EXPECT_EQ(view.output_reload_after_core.size(), 1u);
    EXPECT_EQ(view.input_reload_before_core.size(), 2u);
  }

  // Negative: when `mfc1 s1, $f0` carries explicit `(REG_DEAD $f0)` and an
  // intervening insn overwrites `s2` before `mult.s`, neither reload is folded.
  {
    const std::vector<Insn> neg_body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30)))", 0),
        MakeInsn("(set (reg:SF 17 s1) (reg:SF 32 $f0)) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 1),
        MakeInsn("(set (reg:SF 42 $f10) (reg:SF 18 s2))", 2),
        MakeInsn("(set (reg:SF 18 s2) (const_double:SF (cc0) 0 1071644672))", 3),
        MakeInsn("(set (reg/i:SF 32 $f0) (mult:SF (reg:SF 42 $f10) (reg:SF 54 $f22)))", 4),
        MakeInsn("(use (reg/i:SF 32 $f0))", 5),
    };
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(neg_body, mm, {}, {});
    EXPECT_EQ(view.core_insns.size(), 6u);
    EXPECT_TRUE(view.output_reload_after_core.empty());
    EXPECT_TRUE(view.input_reload_before_core.empty());
  }
}

TEST(SchedModelTest,
     ProjectPostReloadBlockToSched1ViewModelsLocalQtyPriorityAndFoldedSpillLiveAfter) {
  CompilationProfile prof;
  prof.optimize_level = 2;
  prof.c_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // 1. `local-alloc.c:1512-1539, 1597-1623` (`qty_compare_1` / `find_free_reg`):
  //    a short local def of `$v1` (`3`, span 1, higher priority) overlapping a
  //    later longer local def of `$v0` (`2`, span 3, lower priority) is
  //    allocated first while `$v0` is free, so `$v1` stays unrenamed (`3`).
  //    Conversely, when `$v1` has longer span (span 3, lower priority) and the
  //    later `$v0` def has shorter span (span 1, higher priority), `$v0` is
  //    allocated first and occupies `$v0`, so `$v1` lifts to a fresh pseudo.
  {
    const std::vector<Insn> short_v1_long_v0 = {
        MakeInsn("(set (reg:SI 3 v1) (mem/u:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 2 v0) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 4))))", 1),
        MakeInsn("(set (mem:SI (reg:SI 5 a1)) (reg:SI 3 v1)) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                 2),
        MakeInsn("(set (reg:SI 8 t0) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 8)))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 3),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 5 a1) (const_int 4))) (reg:SI 2 v0)) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                 4),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))", 5),
        MakeInsn("(use (reg/i:SI 2 v0))", 6),
    };
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(short_v1_long_v0, mm, {}, ctx);
    ASSERT_EQ(view.core_insns.size(), 7u);
    EXPECT_EQ(view.core_insns[0].pn.pattern.SetDest().RegNo(), 3);
  }
  {
    const std::vector<Insn> long_v1_short_v0 = {
        MakeInsn("(set (reg:SI 3 v1) (mem/u:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 2 v0) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 4))))", 1),
        MakeInsn("(set (mem:SI (reg:SI 5 a1)) (reg:SI 2 v0)) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 2),
        MakeInsn("(set (reg:SI 8 t0) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 8)))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 3),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 5 a1) (const_int 4))) (reg:SI 3 v1)) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                 4),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))", 5),
        MakeInsn("(use (reg/i:SI 2 v0))", 6),
    };
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(long_v1_short_v0, mm, {}, ctx);
    ASSERT_EQ(view.core_insns.size(), 7u);
    EXPECT_GE(view.core_insns[0].pn.pattern.SetDest().RegNo(), kMipsFirstPseudoRegister);
  }

  // 2. `reload1.c:2309-2400, 5641-6160` (`ComputeCoreHardRegLiveAfterMasks`):
  //    when `$v0` (`2`) is only read at consumer `5` via a folded input-reload
  //    spill load `lw v0, 24(sp)` paired with a pre-call spill store `sw t0, 24(sp)`,
  //    `$v0` is NOT live pre-reload across `$v1` (`3`), so `$v1` stays unrenamed.
  //    Contrast: when `$v0` is a genuine live value (no spill load) read across
  //    `$v1`'s live range, `$v0` blocks `$v1` and `$v1` lifts to a pseudo.
  {
    const std::vector<Insn> folded_spill_v0 = {
        MakeInsn("(set (reg:SI 8 t0) (mem/u:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 8 t0)) "
                 "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                 1),
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                 2, Insn::Kind::kCall),
        MakeInsn("(set (reg:SI 3 v1) (mem/u:SI (plus:SI (reg:SI 16 s0) (const_int 4))))", 3),
        MakeInsn("(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))", 4),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))",
                 5),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))", 6),
        MakeInsn("(use (reg/i:SI 2 v0))", 7),
    };
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(folded_spill_v0, mm, {}, ctx);
    ASSERT_EQ(view.core_insns.size(), 6u);
    EXPECT_EQ(view.core_insns[2].pn.pattern.SetDest().RegNo(), 3);
  }
  {
    const std::vector<Insn> genuine_live_v0 = {
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                 0, Insn::Kind::kCall),
        MakeInsn("(set (reg:SI 3 v1) (mem/u:SI (plus:SI (reg:SI 16 s0) (const_int 4))))", 1),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))",
                 2),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))", 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(genuine_live_v0, mm, {}, ctx);
    ASSERT_EQ(view.core_insns.size(), 5u);
    EXPECT_GE(view.core_insns[1].pn.pattern.SetDest().RegNo(), kMipsFirstPseudoRegister);
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

TEST(SchedModelTest, InvertBlockOrderAndSafetyRelaxPendingListFlushCapArtifacts) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = kMipsFirstPseudoRegister;

  // Build a `> 32`-MEM-insn block (`sched.c:1740`, `sched.c:1748` `flush_pending_lists`):
  // 31 independent `mem/u` reads into rotating scratch regs, followed in the
  // pre-`sched2` order `Q` by a stack store `S` (32nd MEM insn, so
  // `pending_lists_length == 31 <= 32`, NO cap flush at `S`), then 2 more
  // `mem/u` loads (`L_hoist1`, `L_hoist2`) whose consumers follow.
  // In the post-`sched2` order `S_out`, `L_hoist1`/`L_hoist2` appear ahead of
  // `S`, pushing `pending_lists_length > 32` at `S` and causing `Analyze(S_out)`
  // to emit hallucinated `sched.c:1740` anti-deps `(L_hoist* -> S)`.
  auto build_block = [](bool aliasing_load) {
    std::vector<Insn> blk;
    int luid = 0;
    // 32 `mem/u` loads from `(plus a0 offset)` into `$t0`..`$t7` (disjoint offsets).
    for (int i = 0; i < 32; ++i) {
      const int reg = 8 + (i % 8);  // t0..t7
      blk.push_back(
          MakeInsn(absl::StrCat("(set (reg:SI ", reg,
                                ") (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int ", i * 4, "))))"),
                   luid++));
    }
    // Load `L_target` (`idx = 32`): either `mem/u` from `a0+128` (non-aliasing with
    // stack store `S`) or non-`mem/u` from the SAME stack slot `24(sp)` (real
    // `AntiDependence` with `S`).
    if (aliasing_load) {
      blk.push_back(MakeInsn(
          "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))", luid++));
    } else {
      blk.push_back(MakeInsn(
          "(set (reg:SI 16 s0) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 128))))", luid++));
    }
    // Store `S` (`idx = 33`): writes `24(sp)`. At index 33 in `S_out`,
    // `pending_lists_length == 33 > 32` triggers `sched.c:1740` `FlushPendingLists`.
    blk.push_back(
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 5 a1))", luid++));
    blk.push_back(MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 8 t0)))", luid++));
    blk.push_back(MakeInsn("(use (reg/i:SI 2 v0))", luid++));
    return blk;
  };

  // Positive: with `L_target` as `mem/u`, `Analyze(post_blk)` emits the
  // position-dependent `sched.c:1740` cap-flush edge `(32 -> 33)`, whereas
  // `ExtractCoreSched2Edges` and `PreservesPostReloadOrderSafety` exclude that
  // artifact, and `InvertBlockOrder` recovers a preimage placing `S` (33)
  // before `L_target` (32) whenever `S` + `L_target` replay to `post_blk`.
  const std::vector<Insn> post_blk = build_block(/*aliasing_load=*/false);
  const DepGraph g_cap = Analyze(post_blk, facts, mm, Phase::kSched2);
  bool has_cap_flush_edge = false;
  for (const Link& l : g_cap.log_links[33]) {
    if (l.target == 32) {
      has_cap_flush_edge = true;
    }
  }
  ASSERT_TRUE(has_cap_flush_edge);

  Sched1ProjectionContext proj_ctx;
  const Sched1ProjectionView view =
      ProjectPostReloadBlockToSched1View(post_blk, mm, /*live_at_end_s2=*/{}, proj_ctx);
  const std::vector<std::pair<int, int>> s2_edges = ExtractCoreSched2Edges(
      post_blk, /*prologue_end=*/0, static_cast<int>(post_blk.size()), view, facts, mm);
  int core_load = -1;
  int core_store = -1;
  for (size_t ci = 0; ci < view.core_to_orig_index.size(); ++ci) {
    if (view.core_to_orig_index[ci] == 32) {
      core_load = static_cast<int>(ci);
    } else if (view.core_to_orig_index[ci] == 33) {
      core_store = static_cast<int>(ci);
    }
  }
  ASSERT_GE(core_load, 0);
  ASSERT_GE(core_store, 0);
  EXPECT_EQ(std::find(s2_edges.begin(), s2_edges.end(), std::make_pair(core_load, core_store)),
            s2_edges.end());

  // Swap `(32, 33)` in `cand_in_base`: `PreservesPostReloadOrderSafety` must
  // accept swapping a `mem/u` load past a store despite the `sched.c:1740` cap.
  std::vector<int> swapped(post_blk.size());
  for (size_t i = 0; i < post_blk.size(); ++i) {
    swapped[i] = static_cast<int>(i);
  }
  std::swap(swapped[32], swapped[33]);
  EXPECT_TRUE(PreservesPostReloadOrderSafety(
      post_blk, swapped, /*prologue_end=*/0, static_cast<int>(post_blk.size()),
      /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/false));

  // Verify `InvertBlockOrder` relaxes the `sched.c:1740` artifact edge when
  // `Q` (with store before `mem/u` load) genuinely schedules to `target`.
  std::vector<Insn> pre_q;
  pre_q.reserve(post_blk.size());
  for (size_t i = 0; i < swapped.size(); ++i) {
    Insn insn = post_blk[static_cast<size_t>(swapped[i])];
    insn.luid = static_cast<int>(i);
    pre_q.push_back(std::move(insn));
  }
  const DepGraph g_pre = Analyze(pre_q, facts, mm, Phase::kSched2);
  const ScheduleResult sr_pre = Schedule(pre_q, g_pre, facts, mm, Phase::kSched2, {});
  ASSERT_EQ(sr_pre.order.size(), pre_q.size());
  std::vector<Insn> target_from_q;
  std::vector<int> q_in_target(pre_q.size(), -1);
  for (size_t pos = 0; pos < sr_pre.order.size(); ++pos) {
    Insn insn = pre_q[static_cast<size_t>(sr_pre.order[pos])];
    insn.luid = static_cast<int>(pos);
    target_from_q.push_back(std::move(insn));
    q_in_target[static_cast<size_t>(sr_pre.order[pos])] = static_cast<int>(pos);
  }
  const std::optional<std::vector<int>> inv =
      InvertBlockOrder(target_from_q, facts, mm, Phase::kSched2, {}, q_in_target);
  ASSERT_TRUE(inv.has_value());
  EXPECT_EQ(*inv, q_in_target);

  // Negative: when `L_target` reads the SAME stack slot `24(sp)` written by `S`
  // (real `AntiDependence`), neither `ExtractCoreSched2Edges` nor
  // `PreservesPostReloadOrderSafety` may drop `(32 -> 33)`.
  const std::vector<Insn> alias_blk = build_block(/*aliasing_load=*/true);
  const Sched1ProjectionView alias_view =
      ProjectPostReloadBlockToSched1View(alias_blk, mm, /*live_at_end_s2=*/{}, proj_ctx);
  const std::vector<std::pair<int, int>> alias_edges = ExtractCoreSched2Edges(
      alias_blk, /*prologue_end=*/0, static_cast<int>(alias_blk.size()), alias_view, facts, mm);
  int alias_load = -1;
  int alias_store = -1;
  for (size_t ci = 0; ci < alias_view.core_to_orig_index.size(); ++ci) {
    if (alias_view.core_to_orig_index[ci] == 32) {
      alias_load = static_cast<int>(ci);
    } else if (alias_view.core_to_orig_index[ci] == 33) {
      alias_store = static_cast<int>(ci);
    }
  }
  ASSERT_GE(alias_load, 0);
  ASSERT_GE(alias_store, 0);
  EXPECT_NE(
      std::find(alias_edges.begin(), alias_edges.end(), std::make_pair(alias_load, alias_store)),
      alias_edges.end());
  EXPECT_FALSE(PreservesPostReloadOrderSafety(
      alias_blk, swapped, /*prologue_end=*/0, static_cast<int>(alias_blk.size()),
      /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts, mm,
      /*allow_pure_reader_reg_dead_transfer=*/false));
}

TEST(SchedModelTest, ExtractCoreSched2ReplayEdgesProjectsLuidTiesReloadOwnersAndRegDeadOrder) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = kMipsFirstPseudoRegister;

  // 1. Positive + Negative (`sched.c:2428` LUID tie-breaks + `reload1.c:5641`
  //    reload-owner projection):
  //    Under default R4000/R4300 `-O2`, `kLoad` latency is 3 cycles (`pri = 1`
  //    for root loads, `pri = 3` for their ALU consumers):
  //    - insn 7 runs at clk 5 -> unlocks load insn 3 at clk 8 (waits behind insn 4 pri 3)
  //    - insn 6 (owner of input reload insn 2) runs at clk 6 -> unlocks reload insn 2 at clk 9
  //    - at clk 9 (`cursor = 3`), `ready = {3, 2}` (both pri 1, `kUnitMemory`) ->
  //      `insn 3` wins by LUID tie (`2 -> 3`), projecting acyclically to
  //      `(owner insn 6 -> insn 3)` = `(core 5 -> core 2)`.
  const std::vector<Insn> target = {
      MakeInsn("(set (reg:SI 21 s5) (mem/u:SI (reg:SI 20 s4)))", 0),
      MakeInsn("(set (reg:SI 22 s6) (mem/u:SI (reg:SI 19 s3)))", 1),
      MakeInsn("(set (reg:SI 7 a3) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))", 2),
      MakeInsn("(set (reg:SI 23 s7) (mem/u:SI (reg:SI 18 s2)))", 3),
      MakeInsn("(set (reg:SI 21 s5) (xor:SI (reg:SI 21 s5) (const_int 1)))", 4),
      MakeInsn("(set (reg:SI 22 s6) (xor:SI (reg:SI 22 s6) (const_int 1)))", 5),
      MakeInsn("(set (reg:SI 2 v0) (xor:SI (reg:SI 7 a3) (reg:SI 17 s1))) "
               "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
               6),
      MakeInsn("(set (reg:SI 23 s7) (xor:SI (reg:SI 23 s7) (const_int 1)))", 7),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 23 s7)))", 8),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 22 s6)))", 9),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 21 s5)))", 10),
      MakeInsn("(use (reg/i:SI 2 v0))", 11),
  };
  const std::vector<int> id_map = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

  // Build a `Sched1ProjectionView` with insn 2 marked as an excluded input
  // reload owned by core index 5 (orig insn 6), and insns {0,1,3..11} as core {0..10}.
  Sched1ProjectionView view;
  view.core_to_orig_index = {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  for (size_t ci = 0; ci < view.core_to_orig_index.size(); ++ci) {
    Insn insn = target[static_cast<size_t>(view.core_to_orig_index[ci])];
    insn.luid = static_cast<int>(ci);
    view.core_insns.push_back(std::move(insn));
  }
  view.input_reload_before_core[2] = 5;  // reload insn 2 -> core 5 (orig insn 6)

  // `ExtractCoreSched2Edges` only projects semantic `g_s2` DAG edges, so it
  // omits the replay-driven reload-owner tie edge `(core 5 -> core 2)`.
  const std::vector<std::pair<int, int>> s2_dag_edges =
      ExtractCoreSched2Edges(target, 0, static_cast<int>(target.size()), view, facts, mm);
  EXPECT_EQ(std::find(s2_dag_edges.begin(), s2_dag_edges.end(), std::make_pair(5, 2)),
            s2_dag_edges.end());

  const std::vector<std::pair<int, int>> replay_strict = ExtractCoreSched2ReplayEdges(
      target, id_map, 0, static_cast<int>(target.size()), view, facts, mm, /*live_at_end_s2=*/{},
      /*preserve_reg_dead_reader_order=*/true);
  // Positive (reload-owner projection, `reload1.c:5641`): in `target`, reload
  // insn 2 ties insn 3 at `cursor = 3` in `RunForcedReplay` (emitting `2 -> 3`),
  // so projecting input reload 2 onto core owner 5 (orig insn 6) emits
  // `(core 5 -> core 2)`.
  EXPECT_NE(std::find(replay_strict.begin(), replay_strict.end(), std::make_pair(5, 2)),
            replay_strict.end());

  // 2. Positive + Negative (`sched.c:3415-3611` `preserve_reg_dead_reader_order`):
  //    On `pri_target`, insn 1 (`mult`, `REG_DEAD $s2`) has strictly higher
  //    priority than insn 0 (`xor`, pure reader of `$s2`), so `RunForcedReplay`
  //    emits no LUID tie between 0 and 1. `(0, 1)` is omitted when
  //    `preserve_reg_dead_reader_order = false` (Stage 0 transfer search) and
  //    emitted when `preserve_reg_dead_reader_order = true` (Stage 3 strict gate).
  const std::vector<Insn> pri_target = {
      MakeInsn("(set (reg:SI 23 s7) (xor:SI (reg:SI 23 s7) (reg:SI 18 s2)))", 0),
      MakeInsn("(set (reg:SI 21 s5) (mult:SI (reg:SI 21 s5) (reg:SI 18 s2))) "
               "(expr_list:REG_DEAD (reg:SI 18 s2) (nil))",
               1),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 21 s5) (reg:SI 23 s7)))", 2),
      MakeInsn("(use (reg/i:SI 2 v0))", 3),
  };
  Sched1ProjectionView pri_view;
  pri_view.core_to_orig_index = {0, 1, 2, 3};
  pri_view.core_insns = pri_target;
  const std::vector<int> pri_id = {0, 1, 2, 3};
  const std::vector<std::pair<int, int>> pri_relaxed =
      ExtractCoreSched2ReplayEdges(pri_target, pri_id, 0, static_cast<int>(pri_target.size()),
                                   pri_view, facts, mm, /*live_at_end_s2=*/{},
                                   /*preserve_reg_dead_reader_order=*/false);
  const std::vector<std::pair<int, int>> pri_strict =
      ExtractCoreSched2ReplayEdges(pri_target, pri_id, 0, static_cast<int>(pri_target.size()),
                                   pri_view, facts, mm, /*live_at_end_s2=*/{},
                                   /*preserve_reg_dead_reader_order=*/true);
  EXPECT_EQ(std::find(pri_relaxed.begin(), pri_relaxed.end(), std::make_pair(0, 1)),
            pri_relaxed.end());
  EXPECT_NE(std::find(pri_strict.begin(), pri_strict.end(), std::make_pair(0, 1)),
            pri_strict.end());
}

TEST(SchedModelTest, CollectSched1TieFlipCoreOrdersClustersHoistedLeavesAndRanksZeroViolations) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Build a `kSched1` core block simulating post-`sched2` hoisted independent
  // 1-cycle leaf computations (`72`, `73`, `74`) followed by their consuming
  // ALU ops (`75`, `76`, `77`), all tying at `kLaunchPriority` (`birthing_insn_p
  // == 1`, `sched.c:2428`). Require a replay ordering where consumer `75`
  // precedes leaf `73` and consumer `76` precedes leaf `74` (`expr.c:4027`
  // expression-tree clustering).
  Sched1ProjectionView view;
  view.core_insns = {
      MakeInsn("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(set (reg:SI 73) (plus:SI (reg:SI 4 a0) (const_int 4)))", 1),
      MakeInsn("(set (reg:SI 74) (plus:SI (reg:SI 4 a0) (const_int 8)))", 2),
      MakeInsn("(set (reg:SI 75) (xor:SI (reg:SI 72) (const_int 3)))", 3),
      MakeInsn("(set (reg:SI 76) (xor:SI (reg:SI 73) (const_int 5)))", 4),
      MakeInsn("(set (reg:SI 77) (xor:SI (reg:SI 74) (const_int 7)))", 5),
      MakeInsn("(set (reg:SI 78) (plus:SI (reg:SI 75) (reg:SI 76)))", 6),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 78) (reg:SI 77)))", 7),
      MakeInsn("(use (reg/i:SI 2 v0))", 8),
  };
  view.core_to_orig_index = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  view.facts_s1.max_reg_num = 79;
  for (int r = 72; r <= 78; ++r) {
    view.facts_s1.reg_n_sets[r] = 1;
  }

  // `core_s2_replay_edges` requires `(3 -> 1)` (consumer `75` before hoisted
  // load `73`) and `(4 -> 2)` (consumer `76` before hoisted load `74`).
  const std::vector<std::pair<int, int>> core_s2_edges = {{0, 3}, {1, 4}, {2, 5}, {3, 6},
                                                          {4, 6}, {5, 7}, {6, 7}, {7, 8}};
  const std::vector<std::pair<int, int>> core_s2_replay_edges = {{3, 1}, {4, 2}};

  const std::vector<std::vector<int>> orders = CollectSched1TieFlipCoreOrders(
      view, mm, /*max_orders=*/12, core_s2_edges, core_s2_replay_edges);
  ASSERT_FALSE(orders.empty());

  // Positive: `orders[0]` must satisfy ALL `core_s2_edges` and
  // `core_s2_replay_edges` (zero violations) AND be genuinely `kSched1`-achievable.
  const std::vector<int>& best = orders.front();
  ASSERT_EQ(best.size(), view.core_insns.size());
  std::vector<int> pos_in_best(view.core_insns.size(), -1);
  std::vector<Insn> best_seq;
  best_seq.reserve(view.core_insns.size());
  for (size_t pos = 0; pos < best.size(); ++pos) {
    pos_in_best[static_cast<size_t>(best[pos])] = static_cast<int>(pos);
    Insn insn = view.core_insns[static_cast<size_t>(best[pos])];
    insn.luid = static_cast<int>(pos);
    best_seq.push_back(std::move(insn));
  }
  for (const auto& [u, v] : core_s2_edges) {
    EXPECT_LT(pos_in_best[static_cast<size_t>(u)], pos_in_best[static_cast<size_t>(v)]);
  }
  for (const auto& [u, v] : core_s2_replay_edges) {
    EXPECT_LT(pos_in_best[static_cast<size_t>(u)], pos_in_best[static_cast<size_t>(v)]);
  }
  EXPECT_TRUE(IsAchievable(best_seq, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1));

  // Negative: `max_orders == 0` returns empty.
  EXPECT_TRUE(CollectSched1TieFlipCoreOrders(view, mm, /*max_orders=*/0, core_s2_edges,
                                             core_s2_replay_edges)
                  .empty());
}

TEST(SchedModelTest,
     PreservesPostReloadOrderSafetyAllowsSymmetricMemLoadEquivRestorationOnBaseDeath) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = 72;

  // Positive (`local-alloc.c:589-599, 1051-1055`): `sched2` hoisted insn 1
  // carrying `(REG_DEAD $2 v0)` ahead of `MEM` load insn 2 (`4($2 v0)` without
  // `REG_EQUIV`), causing `validate_equiv_mem` on `base_block` to spuriously
  // report `bv = true`. Restoring insn 1 after insn 2 (`cand = {0, 2, 1, 3, 4}`)
  // transitions `validate_equiv_mem` back to `cv = false`, matching the absence
  // of `REG_EQUIV`.
  const std::vector<Insn> base_no_equiv = {
      MakeInsn("(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))", 0),
      MakeInsn("(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 8)))) "
               "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
               1),
      MakeInsn("(set (reg:SI 16 s0) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4))))", 2),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 17 s1))) "
               "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SI 17 s1) (nil)))",
               3),
      MakeInsn("(use (reg/i:SI 2 v0))", 4),
  };
  const std::vector<int> cand_uninvert = {0, 2, 1, 3, 4};
  EXPECT_TRUE(PreservesPostReloadOrderSafety(base_no_equiv, cand_uninvert, /*prologue_end=*/0,
                                             /*epilogue_begin=*/5, /*live_at_end_s2=*/{},
                                             /*cfg_copy_pairs=*/{}, facts, mm,
                                             /*allow_pure_reader_reg_dead_transfer=*/false));

  // Negative 1: if insn 2 DOES carry matching `REG_EQUIV (mem/u:SI ...)`,
  // transitioning `validate_equiv_mem` from `bv = true` to `cv = false` must
  // be rejected because `update_equiv_regs` would no longer attach `REG_EQUIV`.
  std::vector<Insn> base_with_equiv = base_no_equiv;
  base_with_equiv[2] = MakeInsn(
      "(set (reg:SI 16 s0) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4)))) "
      "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4))) (nil))",
      2);
  EXPECT_FALSE(PreservesPostReloadOrderSafety(base_with_equiv, cand_uninvert, /*prologue_end=*/0,
                                              /*epilogue_begin=*/5, /*live_at_end_s2=*/{},
                                              /*cfg_copy_pairs=*/{}, facts, mm,
                                              /*allow_pure_reader_reg_dead_transfer=*/false));

  // Negative 2: when insn 2 loads from an unrelated base `$5 a1` and moving an
  // aliasing memory store past it breaks `validate_equiv_mem` (`bv = true ->
  // cv = false`) without moving any base-reg death after insn 2, reject.
  const std::vector<Insn> base_unrelated_store = {
      MakeInsn("(set (mem:SI (reg:SI 6 a2)) (reg:SI 4 a0))", 0),
      MakeInsn("(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 5 a1) (const_int 4))))", 1),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (const_int 1))) "
               "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
               2),
      MakeInsn("(use (reg/i:SI 2 v0))", 3),
  };
  const std::vector<int> cand_move_store_after = {1, 0, 2, 3};
  EXPECT_FALSE(PreservesPostReloadOrderSafety(base_unrelated_store, cand_move_store_after,
                                              /*prologue_end=*/0, /*epilogue_begin=*/4,
                                              /*live_at_end_s2=*/{}, /*cfg_copy_pairs=*/{}, facts,
                                              mm, /*allow_pure_reader_reg_dead_transfer=*/false));
}

TEST(SchedModelTest, BuildInvertedRegDeadSeedOrderInBaseSupportsHoistPullAndDeferModes) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = 72;

  // Positive (`flow.c:2506`, `sched.c:3415-3611`): insn 1 carries premature
  // `(REG_DEAD $f6)` before independent insn 2 and later pure reader insn 3.
  const std::vector<Insn> inverted = {
      MakeInsn("(set (reg:SF 38 $f6) (mem:SF (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SF 42 $f10) (plus:SF (reg:SF 42 $f10) (reg:SF 38 $f6))) "
               "(expr_list:REG_DEAD (reg:SF 38 $f6) (nil))",
               1),
      MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 5 a1) (const_int 7)))", 2),
      MakeInsn("(set (reg:SF 40 $f8) (minus:SF (reg:SF 48 $f16) (reg:SF 38 $f6)))", 3),
      MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 42 $f10) (reg:SF 40 $f8)))", 4),
      MakeInsn("(use (reg/i:SF 32 $f0))", 5),
  };
  const std::optional<std::vector<int>> hoist = BuildInvertedRegDeadSeedOrderInBase(
      inverted, 0, 6, facts, mm, InvertedRegDeadSeedMode::kHoistEarliest);
  const std::optional<std::vector<int>> pull = BuildInvertedRegDeadSeedOrderInBase(
      inverted, 0, 6, facts, mm, InvertedRegDeadSeedMode::kPullBeforeDead);
  const std::optional<std::vector<int>> defer = BuildInvertedRegDeadSeedOrderInBase(
      inverted, 0, 6, facts, mm, InvertedRegDeadSeedMode::kDeferAfterReaders);
  ASSERT_TRUE(hoist.has_value());
  ASSERT_TRUE(pull.has_value());
  ASSERT_TRUE(defer.has_value());
  EXPECT_EQ(*hoist, (std::vector<int>{0, 3, 1, 2, 4, 5}));
  EXPECT_EQ(*pull, (std::vector<int>{0, 3, 1, 2, 4, 5}));
  EXPECT_EQ(*defer, (std::vector<int>{0, 2, 3, 1, 4, 5}));

  // Negative 1: when insn 2 depends on insn 1 (`$f10`), `kDeferAfterReaders`
  // must NOT move insn 1 past its dependent insn 2; it falls back to hoisting 3.
  std::vector<Insn> dep_between = inverted;
  dep_between[2] =
      MakeInsn("(set (reg:SF 44 $f12) (plus:SF (reg:SF 42 $f10) (reg:SF 44 $f12)))", 2);
  const std::optional<std::vector<int>> defer_blocked = BuildInvertedRegDeadSeedOrderInBase(
      dep_between, 0, 6, facts, mm, InvertedRegDeadSeedMode::kDeferAfterReaders);
  ASSERT_TRUE(defer_blocked.has_value());
  EXPECT_EQ(*defer_blocked, (std::vector<int>{0, 3, 1, 2, 4, 5}));

  // Negative 2: when `(REG_DEAD $f6)` is already on the last reader (insn 3),
  // all three modes return `std::nullopt`.
  std::vector<Insn> non_inverted = inverted;
  non_inverted[1] =
      MakeInsn("(set (reg:SF 42 $f10) (plus:SF (reg:SF 42 $f10) (reg:SF 38 $f6)))", 1);
  non_inverted[3] = MakeInsn(
      "(set (reg:SF 40 $f8) (minus:SF (reg:SF 48 $f16) (reg:SF 38 $f6))) "
      "(expr_list:REG_DEAD (reg:SF 38 $f6) (nil))",
      3);
  EXPECT_FALSE(BuildInvertedRegDeadSeedOrderInBase(non_inverted, 0, 6, facts, mm,
                                                   InvertedRegDeadSeedMode::kHoistEarliest)
                   .has_value());
  EXPECT_FALSE(BuildInvertedRegDeadSeedOrderInBase(non_inverted, 0, 6, facts, mm,
                                                   InvertedRegDeadSeedMode::kPullBeforeDead)
                   .has_value());
  EXPECT_FALSE(BuildInvertedRegDeadSeedOrderInBase(non_inverted, 0, 6, facts, mm,
                                                   InvertedRegDeadSeedMode::kDeferAfterReaders)
                   .has_value());
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesRepairsCombineRegsPureReaderConflict) {
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

TEST(SchedModelTest, FindPostReloadBodyBoundsSkipsPrologueSmallStructArgShifts) {
  // Positive (`config/mips/mips.c:4976-4992` `mips_expand_prologue`): leading
  // `< 4`-byte by-value struct/union argument right-shifts on strictly
  // increasing arg regs `$a0..$a3` (`4..7`) with optional flow-liveness notes
  // (`REG_DEAD`/`REG_UNUSED`) advance `prologue_end` past the shift prefix.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 4 a0) (const_int 16))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 0),
        MakeInsn("(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 5 a1) (const_int 24)))", 1),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))", 2),
        MakeInsn("(use (reg/i:SI 2 v0))", 3),
    };
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(blk, /*block_idx=*/0);
    EXPECT_EQ(bounds.prologue_end, 2);
  }

  // Negative 1: non-increasing register order (`$5` followed by `$4`) stops
  // after `$5` (`prologue_end == 1`).
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 5 a1) (const_int 16)))", 0),
        MakeInsn("(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 4 a0) (const_int 16)))", 1),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))", 2),
        MakeInsn("(use (reg/i:SI 2 v0))", 3),
    };
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(blk, /*block_idx=*/0);
    EXPECT_EQ(bounds.prologue_end, 1);
  }

  // Negative 2: a non-flow note (`REG_EQUIV`) or non-byte-multiple shift count
  // (`const_int 4`) is NOT a `mips_expand_prologue` struct-arg shift.
  {
    const std::vector<Insn> blk_note = {
        MakeInsn("(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 4 a0) (const_int 16))) "
                 "(expr_list:REG_EQUIV (const_int 0) (nil))",
                 0),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 4 a0))", 1),
        MakeInsn("(use (reg/i:SI 2 v0))", 2),
    };
    EXPECT_EQ(FindPostReloadBodyBounds(blk_note, /*block_idx=*/0).prologue_end, 0);

    const std::vector<Insn> blk_shift = {
        MakeInsn("(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 4 a0) (const_int 4)))", 0),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 4 a0))", 1),
        MakeInsn("(use (reg/i:SI 2 v0))", 2),
    };
    EXPECT_EQ(FindPostReloadBodyBounds(blk_shift, /*block_idx=*/0).prologue_end, 0);
  }
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewKeepsLowerAllocOrderFreeRegUnrenamed) {
  const CompilationProfile mips1_o2{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const MachineModel mm = MachineModel::FromProfile(mips1_o2);

  // Positive (`local-alloc.c:2158-2181, 2243-2263`, `config/mips/mips.h:1186-1209, 1255-1274`):
  // when block-local FP def `$f2 (34)` has strictly lower same-bank call-used
  // `$f0 (32)` completely free across `[0, 2)` (live range `[def, death)`),
  // `HasLocalLowerAllocOrderFreeRegConflict` keeps `$f2 (34)` unrenamed as a
  // hard register rather than lifting it to a fresh pseudo.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (reg:SF 36 $f4) (mult:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 1),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 36 $f4))) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 36 $f4) (nil)))",
                 2),
        MakeInsn("(use (reg/i:SF 32 $f0))", 3),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, {});
    ASSERT_EQ(view.core_insns.size(), 4u);
    EXPECT_EQ(view.core_insns[0].pn.pattern.SetDest().RegNo(), 34);
  }

  // Negative contrast: when `$f0 (32)` IS live across `[1, 2)` (defined at `[0]`
  // and dying at `[2]`), `$f2 (34)` has no free strictly-lower same-bank reg and
  // lifts normally to a pseudo `>= kMipsFirstPseudoRegister`.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 32 $f0) (mult:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 1),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
                 2),
        MakeInsn("(use (reg/i:SF 32 $f0))", 3),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, {});
    ASSERT_EQ(view.core_insns.size(), 4u);
    EXPECT_GE(view.core_insns[1].pn.pattern.SetDest().RegNo(), kMipsFirstPseudoRegister);
  }
}

TEST(SchedModelTest, PreservesPostReloadOrderSafetyIncomingStackParamAndBaseRegDeathEquivWindow) {
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

TEST(SchedModelTest, CollectSched1SunkConsumerHoistCoreOrdersPositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`sched.c:3144-3942`): producer `p=0` (`l.s $f2`) has its consumer
  // `c_p=3` (`add.s $f2`) sunk past `g=1` (`l.s $f4`) and `g`'s consumer
  // `c_g=2` (`add.s $f4`), so forward backward-list `Schedule(kSched1)` places
  // `g=1` ahead of `p=0`. `CollectSched1SunkConsumerHoistCoreOrders` must emit
  // a candidate permutation hoisting `c_p=3` (with its in-window predecessors)
  // ahead of `c_g=2`.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SF 36 $f4) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4)))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 1),
        MakeInsn("(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 44 $f12))) "
                 "(expr_list:REG_DEAD (reg:SF 36 $f4) (expr_list:REG_DEAD (reg:SF 44 $f12) (nil)))",
                 2),
        MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 34 $f2) (reg:SF 46 $f14))) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 46 $f14) (nil)))",
                 3),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 36 $f4))) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 36 $f4) (nil)))",
                 4),
        MakeInsn("(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))", 5),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, {});
    ASSERT_EQ(view.core_insns.size(), 6u);
    const std::vector<int> s1_ord = ScheduleSched1ViewCoreOrder(view, mm);
    ASSERT_EQ(s1_ord.size(), 6u);
    // Sanity check: `p=0` was displaced after `g=1` in forward `Schedule(kSched1)`.
    const int pos0 = static_cast<int>(std::find(s1_ord.begin(), s1_ord.end(), 0) - s1_ord.begin());
    const int pos1 = static_cast<int>(std::find(s1_ord.begin(), s1_ord.end(), 1) - s1_ord.begin());
    ASSERT_GT(pos0, pos1);

    const std::vector<std::vector<int>> hoists =
        CollectSched1SunkConsumerHoistCoreOrders(view, mm, /*max_orders=*/8);
    ASSERT_FALSE(hoists.empty());
    bool saw_c_p_before_c_g = false;
    for (const std::vector<int>& ord : hoists) {
      ASSERT_EQ(ord.size(), 6u);
      const int p3 = static_cast<int>(std::find(ord.begin(), ord.end(), 3) - ord.begin());
      const int p2 = static_cast<int>(std::find(ord.begin(), ord.end(), 2) - ord.begin());
      if (p3 < p2) {
        saw_c_p_before_c_g = true;
      }
    }
    EXPECT_TRUE(saw_c_p_before_c_g);
  }

  // Negative: when `p=0`'s consumer `c_p=2` is already ahead of `g=1`'s
  // consumer `c_g=3`, no producer is displaced and no hoist orders are emitted.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SF 36 $f4) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4)))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 1),
        MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 34 $f2) (reg:SF 46 $f14))) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 46 $f14) (nil)))",
                 2),
        MakeInsn("(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 44 $f12))) "
                 "(expr_list:REG_DEAD (reg:SF 36 $f4) (expr_list:REG_DEAD (reg:SF 44 $f12) (nil)))",
                 3),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 36 $f4))) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 36 $f4) (nil)))",
                 4),
        MakeInsn("(use (reg/i:SF 32 $f0)) (expr_list:REG_DEAD (reg/i:SF 32 $f0) (nil))", 5),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{}, {});
    EXPECT_TRUE(CollectSched1SunkConsumerHoistCoreOrders(view, mm, /*max_orders=*/8).empty());
  }
}

TEST(SchedModelTest, FoldFprOutputReloadsCallUsedGprGuardPositiveAndNegative) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`reload1.c:5641`): a call-used GPR copy `(set (reg:SF 5 a1) (reg:SF 32 $f0))`
  // whose source `$f0` is also read by another FP insn (`add.s $f2, $f0, $f16`)
  // is a pre-reload call-argument copy, NOT a `reload1.c` output reload, so
  // `ProjectPostReloadBlockToSched1View` must NOT fold it onto the producer.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 32 $f0) (reg:SF 48 $f16)))", 1),
        MakeInsn("(set (reg:SF 5 a1) (reg:SF 32 $f0))", 2),
        MakeInsn("(set (reg:SF 32 $f0) (reg:SF 34 $f2))", 3),
        MakeInsn("(use (reg/i:SF 32 $f0))", 4),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{5}, {});
    EXPECT_FALSE(view.output_reload_after_core.contains(2));
    EXPECT_EQ(view.core_insns.size(), 5u);
  }

  // Negative 1: when the destination GPR is callee-saved (`$s0`, `!CallUsed`),
  // an inherited FP reader of `$f0` DOES fold via `reload1.c:5186-5249, 5641`.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 32 $f0) (reg:SF 48 $f16)))", 1),
        MakeInsn("(set (reg:SF 16 s0) (reg:SF 32 $f0))", 2),
        MakeInsn("(set (reg:SF 32 $f0) (reg:SF 34 $f2))", 3),
        MakeInsn("(use (reg/i:SF 32 $f0))", 4),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{16}, {});
    ASSERT_TRUE(view.output_reload_after_core.contains(2));
    EXPECT_EQ(view.output_reload_after_core.at(2), 0);
  }

  // Negative 2: when a call-used GPR move (`$a1`) has NO other FP readers of
  // `$f0`, folding it as an output reload remains allowed.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (reg:SF 5 a1) (reg:SF 32 $f0))", 1),
        MakeInsn("(set (reg:SF 32 $f0) (reg:SF 48 $f16))", 2),
        MakeInsn("(use (reg/i:SF 32 $f0))", 3),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{5}, {});
    ASSERT_TRUE(view.output_reload_after_core.contains(1));
    EXPECT_EQ(view.output_reload_after_core.at(1), 0);
  }
}

TEST(SchedModelTest, BuildInvertedRegDeadSeedDeferForwardClosureAndEquivDeath) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts_s2;
  facts_s2.max_reg_num = kMipsFirstPseudoRegister;

  // 1. Positive (`kDeferAfterReaders` forward closure): insn 0 carries
  // `(REG_DEAD s0)` and feeds insn 1 (`t1 = t0 + 2`), while insn 2 is an
  // independent later reader of `s0`. `kDeferAfterReaders` must defer BOTH 0
  // and 1 together after 2, preserving `0 < 1` and placing `2 < 0`.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 16 s0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                 0),
        MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 8 t0) (const_int 2))) "
                 "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                 1),
        MakeInsn("(set (reg:SI 10 t2) (xor:SI (reg:SI 16 s0) (const_int 3)))", 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (reg:SI 10 t2)))", 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
    const std::optional<std::vector<int>> seed =
        BuildInvertedRegDeadSeedOrderInBase(blk, /*prologue_end=*/0, /*epilogue_begin=*/4, facts_s2,
                                            mm, InvertedRegDeadSeedMode::kDeferAfterReaders);
    ASSERT_TRUE(seed.has_value());
    EXPECT_EQ(*seed, (std::vector<int>{2, 0, 1, 3, 4}));
  }

  // 2. Negative (`kDeferAfterReaders` dependency guard): when the later reader
  // of `s0` ALSO depends on `i`'s output `t0`, neither deferring `i` after `j`
  // nor hoisting `j` before `i` is legal, so the seed returns `std::nullopt`.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 16 s0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                 0),
        MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 8 t0) (reg:SI 16 s0)))", 1),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 9 t1))", 2),
        MakeInsn("(use (reg/i:SI 2 v0))", 3),
    };
    const std::optional<std::vector<int>> seed =
        BuildInvertedRegDeadSeedOrderInBase(blk, /*prologue_end=*/0, /*epilogue_begin=*/3, facts_s2,
                                            mm, InvertedRegDeadSeedMode::kDeferAfterReaders);
    EXPECT_FALSE(seed.has_value());
  }

  // 3. Positive + Negative (`local-alloc.c:580-599` `validate_equiv_mem`):
  // when later reader `j=1` is a `MEM` load carrying `(REG_EQUIV (mem ... v0))`
  // whose dest `t0` dies at `d=2`, `kDeferAfterReaders` defers `i=0`
  // (`REG_DEAD v0`) past `d=2`; without `REG_EQUIV`, it defers `i=0` right
  // after `j=1` (before `d=2`).
  auto make_equiv_block = [](bool attach_reg_equiv) {
    const std::string load_str =
        attach_reg_equiv
            ? "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 2 v0) (const_int 4)))) "
              "(expr_list:REG_EQUIV (mem:SI (plus:SI (reg:SI 2 v0) (const_int 4))) (nil))"
            : "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 2 v0) (const_int 4))))";
    return std::vector<Insn>{
        MakeInsn("(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 2 v0) (const_int 8)))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 0),
        MakeInsn(load_str, 1),
        MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 8 t0) (const_int 5))) "
                 "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 9 t1)))", 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
  };
  {
    const std::optional<std::vector<int>> with_equiv = BuildInvertedRegDeadSeedOrderInBase(
        make_equiv_block(/*attach_reg_equiv=*/true), /*prologue_end=*/0, /*epilogue_begin=*/4,
        facts_s2, mm, InvertedRegDeadSeedMode::kDeferAfterReaders);
    ASSERT_TRUE(with_equiv.has_value());
    EXPECT_EQ(*with_equiv, (std::vector<int>{1, 2, 0, 3, 4}));

    const std::optional<std::vector<int>> without_equiv = BuildInvertedRegDeadSeedOrderInBase(
        make_equiv_block(/*attach_reg_equiv=*/false), /*prologue_end=*/0, /*epilogue_begin=*/4,
        facts_s2, mm, InvertedRegDeadSeedMode::kDeferAfterReaders);
    ASSERT_TRUE(without_equiv.has_value());
    EXPECT_EQ(*without_equiv, (std::vector<int>{1, 0, 2, 3, 4}));
  }

  // 4. Positive + Negative (`kPullBeforeDead` backward `base_graph` ancestor
  // closure): when later reader `j=2` (`a3 = mem[v0+20]`) has a `kSched2`
  // anti-dep ancestor `a=1` (`t0 = a3 + 4`) after `i=0` (`REG_DEAD v0`),
  // `kPullBeforeDead` pulls `[1, 2]` together ahead of `0` preserving `1 < 2`.
  // Negative: when `a=1` depends on `i=0`, `kPullBeforeDead` cannot pull `j=2`
  // ahead of `0` and returns `std::nullopt`.
  {
    const std::vector<Insn> pull_pos_blk = {
        MakeInsn("(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 2 v0) (const_int 32)))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 0),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 7 a3) (const_int 4))) "
                 "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                 1),
        MakeInsn("(set (reg:SI 7 a3) (mem:SI (plus:SI (reg:SI 2 v0) (const_int 20))))", 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 7 a3)))", 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
    const std::optional<std::vector<int>> pull_pos =
        BuildInvertedRegDeadSeedOrderInBase(pull_pos_blk, /*prologue_end=*/0, /*epilogue_begin=*/4,
                                            facts_s2, mm, InvertedRegDeadSeedMode::kPullBeforeDead);
    ASSERT_TRUE(pull_pos.has_value());
    EXPECT_EQ(*pull_pos, (std::vector<int>{1, 2, 0, 3, 4}));

    const std::vector<Insn> pull_neg_blk = {
        MakeInsn("(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 2 v0) (const_int 32)))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 0),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 16 s0) (reg:SI 7 a3))) "
                 "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                 1),
        MakeInsn("(set (reg:SI 7 a3) (mem:SI (plus:SI (reg:SI 2 v0) (const_int 20))))", 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 7 a3)))", 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
    const std::optional<std::vector<int>> pull_neg =
        BuildInvertedRegDeadSeedOrderInBase(pull_neg_blk, /*prologue_end=*/0, /*epilogue_begin=*/4,
                                            facts_s2, mm, InvertedRegDeadSeedMode::kPullBeforeDead);
    EXPECT_FALSE(pull_neg.has_value());
  }
}

TEST(SchedModelTest, AttributeSched2SplitRmwLoadRegEquivRecoveryPositiveAndNegative) {
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

TEST(SchedModelTest, FoldInheritedOutputReloadSpillStorePositiveAndNegative) {
  // `reload1.c:1968-1992, 5186-5257, 5641-6155`: when a spilled pseudo's
  // output-reload spill store `(set (mem:DI sp) (reg:DI t2))` has no
  // intra-block input-reload load because a later consumer inherits `t2`
  // directly while carrying the in-place `MEM`-rewritten `(REG_DEAD (mem:DI sp))`
  // note, `ProjectPostReloadBlockToSched1View` must fold the spill store onto
  // its producer (`mult:DI`) so `sched1` does not see a spurious stack store
  // (positive). Without `(REG_DEAD (mem:DI sp))`, the store must remain in
  // `core_insns` as a genuine user stack store (negative).
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  auto make_block = [](bool attach_mem_reg_dead) {
    const std::string cons_str =
        attach_mem_reg_dead
            ? "(parallel [(set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 10 t2))) "
              "(clobber (reg:SI 4 a0))]) (expr_list:REG_DEAD (mem:DI (reg:SI 29 sp)) (nil))"
            : "(parallel [(set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 10 t2))) "
              "(clobber (reg:SI 4 a0))])";
    return std::vector<Insn>{
        MakeInsn("(parallel [(set (reg:DI 64 hi) (mult:DI (sign_extend:DI (reg:SI 7 a3)) "
                 "(sign_extend:DI (reg:SI 6 a2)))) (clobber (reg:SI 65 lo)) "
                 "(clobber (reg:SI 66 accum))])",
                 0),
        MakeInsn("(set (reg:DI 10 t2) (reg:DI 64 hi))", 1),
        MakeInsn("(set (mem:DI (reg:SI 29 sp)) (reg:DI 10 t2))", 2),
        MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 8 t0)))", 3),
        MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))", 4),
        MakeInsn(cons_str, 5),
        MakeInsn("(use (reg/i:DI 2 v0))", 6),
    };
  };

  Sched1ProjectionContext ctx;
  ctx.stack_layout.valid = true;
  ctx.stack_layout.frame_pointer_needed = false;
  ctx.stack_layout.starting_frame_offset = 0;
  ctx.stack_layout.frame_size = 8;

  // Positive: `(REG_DEAD (mem:DI (reg:SI 29 sp)))` on consumer 5 proves store 2
  // is an inherited output-reload spill store; both 1 and 2 fold onto core 0.
  {
    const std::vector<Insn> blk = make_block(/*attach_mem_reg_dead=*/true);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{2, 3}, ctx);
    EXPECT_EQ(view.core_to_orig_index, (std::vector<int>{0, 3, 4, 5, 6}));
    ASSERT_TRUE(view.output_reload_after_core.contains(1));
    ASSERT_TRUE(view.output_reload_after_core.contains(2));
    EXPECT_EQ(view.output_reload_after_core.at(1), 0);
    EXPECT_EQ(view.output_reload_after_core.at(2), 0);
    EXPECT_TRUE(
        IsAchievable(view.core_insns, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1));
  }

  // Negative: without `(REG_DEAD (mem:DI (reg:SI 29 sp)))`, store 2 is NOT
  // folded as an inherited spill store and stays in `core_insns`.
  {
    const std::vector<Insn> blk = make_block(/*attach_mem_reg_dead=*/false);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{2, 3}, ctx);
    EXPECT_EQ(view.core_to_orig_index, (std::vector<int>{0, 2, 3, 4, 5, 6}));
    EXPECT_FALSE(view.output_reload_after_core.contains(2));
  }
}

TEST(SchedModelTest, ProjectPostReloadPreservesCallerSavedLiveAcrossCallAndSpilledRmwDeath) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Part 1 (`caller-save.c:349-620`, `local-alloc.c:2158-2181`):
  // `$v0` (`2`, lowest `REG_ALLOC_ORDER` GPR) is saved to `16($sp)` before
  // `CALL_INSN` and restored after `CALL_INSN`. A local pseudo in `$v1` (`3`,
  // next in `REG_ALLOC_ORDER`) whose `[def, use]` live range sits BEFORE the
  // call (overlapping `$v0`) must observe `$v0` as live across `CALL_INSN` via
  // `preserved_mask` (`ComputeCoreHardRegLiveAfterMasks`) so `$v1` stays a
  // pseudo (positive). Without the caller-save save/restore around the call
  // (negative), `CALL_INSN` clears call-used `$v0` in the backward liveness
  // walk, making `$v0` appear free across `$v1`'s pre-call live range and
  // pinning `$v1` to hard reg `$3 v1`.
  auto make_caller_save_block = [](bool preserve_v0_around_call) {
    std::vector<Insn> blk;
    int id = 0;
    blk.push_back(MakeInsn("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))", id++));
    if (preserve_v0_around_call) {
      blk.push_back(
          MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 2 v0))", id++));
    }
    blk.push_back(MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 16 s0) (const_int 2)))", id++));
    blk.push_back(
        MakeInsn("(set (reg:SI 17 s1) (plus:SI (reg:SI 3 v1) (const_int 5))) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                 id++));
    blk.push_back(
        MakeInsn("(parallel[ (call (mem:SI (symbol_ref:SI (\"SinkVoid\"))) (const_int 16)) "
                 "(clobber (reg:SI 31 ra)) ] )",
                 id++, Insn::Kind::kCall));
    if (preserve_v0_around_call) {
      blk.push_back(
          MakeInsn("(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))", id++));
    }
    blk.push_back(
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 17 s1))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 17 s1) "
                 "(nil)))",
                 id++));
    blk.push_back(MakeInsn("(use (reg/i:SI 2 v0))", id++));
    return blk;
  };

  Sched1ProjectionContext cs_ctx;
  cs_ctx.stack_layout.valid = true;
  cs_ctx.stack_layout.frame_pointer_needed = false;
  cs_ctx.stack_layout.starting_frame_offset = 16;
  cs_ctx.stack_layout.frame_size = 32;

  {
    const std::vector<Insn> blk = make_caller_save_block(/*preserve_v0_around_call=*/true);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{}, cs_ctx);
    // Core insns: [0]: v0=a0+1, [1]: v1=s0+2, [2]: s1=v1+5, [3]: call, [4]: v0=v0+s1, [5]: use v0.
    // Because `$v0` is caller-saved across `call`, `preserved_mask` keeps `$v0`
    // live backward across `call` into `[1, 2]`, so `$v1` stays lifted as a pseudo.
    ASSERT_GE(view.core_insns.size(), 5u);
    EXPECT_TRUE(view.core_insns[1].pn.pattern.SetDest().IsPseudoReg())
        << view.core_insns[1].pn.pattern.ToSexprString();
  }
  {
    const std::vector<Insn> blk = make_caller_save_block(/*preserve_v0_around_call=*/false);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{}, cs_ctx);
    ASSERT_GE(view.core_insns.size(), 5u);
    // Without the caller-save save/restore around `call`, `call` clears `$v0`
    // in backward liveness, so lower-alloc-order `$v0` is free across `[1, 2]`
    // and `$v1` stays pinned as hard reg `(reg:SI 3 v1)`.
    EXPECT_TRUE(view.core_insns[1].pn.pattern.SetDest().IsHardReg())
        << view.core_insns[1].pn.pattern.ToSexprString();
  }

  // Part 2 (`reload1.c:5641-6155`, `local-alloc.c:2158-2181`):
  // `$v1` (`3`, after `$v0 = 2` in `REG_ALLOC_ORDER`) is defined while `$v0`
  // is free, then consumed by a spilled-output dying-input RMW
  // `(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 3))) (REG_DEAD v1)`
  // followed immediately by its spill store to `20($sp)`.
  // `HasLocalLowerAllocOrderFreeRegConflict` recognizes `(REG_DEAD v1)` on the
  // spilled RMW as terminating `$v1`'s local live range while `$v0` is free,
  // pinning `$v1` to hard reg `(reg:SI 3 v1)` (positive). Without `(REG_DEAD
  // v1)` on the spilled RMW (negative), `HasLocalLowerAllocOrderFreeRegConflict`
  // returns false and `$v1` is lifted as a pseudo.
  auto make_spilled_rmw_block = [](bool attach_dead_on_rmw) {
    const std::string rmw = attach_dead_on_rmw
                                ? "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 3))) "
                                  "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))"
                                : "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 3)))";
    return std::vector<Insn>{
        MakeInsn("(set (reg:SI 3 v1) (mem/u:SI (reg:SI 4 a0))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 0),
        MakeInsn(rmw, 1),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 3 v1))", 2),
        MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 7)))", 3),
        MakeInsn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))", 4),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 16 s0))) "
                 "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) "
                 "(expr_list:REG_DEAD (reg:SI 16 s0) (nil)))",
                 5),
        MakeInsn("(use (reg/i:SI 2 v0))", 6),
    };
  };
  {
    const std::vector<Insn> blk = make_spilled_rmw_block(/*attach_dead_on_rmw=*/true);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{}, cs_ctx);
    EXPECT_TRUE(view.output_reload_after_core.contains(2));
    // `$v1` (core idx 0) terminates at the spilled RMW (core idx 1) while `$v0`
    // (`2`) is free across `[0, 1]`, so `$v1` stays pinned as `(reg:SI 3 v1)`.
    ASSERT_GE(view.core_insns.size(), 3u);
    EXPECT_TRUE(view.core_insns[0].pn.pattern.SetDest().IsHardReg())
        << view.core_insns[0].pn.pattern.ToSexprString();
  }
  {
    const std::vector<Insn> blk = make_spilled_rmw_block(/*attach_dead_on_rmw=*/false);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{}, cs_ctx);
    EXPECT_TRUE(view.output_reload_after_core.contains(2));
    // Without `(REG_DEAD v1)` on the spilled RMW, `$v1`'s local live range is
    // not recognized as terminating there, so `$v1` is lifted as a pseudo.
    ASSERT_GE(view.core_insns.size(), 3u);
    EXPECT_TRUE(view.core_insns[0].pn.pattern.SetDest().IsPseudoReg())
        << view.core_insns[0].pn.pattern.ToSexprString();
  }
}

TEST(SchedModelTest, BuildInvertedRegDeadSeedPreservesReloadContiguity) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = 72;

  // Build a block where an earlier `(REG_DEAD v0)` `mem/u` load precedes a
  // later `v0` reader carrying `REG_EQUIV` whose death insn is followed by an
  // adjacent reload spill store (`reload1.c:5641-6155`).
  auto make_block = [](bool invert_v0_dead) {
    std::vector<Insn> blk;
    int id = 0;
    blk.push_back(MakeInsn(
        "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))", id++));
    if (invert_v0_dead) {
      blk.push_back(
          MakeInsn("(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 8))))"
                   " (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                   id++));
    }
    // Later reader of `v0` carrying `REG_EQUIV`, its death insn, and an adjacent
    // stack spill store (`reload_adjacent`).
    blk.push_back(
        MakeInsn("(set (reg:SI 7 a3) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4)))) "
                 "(expr_list:REG_EQUIV (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4))) (nil))",
                 id++));
    blk.push_back(
        MakeInsn("(set (reg:SI 7 a3) (xor:SI (reg:SI 7 a3) (const_int 6))) "
                 "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                 id++));
    blk.push_back(
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 7 a3))", id++));
    if (!invert_v0_dead) {
      blk.push_back(
          MakeInsn("(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 8))))"
                   " (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                   id++));
    }
    blk.push_back(
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 17 s1) (reg:SI 16 s0))) "
                 "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
                 id++));
    blk.push_back(MakeInsn("(use (reg/i:SI 2 v0))", id++));
    return blk;
  };

  // Positive: `BuildInvertedRegDeadSeedOrderInBase` detects the inverted
  // `(REG_DEAD v0)` at idx 1, defers it past the later reader (idx 2), its
  // `REG_EQUIV` death (idx 3), AND the adjacent spill store (idx 4), keeping
  // (3, 4) strictly contiguous.
  {
    const std::vector<Insn> blk = make_block(/*invert_v0_dead=*/true);
    const std::optional<std::vector<int>> seed =
        BuildInvertedRegDeadSeedOrderInBase(blk, /*prologue_end=*/0, static_cast<int>(blk.size()),
                                            facts, mm, InvertedRegDeadSeedMode::kDeferAfterReaders);
    ASSERT_TRUE(seed.has_value());
    std::vector<int> pos(blk.size(), -1);
    for (size_t p = 0; p < seed->size(); ++p) {
      pos[static_cast<size_t>((*seed)[p])] = static_cast<int>(p);
    }
    EXPECT_GT(pos[1], pos[2]);
    EXPECT_GT(pos[1], pos[3]);
    EXPECT_GT(pos[1], pos[4]);
    EXPECT_EQ(pos[4], pos[3] + 1);
  }

  // Negative: when `(REG_DEAD v0)` is already on the last reader of `v0`, no
  // inversion exists and `BuildInvertedRegDeadSeedOrderInBase` returns nullopt.
  {
    const std::vector<Insn> blk = make_block(/*invert_v0_dead=*/false);
    EXPECT_FALSE(BuildInvertedRegDeadSeedOrderInBase(blk, /*prologue_end=*/0,
                                                     static_cast<int>(blk.size()), facts, mm,
                                                     InvertedRegDeadSeedMode::kDeferAfterReaders)
                     .has_value());
  }
}

TEST(SchedModelTest, IsAdjacentReloadContiguityPairPreservesCallerSaveRestoresExcludesSaveStores) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  PostReloadStackLayout layout;
  layout.valid = true;
  layout.frame_pointer_needed = false;
  layout.starting_frame_offset = 32;
  layout.frame_size = 96;
  layout.has_stack_slot_access_info = true;
  layout.stack_slot_store_offsets.insert(72);
  layout.stack_slot_load_offsets.insert(72);
  layout.caller_save_slot_offsets.insert(72);

  // Positive (`caller-save.c:410-411, 568-615` `restore_referenced_regs`):
  // A caller-save restore load from a `caller_save_slot_offsets` slot emitted
  // immediately before its first post-call consumer MUST remain contiguous.
  const Insn restore_load =
      MakeInsn("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))", 0);
  const Insn post_call_consumer =
      MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 20 s4)))", 1);
  EXPECT_TRUE(IsAdjacentReloadContiguityPair(restore_load, post_call_consumer, mm, layout));

  // Negative (`caller-save.c:455-462` `save_call_clobbered_regs`):
  // A caller-save store to a `caller_save_slot_offsets` slot is emitted before
  // `CALL_INSN` rather than adjacent to the defining insn, so it MUST NOT be
  // treated as an adjacent output-spill pair (`reload1.c:5641-6155`).
  const Insn def_insn = MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 5)))", 0);
  const Insn save_store =
      MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))) (reg:SI 3 v1))", 1);
  EXPECT_FALSE(IsAdjacentReloadContiguityPair(def_insn, save_store, mm, layout));

  // Contrast: if slot 72 is a regular `reload1.c` spill slot (NOT in
  // `caller_save_slot_offsets`), the adjacent output-spill pair returns true.
  PostReloadStackLayout spill_layout = layout;
  spill_layout.caller_save_slot_offsets.clear();
  EXPECT_TRUE(IsAdjacentReloadContiguityPair(def_insn, save_store, mm, spill_layout));

  // Verify `caller-save.c:455-462` (`save_call_clobbered_regs`): a caller-save
  // store hoisted ahead of pre-call loads is mapped in `input_reload_before_core`
  // onto its `CALL_INSN` (never left anchored to the next arbitrary load),
  // whereas a `reload1.c:2309` (`alter_reg`) spilled pseudo carrying
  // `(REG_DEAD (mem [sp+72]))` that reuses `$v1` for both output and input
  // reload stays mapped in `output_reload_after_core` onto its defining insn.
  {
    const std::vector<Insn> cs_block = {
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))) (reg:SI 3 v1))", 1),
        MakeInsn("(set (reg:SI 16 s0) (mem/u:SI (reg:SI 5 a1)))", 2),
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ])",
                 3, Insn::Kind::kCall),
        MakeInsn("(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))", 4),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 16 s0)))", 5),
        MakeInsn("(use (reg/i:SI 2 v0))", 6),
    };
    Sched1ProjectionContext ctx;
    ctx.stack_layout = layout;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(cs_block, mm, /*live_at_end_s2=*/{2}, ctx);
    auto it = view.input_reload_before_core.find(1);
    ASSERT_NE(it, view.input_reload_before_core.end());
    EXPECT_EQ(view.core_to_orig_index[static_cast<size_t>(it->second)], 3);

    // Negative contrast: `(REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))`
    // proves slot 72 is a `reload1.c` spilled pseudo, NOT a caller-save slot.
    std::vector<Insn> spill_block = cs_block;
    spill_block[5] = MakeInsn(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 16 s0))) "
        "(expr_list:REG_DEAD (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))) (nil))",
        5);
    Sched1ProjectionContext spill_ctx;
    spill_ctx.stack_layout = spill_layout;
    const Sched1ProjectionView spill_view =
        ProjectPostReloadBlockToSched1View(spill_block, mm, /*live_at_end_s2=*/{2}, spill_ctx);
    EXPECT_FALSE(spill_view.input_reload_before_core.contains(1));
    auto out_it = spill_view.output_reload_after_core.find(1);
    ASSERT_NE(out_it, spill_view.output_reload_after_core.end());
    EXPECT_EQ(spill_view.core_to_orig_index[static_cast<size_t>(out_it->second)], 0);
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

TEST(SchedModelTest, CrossClassFprToGprReloadInheritedReadAndSpillStoreSafety) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts;
  facts.max_reg_num = 72;

  // Part 1 (`local-alloc.c:700-839, 1004-1007` + `reload1.c:5186-5257`):
  // Cross-class FP->GPR copy `(set (reg:SF 16 s0) (reg:SF 32 $f0)) (REG_DEAD $f0)`
  // followed by an FPR-only arithmetic consumer of `$f0` with `$s0` and `$f0`
  // unmodified in between is reload inheritance (NOT an inverted `REG_DEAD`).
  auto make_reload_inherit_block = [](bool clobber_s0_in_middle, bool non_fpr_only_consumer) {
    std::vector<Insn> blk;
    int id = 0;
    blk.push_back(MakeInsn("(set (reg:SF 32 $f0) (mem/u:SF (reg:SI 4 a0)))", id++));
    blk.push_back(MakeInsn(
        "(set (reg:SF 16 s0) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))", id++));
    if (clobber_s0_in_middle) {
      blk.push_back(MakeInsn("(set (reg:SI 16 s0) (const_int 9))", id++));
    }
    if (non_fpr_only_consumer) {
      blk.push_back(MakeInsn("(set (reg:SF 34 $f2) (reg:SF 32 $f0))", id++));
    } else {
      blk.push_back(
          MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 32 $f0) (reg:SF 44 $f12)))", id++));
    }
    blk.push_back(MakeInsn("(set (reg/i:SF 32 $f0) (reg:SF 34 $f2))", id++));
    blk.push_back(MakeInsn("(use (reg/i:SF 32 $f0))", id++));
    return blk;
  };
  // Positive: reload-inherited FPR-only read with `$s0` intact is NOT flagged as
  // an inverted `REG_DEAD` by `BuildInvertedRegDeadSeedOrderInBase`.
  {
    const std::vector<Insn> blk =
        make_reload_inherit_block(/*clobber_s0_in_middle=*/false, /*non_fpr_only_consumer=*/false);
    EXPECT_FALSE(
        BuildInvertedRegDeadSeedOrderInBase(blk, 0, static_cast<int>(blk.size()), facts, mm)
            .has_value());
    // Also verify `PreservesPostReloadOrderSafety(..., false)` accepts moving the
    // reload-inherited consumer back ahead of the copy when `sched2` reordered them.
    const std::vector<Insn> base_swapped = {
        blk[0],
        blk[2],  // FPR-only read before copy in `base_block`
        blk[1],  // copy with `(REG_DEAD $f0)`
        blk[3], blk[4],
    };
    const std::vector<int> cand_restored = {0, 2, 1, 3, 4};
    EXPECT_TRUE(PreservesPostReloadOrderSafety(base_swapped, cand_restored, 0, 5,
                                               /*live_at_end=*/{32},
                                               /*cfg_copy_pairs=*/{}, facts, mm,
                                               /*allow_pure_reader_reg_dead_transfer=*/false));
  }
  // Negative 1: clobbering `$s0` in between invalidates reload inheritance, so
  // `BuildInvertedRegDeadSeedOrderInBase` detects the inverted `(REG_DEAD $f0)`.
  {
    const std::vector<Insn> blk =
        make_reload_inherit_block(/*clobber_s0_in_middle=*/true, /*non_fpr_only_consumer=*/false);
    EXPECT_TRUE(BuildInvertedRegDeadSeedOrderInBase(blk, 0, static_cast<int>(blk.size()), facts, mm)
                    .has_value());
  }
  // Negative 2: a plain register-to-register consumer `(set $f2 $f0)` is NOT an
  // FPR-only arithmetic consumer, so `BuildInvertedRegDeadSeedOrderInBase` flags it.
  {
    const std::vector<Insn> blk =
        make_reload_inherit_block(/*clobber_s0_in_middle=*/false, /*non_fpr_only_consumer=*/true);
    EXPECT_TRUE(BuildInvertedRegDeadSeedOrderInBase(blk, 0, static_cast<int>(blk.size()), facts, mm)
                    .has_value());
  }

  // Part 2 (`PreservesPostReloadOrderSafety` `cfg_copy_pairs` check):
  // A `sched2`-deferred `reload1.c:5641-6155` output reload spill store `(set
  // (mem:SF [sp+40]) (reg:SF 38 $f6))` (no `REG_DEAD $f6`, offset 40 in
  // `stack_slot_load_offsets \ caller_save_slot_offsets`) deferred past
  // `(set (reg:SF 40 $f8) (reg:SF 38 $f6))` induces a spurious `(38, 40)`
  // interference. Restoring the spill store immediately after `$f6`'s definition
  // must be accepted (positive), whereas a `REG_DEAD $f6` user store OR a
  // `caller_save_slot_offsets` store must still be rejected (negative).
  auto make_spill_interference_block = [](bool attach_dead_on_store) {
    const std::string spill_store =
        attach_dead_on_store
            ? "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 40))) (reg:SF 38 $f6)) "
              "(expr_list:REG_DEAD (reg:SF 38 $f6) (nil))"
            : "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 40))) (reg:SF 38 $f6))";
    return std::vector<Insn>{
        MakeInsn("(set (reg:SF 38 $f6) (mem/u:SF (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SF 40 $f8) (reg:SF 38 $f6))", 1),
        MakeInsn(spill_store, 2),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 40 $f8) (reg:SF 44 $f12)))", 3),
        MakeInsn("(use (reg/i:SF 32 $f0))", 4),
    };
  };
  PostReloadStackLayout spill_layout;
  spill_layout.valid = true;
  spill_layout.frame_pointer_needed = false;
  spill_layout.starting_frame_offset = 16;
  spill_layout.frame_size = 64;
  spill_layout.has_stack_slot_access_info = true;
  spill_layout.stack_slot_store_offsets.insert(40);
  spill_layout.stack_slot_load_offsets.insert(40);
  const absl::btree_set<std::pair<int, int>> copy_pairs = {{38, 40}};
  const std::vector<int> cand_move_spill_before_copy = {0, 2, 1, 3, 4};

  // Positive: reload spill store (no `REG_DEAD $f6`, non-caller-save slot).
  {
    const std::vector<Insn> blk = make_spill_interference_block(/*attach_dead_on_store=*/false);
    EXPECT_TRUE(PreservesPostReloadOrderSafety(blk, cand_move_spill_before_copy, 0, 5,
                                               /*live_at_end=*/{32}, copy_pairs, facts, mm,
                                               /*allow_pure_reader_reg_dead_transfer=*/false,
                                               spill_layout));
  }
  // Negative 1: caller-save slot store is NOT an output reload spill store.
  {
    const std::vector<Insn> blk = make_spill_interference_block(/*attach_dead_on_store=*/false);
    PostReloadStackLayout cs_layout = spill_layout;
    cs_layout.caller_save_slot_offsets.insert(40);
    EXPECT_FALSE(PreservesPostReloadOrderSafety(blk, cand_move_spill_before_copy, 0, 5,
                                                /*live_at_end=*/{32}, copy_pairs, facts, mm,
                                                /*allow_pure_reader_reg_dead_transfer=*/false,
                                                cs_layout));
  }
  // Negative 2: user store carrying `(REG_DEAD $f6)` is NOT a reload spill store.
  {
    const std::vector<Insn> blk = make_spill_interference_block(/*attach_dead_on_store=*/true);
    EXPECT_FALSE(PreservesPostReloadOrderSafety(blk, cand_move_spill_before_copy, 0, 5,
                                                /*live_at_end=*/{32}, copy_pairs, facts, mm,
                                                /*allow_pure_reader_reg_dead_transfer=*/false,
                                                spill_layout));
  }
}

TEST(SchedModelTest, FprToFprReloadInheritedInputReloadFolding) {
  const CompilationProfile mips1_prof{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
  };
  const MachineModel mm = MachineModel::FromProfile(mips1_prof);

  // Positive (`reload1.c:5186-5257, 5641, 5849-5871, 6118-6126`):
  // `$f8 = $f6` (reload-inherited FPR->FPR copy, no `REG_DEAD $f6`) feeding
  // output-reloaded FP arithmetic `$f8 = $f0 + $f8 ; $a2 = $f8` folds onto the
  // FP arithmetic insn (`input_reload_before_core` maps the copy to the core
  // `add.s` and substitutes `$f6`'s pseudo directly).
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SF 38 $f6) (mem/u:SF (reg:SI 5 a1)))", 0),
        MakeInsn("(set (reg:SF 32 $f0) (mem/u:SF (plus:SI (reg:SI 5 a1) (const_int 4))))", 1),
        MakeInsn("(set (reg:SF 40 $f8) (reg:SF 38 $f6))", 2),
        MakeInsn("(set (reg:SF 40 $f8) (plus:SF (reg:SF 32 $f0) (reg:SF 40 $f8))) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 3),
        MakeInsn("(set (reg:SF 6 a2) (reg:SF 40 $f8))", 4),
        MakeInsn("(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ])",
                 5, Insn::Kind::kCall, {"(use (reg:SF 6 a2))"}),
        MakeInsn("(use (reg/i:SF 32 $f0))", 6),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{32});
    auto it = view.input_reload_before_core.find(2);
    ASSERT_NE(it, view.input_reload_before_core.end());
    EXPECT_EQ(view.core_to_orig_index[static_cast<size_t>(it->second)], 3);
    const RtlExpr& add_pat = view.core_insns[static_cast<size_t>(it->second)].pn.pattern;
    const int f6_pseudo = view.core_insns[0].pn.pattern.SetDest().RegNo();
    ASSERT_EQ(add_pat.op, RtlOp::kSet);
    EXPECT_EQ(add_pat.SetDest().RegNo(), 6);
    EXPECT_TRUE(add_pat.SetSrc().ContainsReg(f6_pseudo)) << FormatPatternAndNotes(add_pat, {});
  }

  // Negative 1: when the FP consumer is NOT output-reloaded (no `mfc1` into
  // `md_output_reload_dest`), an FPR->FPR copy is NOT folded.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SF 38 $f6) (mem/u:SF (reg:SI 5 a1)))", 0),
        MakeInsn("(set (reg:SF 32 $f0) (mem/u:SF (plus:SI (reg:SI 5 a1) (const_int 4))))", 1),
        MakeInsn("(set (reg:SF 40 $f8) (reg:SF 38 $f6))", 2),
        MakeInsn("(set (reg:SF 40 $f8) (plus:SF (reg:SF 32 $f0) (reg:SF 40 $f8))) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 3),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 40 $f8) (reg:SF 38 $f6))) "
                 "(expr_list:REG_DEAD (reg:SF 40 $f8) (expr_list:REG_DEAD (reg:SF 38 $f6) (nil)))",
                 4),
        MakeInsn("(use (reg/i:SF 32 $f0))", 5),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{32});
    EXPECT_FALSE(view.input_reload_before_core.contains(2));
  }

  // Negative 2: if `src_fpr` (`$f6`) is clobbered between the copy and the FP
  // consumer (`clobbered_fprs`), the copy MUST NOT be folded.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SF 38 $f6) (mem/u:SF (reg:SI 5 a1)))", 0),
        MakeInsn("(set (reg:SF 32 $f0) (mem/u:SF (plus:SI (reg:SI 5 a1) (const_int 4))))", 1),
        MakeInsn("(set (reg:SF 40 $f8) (reg:SF 38 $f6))", 2),
        MakeInsn("(set (reg:SF 38 $f6) (mem/u:SF (plus:SI (reg:SI 5 a1) (const_int 8))))", 3),
        MakeInsn("(set (reg:SF 40 $f8) (plus:SF (reg:SF 32 $f0) (reg:SF 40 $f8))) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 4),
        MakeInsn("(set (reg:SF 6 a2) (reg:SF 40 $f8))", 5),
        MakeInsn("(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ])",
                 6, Insn::Kind::kCall, {"(use (reg:SF 6 a2))"}),
        MakeInsn("(use (reg/i:SF 32 $f0))", 7),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{32});
    EXPECT_FALSE(view.input_reload_before_core.contains(2));
  }

  // Negative 3: a copy carrying `(REG_DEAD (reg:SF 38 $f6))` is NOT a
  // `reload1.c` reload-inherited copy and MUST NOT be folded.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SF 38 $f6) (mem/u:SF (reg:SI 5 a1)))", 0),
        MakeInsn("(set (reg:SF 32 $f0) (mem/u:SF (plus:SI (reg:SI 5 a1) (const_int 4))))", 1),
        MakeInsn("(set (reg:SF 40 $f8) (reg:SF 38 $f6)) (expr_list:REG_DEAD (reg:SF 38 $f6) (nil))",
                 2),
        MakeInsn("(set (reg:SF 40 $f8) (plus:SF (reg:SF 32 $f0) (reg:SF 40 $f8))) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 3),
        MakeInsn("(set (reg:SF 6 a2) (reg:SF 40 $f8))", 4),
        MakeInsn("(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ])",
                 5, Insn::Kind::kCall, {"(use (reg:SF 6 a2))"}),
        MakeInsn("(use (reg/i:SF 32 $f0))", 6),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{32});
    EXPECT_FALSE(view.input_reload_before_core.contains(2));
  }
}

TEST(SchedModelTest, PostCallCombineRegsChainSplitsMultiStepCallFreeRmw) {
  const CompilationProfile mips1_prof{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
  };
  const MachineModel mm = MachineModel::FromProfile(mips1_prof);

  // Positive (`local-alloc.c:1266-1300, 1851-1886`, `sched.c:1732`, `flow.c:2506-2515`):
  // Pre-call callee-saved `$s0` crosses `CALL_INSN`, followed by a multi-step
  // call-free post-call `combine_regs` RMW chain (`s0 = s0 ^ s1 -> s0 = s0 + s2
  // -> return s0`). With `allow_post_call_combine_regs_split = true`, the first
  // post-call RMW splits onto a fresh `reg_n_calls_crossed == 0` pseudo.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 16 s0) (mem/u:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 4))))", 1),
        MakeInsn("(set (reg:SI 18 s2) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 8))))", 2),
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ])",
                 3, Insn::Kind::kCall),
        MakeInsn("(set (reg:SI 16 s0) (xor:SI (reg:SI 16 s0) (reg:SI 17 s1))) "
                 "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
                 4),
        MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 18 s2))) "
                 "(expr_list:REG_DEAD (reg:SI 18 s2) (nil))",
                 5),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                 6),
        MakeInsn("(use (reg/i:SI 2 v0))", 7),
    };
    Sched1ProjectionContext ctx;
    ctx.allow_post_call_combine_regs_split = true;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{2}, ctx);
    const int pre_call_s0 = view.core_insns[0].pn.pattern.SetDest().RegNo();
    const int post_call_s0 = view.core_insns[4].pn.pattern.SetDest().RegNo();
    EXPECT_NE(pre_call_s0, post_call_s0);
    const auto pre_it = view.facts_s1.reg_n_calls_crossed.find(pre_call_s0);
    ASSERT_NE(pre_it, view.facts_s1.reg_n_calls_crossed.end());
    EXPECT_EQ(pre_it->second, 1);
    const auto post_it = view.facts_s1.reg_n_calls_crossed.find(post_call_s0);
    EXPECT_EQ(post_it == view.facts_s1.reg_n_calls_crossed.end() ? 0 : post_it->second, 0);
  }

  // Negative: when a second `CALL_INSN` intervenes before the post-call RMW
  // chain dies, `$s0` still crosses a call and MUST NOT be split into a
  // `reg_n_calls_crossed == 0` pseudo.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (reg:SI 16 s0) (mem/u:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 17 s1) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 4))))", 1),
        MakeInsn("(set (reg:SI 18 s2) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 8))))", 2),
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee1\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ])",
                 3, Insn::Kind::kCall),
        MakeInsn("(set (reg:SI 16 s0) (xor:SI (reg:SI 16 s0) (reg:SI 17 s1))) "
                 "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
                 4),
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee2\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ])",
                 5, Insn::Kind::kCall),
        MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 18 s2))) "
                 "(expr_list:REG_DEAD (reg:SI 18 s2) (nil))",
                 6),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                 7),
        MakeInsn("(use (reg/i:SI 2 v0))", 8),
    };
    Sched1ProjectionContext ctx;
    ctx.allow_post_call_combine_regs_split = true;
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(blk, mm, /*live_at_end_s2=*/{2}, ctx);
    const int pre_call_s0 = view.core_insns[0].pn.pattern.SetDest().RegNo();
    const int post_call_s0 = view.core_insns[4].pn.pattern.SetDest().RegNo();
    EXPECT_EQ(pre_call_s0, post_call_s0);
  }
}

TEST(SchedModelTest,
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

TEST(SchedModelTest, FindPostReloadBodyBoundsSkipsVariadicPreSpDecArgHomeStores) {
  // Mirrors `config/mips/mips.c:4996-5020` (`mips_expand_prologue`):
  // pre-`$sp`-decrement variadic GP argument home stores forming a contiguous
  // suffix `[$start..$a3]` preceding a prologue `$sp` decrement are excluded
  // from the `sched1` body so interleaved post-`$sp` callee-save stores are
  // recognized (positive). In a frameless function with no `$sp` decrement
  // (negative 1) or a non-suffix store `$a0` alone (negative 2), `prologue_end`
  // stays at `0`.
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive: `4($sp)=$a1, 8($sp)=$a2, 12($sp)=$a3` before `$sp -= 72`, followed
  // by interleaved `$f20` save, `$f20 = 0`, `$s4` save, `$s4 = $a0`.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 5 a1))", 0),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 8))) (reg:SI 6 a2)) "
                 "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                 1),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 12))) (reg:SI 7 a3)) "
                 "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                 2),
        MakeInsn("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -72)))", 3),
        MakeInsn("(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 64))) (reg:DF 52 $f20))", 4),
        MakeInsn("(set (reg:DF 52 $f20) (const_double:DF (cc0) 0 0))", 5),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 40))) (reg:SI 20 s4))", 6),
        MakeInsn("(set (reg:SI 20 s4) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))", 7),
        MakeInsn("(use (reg:SI 20 s4))", 8),
    };
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(blk, /*block_idx=*/0);
    EXPECT_EQ(bounds.prologue_end, 5);
    const absl::btree_set<int> interleaved =
        FindInterleavedPrologueSaveBodyIndices(blk, bounds, /*block_idx=*/0, mm);
    EXPECT_TRUE(interleaved.contains(1));  // blk[6] (`sw $s4, 40($sp)`) is body rel index 1.
  }

  // Negative 1: frameless function (no `$sp` decrement ahead) must NOT skip
  // caller-home `assign_parms` stores.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 5 a1))", 0),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 8))) (reg:SI 6 a2))", 1),
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 12))) (reg:SI 7 a3))", 2),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 4 a0))", 3),
        MakeInsn("(use (reg/i:SI 2 v0))", 4),
    };
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(blk, /*block_idx=*/0);
    EXPECT_EQ(bounds.prologue_end, 0);
  }

  // Negative 2: non-suffix store `$a0` alone (missing `$a1..$a3`) before `$sp -= 32`.
  {
    const std::vector<Insn> blk = {
        MakeInsn("(set (mem:SI (reg:SI 29 sp)) (reg:SI 4 a0))", 0),
        MakeInsn("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))", 1),
        MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 5 a1))", 2),
        MakeInsn("(use (reg/i:SI 2 v0))", 3),
    };
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(blk, /*block_idx=*/0);
    EXPECT_EQ(bounds.prologue_end, 0);
  }
}

TEST(SchedModelTest, ExitBlockReturnUseLivenessAndUpwardReturnWebsPositiveAndNegative) {
  // Mirrors `flow.c:2065` + `sched.c:2325-2367, 2469-2501` (`birthing_insn_p`):
  // 1. Trailing `(use (reg/i:SI 2 v0))` after an epilogue boundary seeds
  //    `block_epilogue_use_regs` -> `live_at_end_s1` for the exit block (positive),
  //    whereas a void exit block without `(use (reg/i ...))` leaves it empty (negative).
  // 2. An unrelated cross-block `$v0` def in B0 consumed only in B1 (not reaching
  //    B2's `(use (reg/i:SI 2 v0))`) does NOT inflate `block_unrenamed_hard_defs[0][2]`,
  //    while B1's live-out def reaching B2's `(use (reg/i:SI 2 v0))` DOES.
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  auto make_cfg = [](bool with_return_use) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].successors = {2};
    cfg.rpo_blocks[1].instructions = {
        "(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 2))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 3))) "
        "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
    };
    cfg.rpo_blocks[1].SyncRtlInsnsFromInstructions();

    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].instructions = {
        with_return_use ? "(set (mem:SI (reg:SI 5 a1)) (reg:SI 6 a2))"
                        : "(set (mem:SI (reg:SI 5 a1)) (reg:SI 2 v0)) "
                          "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 16)))",
    };
    if (with_return_use) {
      cfg.rpo_blocks[2].instructions.push_back("(use (reg/i:SI 2 v0))");
    }
    cfg.rpo_blocks[2].instructions.push_back("(return)");
    cfg.rpo_blocks[2].SyncRtlInsnsFromInstructions();

    cfg.sideband.basic_block_live_at_start[0] = {4, 5, 6, 29};
    cfg.sideband.basic_block_live_at_start[1] = {2, 5, 6, 29};
    cfg.sideband.basic_block_live_at_start[2] = {2, 5, 6, 29};
    return cfg;
  };

  {
    const RtlControlFlowGraph pos_cfg = make_cfg(/*with_return_use=*/true);
    const CfgSched1Context ctx = ComputeCfgSched1Context(pos_cfg, pos_cfg, mm);
    EXPECT_TRUE(ctx.block_epilogue_use_regs[2].contains(2));
    const Sched1ProjectionContext proj2 = BuildSched1ProjectionContextForBlock(2, ctx);
    EXPECT_TRUE(proj2.epilogue_use_regs.contains(2));
    // B0's `$v0` def dies in B1 and never reaches B2's `(use (reg/i:SI 2 v0))`.
    EXPECT_EQ(ctx.block_unrenamed_hard_defs[0].count(2), 0u);
    // B1's `$v0` def reaches B2's `(use (reg/i:SI 2 v0))`.
    ASSERT_TRUE(ctx.block_unrenamed_hard_defs[1].contains(2));
    EXPECT_EQ(ctx.block_unrenamed_hard_defs[1].at(2), 1);
  }
  {
    const RtlControlFlowGraph neg_cfg = make_cfg(/*with_return_use=*/false);
    const CfgSched1Context ctx = ComputeCfgSched1Context(neg_cfg, neg_cfg, mm);
    EXPECT_TRUE(ctx.block_epilogue_use_regs[2].empty());
    EXPECT_EQ(ctx.block_unrenamed_hard_defs[0].count(2), 0u);
    EXPECT_EQ(ctx.block_unrenamed_hard_defs[1].count(2), 0u);
  }
}

TEST(SchedModelTest, CallCrossingCopyTieExemptionAndLocalArraySpillBoundsPositiveAndNegative) {
  // 1. Mirrors `global.c:927` / `local-alloc.c:2106`: dropping an interference
  //    between call-used `$a2` (6) and live-out call-crossing `$s6` (22) in
  //    `cfg_copy_pairs` is safe when `$s6` is in `stack_layout.cfg_call_crossing_regs`
  //    (positive), and rejected when `$s6` does not cross any call (negative).
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  RegFacts facts_s2;
  facts_s2.max_reg_num = 72;
  {
    const std::vector<Insn> base_blk = {
        MakeInsn("(set (reg:SI 22 s6) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 104))))", 0),
        MakeInsn("(set (reg:SI 21 s5) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))", 1),
    };
    const std::vector<int> cand_unhoist = {1, 0};
    const absl::btree_set<int> live_out = {21, 22};
    const absl::btree_set<std::pair<int, int>> copy_pairs = {{6, 22}};

    PostReloadStackLayout cross_layout;
    cross_layout.valid = true;
    cross_layout.cfg_call_crossing_regs.insert(22);
    EXPECT_TRUE(PreservesPostReloadOrderSafety(base_blk, cand_unhoist, /*prologue_end=*/0,
                                               /*epilogue_begin=*/2, live_out, copy_pairs, facts_s2,
                                               mm, /*allow_pure_reader_reg_dead_transfer=*/false,
                                               cross_layout));

    PostReloadStackLayout leaf_layout;
    leaf_layout.valid = true;
    EXPECT_FALSE(
        PreservesPostReloadOrderSafety(base_blk, cand_unhoist, /*prologue_end=*/0,
                                       /*epilogue_begin=*/2, live_out, copy_pairs, facts_s2, mm,
                                       /*allow_pure_reader_reg_dead_transfer=*/false, leaf_layout));
  }

  // 2. Mirrors `reload1.c:2352-2358` + `function.c:682-704`: a local `s32[]`
  //    stack array occupying both `+0` and `+4` inside `[0, 8)` invalidates
  //    that 8-byte slot so `min_valid_reload_spill_offset >= 8` and
  //    `IsAdjacentReloadContiguityPair` rejects `0($sp)`/`4($sp)` (negative),
  //    while an upper big-endian spill slot at `12($sp)` (`frame_size = 16`)
  //    remains eligible (positive).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -16)))",
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
        "(set (mem:SI (reg:SI 29 sp)) (reg:SI 2 v0))",
        "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 2)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 3 v1))",
        "(set (reg:SI 8 t0) (mem:SI (reg:SI 29 sp)))",
        "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 12))) (reg:SI 8 t0))",
        "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 12))))",
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (reg:SI 10 t2)))",
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 16)))",
        "(use (reg/i:SI 2 v0))",
        "(return)",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    cfg.sideband.basic_block_live_at_start[0] = {4, 29};
    const CfgSched1Context ctx = ComputeCfgSched1Context(cfg, cfg, mm);
    EXPECT_GE(ctx.stack_layout.min_valid_reload_spill_offset, 8);
    EXPECT_LE(ctx.stack_layout.min_valid_reload_spill_offset, 12);

    const Insn arr_def = MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 2)))", 0);
    const Insn arr_store =
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 3 v1))", 1);
    EXPECT_FALSE(IsAdjacentReloadContiguityPair(arr_def, arr_store, mm, ctx.stack_layout));

    const Insn spill_def = MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 3)))", 2);
    const Insn spill_store =
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 12))) (reg:SI 8 t0))", 3);
    EXPECT_TRUE(IsAdjacentReloadContiguityPair(spill_def, spill_store, mm, ctx.stack_layout));
  }

  // 3. Mirrors `reload1.c:657-658, 1445-1446` + `caller-save.c:315-316`:
  //    4-byte aligned caller-save slots at `32($sp)`/`36($sp)` (allocated after
  //    `alter_reg`) do NOT invalidate a lower `alter_reg` reload spill slot at `20($sp)`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -48)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 5 a1)) "
        "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
        "(set (reg:SI 6 a2) (plus:SI (reg:SI 4 a0) (const_int 1)))",
        "(set (reg:SI 7 a3) (plus:SI (reg:SI 4 a0) (const_int 2)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:SI 6 a2))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))) (reg:SI 7 a3))",
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
        "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))))",
        "(set (reg:SI 7 a3) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))))",
        "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 6 a2) (reg:SI 10 t2)))",
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 48)))",
        "(use (reg/i:SI 2 v0))",
        "(return)",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29};
    const CfgSched1Context ctx = ComputeCfgSched1Context(cfg, cfg, mm);
    EXPECT_LE(ctx.stack_layout.min_valid_reload_spill_offset, 20);
  }
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

TEST(SchedModelTest, ScheduleClosestSched1CoreOrderPreservesAchievablePrefixAcrossConflict) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  // Positive (`sched.c:2428` LUID tie-break): a 2-segment block with an
  // already-achievable equal-priority prefix `[0..1]` (`r82 = 10`, `r83 = 20`)
  // before a call barrier and an isolated unachievable suffix conflict after
  // the call. `ScheduleClosestSched1CoreOrder` must continue collecting LUID
  // constraints past the suffix conflict so the achievable prefix order is
  // preserved while the returned schedule is `kSched1`-achievable.
  {
    Sched1ProjectionView view;
    view.core_insns = {
        MakeInsn("(set (reg:SI 82) (const_int 10))", 0),
        MakeInsn("(set (reg:SI 83) (const_int 20))", 1),
        MakeInsn("(set (reg:SI 4 a0) (plus:SI (reg:SI 82) (reg:SI 83)))", 2),
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                 "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                 3),
        MakeInsn("(set (reg:SI 84) (const_int 3))", 4),
        MakeInsn("(set (reg:SI 85) (plus:SI (reg:SI 2 v0) (const_int 1)))", 5),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 85) (reg:SI 84)))", 6),
        MakeInsn("(use (reg/i:SI 2 v0))", 7),
    };
    view.core_insns[3].kind = Insn::Kind::kCall;
    view.core_insns[3].pn.call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
    };
    view.facts_s1.max_reg_num = 90;
    view.facts_s1.reg_n_sets = {{4, 1}, {2, 2}, {82, 1}, {83, 1}, {84, 1}, {85, 1}};
    view.live_at_end_s1 = {2};

    const std::vector<int> order = ScheduleClosestSched1CoreOrder(view, mm);
    ASSERT_EQ(order.size(), view.core_insns.size());
    std::vector<Insn> reordered;
    reordered.reserve(order.size());
    for (int pos = 0; pos < static_cast<int>(order.size()); ++pos) {
      Insn copy = view.core_insns[static_cast<size_t>(order[static_cast<size_t>(pos)])];
      copy.luid = pos;
      reordered.push_back(std::move(copy));
    }
    EXPECT_TRUE(IsAchievable(reordered, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1));
    // Achievable equal-priority prefix `[0] r82 = 10` before `[1] r83 = 20` is preserved.
    int pos0 = -1;
    int pos1 = -1;
    for (int pos = 0; pos < static_cast<int>(order.size()); ++pos) {
      if (order[static_cast<size_t>(pos)] == 0) pos0 = pos;
      if (order[static_cast<size_t>(pos)] == 1) pos1 = pos;
    }
    EXPECT_LT(pos0, pos1);
  }

  // Negative / boundary: empty and single-instruction views return trivial permutations.
  {
    Sched1ProjectionView empty_view;
    EXPECT_TRUE(ScheduleClosestSched1CoreOrder(empty_view, mm).empty());

    Sched1ProjectionView single_view;
    single_view.core_insns = {MakeInsn("(use (reg/i:SI 2 v0))", 0)};
    single_view.facts_s1.max_reg_num = 82;
    EXPECT_EQ(ScheduleClosestSched1CoreOrder(single_view, mm), (std::vector<int>{0}));
  }
}

TEST(SchedModelTest,
     ProjectPostReloadBlockToSched1ViewPreservesAndHoistsBlock0DirectSpilledEntryParamCopy) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(prof);

  auto make_ctx = [](bool is_entry_block) {
    Sched1ProjectionContext ctx;
    ctx.has_cfg_context = true;
    ctx.is_entry_block = is_entry_block;
    ctx.has_stack_addr_escape = false;
    ctx.stack_layout.valid = true;
    ctx.stack_layout.frame_pointer_needed = false;
    ctx.stack_layout.starting_frame_offset = 16;
    ctx.stack_layout.frame_size = 64;
    ctx.stack_layout.min_valid_reload_spill_offset = 16;
    ctx.other_block_spill_load_offsets.insert(20);
    return ctx;
  };

  auto make_body = [](bool clobber_a1_first) {
    std::vector<Insn> body;
    int luid = 0;
    body.push_back(MakeInsn("(set (reg:SI 17 s1) (mem/u:SI (reg:SI 4 a0)))", luid++));
    if (clobber_a1_first) {
      body.push_back(MakeInsn("(set (reg:SI 5 a1) (const_int 7))", luid++));
    }
    body.push_back(
        MakeInsn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 5 a1)) "
                 "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                 luid++));
    body.push_back(
        MakeInsn("(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))", luid++));
    body.push_back(MakeInsn("(set (reg:SI 16 s0) (const_int 0))", luid++));
    body.push_back(MakeInsn("(set (reg:SI 5 a1) (const_int 0))", luid++));
    body.push_back(MakeInsn(
        "(set (pc) (if_then_else (le:SI (reg:SI 10 t2) (const_int 0)) (label_ref L1) (pc))) "
        "(expr_list:REG_DEAD (reg:SI 10 t2) (nil))",
        luid++));
    body.back().kind = Insn::Kind::kJump;
    return body;
  };

  // Positive (`function.c:assign_parms` + `reload1.c:2309-2400` `alter_reg`):
  // in block 0, `(set (mem:SI [sp+20]) (reg:SI 5 a1))` with no prior producer
  // in B0 is preserved as a core `(set (reg:SI P_slot) (reg:SI 5 a1))` entry
  // copy, its intra-block reload load `(set t2 [sp+20])` folds into `blez P_slot`,
  // `P_slot` is live-out, and `CollectDisplacedBlock0EntryArgCopyBodyIndices`
  // detects it as a displaced entry-parameter copy.
  {
    const std::vector<Insn> body = make_body(/*clobber_a1_first=*/false);
    const Sched1ProjectionContext ctx = make_ctx(/*is_entry_block=*/true);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{5, 16, 17}, ctx);
    // 6 insns minus 1 folded input reload `lw t2, 20(sp)` = 5 core insns.
    ASSERT_EQ(view.core_insns.size(), 5u);
    const RtlExpr& spill_copy = view.core_insns[1].pn.pattern;
    ASSERT_EQ(spill_copy.op, RtlOp::kSet);
    ASSERT_EQ(spill_copy.SetDest().op, RtlOp::kReg);
    const int p_slot = spill_copy.SetDest().RegNo();
    EXPECT_GE(p_slot, 68);
    ASSERT_EQ(spill_copy.SetSrc().op, RtlOp::kReg);
    EXPECT_EQ(spill_copy.SetSrc().RegNo(), 5);
    EXPECT_TRUE(view.live_at_end_s1.contains(p_slot));
    EXPECT_NE(view.core_insns.back().pn.pattern.ToSexprString().find("(reg:SI " +
                                                                     std::to_string(p_slot) + ")"),
              std::string::npos);

    const std::vector<int> displaced = CollectDisplacedBlock0EntryArgCopyBodyIndices(
        body, view, /*excluded_prologue_body_indices=*/{}, mm, /*frame_pointer_needed=*/false);
    EXPECT_EQ(displaced, (std::vector<int>{1}));
  }

  // Negative 1: outside block 0 (`is_entry_block = false`), the store is NOT
  // preserved as a direct spilled entry-parameter copy.
  {
    const std::vector<Insn> body = make_body(/*clobber_a1_first=*/false);
    const Sched1ProjectionContext ctx = make_ctx(/*is_entry_block=*/false);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{5, 16, 17}, ctx);
    for (const Insn& ci : view.core_insns) {
      if (ci.pn.pattern.op == RtlOp::kSet && ci.pn.pattern.SetDest().op == RtlOp::kReg &&
          ci.pn.pattern.SetSrc().op == RtlOp::kReg) {
        EXPECT_NE(ci.pn.pattern.SetSrc().RegNo(), 5);
      }
    }
    const std::vector<int> displaced = CollectDisplacedBlock0EntryArgCopyBodyIndices(
        body, view, /*excluded_prologue_body_indices=*/{}, mm, /*frame_pointer_needed=*/false);
    EXPECT_TRUE(displaced.empty());
  }

  // Negative 2: when `$a1` was overwritten earlier in block 0 (`(set a1 7)`),
  // the store folds onto that producer rather than acting as an entry-param copy.
  {
    const std::vector<Insn> body = make_body(/*clobber_a1_first=*/true);
    const Sched1ProjectionContext ctx = make_ctx(/*is_entry_block=*/true);
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{5, 16, 17}, ctx);
    const std::vector<int> displaced = CollectDisplacedBlock0EntryArgCopyBodyIndices(
        body, view, /*excluded_prologue_body_indices=*/{}, mm, /*frame_pointer_needed=*/false);
    EXPECT_TRUE(displaced.empty());
  }
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesSynthesizesB0Jump2DeletedPreCallArgCopy) {
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

TEST(SchedModelTest, AttributeSched2ErasedMemAliasFlagsPositiveAndNegative) {
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

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesStripsStaleSunkStoreRegEquiv) {
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

TEST(SchedModelTest, AttributeSched2ErasedMemAliasFlagsSuppressesDerivedPtrFromWrittenBase) {
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

TEST(SchedModelTest, AttributeSched2ErasedMemAliasFlagsSuppressesCrossBlockWrittenOrEscapedSymbol) {
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

TEST(SchedModelTest, EarlyclobberDiOutputReloadFoldsInSched1ViewAndAllowsReuniting) {
  // Mirrors `reload1.c:5599-5665` (`emit_reload_insns`) + `mips.md:407-411`
  // (`adddi3_internal_1` `"=d,&d"` / `"0,d"`): when `adddi3` writes an
  // earlyclobber reload reg `$t8` (`24`) with RHS reading `$v0` (`2`) followed
  // by `(set (reg/i:DI 2 v0) (reg:DI 24 t8))` where `$t8` dies,
  // `ProjectPostReloadBlockToSched1View` folds the pair into one pre-reload
  // insn and `PreservesPostReloadOrderSafety` exempts the reunited `(2, 24)`
  // copy-pair interference drop.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  auto make_insns = [](bool lhs_is_orig, bool extra_t8_read) {
    const std::string add_pat =
        lhs_is_orig
            ? "(parallel[ (set (reg:DI 24 t8) (plus:DI (reg:DI 2 v0) (reg:DI 10 t2))) "
              "(clobber (reg:SI 4 a0)) ] ) (expr_list:REG_DEAD (reg:DI 10 t2) "
              "(expr_list:REG_DEAD (reg:DI 2 v0) (expr_list:REG_UNUSED (reg:SI 4 a0) (nil))))"
            : "(parallel[ (set (reg:DI 24 t8) (plus:DI (reg:DI 10 t2) (reg:DI 2 v0))) "
              "(clobber (reg:SI 4 a0)) ] ) (expr_list:REG_DEAD (reg:DI 10 t2) "
              "(expr_list:REG_DEAD (reg:DI 2 v0) (expr_list:REG_UNUSED (reg:SI 4 a0) (nil))))";
    std::vector<Insn> seq = {
        MakeInsn("(set (reg:DI 10 t2) (mem:DI (reg:SI 4 a0))) "
                 "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                 0),
        MakeInsn("(set (reg:DI 2 v0) (mem:DI (reg:SI 5 a1))) "
                 "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                 1),
        MakeInsn(add_pat, 2),
        MakeInsn("(set (mem:DI (reg:SI 6 a2)) (reg:DI 2 v0)) "
                 "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                 3),
        MakeInsn("(set (reg/i:DI 2 v0) (reg:DI 24 t8))", 4),
    };
    if (extra_t8_read) {
      seq.push_back(
          MakeInsn("(set (mem:DI (plus:SI (reg:SI 6 a2) (const_int 8))) (reg:DI 24 t8))", 5));
    }
    seq.push_back(MakeInsn("(use (reg/i:DI 2 v0))", static_cast<int>(seq.size())));
    return seq;
  };

  // Positive: reuniting `[0, 1, 3, 2, 4, 5]` folds `(2, 4)` into one core insn
  // and passes `PreservesPostReloadOrderSafety` despite `cfg_copy_pairs` `(2, 24)`.
  {
    const std::vector<Insn> base = make_insns(/*lhs_is_orig=*/false, /*extra_t8_read=*/false);
    const std::vector<int> reunited_order = {0, 1, 3, 2, 4, 5};
    std::vector<Insn> reunited;
    for (int pos = 0; pos < static_cast<int>(reunited_order.size()); ++pos) {
      Insn insn = base[static_cast<size_t>(reunited_order[static_cast<size_t>(pos)])];
      insn.luid = pos;
      reunited.push_back(std::move(insn));
    }
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(reunited, mm, {}, ctx);
    EXPECT_EQ(view.core_insns.size(), 5u);
    const absl::btree_set<std::pair<int, int>> copy_pairs = {{2, 24}, {3, 25}};
    RegFacts facts_s2;
    EXPECT_TRUE(PreservesPostReloadOrderSafety(base, reunited_order, /*prologue_end=*/0,
                                               /*epilogue_begin=*/6, {}, copy_pairs, facts_s2, mm,
                                               /*allow_pure_reader_reg_dead_transfer=*/false));
  }

  // Negative 1: when `t8` is still read after `(set v0 t8)`, it is not a pure
  // earlyclobber output-reload scratch and must not fold in `sched1` view.
  {
    const std::vector<Insn> base = make_insns(/*lhs_is_orig=*/false, /*extra_t8_read=*/true);
    const std::vector<int> reunited_order = {0, 1, 3, 2, 4, 5, 6};
    std::vector<Insn> reunited;
    for (int pos = 0; pos < static_cast<int>(reunited_order.size()); ++pos) {
      Insn insn = base[static_cast<size_t>(reunited_order[static_cast<size_t>(pos)])];
      insn.luid = pos;
      reunited.push_back(std::move(insn));
    }
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(reunited, mm, {}, ctx);
    EXPECT_EQ(view.core_insns.size(), 7u);
  }

  // Negative 2: when `orig_hr` is LHS of `adddi3_internal_1` (`"0"` alternative
  // matches without earlyclobber), the pair must neither fold nor exempt the
  // dropped `(2, 24)` copy-pair interference in `PreservesPostReloadOrderSafety`.
  {
    const std::vector<Insn> base = make_insns(/*lhs_is_orig=*/true, /*extra_t8_read=*/false);
    const std::vector<int> reunited_order = {0, 1, 3, 2, 4, 5};
    std::vector<Insn> reunited;
    for (int pos = 0; pos < static_cast<int>(reunited_order.size()); ++pos) {
      Insn insn = base[static_cast<size_t>(reunited_order[static_cast<size_t>(pos)])];
      insn.luid = pos;
      reunited.push_back(std::move(insn));
    }
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(reunited, mm, {}, ctx);
    EXPECT_EQ(view.core_insns.size(), 6u);
    const absl::btree_set<std::pair<int, int>> copy_pairs = {{2, 24}, {3, 25}};
    RegFacts facts_s2;
    EXPECT_FALSE(PreservesPostReloadOrderSafety(base, reunited_order, /*prologue_end=*/0,
                                                /*epilogue_begin=*/6, {}, copy_pairs, facts_s2, mm,
                                                /*allow_pure_reader_reg_dead_transfer=*/false));
  }
}

TEST(SchedModelTest, ProjectSched1ViewRenamesIndirectCallAddrAndPreservesNoConflictCrossCall) {
  // Mirrors `calls.c:emit_call_1`, `optabs.c:2485-2525` (`emit_no_conflict_block`),
  // and `local-alloc.c:540-665, 990-1000, 1266-1300`:
  // 1. An indirect `CALL_INSN` callee address `(call (mem:SI (reg:SI 3 v1)) ...)`
  //    renames to the producer's pseudo while keeping the call's return dest `$v0`
  //    physical.
  // 2. A callee-saved copy `(set (reg:SI 16 s0) (reg:SI 2 v0))` crossing a call
  //    into a `REG_LIBCALL`/`REG_NO_CONFLICT` `extendsidi2` stays lifted as a pseudo.
  // 3. A post-call `combine_regs` RMW carrying `REG_EQUIV (mem ...)` splits cleanly,
  //    whereas `REG_EQUIV (const_double ...)` does not.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  auto make_body = [](bool with_no_conflict_cross_call, bool rmw_mem_equiv) {
    std::vector<Insn> body;
    int luid = 0;
    body.push_back(MakeInsn(
        "(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))", luid++));
    body.push_back(MakeInsn(
        "(set (reg:SF 52 $f20) (reg:SF 7 a3)) (expr_list:REG_DEAD (reg:SF 7 a3) (nil))", luid++));
    body.push_back(MakeInsn("(set (reg:SF 54 $f22) (const_double:SF (cc0) 0 1072693248))", luid++));
    body.push_back(
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"HelperA\"))) "
                 "(const_int 16))) "
                 "(clobber (reg:SI 31 ra)) ] )",
                 luid++, Insn::Kind::kCall));
    body.push_back(MakeInsn(
        "(set (reg:SI 3 v1) (mem:SI (reg:SI 16 s0))) (expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
        luid++));
    body.push_back(MakeInsn(
        "(set (reg:SI 16 s0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))", luid++));
    body.push_back(MakeInsn("(set (reg:SI 4 a0) (plus:SI (reg:SI 16 s0) (const_int 1)))", luid++));
    body.push_back(
        MakeInsn("(parallel[ (set (reg:SI 2 v0) (call (mem:SI (reg:SI 3 v1)) (const_int 16))) "
                 "(clobber (reg:SI 31 ra)) ] )",
                 luid++, Insn::Kind::kCall, {"(use (reg:SI 4 a0))"}));
    if (with_no_conflict_cross_call) {
      body.push_back(MakeInsn("(clobber (reg:DI 16 s0)) (insn_list:REG_LIBCALL (nil))", luid++));
      body.push_back(
          MakeInsn("(set (subreg:SI (reg:DI 16 s0) 1) (reg:SI 16 s0)) "
                   "(expr_list:REG_NO_CONFLICT (reg:SI 16 s0) (nil))",
                   luid++));
      body.push_back(MakeInsn(
          "(set (subreg:SI (reg:DI 16 s0) 0) (ashiftrt:SI (reg:SI 16 s0) (const_int 31))) "
          "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_NO_CONFLICT (reg:SI 16 s0) (nil)))",
          luid++));
      body.push_back(
          MakeInsn("(set (reg/i:DI 2 v0) (reg:DI 16 s0)) (expr_list:REG_DEAD (reg:DI 16 s0) (nil))",
                   luid++));
    } else {
      body.push_back(MakeInsn("(set (reg/i:SI 2 v0) (reg:SI 2 v0))", luid++));
    }
    const std::string rmw_str =
        rmw_mem_equiv ? "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22))) "
                        "(expr_list:REG_EQUIV (mem:SF (reg:SI 18 s2)) "
                        "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil)))"
                      : "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22))) "
                        "(expr_list:REG_EQUIV (const_double:SF (cc0) 0 1072693248) "
                        "(expr_list:REG_DEAD (reg:SF 54 $f22) (nil)))";
    body.push_back(MakeInsn(rmw_str, luid++));
    body.push_back(
        MakeInsn("(set (mem:SF (reg:SI 18 s2)) (reg:SF 52 $f20)) "
                 "(expr_list:REG_DEAD (reg:SF 52 $f20) (nil))",
                 luid++));
    return body;
  };

  auto find_core_for_orig = [](const Sched1ProjectionView& view, int orig) -> int {
    for (size_t ci = 0; ci < view.core_to_orig_index.size(); ++ci) {
      if (view.core_to_orig_index[ci] == orig) {
        return static_cast<int>(ci);
      }
    }
    return -1;
  };

  // Positive: indirect call address renamed to pseudo (`>= 68`), return dest stays
  // `$v0`, `s0 = v0` lifted to pseudo (`>= 68`), and `REG_EQUIV (mem)` RMW splits.
  {
    const std::vector<Insn> body =
        make_body(/*with_no_conflict_cross_call=*/true, /*rmw_mem_equiv=*/true);
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    ctx.allow_post_call_combine_regs_split = true;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(body, mm, {}, ctx);
    const int ci_load = find_core_for_orig(view, 4);
    const int ci_s0 = find_core_for_orig(view, 5);
    const int ci_call = find_core_for_orig(view, 7);
    ASSERT_GE(ci_load, 0);
    ASSERT_GE(ci_s0, 0);
    ASSERT_GE(ci_call, 0);
    const int fn_ptr_pseudo = view.core_insns[ci_load].pn.pattern.SetDest().RegNo();
    EXPECT_GE(fn_ptr_pseudo, kMipsFirstPseudoRegister);
    const RtlExpr& call_set = view.core_insns[ci_call].pn.pattern.children[0];
    EXPECT_EQ(call_set.SetDest().RegNo(), 2);
    EXPECT_TRUE(call_set.SetSrc().ContainsReg(fn_ptr_pseudo));
    EXPECT_GE(view.core_insns[ci_s0].pn.pattern.SetDest().RegNo(), kMipsFirstPseudoRegister);
    EXPECT_EQ(view.implicit_combine_regs_split_orig_indices.size(), 1u);
  }

  // Negative: without post-call `REG_NO_CONFLICT`, `s0 = v0` has a local dying
  // `$v0` suggestion conflict (stays hard reg `16`), and `REG_EQUIV (const_double)`
  // RMW does not split.
  {
    const std::vector<Insn> body =
        make_body(/*with_no_conflict_cross_call=*/false, /*rmw_mem_equiv=*/false);
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    ctx.allow_post_call_combine_regs_split = true;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(body, mm, {}, ctx);
    const int ci_s0 = find_core_for_orig(view, 5);
    ASSERT_GE(ci_s0, 0);
    EXPECT_EQ(view.core_insns[ci_s0].pn.pattern.SetDest().RegNo(), 16);
    EXPECT_TRUE(view.implicit_combine_regs_split_orig_indices.empty());
  }
}

TEST(SchedModelTest, CombineRegsRmwSplitRejectsSubwordAccumulationOntoWiderDiModeBirthDef) {
  // Mirrors `flow.c:2506-2515` + `sched.c:2872-2906`: an `SImode` high-word
  // RMW accumulation (`(set (reg:SI 18 s2) (plus:SI (reg:SI 18 s2) ...))`) onto
  // a wider `DImode` birth def (`(set (reg:DI 18 s2) (mult:DI ...))`) cannot be
  // split into a fresh pseudo by `ProjectPostReloadBlockToSched1View` without
  // creating an illegal self-set `(REG_DEAD (reg:SI 18 s2))` in Stage 4,
  // whereas a same-mode `SImode` RMW chain still splits normally.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  auto run_projection = [&](bool wider_di_birth) {
    std::vector<Insn> body;
    int luid = 0;
    if (wider_di_birth) {
      body.push_back(MakeInsn(
          "(parallel[ (set (reg:DI 18 s2) (mult:DI (zero_extend:DI (reg:SI 4 a0)) "
          "(zero_extend:DI (reg:SI 5 a1)))) (clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) ] )",
          luid++));
    } else {
      body.push_back(
          MakeInsn("(set (reg:SI 18 s2) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))", luid++));
    }
    body.push_back(MakeInsn("(set (reg:SI 18 s2) (plus:SI (reg:SI 18 s2) (reg:SI 6 a2)))", luid++));
    body.push_back(MakeInsn(
        "(set (reg/i:SI 2 v0) (reg:SI 18 s2)) (expr_list:REG_DEAD (reg:SI 18 s2) (nil))", luid++));
    body.push_back(MakeInsn("(use (reg/i:SI 2 v0))", luid++));

    Sched1ProjectionContext ctx;
    ctx.is_entry_block = true;
    ctx.allow_general_combine_regs_split = true;
    return ProjectPostReloadBlockToSched1View(body, mm, {}, ctx);
  };

  // Positive: same-mode `SImode` RMW chain splits index 1 into a fresh pseudo.
  {
    const Sched1ProjectionView view = run_projection(/*wider_di_birth=*/false);
    EXPECT_EQ(view.implicit_combine_regs_split_orig_indices.size(), 1u);
    ASSERT_GE(view.core_insns.size(), 2u);
    EXPECT_NE(view.core_insns[0].pn.pattern.SetDest().RegNo(),
              view.core_insns[1].pn.pattern.SetDest().RegNo());
  }

  // Negative: subword `SImode` RMW onto a wider `DImode` birth def does NOT split.
  {
    const Sched1ProjectionView view = run_projection(/*wider_di_birth=*/true);
    EXPECT_TRUE(view.implicit_combine_regs_split_orig_indices.empty());
  }
}

TEST(SchedModelTest, PreservesPostReloadOrderSafetyStoreEquivInvalidatorMovedIntoWindow) {
  // Mirrors `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`,
  // `memref_used_between_p`, `update_equiv_regs`): when `sched2` hoists an
  // address-base update (`addu $a2, $a2, 4`) ahead of a store-equiv producer
  // (`add.s $f2` -> `s.s $f2, 0($a2)`), the post-`sched2` order carries a
  // spurious `(REG_EQUIV (mem:SF (reg:SI 6 a2)))` while the pre-`sched2`
  // candidate moves the base update back into `[producer, store]`. With
  // `allow_pure_reader_reg_dead_transfer = true` (Stage 0) this is accepted so
  // Stage 0 can strip the spurious note; with `false` (Stage 3, after Stage 0
  // has normalized notes) or when no invalidator enters the window, strict
  // equiv-validity preservation rejects the discrepancy.
  const CompilationProfile o2_mips1{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-funsigned-char"}};
  const MachineModel mm = MachineModel::FromProfile(o2_mips1);
  const RegFacts facts_s2;

  // Post-`sched2` order:
  //   [0] `addu $a2, $a2, 4`    (hoisted base update)
  //   [1] `add.s $f2, $f4, $f6` with `(REG_EQUIV (mem:SF (reg:SI 6 a2)))`
  //   [2] `s.s $f2, 0($a2)`     with `(REG_DEAD $f2)`
  std::vector<Insn> base = {
      MakeInsn("(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 4)))", 0),
      MakeInsn("(set (reg:SF 34 $f2) (plus:SF (reg:SF 36 $f4) (reg:SF 38 $f6))) "
               "(expr_list:REG_EQUIV (mem:SF (reg:SI 6 a2)) (nil))",
               1),
      MakeInsn("(set (mem:SF (reg:SI 6 a2)) (reg:SF 34 $f2)) "
               "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
               2),
  };
  // Candidate pre-`sched2` order `[1, 0, 2]` moves `[0]` (`addu $a2`) into the
  // `[producer, store]` window of `$f2`, invalidating `REG_EQUIV (mem:SF a2)`.
  const std::vector<int> cand_moves_invalidator_in = {1, 0, 2};

  // Positive (Stage 0 `allow_transfer = true`): accepted because moving the base
  // update into the `[producer, store]` window explains why post-`sched2` had a
  // spurious store `REG_EQUIV` that Stage 0 will strip.
  EXPECT_TRUE(PreservesPostReloadOrderSafety(base, cand_moves_invalidator_in, 0, 3, {6}, {},
                                             facts_s2, mm,
                                             /*allow_pure_reader_reg_dead_transfer=*/true));

  // Negative (Stage 3 `allow_transfer = false`): rejected because Stage 3 does
  // not strip `REG_EQUIV` notes and must preserve `mem_equiv_valid`.
  EXPECT_FALSE(PreservesPostReloadOrderSafety(base, cand_moves_invalidator_in, 0, 3, {6}, {},
                                              facts_s2, mm,
                                              /*allow_pure_reader_reg_dead_transfer=*/false));

  // Negative 2: when the hoisted insn does NOT invalidate the store-equiv mem
  // (writes unrelated `$a3`, not `$a2`), `REG_EQUIV (mem:SF a2)` remains valid
  // in both orders (`bv && cv`), so both modes accept `[1, 0, 2]`.
  std::vector<Insn> unrelated_base = base;
  unrelated_base[0] = MakeInsn("(set (reg:SI 7 a3) (plus:SI (reg:SI 7 a3) (const_int 4)))", 0);
  EXPECT_TRUE(PreservesPostReloadOrderSafety(unrelated_base, cand_moves_invalidator_in, 0, 3,
                                             {6, 7}, {}, facts_s2, mm,
                                             /*allow_pure_reader_reg_dead_transfer=*/false));
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewMapsProvenIntraBlockSpillWithoutCall) {
  // Mirrors `reload1.c:2309-2400, 5641-6160`: an intra-block reload spill pair
  // `(sw $f0, 28($sp))` ... clobber of `$f0` ... `(lw $f2, 28($sp))` (or with
  // `(REG_DEAD (mem:SF [sp+28]))`) without any intervening `CALL_INSN` must be
  // mapped onto a shared spill pseudo `P_slot` in `ProjectPostReloadBlockToSched1View`,
  // whereas an adjacent leaf stack copy with no intervening clobber or `MEM`
  // death must remain explicit memory instructions.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  Sched1ProjectionContext ctx;
  ctx.is_entry_block = false;
  ctx.stack_layout.valid = true;
  ctx.stack_layout.frame_size = 64;
  ctx.stack_layout.starting_frame_offset = 16;
  ctx.stack_layout.min_valid_reload_spill_offset = 16;

  // Positive: `$f0` is spilled to `28($sp)`, clobbered by a new computation,
  // and reloaded into `$f2` for a consumer carrying `(REG_DEAD (mem [sp+28]))`.
  {
    std::vector<Insn> spill_body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))", 1),
        MakeInsn("(set (reg:SF 32 $f0) (mult:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 2),
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))", 3),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
                 "(expr_list:REG_DEAD (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (nil))",
                 4),
    };
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(spill_body, mm, {32}, ctx);
    // Spill store `[1]` and reload `[3]` are folded into the shared pseudo `P_slot`.
    EXPECT_EQ(view.core_insns.size(), 3u);
    const int p_spill = view.core_insns[0].pn.pattern.SetDest().RegNo();
    EXPECT_GE(p_spill, kMipsFirstPseudoRegister);
    const RtlExpr& final_rhs = view.core_insns[2].pn.pattern.SetSrc();
    ASSERT_EQ(final_rhs.children.size(), 2u);
    EXPECT_EQ(final_rhs.children[1].RegNo(), p_spill);
  }

  // Negative: adjacent leaf stack store + load (`$f0` -> `28($sp)` -> `$f2`)
  // with NO intervening clobber of `$f0` and NO `(REG_DEAD (mem ...))` is NOT
  // inserted into `intra_reload_spill_{store,load}_indices`, so `core[0]`'s def
  // is not remapped onto `core[1]`'s read of `$f2`.
  {
    std::vector<Insn> leaf_body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))", 1),
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))", 2),
        MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2)))", 3),
    };
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(leaf_body, mm, {32}, ctx);
    ASSERT_EQ(view.core_insns.size(), 2u);
    const int def_p = view.core_insns[0].pn.pattern.SetDest().RegNo();
    const RtlExpr& final_rhs = view.core_insns[1].pn.pattern.SetSrc();
    ASSERT_EQ(final_rhs.children.size(), 2u);
    EXPECT_NE(final_rhs.children[1].RegNo(), def_p);
  }
}

TEST(SchedModelTest, PreservesPostReloadOrderSafetyRejectsDroppedPerLiveRangeLocalInterference) {
  // Mirrors `local-alloc.c:2158-2182` (`find_free_reg` ascending regno scan):
  // when a standalone non-RMW local live range of `hi` (`$f2`) straddles an
  // independent ordinary birth of lower same-class `lo` (`$f0`), moving `lo`'s
  // birth past `hi`'s death drops that per-live-range `($f0, $f2)` interference
  // (even when `($f0, $f2)` still overlaps in a separate later live range in the
  // same block!). Conversely, a non-straddling move (where `lo` already died
  // before `hi` or was born before `hi`) is allowed.
  const CompilationProfile o2_mips1{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-funsigned-char"}};
  const MachineModel mm = MachineModel::FromProfile(o2_mips1);
  const RegFacts facts_s2;

  // First live range of `$f2` (`[0]..[2]`) straddles `$f0`'s birth at `[1]`
  // (which stays live until `[3]`). A second independent overlap of `($f0, $f2)`
  // exists at `[4]..[6]` so whole-block `interferences` still contains `(32, 34)`.
  std::vector<Insn> base = {
      MakeInsn("(set (reg:SF 34 $f2) (mem:SF (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1048576000))", 1),
      MakeInsn("(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 34 $f2))) "
               "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
               2),
      MakeInsn("(set (reg:SF 54 $f22) (mult:SF (reg:SF 54 $f22) (reg:SF 32 $f0))) "
               "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
               3),
      MakeInsn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))))", 4),
      MakeInsn("(set (reg:SF 32 $f0) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 8))))", 5),
      MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
               "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
               6),
  };

  // Positive: moving `[1]` (`$f0 = 0.25`) after `[2]` drops the per-live-range
  // `($f0, $f2)` interference on `$f2`'s first live range `[0]..[2]`, so
  // `PreservesPostReloadOrderSafety` rejects `[0, 2, 1, 3, 4, 5, 6]`.
  const std::vector<int> drops_first_range_interference = {0, 2, 1, 3, 4, 5, 6};
  EXPECT_FALSE(PreservesPostReloadOrderSafety(base, drops_first_range_interference, 0, 7,
                                              {32, 52, 54}, {}, facts_s2, mm));

  // Negative: swapping `[4]` and `[5]` (where `$f0` and `$f2` both stay live
  // into `[6]` — neither drops a straddling `lo` born strictly inside `hi`) is
  // accepted.
  const std::vector<int> preserves_interference = {0, 1, 2, 3, 5, 4, 6};
  EXPECT_TRUE(PreservesPostReloadOrderSafety(base, preserves_interference, 0, 7, {32, 52, 54}, {},
                                             facts_s2, mm));

  // Negative 2: when a lower allocatable same-class register (`$v1 = 3`) is
  // free throughout `hi = $t1 (9)`'s live range, `find_free_reg` was never
  // constrained to `9` by `lo = $t0 (8)`, so dropping `(8, 9)` is accepted.
  std::vector<Insn> free_lower_gpr = {
      MakeInsn("(set (reg:SI 9 t1) (plus:SI (reg:SI 6 a2) (const_int 3)))", 0),
      MakeInsn("(set (reg:SI 8 t0) (ashift:SI (reg:SI 4 a0) (const_int 1)))", 1),
      MakeInsn("(set (mem:SI (symbol_ref:SI (\"g_flag\"))) (reg:SI 8 t0))", 2),
      MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 9 t1))) "
               "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
               3),
  };
  const std::vector<int> move_t1_before_t0 = {0, 3, 1, 2};
  EXPECT_TRUE(PreservesPostReloadOrderSafety(free_lower_gpr, move_t1_before_t0, 0, 4, {2, 8}, {},
                                             facts_s2, mm));

  // Negative 3: an RMW continuation `(set $f0 (plus $f0 $f4))` of an already-live
  // `$f0` inside `$f2`'s live range is NOT a fresh straddling birth of `$f0`
  // inside `$f2`, so moving it past `$f2`'s death is accepted.
  std::vector<Insn> rmw_continuation = {
      MakeInsn("(set (reg:SF 32 $f0) (mem:SF (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SF 34 $f2) (const_double:SF (cc0) 0 1056964608))", 1),
      MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4)))", 2),
      MakeInsn("(set (reg:SF 52 $f20) (mult:SF (reg:SF 52 $f20) (reg:SF 34 $f2))) "
               "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
               3),
      MakeInsn("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 52 $f20))) "
               "(expr_list:REG_DEAD (reg:SF 52 $f20) (nil))",
               4),
  };
  const std::vector<int> defer_rmw_after_f2_death = {0, 1, 3, 2, 4};
  EXPECT_TRUE(PreservesPostReloadOrderSafety(rmw_continuation, defer_rmw_after_f2_death, 0, 5, {32},
                                             {}, facts_s2, mm));
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewCallUsedOnlyCombineRegsRmwSplits) {
  // Mirrors `local-alloc.c:1266-1300, 1851-1886, 2106`: setting
  // `allow_call_used_general_combine_regs_split = true` splits call-used RMWs
  // (`$f12`) without splitting callee-saved multi-set accumulators (`$f20`),
  // whereas leaving both general split flags false splits neither.
  const CompilationProfile o2_mips1{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-funsigned-char"}};
  const MachineModel mm = MachineModel::FromProfile(o2_mips1);

  std::vector<Insn> body = {
      MakeInsn("(set (reg:SF 44 $f12) (mem:SF (reg:SI 4 a0)))", 0),
      MakeInsn("(set (reg:SF 52 $f20) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))))", 1),
      MakeInsn("(set (reg:SF 44 $f12) (plus:SF (reg:SF 44 $f12) (reg:SF 52 $f20)))", 2),
      MakeInsn("(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 44 $f12))) "
               "(expr_list:REG_DEAD (reg:SF 44 $f12) (nil))",
               3),
      MakeInsn("(set (reg/i:SF 32 $f0) (reg:SF 52 $f20)) "
               "(expr_list:REG_DEAD (reg:SF 52 $f20) (nil))",
               4),
  };

  // Positive: `allow_call_used_general_combine_regs_split = true` splits only
  // call-used `$f12` at index `2`, leaving callee-saved `$f20` at index `3`
  // coalesced as a single multi-set pseudo.
  {
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = false;
    ctx.allow_call_used_general_combine_regs_split = true;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(body, mm, {32}, ctx);
    EXPECT_EQ(view.implicit_combine_regs_split_orig_indices, (std::vector<int>{2}));
    EXPECT_NE(view.core_insns[0].pn.pattern.SetDest().RegNo(),
              view.core_insns[2].pn.pattern.SetDest().RegNo());
    EXPECT_EQ(view.core_insns[1].pn.pattern.SetDest().RegNo(),
              view.core_insns[3].pn.pattern.SetDest().RegNo());
  }

  // Negative: with `allow_call_used_general_combine_regs_split = false` and
  // `allow_general_combine_regs_split = false`, neither RMW is split.
  {
    Sched1ProjectionContext ctx;
    ctx.is_entry_block = false;
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(body, mm, {32}, ctx);
    EXPECT_TRUE(view.implicit_combine_regs_split_orig_indices.empty());
    EXPECT_EQ(view.core_insns[0].pn.pattern.SetDest().RegNo(),
              view.core_insns[2].pn.pattern.SetDest().RegNo());
  }
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewAnchorsMdInputReloadBeforeConsumer) {
  // Mirrors `reload1.c:5641` (`emit_reload_insns`): `(set gpr lo)` feeding a
  // forward consumer carrying `(REG_DEAD lo)` is an `MD_REGS` **input reload**
  // and must be anchored in `input_reload_before_core` (bound to its consumer),
  // whereas `(set gpr lo)` following `(set lo (mult ...))` with no consumer
  // `(REG_DEAD lo)` remains an `MD_REGS` **output reload** anchored in
  // `output_reload_after_core` (bound to the `mult` producer).
  const CompilationProfile o2_mips1{.optimize_level = 2,
                                    .c_flags = {"-O2", "-mips1", "-funsigned-char"}};
  const MachineModel mm = MachineModel::FromProfile(o2_mips1);
  Sched1ProjectionContext ctx;
  ctx.is_entry_block = false;

  // Positive (input reload): `mflo $t1` (`[2]`) feeds `addu $v1, $t1, $a2` (`[3]`)
  // carrying `(REG_DEAD (reg:SI 65 lo))`, separated from `mult` (`[0]`) by `[1]`.
  {
    std::vector<Insn> in_reload_body = {
        MakeInsn("(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                 "(clobber (reg:SI 64 hi)) ] )",
                 0),
        MakeInsn("(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (const_int 1)))", 1),
        MakeInsn("(set (reg:SI 9 t1) (reg:SI 65 lo))", 2),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (reg:SI 6 a2))) "
                 "(expr_list:REG_DEAD (reg:SI 65 lo) (nil))",
                 3),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(in_reload_body, mm, {2}, ctx);
    ASSERT_EQ(view.core_insns.size(), 3u);
    EXPECT_TRUE(view.input_reload_before_core.contains(2));
    EXPECT_EQ(view.input_reload_before_core.at(2), 2);
    EXPECT_FALSE(view.output_reload_after_core.contains(2));
  }

  // Negative (output reload): `mflo $t1` (`[1]`) directly follows `mult` (`[0]`)
  // with `(REG_DEAD lo)` on `mflo` itself (not on `[2]`), so it anchors in
  // `output_reload_after_core` on producer core `0`.
  {
    std::vector<Insn> out_reload_body = {
        MakeInsn("(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                 "(clobber (reg:SI 64 hi)) ] )",
                 0),
        MakeInsn("(set (reg:SI 9 t1) (reg:SI 65 lo)) (expr_list:REG_DEAD (reg:SI 65 lo) (nil))", 1),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) (reg:SI 6 a2))) "
                 "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
                 2),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(out_reload_body, mm, {2}, ctx);
    ASSERT_EQ(view.core_insns.size(), 2u);
    EXPECT_TRUE(view.output_reload_after_core.contains(1));
    EXPECT_EQ(view.output_reload_after_core.at(1), 0);
    EXPECT_FALSE(view.input_reload_before_core.contains(1));
  }
}

TEST(SchedModelTest, ProjectPostReloadBlockToSched1ViewCountsMultiRegHardDefNSetsAcrossSubregs) {
  // Mirrors `flow.c:1916-1958, 2035-2049` (`mark_set_regs` / `mark_set_1`) +
  // `sched.c:2469-2501` (`birthing_insn_p`): a wide hard-reg `SET`
  // `(set (reg:DF 6 a2) ...)` covers both `$a2` (`6`) and `$a3` (`7`), so
  // `facts_s1.reg_n_sets[7]` must be incremented alongside `reg_n_sets[6]`,
  // clearing `birthing_insn_p` on a subsequent `(set (reg:SI 7 a3) ...)`.
  // Conversely, a narrow `(set (reg:SI 6 a2) ...)` must NOT increment
  // `reg_n_sets[7]`.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  auto make_body = [](bool wide_a2_def) {
    std::vector<Insn> body;
    int luid = 0;
    if (wide_a2_def) {
      body.push_back(MakeInsn("(set (reg:DF 6 a2) (reg:DF 58 $f26))", luid++));
    } else {
      body.push_back(MakeInsn("(set (reg:SI 6 a2) (reg:SI 16 s0))", luid++));
    }
    Insn call1 = MakeInsn(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"f1\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
        luid++);
    call1.kind = Insn::Kind::kCall;
    call1.pn.call_fusage = {
        RtlExpr::MakeUse(
            RtlExpr::MakeReg(wide_a2_def ? RtlMachineMode::kDf : RtlMachineMode::kSi, 6)),
    };
    body.push_back(std::move(call1));
    body.push_back(MakeInsn("(set (reg:SI 7 a3) (plus:SI (reg:SI 16 s0) (const_int 1)))", luid++));
    Insn call2 = MakeInsn(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"f2\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
        luid++);
    call2.kind = Insn::Kind::kCall;
    call2.pn.call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 7)),
    };
    body.push_back(std::move(call2));
    return body;
  };

  Sched1ProjectionContext ctx;
  ctx.is_entry_block = false;

  // Positive: `DFmode` set of `a2` (`6`) covers both `6` and `7`, so `reg_n_sets[7] == 2`.
  {
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(make_body(/*wide_a2_def=*/true), mm, {2}, ctx);
    ASSERT_TRUE(view.facts_s1.reg_n_sets.contains(6));
    ASSERT_TRUE(view.facts_s1.reg_n_sets.contains(7));
    EXPECT_EQ(view.facts_s1.reg_n_sets.at(6), 1);
    EXPECT_EQ(view.facts_s1.reg_n_sets.at(7), 2);
  }

  // Negative: `SImode` set of `a2` (`6`) does NOT cover `7`, so `reg_n_sets[7] == 1`.
  {
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(make_body(/*wide_a2_def=*/false), mm, {2}, ctx);
    ASSERT_TRUE(view.facts_s1.reg_n_sets.contains(6));
    ASSERT_TRUE(view.facts_s1.reg_n_sets.contains(7));
    EXPECT_EQ(view.facts_s1.reg_n_sets.at(6), 1);
    EXPECT_EQ(view.facts_s1.reg_n_sets.at(7), 1);
  }

  // Positive (CFG cross-block `other_block_total_defs`): a `DFmode` unrenamed
  // hard-reg call-arg def of `a2` in B0 counts toward `other_block_total_defs[7]`
  // in B1 (`flow.c:2035-2049`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks.resize(2);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:DF 6 a2) (reg:DF 58 $f26))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"f1\"))) (const_int 16))) "
        "(clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage = true;
    cfg.rpo_blocks[0].rtl_insns[1].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kDf, 6)),
    };
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 7 a3) (plus:SI (reg:SI 16 s0) (const_int 1)))");
    cfg.rpo_blocks[1].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"f2\"))) (const_int 16))) "
        "(clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[1].rtl_insns[1].has_explicit_call_fusage = true;
    cfg.rpo_blocks[1].rtl_insns[1].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 7)),
    };
    cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");
    const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(cfg, cfg, mm);
    const Sched1ProjectionContext b1_ctx = BuildSched1ProjectionContextForBlock(1, cfg_ctx);
    ASSERT_TRUE(b1_ctx.other_block_total_defs.contains(7));
    EXPECT_GE(b1_ctx.other_block_total_defs.at(7), 1);
  }
}

TEST(SchedModelTest, SpillSlotPseudoVersioningSeparatesDisjointLifetimesInSched1View) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  Sched1ProjectionContext ctx;
  ctx.has_cfg_context = true;
  ctx.is_entry_block = false;
  ctx.stack_layout.valid = true;
  ctx.stack_layout.frame_pointer_needed = false;
  ctx.stack_layout.starting_frame_offset = 16;
  ctx.stack_layout.frame_size = 48;
  ctx.stack_layout.min_valid_reload_spill_offset = 16;
  ctx.stack_layout.stack_slot_store_offsets.insert(28);
  ctx.stack_layout.stack_slot_load_offsets.insert(28);
  ctx.other_block_spill_load_offsets.insert(28);

  // Positive (`reload1.c:2305-2306, 2362-2367`): two disjoint spill lifetimes
  // across calls reusing the same 8-byte stack slot `[sp+28]` (using different
  // reload scratch regs `$f0` / `$f2`) receive distinct synthetic
  // `SpillPseudoKey` pseudos in `ProjectPostReloadBlockToSched1View` so the
  // second lifetime is not merged with the first.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22)))", 0),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))", 1),
        MakeInsn("(parallel[ (call (mem:SI (symbol_ref:SI (\"c1\"))) (const_int 16)) "
                 "(clobber (reg:SI 31 ra)) ] )",
                 2, Insn::Kind::kCall),
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))", 3),
        MakeInsn("(set (reg:SF 56 $f24) (mult:SF (reg:SF 34 $f2) (reg:SF 52 $f20)))", 4),
        MakeInsn("(set (reg:SF 32 $f0) (minus:SF (reg:SF 52 $f20) (reg:SF 54 $f22)))", 5),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))", 6),
        MakeInsn("(parallel[ (call (mem:SI (symbol_ref:SI (\"c2\"))) (const_int 16)) "
                 "(clobber (reg:SI 31 ra)) ] )",
                 7, Insn::Kind::kCall),
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))", 8),
        MakeInsn("(set (reg:SF 58 $f26) (plus:SF (reg:SF 56 $f24) (reg:SF 34 $f2)))", 9),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{58}, ctx);
    ASSERT_EQ(view.core_insns.size(), 6u);
    const int p1 = view.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p2 = view.core_insns[3].pn.pattern.SetDest().RegNo();
    EXPECT_GE(p1, kMipsFirstPseudoRegister);
    EXPECT_GE(p2, kMipsFirstPseudoRegister);
    EXPECT_NE(p1, p2);
    EXPECT_EQ(view.core_insns[2].pn.pattern.SetSrc().children[0].RegNo(), p1);
    EXPECT_EQ(view.core_insns[5].pn.pattern.SetSrc().children[1].RegNo(), p2);
  }

  // Negative: an in-place RMW that reloads `[sp+28]` across a call, updates
  // it, and stores back to `[sp+28]` preserves the SAME `SpillPseudoKey`
  // version across the load-modify-store sequence.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 52 $f20) (reg:SF 54 $f22)))", 0),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))", 1),
        MakeInsn("(parallel[ (call (mem:SI (symbol_ref:SI (\"c1\"))) (const_int 16)) "
                 "(clobber (reg:SI 31 ra)) ] )",
                 2, Insn::Kind::kCall),
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))", 3),
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 52 $f20)))", 4),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))", 5),
        MakeInsn("(parallel[ (call (mem:SI (symbol_ref:SI (\"c2\"))) (const_int 16)) "
                 "(clobber (reg:SI 31 ra)) ] )",
                 6, Insn::Kind::kCall),
        MakeInsn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))", 7),
        MakeInsn("(set (reg:SF 56 $f24) (mult:SF (reg:SF 34 $f2) (reg:SF 54 $f22)))", 8),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{56}, ctx);
    ASSERT_EQ(view.core_insns.size(), 5u);
    const int p_init = view.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p_rmw_dst = view.core_insns[2].pn.pattern.SetDest().RegNo();
    const int p_rmw_src = view.core_insns[2].pn.pattern.SetSrc().children[0].RegNo();
    const int p_use = view.core_insns[4].pn.pattern.SetSrc().children[0].RegNo();
    EXPECT_GE(p_init, kMipsFirstPseudoRegister);
    EXPECT_EQ(p_rmw_src, p_init);
    EXPECT_EQ(p_rmw_dst, p_init);
    EXPECT_EQ(p_use, p_init);
  }
}

TEST(SchedModelTest, ModeChangingInPlaceRmwSplitsIntoDistinctPseudosInSched1View) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  Sched1ProjectionContext ctx;
  ctx.has_cfg_context = true;
  ctx.is_entry_block = false;
  ctx.allow_post_call_combine_regs_split = false;
  ctx.allow_general_combine_regs_split = false;

  // Positive (`emit-rtl.c:515-517` `gen_reg_rtx`): an in-place mode-changing
  // conversion `(set (reg:DF 56 $f24) (float_extend:DF (reg:SF 56 $f24)))`
  // without `(REG_DEAD (reg:SF 56 $f24))` MUST split into two distinct pseudos
  // because a single pre-allocation pseudo has one fixed machine mode.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:SF 56 $f24) (mem:SF (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:DF 56 $f24) (float_extend:DF (reg:SF 56 $f24)))", 1),
        MakeInsn("(set (reg:DF 32 $f0) (plus:DF (reg:DF 56 $f24) (reg:DF 36 $f4))) "
                 "(expr_list:REG_DEAD (reg:DF 56 $f24) (nil))",
                 2),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{32, 33}, ctx);
    ASSERT_EQ(view.core_insns.size(), 3u);
    const int p_sf = view.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p_ext_in = view.core_insns[1].pn.pattern.SetSrc().children[0].RegNo();
    const int p_df = view.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_GE(p_sf, kMipsFirstPseudoRegister);
    EXPECT_EQ(p_ext_in, p_sf);
    EXPECT_GE(p_df, kMipsFirstPseudoRegister);
    EXPECT_NE(p_df, p_sf);
    EXPECT_EQ(view.core_insns[2].pn.pattern.SetSrc().children[0].RegNo(), p_df);
  }

  // Negative: a same-mode in-place RMW `(set (reg:DF 56 $f24) (plus:DF (reg:DF 56 $f24) ...))`
  // without `REG_DEAD` keeps the same multi-def pseudo across input and output.
  {
    const std::vector<Insn> body = {
        MakeInsn("(set (reg:DF 56 $f24) (mem:DF (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:DF 56 $f24) (plus:DF (reg:DF 56 $f24) (reg:DF 36 $f4)))", 1),
        MakeInsn("(set (reg:DF 32 $f0) (plus:DF (reg:DF 56 $f24) (reg:DF 38 $f6))) "
                 "(expr_list:REG_DEAD (reg:DF 56 $f24) (nil))",
                 2),
    };
    const Sched1ProjectionView view =
        ProjectPostReloadBlockToSched1View(body, mm, /*live_at_end_s2=*/{32, 33}, ctx);
    ASSERT_EQ(view.core_insns.size(), 3u);
    const int p0 = view.core_insns[0].pn.pattern.SetDest().RegNo();
    const int p1_in = view.core_insns[1].pn.pattern.SetSrc().children[0].RegNo();
    const int p1_out = view.core_insns[1].pn.pattern.SetDest().RegNo();
    EXPECT_GE(p0, kMipsFirstPseudoRegister);
    EXPECT_EQ(p1_in, p0);
    EXPECT_EQ(p1_out, p0);
  }
}

TEST(SchedModelTest, PerLiveRangeLocalFindFreeRegExcludesReloadProducers) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);
  PostReloadStackLayout stack_layout;
  stack_layout.valid = true;
  stack_layout.frame_pointer_needed = false;
  stack_layout.starting_frame_offset = 16;
  stack_layout.frame_size = 48;
  stack_layout.min_valid_reload_spill_offset = 16;

  // Positive (`local-alloc.c:2158-2182` `find_free_reg`): when `$f0` (32) and
  // `$f2` (34) are both ordinary local defs whose live ranges overlap in
  // `base_block`, reordering so `$f0` dies before `$f2` is born drops the
  // `(32, 34)` interference and MUST be rejected by
  // `PreservesPostReloadOrderSafety`.
  {
    const std::vector<Insn> base = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (reg:SF 34 $f2) (minus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 1),
        MakeInsn("(set (mem:SF (reg:SI 4 a0)) (reg:SF 32 $f0)) "
                 "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                 2),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SF 34 $f2)) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
                 3),
    };
    const std::vector<int> disjoint_cand = {0, 2, 1, 3};
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        base, disjoint_cand, /*prologue_end=*/0, /*epilogue_begin=*/4, /*live_at_end_s2=*/{},
        /*cfg_copy_pairs=*/{}, RegFacts{}, mm, /*allow_pure_reader_reg_dead_transfer=*/false,
        stack_layout));
  }

  // Negative 1 (`reload1.c:517-524, 6583`): when `$f0` is a rematerialized
  // `const_double` reload (empty `reg_notes` and NO `(REG_DEAD $f0)` on its
  // last use), `$f0` was inserted post-`local-alloc` into an FP spill reg, so
  // reordering `$f0` ahead of `$f2` does NOT drop a `local-alloc` interference
  // and MUST be allowed.
  {
    const std::vector<Insn> base = {
        MakeInsn("(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1070596096))", 0),
        MakeInsn("(set (reg:SF 34 $f2) (minus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 1),
        MakeInsn("(set (mem:SF (reg:SI 4 a0)) (reg:SF 32 $f0))", 2),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SF 34 $f2)) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
                 3),
    };
    const std::vector<int> disjoint_cand = {0, 2, 1, 3};
    EXPECT_TRUE(PreservesPostReloadOrderSafety(
        base, disjoint_cand, /*prologue_end=*/0, /*epilogue_begin=*/4, /*live_at_end_s2=*/{},
        /*cfg_copy_pairs=*/{}, RegFacts{}, mm, /*allow_pure_reader_reg_dead_transfer=*/false,
        stack_layout));
  }

  // Negative 2 (`reload1.c:5641-6155`): when `$f0` is a spilled-output reload
  // producer (stored to a stack slot `[sp+28]` and immediately dead), `local-alloc`
  // never assigned `$f0` to that pseudo, so reordering `[0, 2]` ahead of `$f2`
  // MUST be allowed.
  {
    const std::vector<Insn> base = {
        MakeInsn("(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 0),
        MakeInsn("(set (reg:SF 34 $f2) (minus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))", 1),
        MakeInsn("(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))", 2),
        MakeInsn("(set (reg:SF 48 $f16) (plus:SF (reg:SF 34 $f2) (reg:SF 44 $f12))) "
                 "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
                 3),
    };
    const std::vector<int> disjoint_cand = {0, 2, 1, 3};
    EXPECT_TRUE(PreservesPostReloadOrderSafety(
        base, disjoint_cand, /*prologue_end=*/0, /*epilogue_begin=*/4, /*live_at_end_s2=*/{48},
        /*cfg_copy_pairs=*/{}, RegFacts{}, mm, /*allow_pure_reader_reg_dead_transfer=*/false,
        stack_layout));
  }
}

TEST(SchedModelTest, ConflictDrivenSched1TieFlipDemotesViolatingConsumerChain) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Two independent 2-insn chains `(0 -> 2)` and `(1 -> 3)` with identical
  // `kSched1` priorities: default backward `Schedule(kSched1)` on `[0, 1, 2, 3]`
  // schedules `3` before `2` (`rank_for_schedule` LUID tie-break), placing `1`
  // before `0` or vice versa. Supplying a `core_s2_edges` constraint `(u, v)`
  // violated by the baseline schedule exercises conflict-driven consumer-chain
  // demotion (`emit_demote_node`) to produce an `Im(F_sched1)` permutation that
  // satisfies `(u, v)`.
  Sched1ProjectionView view;
  view.core_insns = {
      MakeInsn("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
      MakeInsn("(set (reg:SI 73) (plus:SI (reg:SI 5 a1) (const_int 2)))", 1),
      MakeInsn("(set (reg:SI 16 s0) (plus:SI (reg:SI 72) (const_int 10))) "
               "(expr_list:REG_DEAD (reg:SI 72) (nil))",
               2),
      MakeInsn("(set (reg:SI 17 s1) (plus:SI (reg:SI 73) (const_int 20))) "
               "(expr_list:REG_DEAD (reg:SI 73) (nil))",
               3),
  };
  view.core_to_orig_index = {0, 1, 2, 3};
  view.facts_s1.reg_n_sets[72] = 1;
  view.facts_s1.reg_n_sets[73] = 1;
  view.live_at_end_s1 = {16, 17};
  const std::vector<int> base_order = ScheduleSched1ViewCoreOrder(view, mm);
  ASSERT_EQ(base_order.size(), 4u);
  int pos0 = -1, pos1 = -1;
  for (int i = 0; i < 4; ++i) {
    if (base_order[static_cast<size_t>(i)] == 0) pos0 = i;
    if (base_order[static_cast<size_t>(i)] == 1) pos1 = i;
  }
  ASSERT_NE(pos0, -1);
  ASSERT_NE(pos1, -1);
  // Request the opposite relative order between `0` and `1` from `base_order`.
  const std::pair<int, int> req_edge = (pos0 < pos1) ? std::make_pair(1, 0) : std::make_pair(0, 1);

  // Positive: conflict-driven tie-flip search discovers an `Im(F_sched1)` order
  // satisfying `req_edge` by demoting the violating producer's consumer.
  {
    const std::vector<std::vector<int>> orders =
        CollectSched1TieFlipCoreOrders(view, mm, /*max_orders=*/8, /*core_s2_edges=*/{req_edge});
    bool found_satisfying = false;
    for (const std::vector<int>& ord : orders) {
      ASSERT_EQ(ord.size(), 4u);
      int pu = -1, pv = -1;
      for (int i = 0; i < 4; ++i) {
        if (ord[static_cast<size_t>(i)] == req_edge.first) pu = i;
        if (ord[static_cast<size_t>(i)] == req_edge.second) pv = i;
      }
      if (pu >= 0 && pv >= 0 && pu < pv) {
        found_satisfying = true;
        break;
      }
    }
    EXPECT_TRUE(found_satisfying);
  }

  // Negative: when an unsatisfiable cyclic `core_s2_edges` constraint `(2, 0)`
  // contradicts a hard `kSched1` true-dependence edge `(0 -> 2)`, every returned
  // order still respects `(0 -> 2)` and lies strictly in `Im(F_sched1)`.
  {
    const std::vector<std::vector<int>> orders =
        CollectSched1TieFlipCoreOrders(view, mm, /*max_orders=*/8, /*core_s2_edges=*/{{2, 0}});
    for (const std::vector<int>& ord : orders) {
      ASSERT_EQ(ord.size(), 4u);
      int p0 = -1, p2 = -1;
      for (int i = 0; i < 4; ++i) {
        if (ord[static_cast<size_t>(i)] == 0) p0 = i;
        if (ord[static_cast<size_t>(i)] == 2) p2 = i;
      }
      EXPECT_LT(p0, p2);
    }
  }
}

TEST(SchedModelTest, ExtractCoreSched2ReplayEdgesProjectsInvertedRegDeadReaderOrder) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`sched.c:3415-3611`): post-`sched2` target has `(REG_DEAD a2)`
  // hoisted onto pure reader `2` (`v1 = v1 + a2`) ahead of later pure reader
  // `3` (`t0 = a2 + t0`, which has strictly higher backward `kSched2` priority
  // via `lw` producer `1`, so `kSched2` replay does not tie-constrain `2 < 3`).
  // With `preserve_reg_dead_reader_order=true`, `ExtractCoreSched2ReplayEdges`
  // must project the inverted-`REG_DEAD` edge from core insn `2` (target `3`)
  // to core insn `3` (target `2`); with `preserve_reg_dead_reader_order=false`,
  // that edge is omitted.
  {
    const std::vector<Insn> target = {
        MakeInsn("(set (reg:SI 6 a2) (plus:SI (reg:SI 4 a0) (const_int 1)))", 0),
        MakeInsn("(set (reg:SI 8 t0) (mem/u:SI (reg:SI 4 a0)))", 1),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2))) "
                 "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                 2),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (reg:SI 8 t0)))", 3),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
                 "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
                 4),
    };
    const std::vector<int> cur_to_target = {0, 1, 3, 2, 4};
    std::vector<Insn> cur_body;
    for (int p = 0; p < 5; ++p) {
      Insn insn = target[static_cast<size_t>(cur_to_target[static_cast<size_t>(p)])];
      insn.luid = p;
      cur_body.push_back(std::move(insn));
    }
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(
        cur_body, mm, /*live_at_end_s2=*/{2}, Sched1ProjectionContext{});
    ASSERT_EQ(view.core_insns.size(), 5u);
    // In `cur_body`, core index `2` is target `3` and core index `3` is target `2`.
    const std::vector<std::pair<int, int>> edges_with_dead = ExtractCoreSched2ReplayEdges(
        target, cur_to_target, /*prologue_end=*/0, /*epilogue_begin=*/5, view, RegFacts{}, mm,
        /*live_at_end_s2=*/{2}, /*preserve_reg_dead_reader_order=*/true);
    EXPECT_NE(std::find(edges_with_dead.begin(), edges_with_dead.end(), std::make_pair(2, 3)),
              edges_with_dead.end());

    const std::vector<std::pair<int, int>> edges_without_dead = ExtractCoreSched2ReplayEdges(
        target, cur_to_target, /*prologue_end=*/0, /*epilogue_begin=*/5, view, RegFacts{}, mm,
        /*live_at_end_s2=*/{2}, /*preserve_reg_dead_reader_order=*/false);
    EXPECT_EQ(std::find(edges_without_dead.begin(), edges_without_dead.end(), std::make_pair(2, 3)),
              edges_without_dead.end());
  }

  // Negative: an intervening redefinition of `a2` at index `2` separates reader
  // `1` (`REG_DEAD a2`) from reader `3` (a distinct live range of `a2`), so
  // `(3 -> 1)` MUST NOT be projected even with `preserve_reg_dead_reader_order=true`.
  {
    const std::vector<Insn> target = {
        MakeInsn("(set (reg:SI 6 a2) (mem/u:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2))) "
                 "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                 1),
        MakeInsn("(set (reg:SI 6 a2) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 4))))", 2),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (reg:SI 8 t0))) "
                 "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                 3),
        MakeInsn("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0)))", 4),
    };
    const std::vector<int> cur_to_target = {0, 1, 2, 3, 4};
    const Sched1ProjectionView view = ProjectPostReloadBlockToSched1View(
        target, mm, /*live_at_end_s2=*/{2}, Sched1ProjectionContext{});
    ASSERT_EQ(view.core_insns.size(), 5u);
    const std::vector<std::pair<int, int>> edges = ExtractCoreSched2ReplayEdges(
        target, cur_to_target, /*prologue_end=*/0, /*epilogue_begin=*/5, view, RegFacts{}, mm,
        /*live_at_end_s2=*/{2}, /*preserve_reg_dead_reader_order=*/true);
    EXPECT_EQ(std::find(edges.begin(), edges.end(), std::make_pair(3, 1)), edges.end());
  }
}

TEST(SchedModelTest, PreservesPostReloadOrderSafetyAllowsLoadEquivInvalidatorMovedIntoWindow) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}};
  const MachineModel mm = MachineModel::FromProfile(o2_prof);

  // Positive (`local-alloc.c:540-603, 1049-1055` `validate_equiv_mem`): load `0`
  // (`$v0 = mem[a0]`) carries NO `REG_EQUIV`. In post-`sched2` `base`, `REG_DEAD v0`
  // (`1`) was hoisted ahead of aliasing store `2` (`mem[a1] = $v1`), so `base`
  // spuriously reports `validate_equiv_mem == true`. Restoring store `2` into
  // `(load_pos, cand_dst_death_pos)` (`[0, 2, 1]`) restores `validate_equiv_mem == false`
  // matching the absence of `REG_EQUIV` and MUST pass `PreservesPostReloadOrderSafety`.
  {
    const std::vector<Insn> base_no_equiv = {
        MakeInsn("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))", 0),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 2 v0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 1),
        MakeInsn("(set (mem:SI (reg:SI 5 a1)) (reg:SI 3 v1))", 2),
    };
    const std::vector<int> cand_move_store_in = {0, 2, 1};
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        base_no_equiv, cand_move_store_in, /*prologue_end=*/0, /*epilogue_begin=*/3,
        /*live_at_end_s2=*/{8}, /*cfg_copy_pairs=*/{}, RegFacts{}, mm,
        /*allow_pure_reader_reg_dead_transfer=*/false));
    EXPECT_TRUE(PreservesPostReloadOrderSafety(
        base_no_equiv, cand_move_store_in, /*prologue_end=*/0, /*epilogue_begin=*/3,
        /*live_at_end_s2=*/{8}, /*cfg_copy_pairs=*/{}, RegFacts{}, mm,
        /*allow_pure_reader_reg_dead_transfer=*/false, PostReloadStackLayout{},
        /*excluded_prologue_body_indices=*/{},
        /*allow_moved_store_load_equiv_invalidation=*/true));
  }

  // Negative: when load `0` ALREADY carries a matching `REG_EQUIV (mem [a0])`,
  // moving aliasing store `2` into `(load_pos, cand_dst_death_pos)` invalidates
  // the existing equivalence and MUST be rejected even when
  // `allow_moved_store_load_equiv_invalidation` is enabled.
  {
    const std::vector<Insn> base_with_equiv = {
        MakeInsn("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
                 "(expr_list:REG_EQUIV (mem:SI (reg:SI 4 a0)) (nil))",
                 0),
        MakeInsn("(set (reg:SI 8 t0) (plus:SI (reg:SI 2 v0) (const_int 1))) "
                 "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                 1),
        MakeInsn("(set (mem:SI (reg:SI 5 a1)) (reg:SI 3 v1))", 2),
    };
    const std::vector<int> cand_move_store_in = {0, 2, 1};
    EXPECT_FALSE(PreservesPostReloadOrderSafety(
        base_with_equiv, cand_move_store_in, /*prologue_end=*/0, /*epilogue_begin=*/3,
        /*live_at_end_s2=*/{8}, /*cfg_copy_pairs=*/{}, RegFacts{}, mm,
        /*allow_pure_reader_reg_dead_transfer=*/false, PostReloadStackLayout{},
        /*excluded_prologue_body_indices=*/{},
        /*allow_moved_store_load_equiv_invalidation=*/true));
  }
}

TEST(SchedModelTest, AttributeSched2PureReaderRegDeadNotesRepairsRegEquivFreshDestCombineConflict) {
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
