#include "lifter/invert_alter_reg.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

void SyncAllBlocks(RtlControlFlowGraph* cfg) {
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    blk.SyncRtlInsnsFromInstructions();
  }
}

bool IsValidRecoveredHardRegHome(int hard_reg) {
  return hard_reg == -1 || (hard_reg >= 2 && hard_reg <= 25) || (hard_reg >= 32 && hard_reg <= 63);
}

int CountIntegratedRegNodesInExpr(const RtlExpr& expr) {
  int count = (expr.op == RtlOp::kReg && expr.flags.integrated && expr.RegNo() < 68) ? 1 : 0;
  for (const RtlExpr& child : expr.children) {
    count += CountIntegratedRegNodesInExpr(child);
  }
  return count;
}

int CountCallHardReturnDestNodesInExpr(const RtlExpr& expr) {
  if (expr.op == RtlOp::kSet && expr.children.size() == 2 && expr.children[1].op == RtlOp::kCall &&
      expr.children[0].op == RtlOp::kReg && expr.children[0].RegNo() < 68) {
    return 1;
  }
  int count = 0;
  for (const RtlExpr& child : expr.children) {
    count += CountCallHardReturnDestNodesInExpr(child);
  }
  return count;
}

int CountIntegratedRegNodesInCfg(const RtlControlFlowGraph& cfg) {
  int total = 0;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& insn : blk.instructions) {
      auto parsed = ParsePatternAndNotes(insn);
      if (parsed.ok()) {
        total += CountIntegratedRegNodesInExpr(parsed->pattern);
      }
    }
  }
  return total;
}

int CountCallHardReturnDestNodesInCfg(const RtlControlFlowGraph& cfg) {
  int total = 0;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& insn : blk.instructions) {
      auto parsed = ParsePatternAndNotes(insn);
      if (parsed.ok()) {
        total += CountCallHardReturnDestNodesInExpr(parsed->pattern);
      }
    }
  }
  return total;
}

bool IsEntryArgHardRegWord(int r) {
  return (r >= 4 && r <= 7) || (r >= 44 && r <= 47);
}

int ModeRegSpanO32(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kDI:
    case RtlMachineMode::kDF:
      return 2;
    case RtlMachineMode::kTI:
    case RtlMachineMode::kTF:
      return 4;
    default:
      return 1;
  }
}

void VerifyExprUpwardExposedEntryArgReads(const RtlExpr& fresh_expr, const RtlExpr& lifted_expr,
                                          const std::array<bool, 68>& killed,
                                          std::string_view test_name) {
  if (fresh_expr.op == RtlOp::kSubreg && !fresh_expr.children.empty() &&
      fresh_expr.children[0].op == RtlOp::kReg) {
    const RtlExpr& inner = fresh_expr.children[0];
    const int r = inner.RegNo();
    const int full_span = ModeRegSpanO32(inner.mode);
    const int word_off = static_cast<int>(fresh_expr.int_val);
    const int sub_span = ModeRegSpanO32(fresh_expr.mode);
    const int start_w = (word_off >= 0 && word_off < full_span) ? (r + word_off) : r;
    const int check_span =
        (word_off >= 0 && word_off < full_span && sub_span < full_span) ? sub_span : full_span;
    bool touches_live_entry_arg = false;
    for (int w = start_w; w < start_w + check_span && w < 68; ++w) {
      if (w >= 0 && IsEntryArgHardRegWord(w) && !killed[w]) {
        touches_live_entry_arg = true;
      }
    }
    if (touches_live_entry_arg) {
      ASSERT_EQ(lifted_expr.op, RtlOp::kSubreg) << test_name;
      ASSERT_FALSE(lifted_expr.children.empty()) << test_name;
      ASSERT_EQ(lifted_expr.children[0].op, RtlOp::kReg) << test_name;
      EXPECT_EQ(lifted_expr.children[0].RegNo(), r)
          << "Lifted upward-exposed SUBREG entry argument read on " << test_name;
    }
    return;
  }

  if (fresh_expr.op == RtlOp::kReg) {
    const int r = fresh_expr.RegNo();
    const int span = ModeRegSpanO32(fresh_expr.mode);
    bool touches_live_entry_arg = false;
    for (int w = r; w < r + span && w < 68; ++w) {
      if (w >= 0 && IsEntryArgHardRegWord(w) && !killed[w]) {
        touches_live_entry_arg = true;
      }
    }
    if (touches_live_entry_arg) {
      ASSERT_EQ(lifted_expr.op, RtlOp::kReg) << test_name;
      EXPECT_EQ(lifted_expr.RegNo(), r)
          << "Lifted upward-exposed entry argument read on " << test_name;
    }
    return;
  }

  if (fresh_expr.children.size() != lifted_expr.children.size()) {
    return;
  }
  for (size_t i = 0; i < fresh_expr.children.size(); ++i) {
    VerifyExprUpwardExposedEntryArgReads(fresh_expr.children[i], lifted_expr.children[i], killed,
                                         test_name);
  }
}

void ApplyPatternKillsO32(const RtlExpr& pat, std::array<bool, 68>* killed) {
  bool has_call = false;
  pat.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kCall) {
      has_call = true;
    }
  });
  if (has_call) {
    for (int r = 0; r <= 15; ++r) (*killed)[r] = true;
    for (int r = 24; r <= 29; ++r) (*killed)[r] = true;
    (*killed)[31] = true;
    for (int r = 32; r <= 51; ++r) (*killed)[r] = true;
  }

  auto kill_dest = [&](const RtlExpr& raw_dst) {
    const RtlExpr* dst = &raw_dst;
    while (dst->op == RtlOp::kStrictLowPart || dst->op == RtlOp::kZeroExtract ||
           dst->op == RtlOp::kSignExtract) {
      if (dst->children.empty()) return;
      dst = &dst->children[0];
    }
    if (dst->op == RtlOp::kSubreg && !dst->children.empty() && dst->children[0].op == RtlOp::kReg) {
      const RtlExpr& inner = dst->children[0];
      const int r = inner.RegNo();
      const int full_span = ModeRegSpanO32(inner.mode);
      const int word_off = static_cast<int>(dst->int_val);
      const int sub_span = ModeRegSpanO32(dst->mode);
      if (r >= 0 && r < 68) {
        if (word_off >= 0 && word_off < full_span && sub_span < full_span) {
          for (int w = r + word_off; w < r + word_off + sub_span && w < 68; ++w) {
            (*killed)[w] = true;
          }
        } else {
          for (int w = r; w < r + full_span && w < 68; ++w) {
            (*killed)[w] = true;
          }
        }
      }
      return;
    }
    if (dst->op == RtlOp::kReg && dst->RegNo() >= 0 && dst->RegNo() < 68) {
      const int r = dst->RegNo();
      const int span = ModeRegSpanO32(dst->mode);
      for (int w = r; w < r + span && w < 68; ++w) {
        (*killed)[w] = true;
      }
    }
  };

  if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
    kill_dest(pat.children[0]);
  } else if (pat.op == RtlOp::kClobber && !pat.children.empty()) {
    kill_dest(pat.children[0]);
  } else if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        kill_dest(elem.children[0]);
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
        kill_dest(elem.children[0]);
      }
    }
  }
}

