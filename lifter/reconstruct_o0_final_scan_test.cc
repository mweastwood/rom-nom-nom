#include "lifter/reconstruct_o0_final_scan.h"

#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

TEST(ReconstructO0FinalScanTest, SkipsNonO0ProfilesAndReconstructsNoConflictDiSequence) {
  const CompilationProfile o0_prof{.optimize_level = 0, .c_flags = {"-O0", "-mips2"}};
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  auto make_no_conflict_cfg = [](bool insert_call_before_consumer) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (reg:SI 4 a0))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))");
    if (insert_call_before_consumer) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(parallel[ (call (mem:SI (symbol_ref:SI (\"barrier\"))) (const_int 0)) "
          "(clobber (reg:SI 31 ra)) ] )");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(set (mem:DI (reg:SI 6 a2)) (reg:DI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    return cfg;
  };

  // Positive (`-O0`): reconstructs `(clobber (reg:DI 2 v0))`, `(subreg:SI (reg:DI 2 v0) 1/0)`,
  // and trailing `(set (reg:DI 2 v0) (reg:DI 2 v0))`.
  {
    RtlControlFlowGraph cfg = make_no_conflict_cfg(/*insert_call_before_consumer=*/false);
    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    const std::string summary = cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(summary, "(clobber (reg:DI 2 v0))")) << summary;
    EXPECT_TRUE(absl::StrContains(summary, "(subreg:SI (reg:DI 2 v0) 1)")) << summary;
    EXPECT_TRUE(absl::StrContains(summary, "(subreg:SI (reg:DI 2 v0) 0)")) << summary;
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:DI 2 v0) (reg:DI 2 v0))")) << summary;
  }

  // Negative 1: non-`-O0` (`-O2`) leaves the CFG completely untouched.
  {
    RtlControlFlowGraph cfg = make_no_conflict_cfg(/*insert_call_before_consumer=*/false);
    const std::vector<std::string> orig = cfg.rpo_blocks[0].instructions;
    ReconstructO0FinalScanDeletions(&cfg, o2_prof);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, orig);
  }

  // Negative 2: intervening `CALL_INSN` blocks `NoConflict` clobber/subreg reconstruction.
  {
    RtlControlFlowGraph cfg = make_no_conflict_cfg(/*insert_call_before_consumer=*/true);
    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    const std::string summary = cfg.FormatSummary();
    EXPECT_FALSE(absl::StrContains(summary, "(clobber (reg:DI 2 v0))")) << summary;
    EXPECT_FALSE(absl::StrContains(summary, "(subreg:SI (reg:DI 2 v0)")) << summary;
  }
}

TEST(ReconstructO0FinalScanTest, ReconstructsSubwordTruncatingSubregsPositiveAndNegative) {
  const CompilationProfile o0_prof{.optimize_level = 0, .c_flags = {"-O0", "-mips2"}};

  // Positive: `SImode` ALU def of `$2 v0` followed by `QImode` store wraps read in `subreg:QI`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (mem:QI (reg:SI 5 a1)) (reg:QI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    EXPECT_TRUE(absl::StrContains(cfg.FormatSummary(), "(subreg:QI (reg:SI 2 v0) 0)"))
        << cfg.FormatSummary();
  }

  // Negative: `(zero_extend:SI (mem:QI ...))` load does NOT wrap subsequent `QImode` read.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (zero_extend:SI (mem:QI (reg:SI 4 a0))))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (mem:QI (reg:SI 5 a1)) (reg:QI 2 v0))");
    cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    EXPECT_FALSE(absl::StrContains(cfg.FormatSummary(), "(subreg:QI (reg:SI 2 v0) 0)"))
        << cfg.FormatSummary();
  }
}

TEST(ReconstructO0FinalScanTest, ReconstructsTablejumpIndexSelfMovePositiveAndLiveIndexNegative) {
  const CompilationProfile o0_prof{.optimize_level = 0, .c_flags = {"-O0", "-mips2"}};

  auto make_tablejump_cfg = [](bool index_live_in_target) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(2);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].addr_vec_targets = {1};
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
    cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 3 v1) (mem:SI (reg:SI 3 v1)))");
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (pc) (reg:SI 3 v1)) (use (label_ref L1)) ] )");

    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].has_label = true;
    cfg.rpo_blocks[1].predecessors = {0};
    if (index_live_in_target) {
      cfg.rpo_blocks[1].AddPatternOnly(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
    } else {
      cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 7))");
    }
    cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: dead tablejump index `$2 v0` restores `(set (reg:SI 2 v0) (reg:SI 2 v0))` before shl.
  {
    RtlControlFlowGraph cfg = make_tablejump_cfg(/*index_live_in_target=*/false);
    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    EXPECT_TRUE(
        absl::StrContains(cfg.rpo_blocks[0].instructions[1], "(set (reg:SI 2 v0) (reg:SI 2 v0))"))
        << cfg.FormatSummary();
  }

  // Negative: when `$2 v0` is live into jump-table target `B1`, no self-move is inserted.
  {
    RtlControlFlowGraph cfg = make_tablejump_cfg(/*index_live_in_target=*/true);
    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    EXPECT_FALSE(absl::StrContains(cfg.FormatSummary(), "(set (reg:SI 2 v0) (reg:SI 2 v0))"))
        << cfg.FormatSummary();
  }
}

