#include "lifter/stupid_alloc_model.h"

#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

RtlPatternAndNotes MustParsePn(std::string_view sexpr) {
  auto parsed = ParsePatternAndNotes(sexpr);
  EXPECT_TRUE(parsed.ok()) << sexpr << ": " << parsed.status();
  return *std::move(parsed);
}

TEST(StupidAllocModelTest, BuildTrialO0PseudoModelPartitionsWebsAndPinsCallAbi) {
  // Block 0:
  //   i=0: (set (reg:SI 8 t0) (const_int 10))         -> non-ABI web -> pseudo
  //   i=1: (set (reg:SI 9 t1) (plus:SI (reg:SI 8 t0) (const_int 1))) -> non-ABI web -> pseudo
  //   i=2: (set (reg:SI 4 a0) (reg:SI 9 t1))          -> pinned by call_fusage ($a0)
  //   i=3: call returning (reg:SI 2 v0) with call_fusage (use (reg:SI 4 a0))
  //   i=4: (set (reg:SI 16 s0) (reg:SI 2 v0))         -> non-ABI web -> pseudo ($s0)
  //   i=5: (parallel [(set (reg:SI 65 lo) (mult:SI (reg:SI 16 s0) (reg:SI 16 s0)))
  //                   (clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum))]) -> scratch stripped
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;

  RtlPatternAndNotes call_pn = MustParsePn(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"foo\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");
  call_pn.has_explicit_call_fusage = true;
  call_pn.call_fusage = {MustParsePn("(use (reg:SI 4 a0))").pattern};

  std::vector<std::vector<RtlPatternAndNotes>> all_blocks = {{
      MustParsePn("(set (reg:SI 8 t0) (const_int 10))"),
      MustParsePn("(set (reg:SI 9 t1) (plus:SI (reg:SI 8 t0) (const_int 1)))"),
      MustParsePn("(set (reg:SI 4 a0) (reg:SI 9 t1))"),
      std::move(call_pn),
      MustParsePn("(set (reg:SI 16 s0) (reg:SI 2 v0))"),
      MustParsePn("(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 16 s0) (reg:SI 16 s0))) "
                  "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] )"),
  }};

  O0PrologueInfo pinfo;
  pinfo.prologue_end = 0;
  const TrialO0Model model =
      BuildTrialO0PseudoModel(cfg, pinfo, /*gp64=*/false, /*fp64=*/false, all_blocks);

  ASSERT_EQ(model.blocks.size(), 1u);
  ASSERT_EQ(model.blocks[0].size(), 6u);

  // Positive: `$t0`, `$t1`, and `$s0` are lifted to distinct pseudos `>= 72`.
  EXPECT_TRUE(model.blocks[0][0].pattern.SetDest().IsPseudoReg());
  EXPECT_TRUE(model.blocks[0][1].pattern.SetDest().IsPseudoReg());
  EXPECT_TRUE(model.blocks[0][4].pattern.SetDest().IsPseudoReg());
  const int p_t0 = model.blocks[0][0].pattern.SetDest().RegNo();
  const int p_t1 = model.blocks[0][1].pattern.SetDest().RegNo();
  const int p_s0 = model.blocks[0][4].pattern.SetDest().RegNo();
  EXPECT_EQ(model.reg_renumber.at(p_t0), 8);
  EXPECT_EQ(model.reg_renumber.at(p_t1), 9);
  EXPECT_EQ(model.reg_renumber.at(p_s0), 16);

  // Positive: `match_scratch` clobbers on `mulsi3` are stripped back to `(scratch:SI)`.
  EXPECT_EQ(model.blocks[0][5].pattern.children[1].children[0].op, RtlOp::kScratch);
  EXPECT_EQ(model.blocks[0][5].pattern.children[2].children[0].op, RtlOp::kScratch);

  // Negative: call-argument `$a0` (in `call_fusage`) and call-return `$v0` stay pinned hard regs.
  EXPECT_TRUE(model.blocks[0][2].pattern.SetDest().IsHardReg());
  EXPECT_EQ(model.blocks[0][2].pattern.SetDest().RegNo(), 4);
  EXPECT_TRUE(model.blocks[0][4].pattern.SetSrc().IsHardReg());
  EXPECT_EQ(model.blocks[0][4].pattern.SetSrc().RegNo(), 2);
}