void VerifySinglePatternEntryArgReadsAndStep(const RtlExpr& fresh_pat, const RtlExpr& lifted_pat,
                                             std::array<bool, 68>* killed,
                                             std::string_view test_name) {
  auto check_dest_reads = [&](const RtlExpr& f_dst, const RtlExpr& l_dst) {
    if (f_dst.op == RtlOp::kMem || f_dst.op == RtlOp::kStrictLowPart ||
        f_dst.op == RtlOp::kZeroExtract || f_dst.op == RtlOp::kSignExtract) {
      VerifyExprUpwardExposedEntryArgReads(f_dst, l_dst, *killed, test_name);
    }
  };

  if (fresh_pat.op == RtlOp::kSet && fresh_pat.children.size() == 2 &&
      lifted_pat.op == RtlOp::kSet && lifted_pat.children.size() == 2) {
    check_dest_reads(fresh_pat.children[0], lifted_pat.children[0]);
    VerifyExprUpwardExposedEntryArgReads(fresh_pat.children[1], lifted_pat.children[1], *killed,
                                         test_name);
  } else if (fresh_pat.op == RtlOp::kParallel && lifted_pat.op == RtlOp::kParallel &&
             fresh_pat.children.size() == lifted_pat.children.size()) {
    for (size_t k = 0; k < fresh_pat.children.size(); ++k) {
      const RtlExpr& fe = fresh_pat.children[k];
      const RtlExpr& le = lifted_pat.children[k];
      if (fe.op == RtlOp::kSet && fe.children.size() == 2 && le.op == RtlOp::kSet &&
          le.children.size() == 2) {
        check_dest_reads(fe.children[0], le.children[0]);
        VerifyExprUpwardExposedEntryArgReads(fe.children[1], le.children[1], *killed, test_name);
      } else if (fe.op == RtlOp::kClobber && !fe.children.empty() && le.op == RtlOp::kClobber &&
                 !le.children.empty()) {
        check_dest_reads(fe.children[0], le.children[0]);
      } else if (fe.op != RtlOp::kClobber) {
        VerifyExprUpwardExposedEntryArgReads(fe, le, *killed, test_name);
      }
    }
  } else if (fresh_pat.op != RtlOp::kClobber) {
    VerifyExprUpwardExposedEntryArgReads(fresh_pat, lifted_pat, *killed, test_name);
  }

  ApplyPatternKillsO32(fresh_pat, killed);
}

void ExpectBlockZeroUpwardExposedEntryArgReadsPreserved(const RtlControlFlowGraph& fresh_greg,
                                                        const RtlControlFlowGraph& lifted_cfg,
                                                        std::string_view test_name) {
  if (fresh_greg.rpo_blocks.empty() || lifted_cfg.rpo_blocks.empty()) {
    return;
  }
  ASSERT_EQ(fresh_greg.rpo_blocks[0].instructions.size(),
            lifted_cfg.rpo_blocks[0].instructions.size())
      << test_name;
  std::array<bool, 68> killed{};
  for (size_t i = 0; i < fresh_greg.rpo_blocks[0].instructions.size(); ++i) {
    auto f_parsed = ParsePatternAndNotes(fresh_greg.rpo_blocks[0].instructions[i]);
    auto l_parsed = ParsePatternAndNotes(lifted_cfg.rpo_blocks[0].instructions[i]);
    if (!f_parsed.ok() || !l_parsed.ok()) {
      continue;
    }
    if (f_parsed->pattern.op == RtlOp::kSequence && l_parsed->pattern.op == RtlOp::kSequence &&
        f_parsed->pattern.children.size() == l_parsed->pattern.children.size()) {
      for (size_t s = 0; s < f_parsed->pattern.children.size(); ++s) {
        if (!f_parsed->pattern.children[s].children.empty() &&
            !l_parsed->pattern.children[s].children.empty()) {
          VerifySinglePatternEntryArgReadsAndStep(f_parsed->pattern.children[s].children[0],
                                                  l_parsed->pattern.children[s].children[0],
                                                  &killed, test_name);
        }
      }
    } else {
      VerifySinglePatternEntryArgReadsAndStep(f_parsed->pattern, l_parsed->pattern, &killed,
                                              test_name);
    }
  }
}

// Subclasses `SubfunctionInverseProvider` directly (never `ToySubfunctionInverses`)
// so `RightInverseVerifier` exercises strictly the production `InvertAlterReg`.
class ProductionAlterRegProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     int pseudo_regno) const override {
    return ::rom_nom_nom::lifter::InvertAlterReg(allocated_cfg_x, pseudo_regno);
  }

  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     const CompilationProfile& profile,
                                     int pseudo_regno) const override {
    return ::rom_nom_nom::lifter::InvertAlterReg(allocated_cfg_x, profile, pseudo_regno);
  }
};

class CorruptedAlterRegProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     const CompilationProfile& profile,
                                     int pseudo_regno) const override {
    RtlControlFlowGraph bad =
        ::rom_nom_nom::lifter::InvertAlterReg(allocated_cfg_x, profile, pseudo_regno);
    bad.sideband.reg_renumber[pseudo_regno] = 8;
    return bad;
  }

  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     int pseudo_regno) const override {
    RtlControlFlowGraph bad = ::rom_nom_nom::lifter::InvertAlterReg(allocated_cfg_x, pseudo_regno);
    bad.sideband.reg_renumber[pseudo_regno] = 8;
    return bad;
  }
};

class InvertAlterRegTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hooks_.ApplyCompilationProfile(CompilationProfile{.optimize_level = 2, .c_flags = {"-O2"}});
  }

  GccContext ctx_;
  FunctionHooks hooks_{&ctx_};
  ProductionAlterRegProvider provider_;
  RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_F(InvertAlterRegTest, ReversesSingleGprAndPreservesIncomingArgAndReturnReg) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1], "(set (reg/i:SI 2 v0) (reg:SI 72))");
  ASSERT_TRUE(lifted.sideband.reg_renumber.contains(72));
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 2);
  EXPECT_GE(lifted.sideband.max_regno, 73);

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertAlterRegTest,
       SplitsDisjointScratchLifetimesInSameBlockAndAcrossCallIntoDistinctPseudos) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              // First lifetime in `$t0` (`8`).
              "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
              "(set (reg:SI 16 s0) (ashift:SI (reg:SI 8 t0) (const_int 2))) "
              "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
              // Second completely disjoint lifetime in `$t0` (`8`) before call.
              "(set (reg:SI 8 t0) (minus:SI (reg:SI 5 a1) (const_int 3)))",
              "(set (reg:SI 17 s1) (plus:SI (reg:SI 16 s0) (reg:SI 8 t0))) "
              "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
              // Call clobbering `$t0` (`8`) and `$v0` (`2`), consuming outgoing `$a0`.
              "(set (reg:SI 4 a0) (reg:SI 17 s1))",
              "(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 0)))",
              "(set (reg:SI 18 s2) (reg:SI 2 v0))",
              // Third disjoint lifetime in `$t0` (`8`) after call.
              "(set (reg:SI 8 t0) (plus:SI (reg:SI 18 s2) (const_int 9)))",
              "(set (reg/i:SI 2 v0) (reg:SI 8 t0)) "
              "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);
  auto use_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  ASSERT_TRUE(use_a0.ok());
  cfg.rpo_blocks[0].rtl_insns[5].has_explicit_call_fusage = true;
  cfg.rpo_blocks[0].rtl_insns[5].call_fusage = {use_a0->pattern};

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);

  // Count distinct pseudos assigned to `$t0` (`8`): must be 3 separate webs!
  std::vector<int> t0_pseudos;
  for (const auto& [pseudo, hard] : lifted.sideband.reg_renumber) {
    if (hard == 8) {
      t0_pseudos.push_back(pseudo);
    }
  }
  ASSERT_EQ(t0_pseudos.size(), 3u);

  // Outgoing `$a0` setup (`insn 4`), `CALL_INSN` return `(reg:SI 2 v0)` (`insn 5`),
  // and immediate copy-out RHS (`insn 6`) must remain hardware ABI registers.
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[4], "(set (reg:SI 4 a0) (reg:SI 75))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[5],
            "(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 0)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[6], "(set (reg:SI 76) (reg:SI 2 v0))");

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertAlterRegTest, UnifiesDiamondPhiJoinAndLoopLatchWebWhileSplittingPostLoopRedefinition) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks = {
      // Block 0: branch into diamond (`2` taken vs `1` fallthrough).
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (pc) (if_then_else (ne (reg:SI 4 a0) (const_int 0)) "
                  "(label_ref L0) (pc)))",
              },
          .successors = {2, 1},
      },
      // Block 1: diamond arm A defines `$t0` (`8`).
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:SI 8 t0) (plus:SI (reg:SI 5 a1) (const_int 10)))",
                  "(set (pc) (label_ref L1))",
              },
          .successors = {3},
      },
      // Block 2: diamond arm B defines `$t0` (`8`).
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(set (reg:SI 8 t0) (minus:SI (reg:SI 5 a1) (const_int 10)))",
              },
          .successors = {3},
          .has_label = true,
      },
      // Block 3: loop header + latch consuming diamond-joined `$t0` and updating `$t0`.
      RtlBasicBlock{
          .id = 3,
          .instructions =
              {
                  "(note NOTE_INSN_LOOP_BEG)",
                  "(set (reg:SI 8 t0) (plus:SI (reg:SI 8 t0) (const_int -1)))",
                  "(set (pc) (if_then_else (gt (reg:SI 8 t0) (const_int 0)) "
                  "(label_ref L1) (pc)))",
              },
          .successors = {3, 4},
          .has_label = true,
      },
      // Block 4: post-loop exit block consumes loop result in `$t0`, then reuses `$t0`
      // for a fresh independent computation.
      RtlBasicBlock{
          .id = 4,
          .instructions =
              {
                  "(note NOTE_INSN_LOOP_END)",
                  "(set (reg:SI 9 t1) (ashift:SI (reg:SI 8 t0) (const_int 1))) "
                  "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                  "(set (reg:SI 8 t0) (plus:SI (reg:SI 9 t1) (const_int 42)))",
                  "(set (reg/i:SI 2 v0) (reg:SI 8 t0))",
                  "(use (reg/i:SI 2 v0))",
              },
          .successors = {},
      },
  };
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 5u);

  // Diamond arm A, diamond arm B, loop latch, and first read in block 4 must all
  // unify into pseudo `72`, while the post-kill redefinition in block 4 splits into `74`!
  ASSERT_TRUE(lifted.sideband.reg_renumber.contains(72));
  ASSERT_TRUE(lifted.sideband.reg_renumber.contains(73));
  ASSERT_TRUE(lifted.sideband.reg_renumber.contains(74));
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 8);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 9);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(74), 8);

  EXPECT_EQ(lifted.rpo_blocks[1].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 5 a1) (const_int 10)))");
  EXPECT_EQ(lifted.rpo_blocks[2].instructions[0],
            "(set (reg:SI 72) (minus:SI (reg:SI 5 a1) (const_int 10)))");
  EXPECT_EQ(lifted.rpo_blocks[3].instructions[1],
            "(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int -1)))");
  EXPECT_EQ(lifted.rpo_blocks[4].instructions[1],
            "(set (reg:SI 73) (ashift:SI (reg:SI 72) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[4].instructions[2],
            "(set (reg:SI 74) (plus:SI (reg:SI 73) (const_int 42)))");
  EXPECT_EQ(lifted.rpo_blocks[4].instructions[3], "(set (reg/i:SI 2 v0) (reg:SI 74))");

  // Loop-depth weighting: inside loop depth 2, 3 refs * 2 = 6, plus 3 outer refs = 9.
  EXPECT_EQ(lifted.sideband.reg_n_refs.at(72), 9);
  EXPECT_EQ(lifted.sideband.reg_n_sets.at(72), 3);

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, PreservesEntryArgAndLiftsSubsequentLocalOverwriteOfSameArgRegister) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              // RHS reads incoming ABI `$a0`; LHS starts a local scratch web in `$a0`!
              "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 5)))",
              "(set (reg:SI 4 a0) (ashift:SI (reg:SI 4 a0) (const_int 1)))",
              "(set (reg/i:SI 2 v0) (reg:SI 4 a0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 4);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 4);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 5)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (ashift:SI (reg:SI 72) (const_int 1)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2], "(set (reg/i:SI 2 v0) (reg:SI 73))");

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, UnifiesMultiWordSubregWritesAndSplitsIndependentDiModeLifetimes) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (subreg:SI (reg:DI 8 t0) 0) (reg:SI 4 a0))",
              "(set (subreg:SI (reg:DI 8 t0) 1) (reg:SI 5 a1))",
              "(set (reg:DI 16 s0) (reg:DI 8 t0))",
              "(set (subreg:SI (reg:DI 8 t0) 0) (reg:SI 6 a2))",
              "(set (subreg:SI (reg:DI 8 t0) 1) (reg:SI 7 a3))",
              "(set (reg/i:DI 2 v0) (plus:DI (reg:DI 16 s0) (reg:DI 8 t0)))",
              "(use (reg/i:DI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 3u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 8);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 16);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(74), 8);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0], "(set (subreg:SI (reg:DI 72) 0) (reg:SI 4 a0))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1], "(set (subreg:SI (reg:DI 72) 1) (reg:SI 5 a1))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2], "(set (reg:DI 73) (reg:DI 72))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[3], "(set (subreg:SI (reg:DI 74) 0) (reg:SI 6 a2))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[4], "(set (subreg:SI (reg:DI 74) 1) (reg:SI 7 a3))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[5],
            "(set (reg/i:DI 2 v0) (plus:DI (reg:DI 73) (reg:DI 74)))");

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, ReversesMultipleGprsAndFprsAcrossBlocks) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (reg/v:SI 16 s0) (plus:SI (reg:SI 4 a0) (const_int 5)))",
                  "(set (reg:SI 8 t0) (gt:SI (reg/v:SI 16 s0) (const_int 10)))",
                  "(set (pc) (if_then_else (ne (reg:SI 8 t0) (const_int 0)) "
                  "(label_ref L0) (pc)))",
              },
          .successors = {2, 1},
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:DF 32 $f0) (plus:DF (reg:DF 44 $f12) (reg:DF 44 $f12)))",
                  "(set (reg/v:SI 16 s0) (minus:SI (reg/v:SI 16 s0) (const_int 1)))",
              },
          .successors = {2},
      },
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(set (reg/i:SI 2 v0) (reg/v:SI 16 s0))",
                  "(use (reg/i:SI 2 v0))",
              },
          .successors = {},
          .has_label = true,
      },
  };
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  EXPECT_EQ(lifted.sideband.reg_renumber.size(), 3u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 16);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 8);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(74), 32);

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, SeparatesSameHardRegWithDistinctFlagsOrModesIntoDistinctPseudos) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg/v:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3)))",
              "(set (reg:SI 2 v0) (ashift:SI (reg/v:SI 2 v0) (const_int 2))) "
              "(expr_list:REG_DEAD (reg/v:SI 2 v0) (nil))",
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 2);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 2);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg/v:SI 72) (plus:SI (reg:SI 4 a0) (const_int 3)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (ashift:SI (reg/v:SI 72) (const_int 2))) "
            "(expr_list:REG_DEAD (reg/v:SI 72) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2], "(set (reg/i:SI 2 v0) (reg:SI 73))");

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertAlterRegTest,
       PinsMixedSignatureMultiWordAndSubwordHardRegisterOverlapsWithoutSeveredDefs) {
  // 1. `(reg:DI 8 t0)` def read via lowered `(reg:SI 8 t0)` and `(reg:SI 9 t1)`
  //    occurrences: both the `DImode` def and `SImode` reads must stay pinned on
  //    physical hard registers with zero `reg_n_sets == 0` pseudos.
  RtlControlFlowGraph di_to_si_cfg;
  di_to_si_cfg.pass = RtlPassKind::kGreg;
  di_to_si_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 8 t0) (reg:DI 4 a0))",
              "(set (reg:SI 16 s0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1)))",
              "(set (reg/i:SI 2 v0) (reg:SI 16 s0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&di_to_si_cfg);

  const RtlControlFlowGraph lifted_di_to_si = InvertAlterReg(di_to_si_cfg, 72);
  ASSERT_EQ(lifted_di_to_si.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted_di_to_si.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_di_to_si.sideband.reg_renumber.at(72), 16);
  EXPECT_GE(lifted_di_to_si.sideband.reg_n_sets.at(72), 1);
  EXPECT_EQ(lifted_di_to_si.rpo_blocks[0].instructions[0], "(set (reg:DI 8 t0) (reg:DI 4 a0))");
  EXPECT_EQ(lifted_di_to_si.rpo_blocks[0].instructions[1],
            "(set (reg:SI 72) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1)))");
  EXPECT_TRUE(verifier_.VerifyAlterRegRightInverse(di_to_si_cfg).matches);

  // 2. `(reg:SI 8 t0)` + `(reg:SI 9 t1)` defs read together as `(reg:DI 8 t0)`:
  //    all overlapping defs and read must stay pinned on physical hard registers.
  RtlControlFlowGraph si_to_di_cfg;
  si_to_di_cfg.pass = RtlPassKind::kGreg;
  si_to_di_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
              "(set (reg:SI 9 t1) (plus:SI (reg:SI 5 a1) (const_int 2)))",
              "(set (reg:DI 16 s0) (reg:DI 8 t0))",
              "(set (reg/i:DI 2 v0) (reg:DI 16 s0))",
              "(use (reg/i:DI 2 v0))",
          },
  });
  SyncAllBlocks(&si_to_di_cfg);

  const RtlControlFlowGraph lifted_si_to_di = InvertAlterReg(si_to_di_cfg, 72);
  ASSERT_EQ(lifted_si_to_di.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted_si_to_di.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_si_to_di.sideband.reg_renumber.at(72), 16);
  EXPECT_GE(lifted_si_to_di.sideband.reg_n_sets.at(72), 1);
  EXPECT_EQ(lifted_si_to_di.rpo_blocks[0].instructions[2], "(set (reg:DI 72) (reg:DI 8 t0))");
  EXPECT_TRUE(verifier_.VerifyAlterRegRightInverse(si_to_di_cfg).matches);

  // 3. Partial subword overwrite `(set (reg:SI 9 t1) ...)` while word 0 of
  //    `(reg:DI 8 t0)` is still live and read afterward via `SUBREG`: pins the
  //    `DImode` def, `SImode` overwriter, and `SUBREG` reader so no `DImode`
  //    pseudo interferes with `(reg:SI 9 t1)`.
  RtlControlFlowGraph live_partial_overwrite_cfg;
  live_partial_overwrite_cfg.pass = RtlPassKind::kGreg;
  live_partial_overwrite_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 8 t0) (reg:DI 4 a0))",
              "(set (reg:SI 9 t1) (plus:SI (reg:SI 6 a2) (const_int 5)))",
              "(set (reg:SI 16 s0) (plus:SI (subreg:SI (reg:DI 8 t0) 0) (reg:SI 9 t1)))",
              "(set (reg/i:SI 2 v0) (reg:SI 16 s0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&live_partial_overwrite_cfg);

  const RtlControlFlowGraph lifted_partial = InvertAlterReg(live_partial_overwrite_cfg, 72);
  ASSERT_EQ(lifted_partial.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted_partial.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_partial.sideband.reg_renumber.at(72), 16);
  EXPECT_EQ(lifted_partial.rpo_blocks[0].instructions[2],
            "(set (reg:SI 72) (plus:SI (subreg:SI (reg:DI 8 t0) 0) (reg:SI 9 t1)))");
  EXPECT_TRUE(verifier_.VerifyAlterRegRightInverse(live_partial_overwrite_cfg).matches);

  // 4. Contrast: when `(reg:DI 8 t0)` is already dead before `(set (reg:SI 9 t1) ...)`,
  //    neither web is pinned—both lift cleanly to independent pseudos `72` and `74`.
  RtlControlFlowGraph dead_di_then_si_cfg;
  dead_di_then_si_cfg.pass = RtlPassKind::kGreg;
  dead_di_then_si_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 8 t0) (reg:DI 4 a0))",
              "(set (reg:SI 16 s0) (subreg:SI (reg:DI 8 t0) 1)) "
              "(expr_list:REG_DEAD (reg:DI 8 t0) (nil))",
              "(set (reg:SI 9 t1) (plus:SI (reg:SI 6 a2) (const_int 5)))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 9 t1)))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&dead_di_then_si_cfg);

  const RtlControlFlowGraph lifted_dead_di = InvertAlterReg(dead_di_then_si_cfg, 72);
  ASSERT_EQ(lifted_dead_di.sideband.reg_renumber.size(), 3u);
  EXPECT_EQ(lifted_dead_di.sideband.reg_renumber.at(72), 8);
  EXPECT_EQ(lifted_dead_di.sideband.reg_renumber.at(73), 16);
  EXPECT_EQ(lifted_dead_di.sideband.reg_renumber.at(74), 9);
  EXPECT_TRUE(verifier_.VerifyAlterRegRightInverse(dead_di_then_si_cfg).matches);

  // 5. Uninitialized read-modify-write local initialization (e.g. byte/halfword
  //    lane insertion into a union local) unifies the initial upward-exposed
  //    masking read with the subsequent write into ONE pseudo (`reg_n_sets >= 1`),
  //    whereas reading a scratch hard register clobbered by a `PARALLEL` stays
  //    pinned on the physical hard register without inventing a `0`-set pseudo.
  RtlControlFlowGraph rmw_and_scratch_cfg;
  rmw_and_scratch_cfg.pass = RtlPassKind::kGreg;
  rmw_and_scratch_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:SI 8 t0) (and:SI (reg:SI 3 v1) (const_int 65535)))",
              "(set (reg:SI 3 v1) (ior:SI (reg:SI 8 t0) (ashift:SI (reg:SI 4 a0) (const_int 16))))",
              "(parallel[ (set (reg:DI 10 t2) (plus:DI (reg:DI 6 a2) (const_int 1))) "
              "(clobber (reg:SI 9 t1)) ] )",
              "(set (mem:QI (plus:SI (reg:SI 5 a1) (const_int 4))) (subreg:QI (reg:SI 9 t1) 0))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (subreg:SI (reg:DI 10 t2) 1)))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&rmw_and_scratch_cfg);

  const RtlControlFlowGraph lifted_rmw = InvertAlterReg(rmw_and_scratch_cfg, 72);
  ASSERT_EQ(lifted_rmw.sideband.reg_renumber.size(), 3u);
  EXPECT_EQ(lifted_rmw.sideband.reg_renumber.at(72), 8);
  EXPECT_EQ(lifted_rmw.sideband.reg_renumber.at(73), 3);
  EXPECT_EQ(lifted_rmw.sideband.reg_renumber.at(74), 10);
  EXPECT_GE(lifted_rmw.sideband.reg_n_sets.at(72), 1);
  EXPECT_GE(lifted_rmw.sideband.reg_n_sets.at(73), 1);
  EXPECT_GE(lifted_rmw.sideband.reg_n_sets.at(74), 1);
  EXPECT_EQ(lifted_rmw.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (and:SI (reg:SI 73) (const_int 65535)))");
  EXPECT_EQ(lifted_rmw.rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (ior:SI (reg:SI 72) (ashift:SI (reg:SI 4 a0) (const_int 16))))");
  EXPECT_EQ(lifted_rmw.rpo_blocks[0].instructions[3],
            "(set (mem:QI (plus:SI (reg:SI 5 a1) (const_int 4))) (subreg:QI (reg:SI 9 t1) 0))");
  const RightInverseResult rmw_res = verifier_.VerifyAlterRegRightInverse(rmw_and_scratch_cfg);
  EXPECT_TRUE(rmw_res.matches) << rmw_res.diff_detail;
}