TEST(ReconstructO0FinalScanTest, ReconstructsCallArgAndCallReturnSelfMovesPositiveAndNegative) {
  const CompilationProfile o0_prof{.optimize_level = 0, .c_flags = {"-O0", "-mips2"}};

  auto make_call_cfg = [](bool precomputed_arg_alu, bool direct_ret_alu_consumer) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    if (precomputed_arg_alu) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg:SI 4 a0) (plus:SI (reg:SI 16 s0) (const_int 3)))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 4 a0) (reg:SI 16 s0))");
    }
    cfg.rpo_blocks[0].AddPatternOnly(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks[0].rtl_insns.back().has_explicit_call_fusage = true;
    cfg.rpo_blocks[0].rtl_insns.back().call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
    };
    if (direct_ret_alu_consumer) {
      cfg.rpo_blocks[0].AddPatternOnly(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 5)))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
    }
    cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
    return cfg;
  };

  // Positive: precomputed `$a0` ALU arg restores `(set (reg:SI 4 a0) (reg:SI 4 a0))`, and direct
  // ALU consumer of call return `$v0` restores `(set (reg:SI 2 v0) (reg:SI 2 v0))`.
  {
    RtlControlFlowGraph cfg =
        make_call_cfg(/*precomputed_arg_alu=*/true, /*direct_ret_alu_consumer=*/true);
    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    const std::string summary = cfg.FormatSummary();
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SI 4 a0) (reg:SI 4 a0))")) << summary;
    EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SI 2 v0) (reg:SI 2 v0))")) << summary;
  }

  // Negative: plain reg-to-reg `$a0` setup inside the call load window and unused call return
  // insert neither self-move.
  {
    RtlControlFlowGraph cfg =
        make_call_cfg(/*precomputed_arg_alu=*/false, /*direct_ret_alu_consumer=*/false);
    ReconstructO0FinalScanDeletions(&cfg, o0_prof);
    const std::string summary = cfg.FormatSummary();
    EXPECT_FALSE(absl::StrContains(summary, "(set (reg:SI 4 a0) (reg:SI 4 a0))")) << summary;
    EXPECT_FALSE(absl::StrContains(summary, "(set (reg:SI 2 v0) (reg:SI 2 v0))")) << summary;
  }
}

TEST(ReconstructO0FinalScanTest, ReconstructsPrologueAndObeyRegdeclsScopeUses) {
  const CompilationProfile o0_prof{.optimize_level = 0, .c_flags = {"-O0", "-mips2"}};

  // `-O0` function with `$fp` frame prologue (`addiu $sp, -32; sw $fp, 24($sp); sw $s0, 20($sp);
  // move $fp, $sp`) and a callee-saved register variable `$s0` (`16`) live across a call:
  // `AnalyzeStage0O0PrologueAndTopSpills` skips the prologue saves and `obey_regdecls` scope
  // reconstruction preserves prologue integrity and emits scope `(use ...)` markers cleanly.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband.frame_pointer_needed = true;
  cfg.sideband.frame_size_bytes = 32;
  cfg.sideband.saved_gpr_regnos = {16, 30, 31};
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 31 ra))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 30 $fp))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 16 s0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 30 $fp) (reg:SI 29 sp))");
  // Body: register variable in `$s0`, second local in `$s1` (wait: use `$s0` only, across call).
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 16 s0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"work\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0)))");
  cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 29 sp) (reg:SI 30 $fp))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 30 $fp) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))");
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 32)))");
  cfg.rpo_blocks[0].AddPatternOnly("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  ReconstructO0FinalScanDeletions(&cfg, o0_prof);
  const std::string summary = cfg.FormatSummary();
  // Prologue `sp = sp - 32` must remain at index 0 untouched, and call return self-move + no
  // spurious unreferenced register `(use (reg:SI 17 s1))` should be injected.
  EXPECT_EQ(cfg.rpo_blocks[0].instructions[0],
            "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))");
  EXPECT_TRUE(absl::StrContains(summary, "(set (reg:SI 2 v0) (reg:SI 2 v0))")) << summary;
  EXPECT_FALSE(absl::StrContains(summary, "(use (reg:SI 17 s1))")) << summary;
}

}  // namespace
}  // namespace rom_nom_nom::lifter