TEST(StupidAllocModelTest, SimulateO0StupidAllocRespectsOrderConflictsCallsAndClasses) {
  // Construct a deterministic SUID timeline (`exit_suid = 12`) with:
  // - p72: long SI span [1, 10), crosses call at suid 5 -> must get `$s0` (16).
  // - p73: medium SI span [2, 9), no call crossing, `$v0` (2) busy at suid 4 -> gets `$v1` (3).
  // - p74: short SI span [6, 9), no call crossing, overlaps p73 (has 3) -> gets `$v0` (2).
  // - p75: SF span [2, 6), no call crossing, `!changes_size` -> gets `$f0` (32).
  // - p76: SF span [2, 6), `changes_size = true` -> MIPS `CLASS_CANNOT_CHANGE_SIZE`
  //        (`FP_REGS`, `stupid.c:358-362`) forbids FPR, so falls back to GPR `$a0` (4).
  O0StupidTimeline tl;
  tl.exit_suid = 12;
  tl.b0_prefix_suid = 1;
  tl.call_suids = {5};
  tl.after_hr.assign(15, std::array<bool, 68>{});
  tl.after_hr[4][2] = true;  // `$v0` (2) occupied at suid 4.

  tl.spans[72] = O0PseudoSuidSpan{
      .born = 1, .dead = 10, .n_refs = 2, .n_calls = 1, .mode = RtlMachineMode::kSi};
  tl.spans[73] = O0PseudoSuidSpan{
      .born = 2, .dead = 9, .n_refs = 2, .n_calls = 0, .mode = RtlMachineMode::kSi};
  tl.spans[74] = O0PseudoSuidSpan{
      .born = 6, .dead = 9, .n_refs = 2, .n_calls = 0, .mode = RtlMachineMode::kSi};
  tl.spans[75] = O0PseudoSuidSpan{
      .born = 2, .dead = 6, .n_refs = 2, .n_calls = 0, .mode = RtlMachineMode::kSf};
  tl.spans[76] = O0PseudoSuidSpan{.born = 2,
                                  .dead = 6,
                                  .n_refs = 2,
                                  .n_calls = 0,
                                  .mode = RtlMachineMode::kSf,
                                  .changes_size = true};

  const absl::btree_map<int, int> exp_renumber = {
      {72, 16}, {73, 3}, {74, 2}, {75, 32}, {76, 4},
  };
  const absl::btree_map<int, O0ScopeKind> empty_scopes;
  const O0OrderedScopes ord = OrderO0Scopes(tl, exp_renumber, empty_scopes);

  const absl::btree_map<int, int> sim =
      SimulateO0StupidAlloc(/*gp64=*/false, /*fp64=*/false, tl, exp_renumber, empty_scopes, ord);

  EXPECT_EQ(sim.at(72), 16);  // Call-crossing -> callee-saved `$s0` (16).
  EXPECT_EQ(sim.at(73), 3);   // `$v0` (2) blocked at suid 4 -> `$v1` (3).
  EXPECT_EQ(sim.at(74), 2);   // [6, 9) avoids suid 4 and `$v1` (taken by p73) -> `$v0` (2).
  EXPECT_EQ(sim.at(75), 32);  // Clean SF -> `$f0` (32).
  EXPECT_EQ(sim.at(76), 4);   // `changes_size` excludes FP_REGS; 2 & 3 busy in [2,6) -> `$a0` (4).
}

TEST(StupidAllocModelTest, SolveO0ScopesOnModelRecoversObeyRegdeclsScopeAndRejectsNonScoped) {
  // Positive case:
  // `p72` (`$s0 = 16`, local var born before a call, last used before the call)
  // was allocated to callee-saved `$s0` ONLY because `obey_regdecls` extended its
  // lifetime via `(use (reg:SI 16 s0))` at function exit across the later call.
  // Without scope extension, `SimulateO0StupidAlloc` assigns `p72` to `$v0` (2).
  // `p73` (`$v0 = 2`, short temporary after the call) does NOT cross any call and
  // needs NO `obey_regdecls` scope marker.
  TrialO0Model model;
  model.blocks = {{
      MustParsePn("(set (reg:SI 72) (const_int 42))"),
      MustParsePn("(set (mem:SI (reg:SI 29 sp)) (reg:SI 72))"),
      MustParsePn("(parallel[ (call (mem:SI (symbol_ref:SI (\"bar\"))) (const_int 16)) "
                  "(clobber (reg:SI 31 ra)) ] )"),
      MustParsePn("(set (reg:SI 73) (const_int 7))"),
      MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))) (reg:SI 73))"),
  }};
  model.orig_idx = {{0, 1, 2, 3, 4}};
  model.reg_renumber = {{72, 16}, {73, 2}};
  model.pseudo_modes = {{72, RtlMachineMode::kSi}, {73, RtlMachineMode::kSi}};

  const ScopeSolveResult solved = SolveO0ScopesOnModel(/*gp64=*/false, /*fp64=*/false, model);
  EXPECT_TRUE(solved.matched);
  ASSERT_TRUE(solved.scope_kinds.contains(72));
  EXPECT_EQ(solved.scope_kinds.at(72), O0ScopeKind::kBirthLocal);
  // Negative: short post-call temporary `p73` must NOT be falsely promoted to a scope.
  EXPECT_FALSE(solved.scope_kinds.contains(73));

  // Negative case: if `$s0` (16) is clobbered by a later explicit hard-reg write
  // before exit, `p72` cannot be extended to exit (`is_compatible_from` rejects it)
  // and `SolveO0ScopesOnModel` reports `matched == false` rather than fabricating an
  // illegal scope.
  TrialO0Model conflict_model = model;
  conflict_model.blocks[0].push_back(MustParsePn("(set (reg:SI 16 s0) (const_int 0))"));
  conflict_model.orig_idx[0].push_back(5);
  const ScopeSolveResult conflict_solved =
      SolveO0ScopesOnModel(/*gp64=*/false, /*fp64=*/false, conflict_model);
  EXPECT_FALSE(conflict_solved.matched);
  EXPECT_FALSE(conflict_solved.scope_kinds.contains(72));
}

}  // namespace
}  // namespace rom_nom_nom::lifter