TEST_F(InvertAlterRegTest,
       LiftsEmitNoConflictBlockBothUntiedAndLocalAllocTiedWithStandaloneClobberAndNotes) {
  // 1. Untied `emit_no_conflict_block` (`op0` in `$10`, `target` in `$8`).
  RtlControlFlowGraph raw_untied_cfg;
  raw_untied_cfg.pass = RtlPassKind::kGreg;
  raw_untied_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 10 t2) (reg:DI 4 a0))",
              "(clobber (reg:DI 8 t0)) (insn_list:REG_LIBCALL 0 (nil))",
              "(set (subreg:SI (reg:DI 8 t0) 0) (not:SI (subreg:SI (reg:DI 10 t2) 0))) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 10 t2) (nil))",
              "(set (subreg:SI (reg:DI 8 t0) 1) (not:SI (subreg:SI (reg:DI 10 t2) 1))) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 10 t2) "
              "(expr_list:REG_DEAD (reg:DI 10 t2) (nil)))",
              "(set (reg:DI 8 t0) (reg:DI 8 t0)) "
              "(insn_list:REG_RETVAL 0 (expr_list:REG_EQUAL (not:DI (reg:DI 10 t2)) (nil)))",
              "(set (reg/i:DI 2 v0) (reg:DI 8 t0)) (expr_list:REG_DEAD (reg:DI 8 t0) (nil))",
              "(use (reg/i:DI 2 v0))",
          },
  });
  SyncAllBlocks(&raw_untied_cfg);
  ctx_.MaterializeFromCfg(raw_untied_cfg);
  const RtlControlFlowGraph untied_cfg =
      ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_untied = InvertAlterReg(untied_cfg, 72);
  ASSERT_EQ(lifted_untied.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted_untied.sideband.reg_renumber.at(72), 10);
  EXPECT_EQ(lifted_untied.sideband.reg_renumber.at(73), 8);
  EXPECT_EQ(lifted_untied.sideband.reg_n_sets.at(73), 3);
  auto untied_clob = ParsePatternAndNotes(lifted_untied.rpo_blocks[0].instructions[1]);
  ASSERT_TRUE(untied_clob.ok());
  EXPECT_EQ(untied_clob->pattern.ToSexprString(), "(clobber (reg:DI 73))");
  auto untied_ret = ParsePatternAndNotes(lifted_untied.rpo_blocks[0].instructions[4]);
  ASSERT_TRUE(untied_ret.ok());
  EXPECT_EQ(untied_ret->pattern.ToSexprString(), "(set (reg:DI 73) (reg:DI 73))");
  ASSERT_EQ(untied_ret->reg_notes.size(), 2u);
  EXPECT_EQ(untied_ret->reg_notes[1].datum.ToSexprString(), "(not:DI (reg:DI 72))");
  const RightInverseResult untied_res = verifier_.VerifyAlterRegRightInverse(untied_cfg);
  EXPECT_TRUE(untied_res.matches) << untied_res.diff_detail;

  // 2. `local_alloc`-tied `emit_no_conflict_block` (`op0` AND `target` both in `$8`),
  //    including a subword constant write where `REG_NO_CONFLICT` has no read in `read_occs`.
  RtlControlFlowGraph raw_tied_cfg;
  raw_tied_cfg.pass = RtlPassKind::kGreg;
  raw_tied_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 8 t0) (reg:DI 4 a0))",
              "(clobber (reg:DI 8 t0)) (insn_list:REG_LIBCALL 0 (nil))",
              "(set (subreg:SI (reg:DI 8 t0) 0) (const_int -1)) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 8 t0) (nil))",
              "(set (subreg:SI (reg:DI 8 t0) 1) (not:SI (subreg:SI (reg:DI 8 t0) 1))) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 8 t0) "
              "(expr_list:REG_DEAD (reg:DI 8 t0) (nil)))",
              "(set (reg:DI 8 t0) (reg:DI 8 t0)) "
              "(insn_list:REG_RETVAL 0 "
              "(expr_list:REG_EQUAL (ior:DI (not:DI (reg:DI 8 t0)) (const_int -1)) "
              "(nil)))",
              "(set (reg/i:DI 2 v0) (reg:DI 8 t0)) (expr_list:REG_DEAD (reg:DI 8 t0) (nil))",
              "(use (reg/i:DI 2 v0))",
          },
  });
  SyncAllBlocks(&raw_tied_cfg);
  ctx_.MaterializeFromCfg(raw_tied_cfg);
  const RtlControlFlowGraph tied_cfg = ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_tied = InvertAlterReg(tied_cfg, 72);
  ASSERT_EQ(lifted_tied.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted_tied.sideband.reg_renumber.at(72), 8);
  EXPECT_EQ(lifted_tied.sideband.reg_renumber.at(73), 8);
  EXPECT_EQ(lifted_tied.rpo_blocks[0].instructions[0], "(set (reg:DI 72) (reg:DI 4 a0))");
  auto tied_clob = ParsePatternAndNotes(lifted_tied.rpo_blocks[0].instructions[1]);
  ASSERT_TRUE(tied_clob.ok());
  EXPECT_EQ(tied_clob->pattern.ToSexprString(), "(clobber (reg:DI 73))");
  EXPECT_EQ(lifted_tied.rpo_blocks[0].instructions[2],
            "(set (subreg:SI (reg:DI 73) 0) (const_int -1)) "
            "(expr_list:REG_NO_CONFLICT (reg:DI 72) (nil))");
  EXPECT_EQ(lifted_tied.rpo_blocks[0].instructions[3],
            "(set (subreg:SI (reg:DI 73) 1) (not:SI (subreg:SI (reg:DI 72) 1))) "
            "(expr_list:REG_NO_CONFLICT (reg:DI 72) "
            "(expr_list:REG_DEAD (reg:DI 72) (nil)))");
  auto tied_ret = ParsePatternAndNotes(lifted_tied.rpo_blocks[0].instructions[4]);
  ASSERT_TRUE(tied_ret.ok());
  EXPECT_EQ(tied_ret->pattern.ToSexprString(), "(set (reg:DI 73) (reg:DI 73))");
  ASSERT_EQ(tied_ret->reg_notes.size(), 2u);
  EXPECT_EQ(tied_ret->reg_notes[1].datum.ToSexprString(),
            "(ior:DI (not:DI (reg:DI 72)) (const_int -1))");
  EXPECT_EQ(lifted_tied.rpo_blocks[0].instructions[5],
            "(set (reg/i:DI 2 v0) (reg:DI 73)) (expr_list:REG_DEAD (reg:DI 73) (nil))");
  const RightInverseResult tied_res = verifier_.VerifyAlterRegRightInverse(tied_cfg);
  EXPECT_TRUE(tied_res.matches) << tied_res.diff_detail;
}

TEST_F(InvertAlterRegTest, ReversesSingleStackSlotSpillToNegativeRegRenumber) {
  const CompilationProfile o0_profile{.optimize_level = 0, .c_flags = {"-O0"}};
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_size_bytes = 8;
  cfg.sideband.frame_pointer_needed = true;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 4))) "
              "(plus:SI (reg:SI 4 a0) (const_int 7)))",
              "(set (reg/i:SI 2 v0) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 4))))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o0_profile, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.sideband.frame_size_bytes, 0);
  ASSERT_TRUE(lifted.sideband.reg_renumber.contains(72));
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), -1);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1], "(set (reg/i:SI 2 v0) (reg:SI 72))");

  hooks_.ApplyCompilationProfile(o0_profile);
  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;

  SubfunctionBoundarySnapshot snap;
  snap.subfunction_id = SubfunctionId::kAlterReg;
  snap.optimize_level = 0;
  snap.c_flags = {"-O0"};
  snap.pre_state = lifted;
  snap.post_state = cfg;
  const RightInverseResult boundary_res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;
  hooks_.ApplyCompilationProfile(o2_profile);
}

TEST_F(InvertAlterRegTest, ReversesBigEndianSubwordAndDoublewordContiguousSpillSlots) {
  const CompilationProfile o0_profile{.optimize_level = 0, .c_flags = {"-O0"}};
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_size_bytes = 24;
  cfg.sideband.frame_pointer_needed = true;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              // Slot 0 (`0..7`): `QImode` at big-endian `+7`.
              "(set (mem:QI (plus:SI (reg:SI 30 $fp) (const_int 7))) "
              "(subreg:QI (reg:SI 4 a0) 0))",
              // Slot 1 (`8..15`): `HImode` at big-endian `+14` (`8 + 6`).
              "(set (mem:HI (plus:SI (reg:SI 30 $fp) (const_int 14))) "
              "(subreg:HI (reg:SI 5 a1) 0))",
              // Slot 2 (`16..23`): `DFmode` at `+16` (`16 + 0`).
              "(set (mem:DF (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg:DF 44 $f12))",
              "(set (reg:SI 8 t0) (sign_extend:SI "
              "(mem:QI (plus:SI (reg:SI 30 $fp) (const_int 7)))))",
              "(set (reg:SI 9 t1) (sign_extend:SI "
              "(mem:HI (plus:SI (reg:SI 30 $fp) (const_int 14)))))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1)))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o0_profile, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.sideband.frame_size_bytes, 0);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 8);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 9);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(74), -1);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(75), -1);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(76), -1);

  hooks_.ApplyCompilationProfile(o0_profile);
  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
  hooks_.ApplyCompilationProfile(o2_profile);
}

TEST_F(InvertAlterRegTest,
       DistinguishesExpandDeclLocalsEscapesFloatStoreAndOptimizedReloadSlotsFromAlterRegSpills) {
  const CompilationProfile o0_profile{.optimize_level = 0, .c_flags = {"-O0"}};
  const CompilationProfile o2_profile{.optimize_level = 2, .c_flags = {"-O2"}};

  // 1. Two 4-byte-packed `expand_decl` `int` locals at `$fp + 0` and `$fp + 4`
  //    in an 8-byte `-O0` frame MUST stay on the stack.
  RtlControlFlowGraph packed_locals_cfg;
  packed_locals_cfg.pass = RtlPassKind::kGreg;
  packed_locals_cfg.sideband.frame_size_bytes = 8;
  packed_locals_cfg.sideband.frame_pointer_needed = true;
  packed_locals_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (reg:SI 30 $fp)) (reg:SI 4 a0))",
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 4))) (reg:SI 5 a1))",
              "(set (reg:SI 8 t0) (mem:SI (reg:SI 30 $fp)))",
              "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 4))))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1)))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&packed_locals_cfg);

  const RtlControlFlowGraph lifted_packed = InvertAlterReg(packed_locals_cfg, o0_profile, 72);
  ASSERT_EQ(lifted_packed.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted_packed.sideband.frame_size_bytes, 8);
  for (const auto& [_, hard] : lifted_packed.sideband.reg_renumber) {
    EXPECT_NE(hard, -1);
  }

  // 2. Address-taken multi-slot aggregate at `$fp + 0` (`B3.2`) MUST invalidate
  //    `[0, frame_size_bytes)` upward so upper slot `8` (`$fp + 12`) is never lifted!
  RtlControlFlowGraph multi_slot_addr_cfg;
  multi_slot_addr_cfg.pass = RtlPassKind::kGreg;
  multi_slot_addr_cfg.sideband.frame_size_bytes = 16;
  multi_slot_addr_cfg.sideband.frame_pointer_needed = true;
  multi_slot_addr_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 12))) (reg:SI 4 a0))",
              "(set (reg:SI 4 a0) (reg:SI 30 $fp))",
              "(set (reg/i:SI 2 v0) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 12))))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&multi_slot_addr_cfg);

  const RtlControlFlowGraph lifted_multi_addr = InvertAlterReg(multi_slot_addr_cfg, o0_profile, 72);
  EXPECT_EQ(lifted_multi_addr.sideband.frame_size_bytes, 16);
  for (const auto& [_, hard] : lifted_multi_addr.sideband.reg_renumber) {
    EXPECT_NE(hard, -1);
  }

  // Also check `(plus:SI (reg:SI 30 $fp) (const_int 8))` in a 24-byte frame:
  // slot `0` stays valid below, while slots `8` and `16` are invalidated upward.
  RtlControlFlowGraph upward_addr_cfg;
  upward_addr_cfg.pass = RtlPassKind::kGreg;
  upward_addr_cfg.sideband.frame_size_bytes = 24;
  upward_addr_cfg.sideband.frame_pointer_needed = true;
  upward_addr_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 20))) (reg:SI 4 a0))",
              "(set (reg:SI 8 t0) (plus:SI (reg:SI 30 $fp) (const_int 8)))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) "
              "(mem:SI (plus:SI (reg:SI 30 $fp) (const_int 20)))))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&upward_addr_cfg);

  const RtlControlFlowGraph lifted_upward_addr = InvertAlterReg(upward_addr_cfg, o0_profile, 72);
  EXPECT_EQ(lifted_upward_addr.sideband.frame_size_bytes, 24);
  for (const auto& [_, hard] : lifted_upward_addr.sideband.reg_renumber) {
    EXPECT_NE(hard, -1);
  }

  // 3. Dynamic `$fp` indexing `(plus:SI (reg:SI 30 $fp) (reg:SI 8 t0))` when
  //    `outgoing_args_size == 16` (`B3.1`) MUST invalidate all slots `[0, frame_size_bytes)`.
  RtlControlFlowGraph dyn_fp_cfg;
  dyn_fp_cfg.pass = RtlPassKind::kGreg;
  dyn_fp_cfg.sideband.outgoing_args_size = 16;
  dyn_fp_cfg.sideband.frame_size_bytes = 16;
  dyn_fp_cfg.sideband.frame_pointer_needed = true;
  dyn_fp_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 28))) (reg:SI 4 a0))",
              "(set (reg:SI 8 t0) (ashift:SI (reg:SI 5 a1) (const_int 2)))",
              "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 30 $fp) (reg:SI 8 t0))))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 9 t1) "
              "(mem:SI (plus:SI (reg:SI 30 $fp) (const_int 28)))))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&dyn_fp_cfg);

  const RtlControlFlowGraph lifted_dyn_fp = InvertAlterReg(dyn_fp_cfg, o0_profile, 72);
  EXPECT_EQ(lifted_dyn_fp.sideband.frame_size_bytes, 16);
  for (const auto& [_, hard] : lifted_dyn_fp.sideband.reg_renumber) {
    EXPECT_NE(hard, -1);
  }

  // 4. `BLKmode` and cross-8-byte-chunk `MEM`s (`B3.3`) MUST invalidate every
  //    overlapped 8-byte slot in `[0, frame_size_bytes)`.
  RtlControlFlowGraph blk_and_cross_cfg;
  blk_and_cross_cfg.pass = RtlPassKind::kGreg;
  blk_and_cross_cfg.sideband.frame_size_bytes = 16;
  blk_and_cross_cfg.sideband.frame_pointer_needed = true;
  blk_and_cross_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 12))) (reg:SI 4 a0))",
              "(set (reg:DI 8 t0) (mem:DI (plus:SI (reg:SI 30 $fp) (const_int 4))))",
              "(set (reg/i:SI 2 v0) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 12))))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&blk_and_cross_cfg);

  const RtlControlFlowGraph lifted_cross = InvertAlterReg(blk_and_cross_cfg, o0_profile, 72);
  EXPECT_EQ(lifted_cross.sideband.frame_size_bytes, 16);
  for (const auto& [_, hard] : lifted_cross.sideband.reg_renumber) {
    EXPECT_NE(hard, -1);
  }

  RtlControlFlowGraph blkmode_cfg;
  blkmode_cfg.pass = RtlPassKind::kGreg;
  blkmode_cfg.sideband.frame_size_bytes = 16;
  blkmode_cfg.sideband.frame_pointer_needed = true;
  blkmode_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 12))) (reg:SI 4 a0))",
              "(set (mem:BLK (reg:SI 30 $fp)) (mem:BLK (reg:SI 5 a1)))",
              "(set (reg/i:SI 2 v0) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 12))))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&blkmode_cfg);

  const RtlControlFlowGraph lifted_blkmode = InvertAlterReg(blkmode_cfg, o0_profile, 72);
  EXPECT_EQ(lifted_blkmode.sideband.frame_size_bytes, 16);
  for (const auto& [_, hard] : lifted_blkmode.sideband.reg_renumber) {
    EXPECT_NE(hard, -1);
  }

  // 5. `-ffloat-store` (`B3.4`, last-flag-wins) forces unflagged `DFmode`/`SFmode`
  //    `expand_decl` locals onto the stack at `-O0`, so stack-slot lifting is disabled
  //    under `-ffloat-store` and re-enabled under `-ffloat-store -fno-float-store`.
  RtlControlFlowGraph float_store_cfg;
  float_store_cfg.pass = RtlPassKind::kGreg;
  float_store_cfg.sideband.frame_size_bytes = 8;
  float_store_cfg.sideband.frame_pointer_needed = true;
  float_store_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:DF (reg:SI 30 $fp)) (reg:DF 44 $f12))",
              "(set (reg:DF 34 $f2) (mem:DF (reg:SI 30 $fp)))",
              "(set (reg/i:DF 32 $f0) (reg:DF 34 $f2))",
              "(use (reg/i:DF 32 $f0))",
          },
  });
  SyncAllBlocks(&float_store_cfg);

  const CompilationProfile float_store_profile{.optimize_level = 0,
                                               .c_flags = {"-O0", "-ffloat-store"}};
  const RtlControlFlowGraph lifted_fs = InvertAlterReg(float_store_cfg, float_store_profile, 72);
  EXPECT_EQ(lifted_fs.sideband.frame_size_bytes, 8);
  ASSERT_EQ(lifted_fs.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_fs.sideband.reg_renumber.at(72), 34);

  const CompilationProfile no_float_store_profile{
      .optimize_level = 0, .c_flags = {"-O0", "-ffloat-store", "-fno-float-store"}};
  const RtlControlFlowGraph lifted_no_fs =
      InvertAlterReg(float_store_cfg, no_float_store_profile, 72);
  EXPECT_EQ(lifted_no_fs.sideband.frame_size_bytes, 0);
  ASSERT_EQ(lifted_no_fs.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted_no_fs.sideband.reg_renumber.at(72), 34);
  EXPECT_EQ(lifted_no_fs.sideband.reg_renumber.at(73), -1);

  // 6. At `opt_level > 0` (`B3.5`, e.g. `-O2 -fno-omit-frame-pointer`), `$fp`-relative
  //    stack slots belong to `reload` (`InvertReload`), NEVER `InvertAlterReg`.
  hooks_.ApplyCompilationProfile(o2_profile);
  const RtlControlFlowGraph lifted_o2_fp = InvertAlterReg(float_store_cfg, o2_profile, 72);
  EXPECT_EQ(lifted_o2_fp.sideband.frame_size_bytes, 8);
  ASSERT_EQ(lifted_o2_fp.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_o2_fp.sideband.reg_renumber.at(72), 34);
  const RightInverseResult o2_fp_res = verifier_.VerifyAlterRegRightInverse(float_store_cfg);
  EXPECT_TRUE(o2_fp_res.matches) << o2_fp_res.diff_detail;

  // 7. When `frame_pointer_needed == false`, `$30` (`$s8`) dereferences are pointer loads.
  RtlControlFlowGraph s8_ptr_cfg = float_store_cfg;
  s8_ptr_cfg.sideband.frame_pointer_needed = false;
  const RtlControlFlowGraph lifted_s8 = InvertAlterReg(s8_ptr_cfg, o0_profile, 72);
  EXPECT_EQ(lifted_s8.sideband.frame_size_bytes, 8);
  ASSERT_EQ(lifted_s8.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_s8.sideband.reg_renumber.at(72), 34);
}

TEST_F(InvertAlterRegTest, PreservesFixedStatusAndAccumulatorHardRegisters) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:CC 67 $fcr31) (eq:CC (reg:SF 44 $f12) (reg:SF 46 $f14)))",
              "(set (reg:SI 64 hi) (reg:SI 4 a0))",
              "(set (reg:SI 65 lo) (reg:SI 5 a1))",
              "(set (reg:SI 8 t0) (plus:SI (reg:SI 64 hi) (reg:SI 65 lo)))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 0 $0)))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:CC 67 $fcr31) (eq:CC (reg:SF 44 $f12) (reg:SF 46 $f14)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1], "(set (reg:SI 64 hi) (reg:SI 4 a0))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2], "(set (reg:SI 65 lo) (reg:SI 5 a1))");
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 8);

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, RejectsInvalidAndMalformedInputs) {
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertAlterReg(empty_cfg).rpo_blocks.empty());

  RtlControlFlowGraph valid_cfg;
  valid_cfg.pass = RtlPassKind::kGreg;
  valid_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions = {"(set (reg:SI 2 v0) (const_int 1))"},
  });
  SyncAllBlocks(&valid_cfg);

  // Reject `first_pseudo_regno < 68` (including `kMipsStatusRegnum == 67`).
  EXPECT_TRUE(InvertAlterReg(valid_cfg, 67).rpo_blocks.empty());
  EXPECT_TRUE(InvertAlterReg(valid_cfg, 66).rpo_blocks.empty());
  EXPECT_TRUE(InvertAlterReg(valid_cfg, -1).rpo_blocks.empty());

  // Reject mismatched block id.
  RtlControlFlowGraph bad_id_cfg = valid_cfg;
  bad_id_cfg.rpo_blocks[0].id = 3;
  EXPECT_TRUE(InvertAlterReg(bad_id_cfg).rpo_blocks.empty());

  // Reject out-of-bounds successor.
  RtlControlFlowGraph bad_succ_cfg = valid_cfg;
  bad_succ_cfg.rpo_blocks[0].successors = {1};
  EXPECT_TRUE(InvertAlterReg(bad_succ_cfg).rpo_blocks.empty());

  // Reject out-of-bounds `addr_vec_targets`.
  RtlControlFlowGraph bad_target_cfg = valid_cfg;
  bad_target_cfg.rpo_blocks[0].addr_vec_targets = {2};
  EXPECT_TRUE(InvertAlterReg(bad_target_cfg).rpo_blocks.empty());

  // Reject empty instruction string.
  RtlControlFlowGraph empty_insn_cfg = valid_cfg;
  empty_insn_cfg.rpo_blocks[0].instructions = {""};
  EXPECT_TRUE(InvertAlterReg(empty_insn_cfg).rpo_blocks.empty());

  // Reject unbalanced parentheses.
  RtlControlFlowGraph unbalanced_cfg = valid_cfg;
  unbalanced_cfg.rpo_blocks[0].instructions = {"(set (reg:SI 2 v0) (const_int 1)"};
  EXPECT_TRUE(InvertAlterReg(unbalanced_cfg).rpo_blocks.empty());

  // Reject unknown RTL opcode.
  RtlControlFlowGraph unknown_op_cfg = valid_cfg;
  unknown_op_cfg.rpo_blocks[0].instructions = {"(bogus_rtl_op (reg:SI 2 v0))"};
  EXPECT_TRUE(InvertAlterReg(unknown_op_cfg).rpo_blocks.empty());

  // Reject negative register number.
  RtlControlFlowGraph neg_reg_cfg = valid_cfg;
  neg_reg_cfg.rpo_blocks[0].instructions = {"(set (reg:SI -2) (const_int 1))"};
  EXPECT_TRUE(InvertAlterReg(neg_reg_cfg).rpo_blocks.empty());

  // Reject malformed 1-element sequence and non-wrapper sequence children.
  RtlControlFlowGraph bad_seq_cfg = valid_cfg;
  bad_seq_cfg.rpo_blocks[0].instructions = {
      "(sequence [(insn (set (reg:SI 2 v0) (const_int 1)))])"};
  EXPECT_TRUE(InvertAlterReg(bad_seq_cfg).rpo_blocks.empty());

  RtlControlFlowGraph non_wrapper_seq_cfg = valid_cfg;
  non_wrapper_seq_cfg.rpo_blocks[0].instructions = {
      "(sequence [(set (reg:SI 2 v0) (const_int 1)) (set (reg:SI 3 v1) (const_int 2))])"};
  EXPECT_TRUE(InvertAlterReg(non_wrapper_seq_cfg).rpo_blocks.empty());

  // Reject malformed `epilogue_delay_pattern`.
  RtlControlFlowGraph bad_ep_delay_cfg = valid_cfg;
  bad_ep_delay_cfg.sideband.epilogue_delay_pattern = "(set (reg:SI -5) (const_int 0))";
  EXPECT_TRUE(InvertAlterReg(bad_ep_delay_cfg).rpo_blocks.empty());
}

TEST_F(InvertAlterRegTest, VerifierRejectsCorruptedInverseProvider) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  CorruptedAlterRegProvider corrupted_provider;
  RightInverseVerifier bad_verifier(&ctx_, &hooks_, &corrupted_provider);
  const RightInverseResult res = bad_verifier.VerifyAlterRegRightInverse(cfg);
  EXPECT_FALSE(res.matches);
}

class InvertAlterRegCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionAlterRegProvider();
    verifier = new RightInverseVerifier(ctx, hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
  static ProductionAlterRegProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* InvertAlterRegCorpusTest::ctx = nullptr;
FunctionHooks* InvertAlterRegCorpusTest::hooks = nullptr;
ProductionAlterRegProvider* InvertAlterRegCorpusTest::provider = nullptr;
RightInverseVerifier* InvertAlterRegCorpusTest::verifier = nullptr;

TEST_P(InvertAlterRegCorpusTest, All4172CasesSatisfyBoundaryAndFreshGregRightInverse) {
  const RoundTripTestCase& tc = GetParam();
  const FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);
  ASSERT_FALSE(trace.subfunction_snapshots.empty())
      << "Failed to capture snapshots for " << tc.test_name;

  // 1. Verify isolated `SubfunctionId::kAlterReg` boundary right-inverse
  //    (`f(g(x)) == x`) across every corpus snapshot.
  bool found_alter_reg = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kAlterReg) {
      continue;
    }
    found_alter_reg = true;
    const RightInverseResult res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "SubfunctionId::kAlterReg right-inverse mismatch on "
                             << tc.test_name << ": " << res.diff_detail << "\nexpected:\n"
                             << res.expected_summary << "\nactual:\n"
                             << res.actual_summary;

    const CompilationProfile snap_profile{snap.optimize_level, snap.c_flags, snap.gp_symbol_sizes};
    const RtlControlFlowGraph lifted_snap = InvertAlterReg(snap.post_state, snap_profile, 72);
    ASSERT_FALSE(lifted_snap.rpo_blocks.empty()) << "Empty lifted_snap on " << tc.test_name;
    for (const auto& [pseudo, hard] : lifted_snap.sideband.reg_renumber) {
      EXPECT_GE(pseudo, 72) << tc.test_name;
      EXPECT_TRUE(IsValidRecoveredHardRegHome(hard))
          << "Invalid hard reg " << hard << " on " << tc.test_name;
      if (hard >= 0) {
        ASSERT_TRUE(lifted_snap.sideband.reg_n_sets.contains(pseudo)) << tc.test_name;
        EXPECT_GE(lifted_snap.sideband.reg_n_sets.at(pseudo), 1)
            << "Snapshot pseudo " << pseudo << " (hard " << hard << ") has 0 sets on "
            << tc.test_name;
      }
      if (snap.optimize_level > 0) {
        EXPECT_NE(hard, -1) << "Touched reload stack slot at opt_level > 0 on " << tc.test_name;
      }
    }
  }
  EXPECT_TRUE(found_alter_reg) << "Missing SubfunctionId::kAlterReg snapshot in " << tc.test_name;

  // 2. Fresh-lifted `.09.greg` composition check (`Pass00 -> Pass01 -> Pass02 -> Pass03`
  //    followed by `InvertAlterReg` and `ExecuteSubfunctionBoundary(kAlterReg)`), plus
  //    structural ABI / pseudo / stack-local invariants across all 4,172 cases.
  const RtlControlFlowGraph y0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(y0.rpo_blocks.empty()) << "Empty Pass00 CFG on " << tc.test_name;
  const RtlControlFlowGraph y1 = InvertPass01DbrSchedule(y0, trace.stage0_input.profile);
  ASSERT_FALSE(y1.rpo_blocks.empty()) << "Empty Pass01 CFG on " << tc.test_name;
  const RtlControlFlowGraph y2 = InvertPass02Jump2CrossJump(y1, trace.stage0_input.profile);
  ASSERT_FALSE(y2.rpo_blocks.empty()) << "Empty Pass02 CFG on " << tc.test_name;
  const RtlControlFlowGraph fresh_greg =
      InvertPass03Sched2PostReload(y2, trace.stage0_input.profile);
  ASSERT_FALSE(fresh_greg.rpo_blocks.empty()) << "Empty Pass03 .09.greg CFG on " << tc.test_name;

  const RtlControlFlowGraph lifted_pre_alter =
      InvertAlterReg(fresh_greg, trace.stage0_input.profile, 72);
  ASSERT_FALSE(lifted_pre_alter.rpo_blocks.empty())
      << "Empty InvertAlterReg on fresh .09.greg for " << tc.test_name;

  for (const auto& [pseudo, hard] : lifted_pre_alter.sideband.reg_renumber) {
    EXPECT_GE(pseudo, 72) << tc.test_name;
    EXPECT_TRUE(IsValidRecoveredHardRegHome(hard))
        << "Invalid hard reg " << hard << " in fresh .09.greg for " << tc.test_name;
    ASSERT_TRUE(lifted_pre_alter.sideband.reg_n_refs.contains(pseudo)) << tc.test_name;
    EXPECT_GE(lifted_pre_alter.sideband.reg_n_refs.at(pseudo), 1) << tc.test_name;
    if (hard >= 0) {
      ASSERT_TRUE(lifted_pre_alter.sideband.reg_n_sets.contains(pseudo)) << tc.test_name;
      EXPECT_GE(lifted_pre_alter.sideband.reg_n_sets.at(pseudo), 1)
          << "Pseudo " << pseudo << " (hard " << hard << ") has 0 sets on " << tc.test_name;
    }
    if (trace.stage0_input.profile.optimize_level > 0) {
      EXPECT_NE(hard, -1) << "Touched reload stack slot at opt_level > 0 on " << tc.test_name;
    }
  }
  if (trace.stage0_input.profile.optimize_level > 0) {
    EXPECT_EQ(lifted_pre_alter.sideband.frame_size_bytes, fresh_greg.sideband.frame_size_bytes)
        << "Mutated reload stack frame size at opt_level > 0 on " << tc.test_name;
  }

  // ABI preservation checks on fresh `.09.greg`:
  // - All `reg/i` function return value nodes remain hardware registers `< 68`.
  // - All `CALL_INSN` return destinations `(set (reg:M r) (call ...))` remain `< 68`.
  // - All upward-exposed entry parameter reads in block 0 remain `< 68`.
  EXPECT_EQ(CountIntegratedRegNodesInCfg(lifted_pre_alter),
            CountIntegratedRegNodesInCfg(fresh_greg))
      << "Lifted /i return register to pseudo on " << tc.test_name;
  EXPECT_EQ(CountCallHardReturnDestNodesInCfg(lifted_pre_alter),
            CountCallHardReturnDestNodesInCfg(fresh_greg))
      << "Lifted CALL_INSN return register to pseudo on " << tc.test_name;
  ExpectBlockZeroUpwardExposedEntryArgReadsPreserved(fresh_greg, lifted_pre_alter, tc.test_name);

  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  ctx->MaterializeFromCfg(fresh_greg);
  const RtlControlFlowGraph canonical_fresh_greg =
      ctx->DematerializeToCfg(ctx->GetInsns(), RtlPassKind::kGreg);

  const SubfunctionBoundarySnapshot replayed = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kAlterReg, lifted_pre_alter, {}, trace.stage0_input.profile.optimize_level,
      trace.stage0_input.profile.c_flags, trace.stage0_input.profile.gp_symbol_sizes);
  const RtlCfgPassComparison cmp = replayed.post_state.CompareAgainst(canonical_fresh_greg);
  EXPECT_TRUE(cmp.MatchesExactly())
      << "Fresh .09.greg -> InvertAlterReg -> ExecuteSubfunctionBoundary(kAlterReg) mismatch on "
      << tc.test_name << ":\n"
      << cmp.mismatch_detail << "\nexpected (.09.greg):\n"
      << canonical_fresh_greg.FormatSummary() << "\nactual (.09.greg):\n"
      << replayed.post_state.FormatSummary() << "\nlifted pre-alter:\n"
      << lifted_pre_alter.FormatSummary();
}

INSTANTIATE_TEST_SUITE_P(All4172Cases, InvertAlterRegCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
