#include "lifter/invert_alter_reg.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
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

bool IsValidRecoveredHardRegHome(int hard_reg, bool fp_is_fixed) {
  if (hard_reg == 30 && !fp_is_fixed) {
    return true;
  }
  return hard_reg == -1 || (hard_reg >= 2 && hard_reg <= 25) || (hard_reg >= 32 && hard_reg <= 65);
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
  } else if (fresh_pat.op != RtlOp::kClobber &&
             (fresh_pat.op != RtlOp::kUse || fresh_pat.children.empty() ||
              fresh_pat.children[0].flags.integrated)) {
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

// Subclasses `SubfunctionInverseProvider` directly so `RightInverseVerifier`
// exercises strictly the production `InvertAlterReg`.
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
              // Pin `$v0` (`2`) live across both pre-call lifetimes via a call return.
              "(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"init\"))) (const_int 0)))",
              // First lifetime in `$v1` (`3`).
              "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0)))",
              "(set (mem:SI (reg:SI 6 a2)) (reg:SI 3 v1)) (expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
              // Second completely disjoint lifetime in `$v1` (`3`) before second call.
              "(set (reg:SI 3 v1) (mem:SI (reg:SI 5 a1)))",
              "(set (mem:SI (reg:SI 7 a3)) (reg:SI 3 v1)) (expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
              // Outgoing `$a0` setup consuming `$v0`, followed by second call.
              "(set (reg:SI 4 a0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
              "(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 0)))",
              // Third disjoint lifetime in `$v1` (`3`) after second call while `$v0` stays live.
              "(set (reg:SI 3 v1) (mem:SI (reg:SI 6 a2)))",
              "(set (mem:SI (reg:SI 7 a3)) (reg:SI 3 v1)) (expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);
  cfg.rpo_blocks[0].rtl_insns[0].has_explicit_call_fusage = true;
  auto use_a0 = ParsePatternAndNotes("(use (reg:SI 4 a0))");
  ASSERT_TRUE(use_a0.ok());
  cfg.rpo_blocks[0].rtl_insns[6].has_explicit_call_fusage = true;
  cfg.rpo_blocks[0].rtl_insns[6].call_fusage = {use_a0->pattern};

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);

  // Count distinct pseudos assigned to `$v1` (`3`): must be 3 separate webs!
  std::vector<int> v1_pseudos;
  for (const auto& [pseudo, hard] : lifted.sideband.reg_renumber) {
    if (hard == 3) {
      v1_pseudos.push_back(pseudo);
    }
  }
  ASSERT_EQ(v1_pseudos.size(), 3u) << lifted.FormatSummary();

  // Outgoing `$a0` setup (`insn 5`), `CALL_INSN` return `(reg:SI 2 v0)` (`insn 6`),
  // and `CALL_INSN` return copy (`insn 9`) must remain hardware ABI registers.
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[5],
            "(set (reg:SI 4 a0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[6],
            "(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 0)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[9],
            "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))");

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
      // Block 1: diamond arm A defines `$v0` (`2`).
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 10)))",
                  "(set (pc) (label_ref L1))",
              },
          .successors = {3},
      },
      // Block 2: diamond arm B defines `$v0` (`2`).
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(set (reg:SI 2 v0) (minus:SI (reg:SI 5 a1) (const_int 10)))",
              },
          .successors = {3},
          .has_label = true,
      },
      // Block 3: loop header + latch consuming diamond-joined `$v0` and updating `$v0`.
      RtlBasicBlock{
          .id = 3,
          .instructions =
              {
                  "(note NOTE_INSN_LOOP_BEG)",
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -1)))",
                  "(set (pc) (if_then_else (gt (reg:SI 2 v0) (const_int 0)) "
                  "(label_ref L1) (pc)))",
              },
          .successors = {3, 4},
          .has_label = true,
      },
      // Block 4: post-loop exit block consumes loop result in `$v0`, then reuses `$v0`
      // for a fresh independent computation while `$v1` (`3`) stays live across it.
      RtlBasicBlock{
          .id = 4,
          .instructions =
              {
                  "(note NOTE_INSN_LOOP_END)",
                  "(set (reg:SI 3 v1) (ashift:SI (reg:SI 2 v0) (const_int 1))) "
                  "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 42)))",
                  "(set (mem:SI (reg:SI 6 a2)) (reg:SI 3 v1)) "
                  "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                  "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
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
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 2);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 3);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(74), 2);

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
  EXPECT_EQ(lifted.rpo_blocks[4].instructions[4], "(set (reg/i:SI 2 v0) (reg:SI 74))");

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
              // RHS reads incoming ABI `$a0` (dying at this instruction); LHS starts a local
              // scratch web in `$a0` suggested by `$a0`!
              "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 5))) "
              "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
              "(set (reg:SI 4 a0) (ashift:SI (reg:SI 4 a0) (const_int 1))) "
              "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
              "(set (mem:SI (reg:SI 5 a1)) (reg:SI 4 a0)) "
              "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 4);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 4);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 5))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (ashift:SI (reg:SI 72) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2],
            "(set (mem:SI (reg:SI 5 a1)) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))");

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
                  "(set (reg:SI 2 v0) (gt:SI (reg/v:SI 16 s0) (const_int 10)))",
                  "(set (pc) (if_then_else (ne (reg:SI 2 v0) (const_int 0)) "
                  "(label_ref L0) (pc))) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
              },
          .successors = {2, 1},
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:DF 32 $f0) (mem:DF (reg:SI 5 a1)))",
                  "(set (mem:DF (reg:SI 6 a2)) (reg:DF 32 $f0)) "
                  "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))",
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
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 3u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 16);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), 2);
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
              "(set (reg:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1))) "
              "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))",
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&di_to_si_cfg);

  const RtlControlFlowGraph lifted_di_to_si = InvertAlterReg(di_to_si_cfg, 72);
  ASSERT_EQ(lifted_di_to_si.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted_di_to_si.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_di_to_si.sideband.reg_renumber.at(72), 2);
  EXPECT_GE(lifted_di_to_si.sideband.reg_n_sets.at(72), 1);
  EXPECT_EQ(lifted_di_to_si.rpo_blocks[0].instructions[0], "(set (reg:DI 8 t0) (reg:DI 4 a0))");
  EXPECT_EQ(lifted_di_to_si.rpo_blocks[0].instructions[1],
            "(set (reg:SI 72) (plus:SI (reg:SI 8 t0) (reg:SI 9 t1))) "
            "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))");
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
              "(set (reg:DI 2 v0) (reg:DI 8 t0)) (expr_list:REG_DEAD (reg:DI 8 t0) (nil))",
              "(set (reg/i:DI 2 v0) (reg:DI 2 v0))",
              "(use (reg/i:DI 2 v0))",
          },
  });
  SyncAllBlocks(&si_to_di_cfg);

  const RtlControlFlowGraph lifted_si_to_di = InvertAlterReg(si_to_di_cfg, 72);
  ASSERT_EQ(lifted_si_to_di.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted_si_to_di.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_si_to_di.sideband.reg_renumber.at(72), 2);
  EXPECT_GE(lifted_si_to_di.sideband.reg_n_sets.at(72), 1);
  EXPECT_EQ(lifted_si_to_di.rpo_blocks[0].instructions[2],
            "(set (reg:DI 72) (reg:DI 8 t0)) (expr_list:REG_DEAD (reg:DI 8 t0) (nil))");
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
              "(set (reg:SI 2 v0) (plus:SI (subreg:SI (reg:DI 8 t0) 0) (reg:SI 9 t1))) "
              "(expr_list:REG_DEAD (reg:DI 8 t0) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))",
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&live_partial_overwrite_cfg);

  const RtlControlFlowGraph lifted_partial = InvertAlterReg(live_partial_overwrite_cfg, 72);
  ASSERT_EQ(lifted_partial.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted_partial.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_partial.sideband.reg_renumber.at(72), 2);
  EXPECT_EQ(lifted_partial.rpo_blocks[0].instructions[2],
            "(set (reg:SI 72) (plus:SI (subreg:SI (reg:DI 8 t0) 0) (reg:SI 9 t1))) "
            "(expr_list:REG_DEAD (reg:DI 8 t0) (expr_list:REG_DEAD (reg:SI 9 t1) (nil)))");
  EXPECT_TRUE(verifier_.VerifyAlterRegRightInverse(live_partial_overwrite_cfg).matches);

  // 4. Contrast: when `(reg:DI 2 v0)` is already dead before `(set (reg:SI 3 v1) ...)`,
  //    neither web is pinned—all three webs lift cleanly to independent pseudos.
  RtlControlFlowGraph dead_di_then_si_cfg;
  dead_di_then_si_cfg.pass = RtlPassKind::kGreg;
  dead_di_then_si_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 2 v0) (mem:DI (reg:SI 4 a0)))",
              "(set (mem:SI (reg:SI 5 a1)) (subreg:SI (reg:DI 2 v0) 1)) "
              "(expr_list:REG_DEAD (reg:DI 2 v0) (nil))",
              "(set (reg:SI 2 v0) (mem:SI (reg:SI 6 a2)))",
              "(set (reg:SI 3 v1) (plus:SI (mem:SI (reg:SI 6 a2)) (const_int 5)))",
              "(set (mem:SI (reg:SI 7 a3)) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1))) "
              "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))",
          },
  });
  SyncAllBlocks(&dead_di_then_si_cfg);

  const RtlControlFlowGraph lifted_dead_di = InvertAlterReg(dead_di_then_si_cfg, 72);
  ASSERT_EQ(lifted_dead_di.sideband.reg_renumber.size(), 3u) << lifted_dead_di.FormatSummary();
  EXPECT_EQ(lifted_dead_di.sideband.reg_renumber.at(72), 2);
  EXPECT_EQ(lifted_dead_di.sideband.reg_renumber.at(73), 2);
  EXPECT_EQ(lifted_dead_di.sideband.reg_renumber.at(74), 3);
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
              "(set (reg:SI 2 v0) (and:SI (reg:SI 3 v1) (const_int 65535)))",
              "(set (reg:SI 3 v1) (ior:SI (reg:SI 2 v0) (ashift:SI (reg:SI 4 a0) (const_int 16)))) "
              "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))",
              "(parallel[ (set (reg:DI 6 a2) (plus:DI (reg:DI 6 a2) (const_int 1))) "
              "(clobber (reg:SI 9 t1)) ] ) (expr_list:REG_DEAD (reg:DI 6 a2) (nil))",
              "(set (mem:QI (plus:SI (reg:SI 5 a1) (const_int 4))) (subreg:QI (reg:SI 9 t1) 0)) "
              "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))",
              "(set (mem:SI (reg:SI 5 a1)) (plus:SI (reg:SI 3 v1) (subreg:SI (reg:DI 6 a2) 1))) "
              "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:DI 6 a2) (nil)))",
          },
  });
  SyncAllBlocks(&rmw_and_scratch_cfg);

  const RtlControlFlowGraph lifted_rmw = InvertAlterReg(rmw_and_scratch_cfg, 72);
  ASSERT_EQ(lifted_rmw.sideband.reg_renumber.size(), 3u);
  EXPECT_EQ(lifted_rmw.sideband.reg_renumber.at(72), 2);
  EXPECT_EQ(lifted_rmw.sideband.reg_renumber.at(73), 3);
  EXPECT_EQ(lifted_rmw.sideband.reg_renumber.at(74), 6);
  EXPECT_GE(lifted_rmw.sideband.reg_n_sets.at(72), 1);
  EXPECT_GE(lifted_rmw.sideband.reg_n_sets.at(73), 1);
  EXPECT_GE(lifted_rmw.sideband.reg_n_sets.at(74), 1);
  EXPECT_EQ(lifted_rmw.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (and:SI (reg:SI 73) (const_int 65535)))");
  EXPECT_EQ(lifted_rmw.rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (ior:SI (reg:SI 72) (ashift:SI (reg:SI 4 a0) (const_int 16)))) "
            "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))");
  EXPECT_EQ(lifted_rmw.rpo_blocks[0].instructions[3],
            "(set (mem:QI (plus:SI (reg:SI 5 a1) (const_int 4))) (subreg:QI (reg:SI 9 t1) 0)) "
            "(expr_list:REG_DEAD (reg:SI 9 t1) (nil))");
  const RightInverseResult rmw_res = verifier_.VerifyAlterRegRightInverse(rmw_and_scratch_cfg);
  EXPECT_TRUE(rmw_res.matches) << rmw_res.diff_detail;
}

TEST_F(InvertAlterRegTest,
       LiftsEmitNoConflictBlockBothUntiedAndLocalAllocTiedWithStandaloneClobberAndNotes) {
  // 1. Untied `emit_no_conflict_block` (`op0` in `$2`, `target` in `$8` while `$4..$7` stay live).
  RtlControlFlowGraph raw_untied_cfg;
  raw_untied_cfg.pass = RtlPassKind::kGreg;
  raw_untied_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 2 v0) (mem:DI (reg:SI 4 a0)))",
              "(clobber (reg:DI 8 t0)) (insn_list:REG_LIBCALL 0 (nil))",
              "(set (subreg:SI (reg:DI 8 t0) 0) (not:SI (subreg:SI (reg:DI 2 v0) 0))) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 2 v0) (nil))",
              "(set (subreg:SI (reg:DI 8 t0) 1) (not:SI (subreg:SI (reg:DI 2 v0) 1))) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 2 v0) "
              "(expr_list:REG_DEAD (reg:DI 2 v0) (nil)))",
              "(set (reg:DI 8 t0) (reg:DI 8 t0)) "
              "(insn_list:REG_RETVAL 0 (expr_list:REG_EQUAL (not:DI (reg:DI 2 v0)) (nil)))",
              "(set (mem:DI (plus:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
              "(plus:DI (reg:DI 8 t0) (reg:DI 6 a2))) "
              "(expr_list:REG_DEAD (reg:DI 8 t0) (expr_list:REG_DEAD (reg:DI 6 a2) (nil)))",
          },
  });
  SyncAllBlocks(&raw_untied_cfg);
  ctx_.MaterializeFromCfg(raw_untied_cfg);
  const RtlControlFlowGraph untied_cfg =
      ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_untied = InvertAlterReg(untied_cfg, 72);
  ASSERT_EQ(lifted_untied.sideband.reg_renumber.size(), 2u) << lifted_untied.FormatSummary();
  EXPECT_EQ(lifted_untied.sideband.reg_renumber.at(72), 2);
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

  // 2. `local_alloc`-tied `emit_no_conflict_block` (`op0` AND `target` both in `$2`),
  //    including a subword constant write where `REG_NO_CONFLICT` has no read in `read_occs`.
  RtlControlFlowGraph raw_tied_cfg;
  raw_tied_cfg.pass = RtlPassKind::kGreg;
  raw_tied_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:DI 2 v0) (mem:DI (reg:SI 4 a0)))",
              "(clobber (reg:DI 2 v0)) (insn_list:REG_LIBCALL 0 (nil))",
              "(set (subreg:SI (reg:DI 2 v0) 0) (const_int -1)) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 2 v0) (nil))",
              "(set (subreg:SI (reg:DI 2 v0) 1) (not:SI (subreg:SI (reg:DI 2 v0) 1))) "
              "(expr_list:REG_NO_CONFLICT (reg:DI 2 v0) "
              "(expr_list:REG_DEAD (reg:DI 2 v0) (nil)))",
              "(set (reg:DI 2 v0) (reg:DI 2 v0)) "
              "(insn_list:REG_RETVAL 0 "
              "(expr_list:REG_EQUAL (ior:DI (not:DI (reg:DI 2 v0)) (const_int -1)) "
              "(nil)))",
              "(set (reg/i:DI 2 v0) (reg:DI 2 v0)) (expr_list:REG_DEAD (reg:DI 2 v0) (nil))",
              "(use (reg/i:DI 2 v0))",
          },
  });
  SyncAllBlocks(&raw_tied_cfg);
  ctx_.MaterializeFromCfg(raw_tied_cfg);
  const RtlControlFlowGraph tied_cfg = ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_tied = InvertAlterReg(tied_cfg, 72);
  ASSERT_EQ(lifted_tied.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted_tied.sideband.reg_renumber.at(72), 2);
  EXPECT_EQ(lifted_tied.sideband.reg_renumber.at(73), 2);
  EXPECT_EQ(lifted_tied.rpo_blocks[0].instructions[0], "(set (reg:DI 72) (mem:DI (reg:SI 4 a0)))");
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
              "(set (reg:DF 32 $f0) (mem:DF (reg:SI 30 $fp)))",
              "(set (reg/i:DF 32 $f0) (reg:DF 32 $f0))",
              "(use (reg/i:DF 32 $f0))",
          },
  });
  SyncAllBlocks(&float_store_cfg);

  const CompilationProfile float_store_profile{.optimize_level = 0,
                                               .c_flags = {"-O0", "-ffloat-store"}};
  hooks_.ApplyCompilationProfile(float_store_profile);
  const RtlControlFlowGraph lifted_fs = InvertAlterReg(float_store_cfg, float_store_profile, 72);
  EXPECT_EQ(lifted_fs.sideband.frame_size_bytes, 8);
  ASSERT_EQ(lifted_fs.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_fs.sideband.reg_renumber.at(72), 32);
  const RightInverseResult fs_res = verifier_.VerifyAlterRegRightInverse(float_store_cfg);
  EXPECT_TRUE(fs_res.matches) << fs_res.diff_detail;

  const CompilationProfile no_float_store_profile{
      .optimize_level = 0, .c_flags = {"-O0", "-ffloat-store", "-fno-float-store"}};
  hooks_.ApplyCompilationProfile(no_float_store_profile);
  const RtlControlFlowGraph lifted_no_fs =
      InvertAlterReg(float_store_cfg, no_float_store_profile, 72);
  EXPECT_EQ(lifted_no_fs.sideband.frame_size_bytes, 0);
  ASSERT_EQ(lifted_no_fs.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted_no_fs.sideband.reg_renumber.at(72), 32);
  EXPECT_EQ(lifted_no_fs.sideband.reg_renumber.at(73), -1);
  const RightInverseResult no_fs_res = verifier_.VerifyAlterRegRightInverse(float_store_cfg);
  EXPECT_TRUE(no_fs_res.matches) << no_fs_res.diff_detail;

  // 6. At `opt_level > 0` (`B3.5`, e.g. `-O2 -fno-omit-frame-pointer`), `$fp`-relative
  //    stack slots belong to `reload` (`InvertReload`), NEVER `InvertAlterReg`.
  hooks_.ApplyCompilationProfile(o2_profile);
  const RtlControlFlowGraph lifted_o2_fp = InvertAlterReg(float_store_cfg, o2_profile, 72);
  EXPECT_EQ(lifted_o2_fp.sideband.frame_size_bytes, 8);
  ASSERT_EQ(lifted_o2_fp.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_o2_fp.sideband.reg_renumber.at(72), 32);
  const RightInverseResult o2_fp_res = verifier_.VerifyAlterRegRightInverse(float_store_cfg);
  EXPECT_TRUE(o2_fp_res.matches) << o2_fp_res.diff_detail;

  // 7. When `frame_pointer_needed == false`, `$30` (`$s8`) dereferences are pointer loads.
  RtlControlFlowGraph s8_ptr_cfg = float_store_cfg;
  s8_ptr_cfg.sideband.frame_pointer_needed = false;
  hooks_.ApplyCompilationProfile(o0_profile);
  const RtlControlFlowGraph lifted_s8 = InvertAlterReg(s8_ptr_cfg, o0_profile, 72);
  EXPECT_EQ(lifted_s8.sideband.frame_size_bytes, 8);
  ASSERT_EQ(lifted_s8.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_s8.sideband.reg_renumber.at(72), 32);
  const RightInverseResult s8_res = verifier_.VerifyAlterRegRightInverse(s8_ptr_cfg);
  EXPECT_TRUE(s8_res.matches) << s8_res.diff_detail;
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
              "(set (reg:SI 2 v0) (plus:SI (reg:SI 64 hi) (reg:SI 65 lo)))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 0 $0)))",
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
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 2);

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, LiftsGeneralPurposeS8DisjointWebsWhenFramePointerOmitted) {
  // When `!frame_pointer_needed` and `-fomit-frame-pointer` holds (`-O1+`),
  // `$30` (`$s8`) is an allocatable callee-saved GPR (`reload1.c:2365`) whose
  // disjoint DU webs must be lifted to distinct pseudos (`reg_renumber == 30`)
  // and verified via `VerifyAlterRegRightInverse`.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_size_bytes = 16;
  cfg.sideband.frame_pointer_needed = false;
  cfg.sideband.saved_gpr_regnos = {30};
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:SI 30 $fp) (plus:SI (reg:SI 4 a0) (const_int 1)))",
              "(set (reg:SI 8 t0) (ashift:SI (reg:SI 30 $fp) (const_int 2))) "
              "(expr_list:REG_DEAD (reg:SI 30 $fp) (nil))",
              "(set (reg:SI 30 $fp) (minus:SI (reg:SI 5 a1) (const_int 3)))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 30 $fp))) "
              "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 30 $fp) (nil)))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  int s8_pseudo_count = 0;
  for (const auto& [p, hard] : lifted.sideband.reg_renumber) {
    if (hard == 30) {
      ++s8_pseudo_count;
    }
  }
  EXPECT_EQ(s8_pseudo_count, 2);

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  // Negative check: under `-fno-omit-frame-pointer` (or `frame_pointer_needed`),
  // `$30` is fixed as `$fp` and must NOT be lifted to a pseudo.
  const CompilationProfile no_omit_fp{.optimize_level = 2,
                                      .c_flags = {"-O2", "-fno-omit-frame-pointer"}};
  const RtlControlFlowGraph lifted_fixed_fp = InvertAlterReg(cfg, no_omit_fp, 72);
  for (const auto& [_, hard] : lifted_fixed_fp.sideband.reg_renumber) {
    EXPECT_NE(hard, 30);
  }
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

TEST_F(InvertAlterRegTest, LiftsGeneralPurposeS8CalleeSavedPseudoUnderOmitFramePointer) {
  // Under `-O1+` (`-fomit-frame-pointer` by default on MIPS when
  // `frame_pointer_needed == false`), `$30` (`$s8` / `fp`) is a general-purpose
  // callee-saved register allocated by `local-alloc` / `global_alloc` and
  // rewritten in-place by `alter_reg`. Verify `InvertAlterReg` lifts `$30` to a
  // pseudo with `reg_renumber == 30` and satisfies `SubfunctionId::kAlterReg`
  // right-inverse replay (`matches == true`).
  SubfunctionBoundarySnapshot snap;
  snap.subfunction_id = SubfunctionId::kAlterReg;
  snap.optimize_level = 2;
  snap.c_flags = {"-O2"};
  snap.post_state.pass = RtlPassKind::kGreg;
  snap.post_state.sideband.frame_size_bytes = 24;
  snap.post_state.sideband.outgoing_args_size = 16;
  snap.post_state.sideband.frame_pointer_needed = false;
  snap.post_state.sideband.saved_gpr_regnos = {30, 31};
  snap.post_state.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:SI 30 $fp) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
              "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"helper\"))) "
              "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 30 $fp))) "
              "(expr_list:REG_DEAD (reg:SI 30 $fp) (nil))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&snap.post_state);
  snap.post_state.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage = true;
  snap.post_state.rpo_blocks[0].rtl_insns[1].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };

  const CompilationProfile o2_profile{snap.optimize_level, snap.c_flags, {}};
  const RtlControlFlowGraph lifted = InvertAlterReg(snap.post_state, o2_profile, 80);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  bool found_s8_pseudo = false;
  for (const auto& [pseudo, hr] : lifted.sideband.reg_renumber) {
    if (hr == 30) {
      found_s8_pseudo = true;
    }
  }
  EXPECT_TRUE(found_s8_pseudo);

  const RightInverseResult res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;

  // Negative check: when `frame_pointer_needed == true`, `$30` is the hardware
  // frame pointer and must NOT be lifted to a general-purpose pseudo.
  RtlControlFlowGraph fp_needed_cfg = snap.post_state;
  fp_needed_cfg.sideband.frame_pointer_needed = true;
  const RtlControlFlowGraph lifted_fp = InvertAlterReg(fp_needed_cfg, o2_profile, 80);
  for (const auto& [pseudo, hr] : lifted_fp.sideband.reg_renumber) {
    EXPECT_NE(hr, 30);
  }
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

TEST_F(InvertAlterRegTest, DoesNotLiftOrphanedPostReloadRegLibcallClobberWithoutMatchingRetval) {
  // When post-reload dead-code elimination deletes the `REG_RETVAL` instruction of an
  // `emit_no_conflict_block`, an orphaned `(clobber (reg:DI 2 v0)) (insn_list:REG_LIBCALL ...)`
  // can remain immediately before a separate downstream `DImode` computation in `$2`.
  // `InvertAlterReg` must keep the orphaned clobber pinned on physical hard register `2` rather
  // than merging it into the downstream `DImode` pseudo web.
  RtlControlFlowGraph raw_cfg;
  raw_cfg.pass = RtlPassKind::kGreg;
  raw_cfg.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(clobber (reg:DI 2 v0)) (insn_list:REG_LIBCALL 0 (nil))",
                  "(set (subreg:SI (reg:DI 2 v0) 0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))",
                  "(set (subreg:SI (reg:DI 2 v0) 1) (reg:SI 4 a0)) "
                  "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                  "(set (pc) (label_ref L0))",
              },
          .successors = {1},
          .has_trailing_barrier = true,
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 6 a2))) "
                  "(expr_list:REG_DEAD (reg:DI 2 v0) (expr_list:REG_DEAD (reg:DI 6 a2) (nil)))",
                  "(use (reg/i:DI 2 v0))",
              },
          .has_label = true,
      },
  };
  SyncAllBlocks(&raw_cfg);
  ctx_.MaterializeFromCfg(raw_cfg);
  const RtlControlFlowGraph orphan_cfg =
      ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted = InvertAlterReg(orphan_cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 2u);
  auto clob_parsed = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(clob_parsed.ok());
  EXPECT_EQ(clob_parsed->pattern.ToSexprString(), "(clobber (reg:DI 2 v0))")
      << orphan_cfg.FormatSummary() << "\nlifted:\n"
      << lifted.FormatSummary();
  // Downstream `DImode` pseudo in `$2` still lifts cleanly (`reg_n_sets == 2`) without absorbing
  // the orphaned `REG_LIBCALL` clobber.
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), 2);
  EXPECT_EQ(lifted.sideband.reg_n_sets.at(72), 2);
  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(orphan_cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, RegDeadSplitsKilledHardRegWebsAndO0RegEqualSelfMoveUnifiesDiWeb) {
  // 1. At `-O2`, an explicit `REG_DEAD` note on `(reg:SI 2 v0)` kills `v0`'s reaching definition
  //    both before a subsequent destination write in the same block and across a loop backedge,
  //    keeping the pre-death web (`r72`) separate from the post-death web (`r74`).
  RtlControlFlowGraph raw_loop_cfg;
  raw_loop_cfg.pass = RtlPassKind::kGreg;
  raw_loop_cfg.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
              },
          .successors = {1},
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                  "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                  "(set (pc) (if_then_else (ne (reg:SI 2 v0) (const_int 10)) "
                  "(label_ref L0) (pc))) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
              },
          .successors = {1, 2},
          .has_label = true,
      },
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(set (reg/i:SI 2 v0) (reg:SI 3 v1))",
                  "(use (reg/i:SI 2 v0))",
              },
          .successors = {},
      },
  };
  SyncAllBlocks(&raw_loop_cfg);
  const CompilationProfile o2_prof{2, {"-O2", "-mips2"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);
  regclass_init();
  ctx_.MaterializeFromCfg(raw_loop_cfg);
  const RtlControlFlowGraph loop_cfg = ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_loop = InvertAlterReg(loop_cfg, o2_prof, 72);
  // Two distinct pseudos on hard reg `2` (`r72` in B0->B1[0] and `r74` in B1[1]->B1[2]),
  // not coalesced across `REG_DEAD (reg:SI 2 v0)`.
  int count_hr2_split_pseudos = 0;
  for (const auto& [pseudo, hr] : lifted_loop.sideband.reg_renumber) {
    if (hr == 2) {
      ++count_hr2_split_pseudos;
    }
  }
  EXPECT_EQ(count_hr2_split_pseudos, 2);
  const RightInverseResult loop_res = verifier_.VerifyAlterRegRightInverse(loop_cfg);
  EXPECT_TRUE(loop_res.matches) << loop_res.diff_detail;

  // 2. At `-O0`, `optabs.c` inline `DImode` sequence finishes with `(set (reg:DI 2 v0) (reg:DI 2
  //    v0)) (expr_list:REG_EQUAL ...)` WITHOUT `REG_RETVAL` when no `REG_LIBCALL` note was emitted.
  //    `InvertAlterReg` must unify the subreg writes before the self-move with the `DImode` web
  //    after the self-move into a single pseudo (`-O0` `stupid` allocation).
  RtlControlFlowGraph raw_o0_self_move;
  raw_o0_self_move.pass = RtlPassKind::kGreg;
  raw_o0_self_move.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (subreg:SI (reg:DI 2 v0) 0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))",
                  "(set (subreg:SI (reg:DI 2 v0) 1) (reg:SI 4 a0))",
                  "(set (reg:DI 2 v0) (reg:DI 2 v0)) "
                  "(expr_list:REG_EQUAL (sign_extend:DI (reg:SI 4 a0)) (nil))",
                  "(set (reg/i:DI 2 v0) (reg:DI 2 v0))",
                  "(use (reg/i:DI 2 v0))",
              },
      },
  };
  SyncAllBlocks(&raw_o0_self_move);
  const CompilationProfile o0_prof{0, {"-O0", "-mips2"}, {}};
  hooks_.ApplyCompilationProfile(o0_prof);
  regclass_init();
  ctx_.MaterializeFromCfg(raw_o0_self_move);
  const RtlControlFlowGraph o0_self_move_cfg =
      ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_o0 = InvertAlterReg(o0_self_move_cfg, o0_prof, 72);
  int count_hr2_pseudos = 0;
  for (const auto& [pseudo, hr] : lifted_o0.sideband.reg_renumber) {
    if (hr == 2) {
      ++count_hr2_pseudos;
    }
  }
  EXPECT_EQ(count_hr2_pseudos, 1);
  const RightInverseResult o0_res = verifier_.VerifyAlterRegRightInverse(o0_self_move_cfg);
  EXPECT_TRUE(o0_res.matches) << o0_res.diff_detail;
}

TEST_F(InvertAlterRegTest, DeterministicallyCoalescesMultiBlockFramePointerLocalsWithoutOrphans) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-fomit-frame-pointer"}, {}};

  // Three non-entry blocks (`B1`, `B3`, `B4`) each contain a single-block local computation in
  // `$fp` (`30`) when `!frame_pointer_needed`. `RunSingleLocalResolutionIteration` +
  // `ApplyLocalResolutionRewrites` must transitively coalesce all three into a single canonical
  // `$30` pseudo with zero orphaned `reg_renumber` references in the lifted RTL.
  RtlControlFlowGraph raw_fp_cfg;
  raw_fp_cfg.pass = RtlPassKind::kGreg;
  raw_fp_cfg.sideband.frame_pointer_needed = false;
  raw_fp_cfg.sideband.saved_gpr_regnos = {30};
  raw_fp_cfg.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                  "(label_ref L0) (pc)))",
              },
          .successors = {2, 1},
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:SI 30 $fp) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                  "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                  "(set (mem:SI (reg:SI 6 a2)) (reg:SI 30 $fp)) "
                  "(expr_list:REG_DEAD (reg:SI 30 $fp) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
                  "(set (pc) (label_ref L2))",
              },
          .successors = {5},
      },
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 1)) "
                  "(label_ref L1) (pc))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
              },
          .successors = {4, 3},
          .has_label = true,
      },
      RtlBasicBlock{
          .id = 3,
          .instructions =
              {
                  "(set (reg:SI 30 $fp) (plus:SI (reg:SI 5 a1) (const_int 2))) "
                  "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                  "(set (mem:SI (reg:SI 6 a2)) (reg:SI 30 $fp)) "
                  "(expr_list:REG_DEAD (reg:SI 30 $fp) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
                  "(set (pc) (label_ref L2))",
              },
          .successors = {5},
      },
      RtlBasicBlock{
          .id = 4,
          .instructions =
              {
                  "(set (reg:SI 30 $fp) (plus:SI (reg:SI 5 a1) (const_int 3))) "
                  "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                  "(set (mem:SI (reg:SI 6 a2)) (reg:SI 30 $fp)) "
                  "(expr_list:REG_DEAD (reg:SI 30 $fp) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
              },
          .successors = {5},
          .has_label = true,
      },
      RtlBasicBlock{
          .id = 5,
          .instructions =
              {
                  "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
              },
          .has_label = true,
      },
  };
  SyncAllBlocks(&raw_fp_cfg);
  hooks_.ApplyCompilationProfile(o2_prof);
  regclass_init();
  ctx_.MaterializeFromCfg(raw_fp_cfg);
  const RtlControlFlowGraph fp_cfg = ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_fp = InvertAlterReg(fp_cfg, o2_prof, 72);
  int count_hr30 = 0;
  for (const auto& [pseudo, hr] : lifted_fp.sideband.reg_renumber) {
    if (hr == 30) {
      ++count_hr30;
    }
  }
  EXPECT_EQ(count_hr30, 1) << lifted_fp.FormatSummary();
  for (const RtlBasicBlock& blk : lifted_fp.rpo_blocks) {
    for (const std::string& insn_s : blk.instructions) {
      const auto pn = ParsePatternAndNotes(insn_s);
      ASSERT_TRUE(pn.ok());
      pn->pattern.Visit([&](const RtlExpr& e) {
        if (e.IsPseudoReg()) {
          EXPECT_TRUE(lifted_fp.sideband.reg_renumber.contains(e.RegNo()))
              << "Orphaned pseudo " << e.RegNo() << " in:\n"
              << lifted_fp.FormatSummary();
        }
      });
    }
  }
  const RightInverseResult fp_res = verifier_.VerifyAlterRegRightInverse(fp_cfg);
  EXPECT_TRUE(fp_res.matches) << fp_res.diff_detail;
}

TEST_F(InvertAlterRegTest, DemotesFalsePreferenceConflictWebAndPreservesPinnedArithmeticRmw) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips1", "-funsigned-char"}, {}};

  // 1. Multi-block `QImode` web in `$6` feeds `(set (reg:SI 2 v0) (plus:SI (subreg:SI (reg:QI 6)
  //    0) 32))` where `$2` is pinned by `(set (mem:QI ...) (reg:QI 2 v0))`, while a cross-edge
  //    `$2` web (`B0[1] -> B1[0..1]`) overlaps `$6` without preferring `$2` itself.
  //    `InvertAlterReg` must demote the hoisted `B0[1] -> B1[0..1]` web back to `(reg:SI 2 v0)`
  //    while lifting `$6` (`QImode` global) and `$3` (`B1` local comparison) to pseudos.
  RtlControlFlowGraph raw_pref_cfg;
  raw_pref_cfg.pass = RtlPassKind::kGreg;
  raw_pref_cfg.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (reg:QI 6 a2) (mem:QI (reg:SI 4 a0)))",
                  "(set (reg:SI 2 v0) (plus:SI (subreg:SI (reg:QI 6 a2) 0) (const_int -65)))",
                  "(set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) "
                  "(label_ref L1) (pc)))",
              },
          .successors = {3, 1},
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:SI 3 v1) (ltu:SI (reg:SI 2 v0) (const_int 26)))",
                  "(set (mem:SI (reg:SI 5 a1)) (reg:SI 2 v0)) "
                  "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                  "(set (pc) (if_then_else (eq:SI (reg:SI 3 v1) (const_int 0)) "
                  "(label_ref L1) (pc))) (expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
              },
          .successors = {3, 2},
      },
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(set (reg:SI 2 v0) (plus:SI (subreg:SI (reg:QI 6 a2) 0) (const_int 32))) "
                  "(expr_list:REG_DEAD (reg:QI 6 a2) (nil))",
                  "(set (mem:QI (reg:SI 4 a0)) (reg:QI 2 v0)) "
                  "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))",
              },
          .successors = {3},
      },
      RtlBasicBlock{
          .id = 3,
          .instructions =
              {
                  "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
              },
          .has_label = true,
      },
  };
  SyncAllBlocks(&raw_pref_cfg);
  hooks_.ApplyCompilationProfile(o2_prof);
  regclass_init();
  ctx_.MaterializeFromCfg(raw_pref_cfg);
  const RtlControlFlowGraph pref_cfg = ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);

  const RtlControlFlowGraph lifted_pref = InvertAlterReg(pref_cfg, o2_prof, 72);
  int count_hr3 = 0;
  int count_hr6 = 0;
  int count_hr2 = 0;
  for (const auto& [pseudo, hr] : lifted_pref.sideband.reg_renumber) {
    if (hr == 3) ++count_hr3;
    if (hr == 6) ++count_hr6;
    if (hr == 2) ++count_hr2;
  }
  EXPECT_EQ(count_hr3, 1) << lifted_pref.FormatSummary();
  EXPECT_EQ(count_hr6, 1) << lifted_pref.FormatSummary();
  EXPECT_EQ(count_hr2, 0) << lifted_pref.FormatSummary();
  EXPECT_NE(lifted_pref.rpo_blocks[1].instructions[0].find("(reg:SI 2 v0)"), std::string::npos)
      << lifted_pref.FormatSummary();
  const RightInverseResult pref_res = verifier_.VerifyAlterRegRightInverse(pref_cfg);
  EXPECT_TRUE(pref_res.matches) << pref_res.diff_detail;

  // 2. Loop-carried non-`REG_DEAD` arithmetic RMW on an entry-pinned argument register (`$4 a0`)
  //    at `-O2` keeps `SET_DEST` pinned to `$4 a0` where it merges with entry `$4 a0` at the loop
  //    header rather than splitting off a conflicting pseudo.
  RtlControlFlowGraph raw_rmw_cfg;
  raw_rmw_cfg.pass = RtlPassKind::kGreg;
  raw_rmw_cfg.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (reg/i:SI 2 v0) (mem:SI (reg:SI 4 a0)))",
                  "(set (pc) (if_then_else (eq:SI (reg/i:SI 2 v0) (const_int 0)) "
                  "(label_ref L1) (pc)))",
              },
          .successors = {2, 1},
          .has_label = true,
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 4)))",
                  "(set (pc) (label_ref L0))",
              },
          .successors = {0},
      },
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(use (reg/i:SI 2 v0))",
              },
          .has_label = true,
      },
  };
  SyncAllBlocks(&raw_rmw_cfg);
  ctx_.MaterializeFromCfg(raw_rmw_cfg);
  const RtlControlFlowGraph rmw_cfg = ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);
  const RtlControlFlowGraph lifted_rmw = InvertAlterReg(rmw_cfg, o2_prof, 72);
  EXPECT_TRUE(lifted_rmw.sideband.reg_renumber.empty()) << lifted_rmw.FormatSummary();
  const RightInverseResult rmw_res = verifier_.VerifyAlterRegRightInverse(rmw_cfg);
  EXPECT_TRUE(rmw_res.matches) << rmw_res.diff_detail;
}

TEST_F(InvertAlterRegTest,
       CoalesceSameRegRenumberRmwPseudosIsInvariantToIncomingRegDeadAndLiveAtStartSideband) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips1"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  auto strip_notes_and_live_at_start = [](const RtlControlFlowGraph& in) {
    RtlControlFlowGraph stripped = in;
    stripped.sideband.basic_block_live_at_start.clear();
    for (RtlBasicBlock& blk : stripped.rpo_blocks) {
      for (std::string& insn_str : blk.instructions) {
        auto pn = ParsePatternAndNotes(insn_str);
        EXPECT_TRUE(pn.ok()) << insn_str;
        if (pn.ok()) {
          pn->reg_notes.clear();
          insn_str = FormatGccPrintRtlPatternAndNotes(pn->pattern, pn->reg_notes,
                                                      pn->bracketed_notes, /*call_fusage=*/{});
        }
      }
      blk.SyncRtlInsnsFromInstructions();
    }
    return stripped;
  };

  // Case A (coalesces): `$t0` (`reg 8`) is initialized in `B0` (`r_global == true`), updated in
  // `B1` via a `PARALLEL` RMW (`p_r != p_w`, `!w_global`) and consumed before `B2`, where `$t0`
  // is dead at `B2` entry. Both with full `REG_DEAD` + `basic_block_live_at_start` and with both
  // stripped, `CoalesceSameRegRenumberRmwPseudos` coalesces the RMW into a single pseudo `72`.
  RtlControlFlowGraph raw_coalesce;
  raw_coalesce.pass = RtlPassKind::kGreg;
  raw_coalesce.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 3))) "
                  "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                  "(set (pc) (label_ref L0))",
              },
          .successors = {1},
          .has_trailing_barrier = true,
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(parallel[ (set (reg:SI 8 t0) (plus:SI (reg:SI 8 t0) (const_int 1))) "
                  "(clobber (reg:SI 1 at)) ] ) (expr_list:REG_UNUSED (reg:SI 1 at) (nil))",
                  "(set (reg/i:SI 2 v0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                  "(set (pc) (label_ref L1))",
              },
          .successors = {2},
          .has_label = true,
          .has_trailing_barrier = true,
      },
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
              },
          .has_label = true,
      },
  };
  SyncAllBlocks(&raw_coalesce);
  ctx_.MaterializeFromCfg(raw_coalesce);
  RtlControlFlowGraph coalesce_with_notes =
      ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);
  coalesce_with_notes.sideband.basic_block_live_at_start = {
      {0, {4}},
      {1, {8}},
      {2, {2}},
  };
  const RtlControlFlowGraph coalesce_stripped = strip_notes_and_live_at_start(coalesce_with_notes);

  const RtlControlFlowGraph lifted_coalesce_notes =
      InvertAlterReg(coalesce_with_notes, o2_prof, 72);
  const RtlControlFlowGraph lifted_coalesce_stripped =
      InvertAlterReg(coalesce_stripped, o2_prof, 72);
  EXPECT_EQ(lifted_coalesce_notes.sideband.reg_renumber.size(), 1u);
  EXPECT_EQ(lifted_coalesce_stripped.sideband.reg_renumber,
            lifted_coalesce_notes.sideband.reg_renumber);
  ASSERT_EQ(lifted_coalesce_stripped.rpo_blocks.size(), lifted_coalesce_notes.rpo_blocks.size());
  for (size_t b = 0; b < lifted_coalesce_notes.rpo_blocks.size(); ++b) {
    ASSERT_EQ(lifted_coalesce_stripped.rpo_blocks[b].instructions.size(),
              lifted_coalesce_notes.rpo_blocks[b].instructions.size());
    for (size_t i = 0; i < lifted_coalesce_notes.rpo_blocks[b].instructions.size(); ++i) {
      auto pn_notes = ParsePatternAndNotes(lifted_coalesce_notes.rpo_blocks[b].instructions[i]);
      auto pn_strip = ParsePatternAndNotes(lifted_coalesce_stripped.rpo_blocks[b].instructions[i]);
      ASSERT_TRUE(pn_notes.ok() && pn_strip.ok());
      EXPECT_EQ(pn_strip->pattern.ToSexprString(), pn_notes->pattern.ToSexprString());
    }
  }
  const RightInverseResult coal_notes_res =
      verifier_.VerifyAlterRegRightInverse(coalesce_with_notes);
  EXPECT_TRUE(coal_notes_res.matches) << coal_notes_res.diff_detail;
  const RightInverseResult coal_strip_res = verifier_.VerifyAlterRegRightInverse(coalesce_stripped);
  EXPECT_TRUE(coal_strip_res.matches) << coal_strip_res.diff_detail;

  // Case B (must NOT coalesce): after the `PARALLEL` RMW on `$v0` in `B1` dies on a store, `B1`
  // defines a new `$v0` web that flows live into `B2`. Even when all `REG_DEAD` notes and
  // `basic_block_live_at_start` are stripped, `ReconstructRtlLiveness` detects `$v0` live-in at
  // `B2` and refuses to coalesce `p_w` into global `p_r`.
  RtlControlFlowGraph raw_split;
  raw_split.pass = RtlPassKind::kGreg;
  raw_split.rpo_blocks = {
      RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3))) "
                  "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                  "(set (pc) (label_ref L0))",
              },
          .successors = {1},
          .has_trailing_barrier = true,
      },
      RtlBasicBlock{
          .id = 1,
          .instructions =
              {
                  "(parallel[ (set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1))) "
                  "(clobber (reg:SI 1 at)) ] ) (expr_list:REG_UNUSED (reg:SI 1 at) (nil))",
                  "(set (mem:SI (reg:SI 5 a1)) (reg:SI 2 v0)) "
                  "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))",
                  "(set (reg:SI 2 v0) (plus:SI (reg:SI 6 a2) (const_int 9))) "
                  "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                  "(set (pc) (label_ref L1))",
              },
          .successors = {2},
          .has_label = true,
          .has_trailing_barrier = true,
      },
      RtlBasicBlock{
          .id = 2,
          .instructions =
              {
                  "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                  "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
              },
          .has_label = true,
      },
  };
  SyncAllBlocks(&raw_split);
  ctx_.MaterializeFromCfg(raw_split);
  RtlControlFlowGraph split_with_notes =
      ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);
  split_with_notes.sideband.basic_block_live_at_start = {
      {0, {4, 5, 6}},
      {1, {2, 5, 6}},
      {2, {2}},
  };
  const RtlControlFlowGraph split_stripped = strip_notes_and_live_at_start(split_with_notes);

  const RtlControlFlowGraph lifted_split_notes = InvertAlterReg(split_with_notes, o2_prof, 72);
  const RtlControlFlowGraph lifted_split_stripped = InvertAlterReg(split_stripped, o2_prof, 72);
  EXPECT_EQ(lifted_split_notes.sideband.reg_renumber.size(), 3u);
  EXPECT_EQ(lifted_split_stripped.sideband.reg_renumber, lifted_split_notes.sideband.reg_renumber);
  ASSERT_EQ(lifted_split_stripped.rpo_blocks.size(), lifted_split_notes.rpo_blocks.size());
  for (size_t b = 0; b < lifted_split_notes.rpo_blocks.size(); ++b) {
    ASSERT_EQ(lifted_split_stripped.rpo_blocks[b].instructions.size(),
              lifted_split_notes.rpo_blocks[b].instructions.size());
    for (size_t i = 0; i < lifted_split_notes.rpo_blocks[b].instructions.size(); ++i) {
      auto pn_notes = ParsePatternAndNotes(lifted_split_notes.rpo_blocks[b].instructions[i]);
      auto pn_strip = ParsePatternAndNotes(lifted_split_stripped.rpo_blocks[b].instructions[i]);
      ASSERT_TRUE(pn_notes.ok() && pn_strip.ok());
      EXPECT_EQ(pn_strip->pattern.ToSexprString(), pn_notes->pattern.ToSexprString());
    }
  }
  const RightInverseResult split_notes_res = verifier_.VerifyAlterRegRightInverse(split_with_notes);
  EXPECT_TRUE(split_notes_res.matches) << split_notes_res.diff_detail;
  const RightInverseResult split_strip_res = verifier_.VerifyAlterRegRightInverse(split_stripped);
  EXPECT_TRUE(split_strip_res.matches) << split_strip_res.diff_detail;
}

TEST_F(
    InvertAlterRegTest,
    DemotesVictimWhenHigherPrioritySuggThiefStealsTargetAndDetectsCallCrossingSpillFalseCombine) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips1"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);
  regclass_init();

  // 1. `qty_sugg_compare` orders Loop 1 quantities strictly by suggestion count (`sugg_rank`)
  //    before `reg_n_refs`. Here `g_thief` (`$f4` = `36`, carrying `REG_EQUIV`) has 1 arithmetic
  //    suggestion (`$f2` = `34`, `sugg_rank = 68`), while overlapping `g_victim` (`$f2` = `34`) has
  //    2 arithmetic suggestions (`$f2` and busy `$f0` = `32`, `sugg_rank = 136`). `InvertAlterReg`
  //    must demote `g_victim` to `(reg:DF 34 $f2)` while keeping `g_thief` as a pseudo
  //    (`reg_renumber == 36`) with `REG_EQUIV` intact so `SubfunctionId::kBlockAlloc` reproduces
  //    `reg_renumber == 36`!
  RtlControlFlowGraph raw_thief_cfg;
  raw_thief_cfg.pass = RtlPassKind::kGreg;
  raw_thief_cfg.sideband.saved_gpr_regnos = {31};
  raw_thief_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(parallel[ (set (reg:DF 32 $f0) (call (mem:SI (symbol_ref:SI (\"pin_f0\"))) "
              "(const_int 0))) (clobber (reg:SI 31 ra)) ] )",
              "(set (reg:DF 34 $f2) (plus:DF (reg:DF 44 $f12) (reg:DF 32 $f0))) "
              "(expr_list:REG_DEAD (reg:DF 44 $f12) (nil))",
              "(set (reg:DF 36 $f4) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 16)))) "
              "(expr_list:REG_EQUIV (mem:DF (plus:SI (reg:SI 29 sp) (const_int 16))) (nil))",
              "(set (mem:DF (reg:SI 5 a1)) (reg:DF 36 $f4))",
              "(set (mem:DF (reg:SI 4 a0)) (reg:DF 34 $f2)) "
              "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
              "(set (reg:DF 34 $f2) (neg:DF (reg:DF 36 $f4))) "
              "(expr_list:REG_DEAD (reg:DF 36 $f4) (nil))",
              "(set (mem:SF (reg:SI 6 a2)) (reg:SF 34 $f2)) "
              "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
              "(set (reg/i:DF 32 $f0) (reg:DF 32 $f0)) "
              "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))",
              "(use (reg/i:DF 32 $f0))",
          },
  });
  SyncAllBlocks(&raw_thief_cfg);
  raw_thief_cfg.rpo_blocks[0].rtl_insns[0].has_explicit_call_fusage = true;
  ctx_.MaterializeFromCfg(raw_thief_cfg);
  RtlControlFlowGraph thief_cfg = ctx_.DematerializeToCfg(ctx_.GetInsns(), RtlPassKind::kGreg);
  thief_cfg.rpo_blocks[0].rtl_insns[0].has_explicit_call_fusage = true;

  const RtlControlFlowGraph lifted_thief = InvertAlterReg(thief_cfg, o2_prof, 72);
  bool found_thief_pseudo_36 = false;
  for (const auto& [pseudo, hr] : lifted_thief.sideband.reg_renumber) {
    if (hr == 36) {
      found_thief_pseudo_36 = true;
    }
  }
  EXPECT_TRUE(found_thief_pseudo_36) << lifted_thief.FormatSummary();
  EXPECT_NE(lifted_thief.rpo_blocks[0].instructions[2].find("REG_EQUIV"), std::string::npos)
      << lifted_thief.FormatSummary();
  EXPECT_NE(lifted_thief.rpo_blocks[0].instructions[1].find("(set (reg:DF 34 $f2)"),
            std::string::npos)
      << lifted_thief.FormatSummary();
  const RightInverseResult thief_res = verifier_.VerifyAlterRegRightInverse(thief_cfg);
  EXPECT_TRUE(thief_res.matches) << thief_res.diff_detail;

  RtlControlFlowGraph pre_block = lifted_thief;
  pre_block.pass = RtlPassKind::kLreg;
  pre_block.sideband.reg_renumber.clear();
  const SubfunctionBoundarySnapshot block_replay = hooks_.ExecuteSubfunctionBoundary(
      SubfunctionId::kBlockAlloc, pre_block, {}, o2_prof.optimize_level, o2_prof.c_flags, {});
  EXPECT_EQ(block_replay.post_state.sideband.reg_renumber, lifted_thief.sideband.reg_renumber)
      << lifted_thief.FormatSummary();

  // 2. `IsCallCrossingReloadSpillFalseCombine`: a value spilled to `$sp + 16` before a `CALL_INSN`
  //    and reloaded into `$2 v0` immediately before defining a local quantity in `$3 v1` (`$2`
  //    dying via `REG_DEAD`) would falsely combine in `local-alloc.c:combine_regs` once
  //    `InvertReload` unspills `$sp + 16` back to a call-crossing pseudo. Verify `InvertAlterReg`
  //    preserves the
  //    `$sp + 16` reload slot untouched at `-O2` and resolves the `$3 v1` def cleanly.
  RtlControlFlowGraph raw_spill_cfg;
  raw_spill_cfg.pass = RtlPassKind::kGreg;
  raw_spill_cfg.sideband.frame_size_bytes = 16;
  raw_spill_cfg.sideband.outgoing_args_size = 16;
  raw_spill_cfg.sideband.saved_gpr_regnos = {31};
  raw_spill_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 4 a0)) "
              "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
              "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
              "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
              "(set (mem:SI (reg:SI 6 a2)) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
              "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
              "(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 9))) "
              "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
              "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 7 a3))) "
              "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 7 a3) (nil)))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&raw_spill_cfg);
  raw_spill_cfg.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage = true;

  const RtlControlFlowGraph lifted_spill = InvertAlterReg(raw_spill_cfg, o2_prof, 72);
  EXPECT_EQ(lifted_spill.sideband.frame_size_bytes, 16);
  EXPECT_TRUE(lifted_spill.sideband.reg_renumber.empty()) << lifted_spill.FormatSummary();
  const RightInverseResult spill_res = verifier_.VerifyAlterRegRightInverse(raw_spill_cfg);
  EXPECT_TRUE(spill_res.matches) << spill_res.diff_detail;
}

TEST_F(InvertAlterRegTest, OddFpRegsNotAllocatableUnderMfp32VsMfp64AndDyingLocalIntoSpillSlot) {
  // 1. `config/mips/mips.c:3448` (`HARD_REGNO_MODE_OK`): on `-mfp32`, odd FP
  //    registers (`$f1 = 33`, `$f3 = 35`, ...) are never valid for `SFmode`,
  //    so an `SFmode` local web allocated to `$f2 = 34` while `$f0 = 32` is
  //    busy must remain lifted as a pseudo (`reg_renumber == 34`) rather than
  //    being falsely demoted due to free odd `$f1`. Under `-mfp64`, odd `$f1`
  //    IS allocatable, so `$f2` requires local conflict resolution.
  RtlControlFlowGraph sf_cfg;
  sf_cfg.pass = RtlPassKind::kGreg;
  sf_cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:SF 32 $f0) (mem:SF (reg:SI 4 a0)))",
              "(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4)))) "
              "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
              "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
              "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 34 $f2) (nil)))",
              "(use (reg/i:SF 32 $f0))",
          },
  });
  SyncAllBlocks(&sf_cfg);

  const CompilationProfile mfp32_prof{2, {"-O2", "-mips1", "-mfp32"}, {}};
  const RtlControlFlowGraph lifted_32 = InvertAlterReg(sf_cfg, mfp32_prof, 72);
  bool found_f2_pseudo_32 = false;
  for (const auto& [pseudo, hr] : lifted_32.sideband.reg_renumber) {
    if (hr == 34) {
      found_f2_pseudo_32 = true;
    }
  }
  EXPECT_TRUE(found_f2_pseudo_32) << "Expected $f2 SFmode web to stay lifted under -mfp32:\n"
                                  << lifted_32.FormatSummary();

  const CompilationProfile mfp64_prof{2, {"-O2", "-mips3", "-mfp64"}, {}};
  const RtlControlFlowGraph lifted_64 = InvertAlterReg(sf_cfg, mfp64_prof, 72);
  bool found_f2_pseudo_64 = false;
  for (const auto& [pseudo, hr] : lifted_64.sideband.reg_renumber) {
    if (hr == 34) {
      found_f2_pseudo_64 = true;
    }
  }
  EXPECT_FALSE(found_f2_pseudo_64) << "Expected $f2 SFmode web to resolve conflict with free $f1 "
                                      "under -mfp64:\n"
                                   << lifted_64.FormatSummary();

  // 2. `local-alloc.c:1722` (`combine_regs`): when local `$f2 = 34` dies into
  //    a computation stored immediately to a reload spill slot (`20($sp)`),
  //    `InvertReload` folds the spill into a spilled pseudo and `combine_regs`
  //    spills `$f2` during `block_alloc`, leaving `$f2` free for overlapping
  //    local `$f4 = 36`. `InvertAlterReg` must therefore resolve `$f4` when
  //    the spill slot is reloaded, while keeping `$f4` lifted when no spill
  //    slot exists.
  auto build_spill_overlap_cfg = [](bool with_spill_reload) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = 32;
    cfg.sideband.saved_gpr_regnos = {31};
    std::vector<std::string> insns = {
        "(set (reg:SF 32 $f0) (mem:SF (reg:SI 4 a0)))",
        "(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))))",
        "(set (reg:SF 36 $f4) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 8)))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(set (reg:SF 34 $f2) (minus:SF (reg:SF 34 $f2) (reg:SF 32 $f0))) "
        "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
    };
    if (with_spill_reload) {
      insns.push_back(
          "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SF 34 $f2)) "
          "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))");
    } else {
      insns.push_back(
          "(set (mem:SF (reg:SI 5 a1)) (reg:SF 34 $f2)) "
          "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))");
    }
    insns.push_back(
        "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4))) "
        "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 36 $f4) "
        "(nil)))");
    if (with_spill_reload) {
      insns.push_back(
          "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
          "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
      insns.push_back("(set (reg:SF 38 $f6) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))))");
      insns.push_back(
          "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 38 $f6))) "
          "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 38 $f6) "
          "(nil)))");
    } else {
      insns.push_back(
          "(set (reg/i:SF 32 $f0) (reg:SF 32 $f0)) "
          "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))");
    }
    insns.push_back("(use (reg/i:SF 32 $f0))");
    cfg.rpo_blocks.push_back(RtlBasicBlock{.id = 0, .instructions = std::move(insns)});
    SyncAllBlocks(&cfg);
    if (with_spill_reload) {
      cfg.rpo_blocks[0].rtl_insns[6].has_explicit_call_fusage = true;
    }
    return cfg;
  };

  const RtlControlFlowGraph lifted_with_spill =
      InvertAlterReg(build_spill_overlap_cfg(/*with_spill_reload=*/true), mfp32_prof, 72);
  bool f4_lifted_with_spill = false;
  for (const auto& [pseudo, hr] : lifted_with_spill.sideband.reg_renumber) {
    if (hr == 36) {
      f4_lifted_with_spill = true;
    }
  }
  EXPECT_FALSE(f4_lifted_with_spill) << "Expected $f4 local to resolve conflict when overlapping "
                                        "$f2 dies into reload spill slot:\n"
                                     << lifted_with_spill.FormatSummary();

  const RtlControlFlowGraph lifted_no_spill =
      InvertAlterReg(build_spill_overlap_cfg(/*with_spill_reload=*/false), mfp32_prof, 72);
  bool f4_lifted_no_spill = false;
  for (const auto& [pseudo, hr] : lifted_no_spill.sideband.reg_renumber) {
    if (hr == 36) {
      f4_lifted_no_spill = true;
    }
  }
  EXPECT_TRUE(f4_lifted_no_spill)
      << "Expected $f4 local to stay lifted when overlapping $f2 is not spilled:\n"
      << lifted_no_spill.FormatSummary();
}

TEST_F(InvertAlterRegTest,
       LocalResolutionIgnoresReloadScratchesTiesModesAndAvoidsSelfDeadThiefDemotion) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips1", "-mfp32"}, {}};

  // 1. `toplev.c:3098-3142`, `local-alloc.c:1798, 1824`, `reload1.c:663`:
  //    A single-use GPR-to-FPR input reload scratch (`$f10 = 42` copying `a3`)
  //    is emitted by `reload()` after `local_alloc()` and must not inject a
  //    bogus `qty_phys_sugg` (`42`) into its consumer `$f4 = 36` during local
  //    resolution.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SF 32 $f0) (mem:SF (reg:SI 4 a0)))",
                "(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))))",
                "(set (reg:SF 42 $f10) (reg:SF 7 a3)) (expr_list:REG_DEAD (reg:SF 7 a3) (nil))",
                "(set (reg:SF 36 $f4) (mult:SF (reg:SF 42 $f10) (reg:SF 34 $f2)))",
                "(set (mem:SF (reg:SI 4 a0)) (reg:SF 32 $f0)) "
                "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                "(set (mem:SF (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SF 34 $f2)) "
                "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
                "(set (mem:SF (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:SF 36 $f4)) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SF 36 $f4) (nil)))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    bool f4_lifted = false;
    bool f10_lifted = false;
    for (const auto& [pseudo, hr] : lifted.sideband.reg_renumber) {
      if (hr == 36) {
        f4_lifted = true;
      }
      if (hr == 42) {
        f10_lifted = true;
      }
    }
    EXPECT_TRUE(f4_lifted) << "Expected $f4 consumer of GPR->FPR reload scratch to stay lifted:\n"
                           << lifted.FormatSummary();
    EXPECT_FALSE(f10_lifted) << "Expected GPR->FPR reload scratch $f10 to be demoted:\n"
                             << lifted.FormatSummary();
  }

  // 2. `local-alloc.c:1750-1884` (`combine_regs`): a `QImode` local pseudo in
  //    `2 v0` dying via `(subreg:SI (reg:QI 2 v0) 0)` into chained `SImode`
  //    local pseudos in `2 v0` combines legally (`MODES_TIEABLE_P(QI, SI)` and
  //    `hd == hs == 2`) and must not be marked `false_pseudo_combine`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:QI 2 v0) (mem:QI (reg:SI 4 a0)))",
                "(set (reg:SI 2 v0) (and:SI (subreg:SI (reg:QI 2 v0) 0) (const_int 15))) "
                "(expr_list:REG_DEAD (reg:QI 2 v0) (nil))",
                "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2))) "
                "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0)) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    int v0_pseudos = 0;
    for (const auto& [pseudo, hr] : lifted.sideband.reg_renumber) {
      if (hr == 2) {
        ++v0_pseudos;
      }
    }
    EXPECT_EQ(v0_pseudos, 3)
        << "Expected QI->SI->SI tieable chain in v0 to stay fully lifted as 3 pseudos:\n"
        << lifted.FormatSummary();
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[1].find("2 v0"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[2].find("2 v0"), std::string::npos)
        << lifted.FormatSummary();
  }

  // 3. `flow.c:2507-2550` / `sched.c:2872` / `local-alloc.c:1633`: when
  //    `g_thief` (`3 v1`, `raw_sugg = {2 v0}`) steals `2 v0` from `g_victim`
  //    (`2 v0`, `raw_sugg = {7 a3, 2 v0}`), and demoting `g_victim` would create
  //    an illegal self-dead `REG_DEAD (reg:SI 2 v0)` on `(set (reg/i:SI 2 v0)
  //    ...)`, resolve `g_thief` (`3 v1`) instead so `g_victim` stays lifted.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 2 v0) (ashift:SI (reg:SI 7 a3) (const_int 3))) "
                "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 2 v0))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    bool v0_lifted = false;
    bool v1_lifted = false;
    for (const auto& [pseudo, hr] : lifted.sideband.reg_renumber) {
      if (hr == 2) {
        v0_lifted = true;
      }
      if (hr == 3) {
        v1_lifted = true;
      }
    }
    EXPECT_TRUE(v0_lifted)
        << "Expected victim v0 to stay lifted to avoid self-dead REG_DEAD on reg/i v0:\n"
        << lifted.FormatSummary();
    EXPECT_FALSE(v1_lifted) << "Expected thief v1 to be demoted instead of victim v0:\n"
                            << lifted.FormatSummary();
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 2 v0)"),
              std::string::npos)
        << "Unexpected self-dead REG_DEAD (reg:SI 2 v0) on exit insn:\n"
        << lifted.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, Case3QtySuggCompareLiteralIndexBugResolvesMiddleGroup) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-Wa,-g"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // Positive case (`local-alloc.c:1442-1454` `case 3:` literal `qty_sugg_compare(0, 1)`
  // double-swap bug): 3 overlapping local webs:
  //   - g0 (`hr = 3 v1`, 2 refs): reads dying `$2 v0` -> `raw_sugg = {2 v0}`
  //   - g1 (`hr = 6 a2`, 2 refs): no dying hard-reg read -> `raw_sugg = {}`
  //   - g2 (`hr = 2 v0`, 3 refs): copies to `(reg/i:SI 2 v0)` -> `raw_copy_sugg = {2 v0}`
  // Because `next_qty == 3`, GCC 2.7.2 swaps `[0,1,2] -> [1,0,2] -> [0,1,2]`, so
  // `g0` runs before `g2` and steals `2 v0`. Demoting `g1` (`hr = 6 a2`) drops
  // `next_qty` to 2 (`case 2:`), so `g2` (3 refs) runs first and gets `2 v0`,
  // while `g0` runs second and gets `3 v1`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                "(const_int 0))) (clobber (reg:SI 31 ra)) ] )",
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                "(set (reg:SI 6 a2) (mem:SI (reg:SI 16 s0)))",
                "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 16 s0) (const_int 4))))",
                "(set (mem:SI (plus:SI (reg:SI 16 s0) (const_int 8))) (reg:SI 2 v0))",
                "(set (mem:SI (plus:SI (reg:SI 16 s0) (const_int 12))) (reg:SI 3 v1)) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                "(set (mem:SI (plus:SI (reg:SI 16 s0) (const_int 16))) (reg:SI 6 a2)) "
                "(expr_list:REG_DEAD (reg:SI 16 s0) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
                "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    cfg.rpo_blocks[0].rtl_insns[0].has_explicit_call_fusage = true;
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    bool v0_lifted = false;
    bool v1_lifted = false;
    bool a2_lifted = false;
    for (const auto& [pseudo, hr] : lifted.sideband.reg_renumber) {
      if (hr == 2) v0_lifted = true;
      if (hr == 3) v1_lifted = true;
      if (hr == 6) a2_lifted = true;
    }
    EXPECT_TRUE(v0_lifted) << lifted.FormatSummary();
    EXPECT_TRUE(v1_lifted) << lifted.FormatSummary();
    EXPECT_FALSE(a2_lifted)
        << "Expected middle unsuggested group g1 (6 a2) to be demoted under case 3 bug:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative contrast: when `g0` (`hr = 5 a1`) carries a raw suggestion `{5 a1}`
  // that does NOT overlap `g2.target_hr` (`2 v0`), all 3 local webs stay lifted
  // as pseudos and pass `VerifyAlterRegRightInverse`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0)))",
                "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
                "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:SI 2 v0))",
                "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 12))) (reg:SI 5 a1)) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 16))) (reg:SI 3 v1)) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))",
                "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    bool v0_lifted = false;
    bool v1_lifted = false;
    bool a1_lifted = false;
    for (const auto& [pseudo, hr] : lifted.sideband.reg_renumber) {
      if (hr == 2) v0_lifted = true;
      if (hr == 3) v1_lifted = true;
      if (hr == 5) a1_lifted = true;
    }
    EXPECT_TRUE(v0_lifted) << lifted.FormatSummary();
    EXPECT_TRUE(v1_lifted) << lifted.FormatSummary();
    EXPECT_TRUE(a1_lifted) << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, LocalResolutionDemotesNonEquivNonParallelQuantityMember) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // (a) Single-block local quantity `{p72, p73}` targeting `$3` while `$2` is
  //     free, where `p73` carries `REG_EQUIV (mem ...)` (`local-alloc.c:990-991,
  //     1020`). `SelectLocalResolutionDemoteMember` demotes plain non-`REG_EQUIV`
  //     producer `p72` to hard `$3`, which in turn gives `p73` a dying `$3`
  //     source (`just_try_suggested`) so the `REG_EQUIV` tail stays lifted as a
  //     pseudo (`reg_renumber -> 3`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 7)))",
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) "
                "(expr_list:REG_EQUIV (mem:SI (reg:SI 4 a0)) (nil)))",
                "(set (mem:SI (reg:SI 4 a0)) (reg:SI 3 v1)) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))",
                "(set (reg/i:SI 2 v0) (const_int 0))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // (b) Contrast: when the producer (`$4`) is a multi-clobber `PARALLEL`
  //     (`mulsi3_r4000`) combined into a quantity with conflicting `REG_EQUIV`
  //     tail (`$5`), demoting the `PARALLEL` would reverse `REG_UNUSED` scratch
  //     order under `flow.c:mark_set_1`, so it stays lifted as a pseudo while
  //     the tail (`$5`) is demoted instead!
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(parallel[ (set (reg:SI 4 a0) (mult:SI (reg:SI 4 a0) (reg:SI 6 a2))) "
                "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) "
                "(clobber (reg:SI 66 accum)) ] ) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 5 a1) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) "
                "(expr_list:REG_EQUIV (mem:SI (reg:SI 7 a3)) (nil)))",
                "(set (mem:SI (reg:SI 7 a3)) (reg:SI 5 a1)) "
                "(expr_list:REG_DEAD (reg:SI 7 a3) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                "(set (reg/i:SI 2 v0) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 4 a0)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 5 a1)"), std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, LocalResolutionAvoidsDemotingMemberCreatingSelfSetRegDead) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // Positive: single-block local quantity `{p72, p73}` targeting `$3` (`v1`)
  // while `$2` (`v0`) is free, where `p73` dies on `(set (reg/i:SI 3 v1) ...)`
  // setting hard `$3` (`flow.c:2506-2515`, `sched.c:2872-2906`).
  // `WouldDemoteCreateSelfSetRegDead` rejects demoting `p73` so
  // `SelectLocalResolutionDemoteMember` demotes `p72` (`insn[0]`) instead,
  // keeping `p73` lifted and avoiding a self-setting `REG_DEAD (reg:SI 3 v1)`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 7))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                "(set (reg/i:SI 3 v1) (mem:SI (reg:SI 3 v1))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                "(use (reg/i:SI 3 v1))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 3 v1)"),
              std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative contrast: when `insn[2]` sets disjoint hard register `(reg/i:SI 2 v0)`,
  // demoting `p73` does NOT create a self-setting `REG_DEAD`, so
  // `SelectLocalResolutionDemoteMember` demotes the tail `p73` (`insn[1]`) while
  // keeping `p72` (`insn[0]`) lifted as a pseudo.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 7))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                "(set (reg/i:SI 2 v0) (mem:SI (reg:SI 3 v1))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest,
       LocalResolutionPreservesEarlyclobberFoldTargetAndDemotesLowerOrderBlocker) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // Positive (`reload1.c:5641`, `mips.md:3722-3896`, `flow.c:2505-2520`,
  // `local-alloc.c:1512, 1598`): when a short `$a0` (`4`) local web is defined
  // by an `"=&d"` earlyclobber output reload `(set (reg:DI 4 a0) (reg:DI 10 t2))`
  // whose preceding `ashift:DI` `PARALLEL` carries `REG_DEAD (reg:DI 4 a0)`,
  // `WouldEarlyclobberFoldCreateSelfSetRegDead` forbids demoting `$a0` and
  // `TryDemoteOverlappingLowerOrderBlocker` pins the overlapping longer `$v0`
  // (`2`) blocker (`insn[0]`) instead so `local_alloc` still assigns `$a0`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(parallel[ (set (reg:DI 2 v0) (ashift:DI (reg:DI 4 a0) (const_int 37))) "
                "(clobber (reg:SI 7 a3)) ] ) (expr_list:REG_UNUSED (reg:SI 7 a3) (nil))",
                "(parallel[ (set (reg:DI 8 t0) (ashift:DI (reg:DI 4 a0) (const_int 5))) "
                "(clobber (reg:SI 9 t1)) ] ) (expr_list:REG_UNUSED (reg:SI 9 t1) (nil))",
                "(set (reg:SI 6 a2) (and:SI (reg:SI 6 a2) (const_int 63)))",
                "(parallel[ (set (reg:DI 10 t2) (ashift:DI (reg:DI 4 a0) (reg:SI 6 a2))) "
                "(clobber (reg:SI 7 a3)) ] ) (expr_list:REG_DEAD (reg:DI 4 a0) "
                "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_UNUSED (reg:SI 7 a3) (nil))))",
                "(set (reg:DI 4 a0) (reg:DI 10 t2))",
                "(set (reg:DI 2 v0) (xor:DI (reg:DI 2 v0) (reg:DI 8 t0))) "
                "(expr_list:REG_DEAD (reg:DI 2 v0) (expr_list:REG_DEAD (reg:DI 8 t0) (nil)))",
                "(parallel[ (set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 4 a0))) "
                "(clobber (reg:SI 6 a2)) ] ) (expr_list:REG_DEAD (reg:DI 2 v0) "
                "(expr_list:REG_DEAD (reg:DI 4 a0) (expr_list:REG_UNUSED (reg:SI 6 a2) (nil))))",
                "(use (reg/i:DI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 8u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(set (reg:DI 2 v0)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[4].find("(set (reg:DI 4 a0)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[5].find("(set (reg:DI 2 v0)"), std::string::npos)
        << lifted.FormatSummary();
    bool has_lifted_a0 = false;
    for (const auto& [p, hr] : lifted.sideband.reg_renumber) {
      if (p >= 72 && hr == 4) {
        has_lifted_a0 = true;
      }
    }
    EXPECT_TRUE(has_lifted_a0) << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative contrast: when the short `$a0` web is defined by a plain op whose
  // dying input is a disjoint register (`$s0`), demoting `$a0` creates no
  // self-setting `REG_DEAD`, so `$a0` is demoted directly and `$v0` (`insn[0]`)
  // remains lifted as a pseudo.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(parallel[ (set (reg:DI 2 v0) (ashift:DI (reg:DI 4 a0) (const_int 37))) "
                "(clobber (reg:SI 7 a3)) ] ) (expr_list:REG_UNUSED (reg:SI 7 a3) (nil))",
                "(parallel[ (set (reg:DI 8 t0) (ashift:DI (reg:DI 4 a0) (const_int 5))) "
                "(clobber (reg:SI 9 t1)) ] ) (expr_list:REG_DEAD (reg:DI 4 a0) "
                "(expr_list:REG_UNUSED (reg:SI 9 t1) (nil)))",
                "(set (reg:DI 4 a0) (xor:DI (reg:DI 16 s0) (reg:DI 8 t0))) "
                "(expr_list:REG_DEAD (reg:DI 16 s0) (nil))",
                "(set (reg:DI 2 v0) (xor:DI (reg:DI 2 v0) (reg:DI 8 t0))) "
                "(expr_list:REG_DEAD (reg:DI 2 v0) (expr_list:REG_DEAD (reg:DI 8 t0) (nil)))",
                "(parallel[ (set (reg/i:DI 2 v0) (plus:DI (reg:DI 2 v0) (reg:DI 4 a0))) "
                "(clobber (reg:SI 6 a2)) ] ) (expr_list:REG_DEAD (reg:DI 2 v0) "
                "(expr_list:REG_DEAD (reg:DI 4 a0) (expr_list:REG_UNUSED (reg:SI 6 a2) (nil))))",
                "(use (reg/i:DI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 6u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[0].find("(set (reg:DI 2 v0)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[2].find("(set (reg:DI 4 a0)"), std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, LocalResolutionNoConflictLibcallAndDownstreamSuggGuard) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // 1. Positive (`local-alloc.c:1317-1352, 2282-2315` `no_conflict_p` +
  //    `combine_regs`): a `REG_LIBCALL` `(clobber (reg:DI 4 a0))` paired with
  //    `REG_NO_CONFLICT` subword writes and a closing `REG_RETVAL` +
  //    `REG_EQUAL (sign_extend:DI (reg:SI 4 a0))` unions the `SI` input pseudo
  //    and `DI` output pseudo into one `LocalQtyGroup` so both stay lifted onto
  //    hard reg `4 a0` without false conflict demotion.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                "(clobber (reg:DI 4 a0)) (insn_list:REG_LIBCALL (nil))",
                "(set (subreg:SI (reg:DI 4 a0) 1) (reg:SI 4 a0)) "
                "(expr_list:REG_NO_CONFLICT (reg:SI 4 a0) (nil))",
                "(set (subreg:SI (reg:DI 4 a0) 0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_NO_CONFLICT (reg:SI 4 a0) "
                "(nil)))",
                "(set (reg:DI 4 a0) (reg:DI 4 a0)) "
                "(insn_list:REG_RETVAL "
                "(expr_list:REG_EQUAL (sign_extend:DI (reg:SI 4 a0)) (nil)))",
                "(set (mem:DI (reg:SI 6 a2)) (reg:DI 4 a0)) "
                "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_DEAD (reg:DI 4 a0) (nil)))",
                "(set (reg/i:SI 2 v0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    int a0_pseudos = 0;
    for (const auto& [p, hr] : lifted.sideband.reg_renumber) {
      if (p >= 72 && hr == 4) {
        ++a0_pseudos;
      }
    }
    EXPECT_GE(a0_pseudos, 2) << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. Positive (`local-alloc.c:1798, 1855`, `flow.c:2505-2520`): when a
  //    `false_pseudo_combine(src_p, cand_p)` target (`2 v0`) cannot be demoted
  //    due to Case-2 self-setting `REG_DEAD` on `(set (reg/i:SI 2 v0) ...)`,
  //    `TryDemoteFalseCombineSourceOrLowerOrderBlocker` breaks the tie by
  //    demoting the dying source `src_p` (`6 a2`) instead, keeping `cand_p`
  //    (`2 v0`) lifted without emitting self-setting `REG_DEAD (reg:SI 2 v0)`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 6 a2) (mem:SI (reg:SI 4 a0)))",
                "(set (reg:SI 2 v0) (plus:SI (reg:SI 6 a2) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
                "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 6 a2)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 2 v0)"),
              std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 3. Positive (`local-alloc.c:1266-1300` `poisons_downstream_sugg_member`):
  //    when upstream `p_up` (`$v1`) dies at downstream `p_dn`'s def (`$v1`)
  //    which also carries a conflicting dying hard reg (`4 a0`), demoting `p_up`
  //    first would poison `p_dn` with a Case-1 `(REG_DEAD 3 v1)` while leaving
  //    `p_dn`'s `4 a0` suggestion intact. Selecting `p_dn` (`insn[1]`) instead
  //    keeps `p_up` (`insn[0]`) lifted and passes `VerifyAlterRegRightInverse`
  //    (contrast with `LocalResolutionAvoidsDemotingMemberCreatingSelfSetRegDead`
  //    above where no conflicting hard `REG_DEAD` is present on `insn[1]`).
  {
    const CompilationProfile o1_prof{1, {"-O1", "-mips2", "-mcpu=r4000"}, {}};
    hooks_.ApplyCompilationProfile(o1_prof);
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 3 v1) (ashift:SI (reg:SI 4 a0) (const_int 1)))",
                "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 4 a0))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))",
                "(set (reg/i:SI 3 v1) (mem:SI (reg:SI 3 v1))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                "(use (reg/i:SI 3 v1))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o1_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, LocalResolutionPreservesLiveTicksIntoCrossBlockSpillSlot) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // 1. Positive (`local-alloc.c:470-478, 1773-1774`): local pseudo `p` in `$2 v0`
  //    dies into `dst` (`$7 a3`) which is stored to a cross-block spill slot
  //    `24($sp)` (`reg_qty[sreg] == -1`), so `combine_regs` returns `0` and `p`
  //    continues to occupy `$2 v0` in `regs_live_at`. Overlapping local pseudo
  //    `q` in `$3 v1` therefore sees `$2 v0` occupied and stays lifted (`reg_renumber == 3`).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = 32;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2)))",
                    "(set (reg:SI 7 a3) (plus:SI (reg:SI 2 v0) (const_int 3))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 7 a3)) "
                    "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 3 v1)) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                    "(label_ref L0) (pc))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                },
            .successors = {2, 1},
        },
        RtlBasicBlock{
            .id = 1,
            .instructions =
                {
                    "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
        },
        RtlBasicBlock{
            .id = 2,
            .instructions =
                {
                    "(set (reg/i:SI 2 v0) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
            .has_label = true,
        },
    };
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 6u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << "Overlapping local in $v1 should stay lifted when $v0 dies into cross-block spill:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. Negative contrast (`local-alloc.c:470-478, 1773-1774`): when `24($sp)` is
  //    strictly single-block local with `reg_n_deaths == 1` (`reg_qty[sreg] == -2`),
  //    `combine_regs` combines `p` (`$2 v0`) into the spilled local quantity so
  //    `$2 v0` is free in `regs_live_at`, forcing `q` (`$3 v1`) to be demoted so
  //    `find_free_reg` will not reassign `q` to `$2 v0`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = 32;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2)))",
                    "(set (reg:SI 7 a3) (plus:SI (reg:SI 2 v0) (const_int 3))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 7 a3)) "
                    "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 3 v1)) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
                    "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                    "(use (reg/i:SI 2 v0))",
                },
        },
    };
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 8u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << "Overlapping local in $v1 should be demoted when $v0 combines into local spill:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, DemotesInheritedMultiUseFprOutputReloadScratchToHardReg) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  auto build_cfg = [](bool reader_has_dead_note) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    const std::string reader_insn =
        reader_has_dead_note ? "(set (reg:SF 34 $f2) (plus:SF (reg:SF 34 $f2) (reg:SF 32 $f0))) "
                               "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))"
                             : "(set (reg:SF 34 $f2) (plus:SF (reg:SF 34 $f2) (reg:SF 32 $f0)))";
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SF 34 $f2) (mem/u:SF (reg:SI 4 a0))) "
                "(expr_list:REG_EQUIV (mem/u:SF (reg:SI 4 a0)) (nil))",
                "(set (reg:SF 36 $f4) (mem/u:SF (plus:SI (reg:SI 4 a0) (const_int 4)))) "
                "(expr_list:REG_EQUIV (mem/u:SF (plus:SI (reg:SI 4 a0) (const_int 4))) (nil))",
                "(set (reg:SF 32 $f0) (mem/u:SF (plus:SI (reg:SI 4 a0) (const_int 8)))) "
                "(expr_list:REG_EQUIV (mem/u:SF (plus:SI (reg:SI 4 a0) (const_int 8))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil)))",
                "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4))) "
                "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 36 $f4) (nil)))",
                "(set (reg:SF 16 s0) (reg:SF 32 $f0))",
                reader_insn,
                "(set (mem:SF (reg:SI 5 a1)) (reg:SF 34 $f2)) "
                "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"SinkF\"))) "
                "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                "(set (reg:SF 40 $f8) (reg:SF 16 s0))",
                "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 40 $f8))) "
                "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 16 s0) (nil)))",
                "(use (reg/i:SF 32 $f0))",
            },
    });
    SyncAllBlocks(&cfg);
    return cfg;
  };

  // Positive (`reload1.c:5186-5249, 5641`): `$f0` produced by `add.s` and used
  // by note-free `mfc1 s0,$f0` plus inherited FP reader `add.s $f2,$f2,$f0`
  // must be demoted back to hard reg `32 $f0` so `InvertReload` can fold it.
  {
    const RtlControlFlowGraph cfg = build_cfg(/*reader_has_dead_note=*/false);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 11u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[3].find("(set (reg:SF 32 $f0)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[4].find("(reg:SF 32 $f0))"), std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative 1: when the downstream FP reader carries `(REG_DEAD (reg:SF 32 $f0))`,
  // `$f0` is a normal allocated pseudo rather than a note-free output-reload
  // scratch and stays lifted as a pseudo.
  {
    const RtlControlFlowGraph cfg = build_cfg(/*reader_has_dead_note=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 11u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[3].find("(set (reg:SF 32 $f0)"), std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative 2 (`cse.c:481-486`): when the cross-bank move targets an outgoing
  // call-arg hard register `$a1` (`5`) alongside a pure FP reader, `$f0` is a
  // multi-use `FP_REGS` pseudo copied to `$a1` and must stay lifted as a pseudo.
  {
    RtlControlFlowGraph cfg = build_cfg(/*reader_has_dead_note=*/false);
    cfg.rpo_blocks[0].instructions[4] = "(set (reg:SF 5 a1) (reg:SF 32 $f0))";
    cfg.rpo_blocks[0].instructions[6] =
        "(set (mem:SF (reg:SI 6 a2)) (reg:SF 34 $f2)) "
        "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))";
    cfg.rpo_blocks[0].instructions[7] =
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"SinkF\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )";
    cfg.rpo_blocks[0].instructions[8] = "(set (reg:SF 40 $f8) (reg:SF 32 $f0))";
    cfg.rpo_blocks[0].instructions[9] =
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 40 $f8))) "
        "(expr_list:REG_DEAD (reg:SF 32 $f0) (expr_list:REG_DEAD (reg:SF 40 $f8) (nil)))";
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 11u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[3].find("(set (reg:SF 32 $f0)"), std::string::npos)
        << lifted.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, LocalResolutionSkipsUnallocatableCallCrossingGprFalseCombine) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  auto build_cfg = [](bool occupy_all_callee_saved_gprs) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    std::vector<std::string> insns = {
        "(set (reg:SI 17 s1) (mem/u:SI (reg:SI 4 a0)))",
        "(set (reg:SI 18 s2) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
        "(set (reg:SI 19 s3) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 8))))",
        "(set (reg:SI 20 s4) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 12))))",
        "(set (reg:SI 21 s5) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 16))))",
        "(set (reg:SI 22 s6) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 20))))",
    };
    if (occupy_all_callee_saved_gprs) {
      insns.push_back("(set (reg:SI 23 s7) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 24))))");
    }
    insns.push_back(
        "(set (reg:SI 3 v1) (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int 28)))) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    insns.push_back(
        "(set (reg:SI 16 s0) (plus:SI (reg:SI 3 v1) (const_int 5))) "
        "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))");
    insns.push_back(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    insns.push_back(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 16 s0) (nil)))");
    insns.push_back(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 17 s1))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 17 s1) (nil)))");
    insns.push_back(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 18 s2))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 18 s2) (nil)))");
    insns.push_back(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 19 s3))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 19 s3) (nil)))");
    insns.push_back(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 20 s4))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 20 s4) (nil)))");
    insns.push_back(
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 21 s5))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 21 s5) (nil)))");
    if (occupy_all_callee_saved_gprs) {
      insns.push_back(
          "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 22 s6))) "
          "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 22 s6) (nil)))");
      insns.push_back(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 23 s7))) "
          "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 23 s7) (nil)))");
    } else {
      insns.push_back(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 22 s6))) "
          "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 22 s6) (nil)))");
    }
    insns.push_back("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks.push_back(RtlBasicBlock{.id = 0, .instructions = std::move(insns)});
    SyncAllBlocks(&cfg);
    const size_t call_idx = occupy_all_callee_saved_gprs ? 9 : 8;
    cfg.rpo_blocks[0].rtl_insns[call_idx].has_explicit_call_fusage = true;
    return cfg;
  };

  // Positive (`local-alloc.c:1525-1538, 1855-1870, 2106, 2121, 2205-2216`):
  // when all 8 callee-saved GPRs `$16..$23` are occupied over `[birth(ps),
  // death(pd)]` and `n_refs <= 4 * calls_crossed`, `local-alloc` leaves the
  // combined qty `{ps ($3 v1), pd ($16 s0)}` unallocated (`-1`) so `global-alloc`
  // allocates them separately; `pd` (`$16 s0`) stays lifted as a pseudo.
  {
    const RtlControlFlowGraph cfg = build_cfg(/*occupy_all_callee_saved_gprs=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[8].find("(set (reg:SI 16 s0)"), std::string::npos)
        << "Expected call-crossing $s0 to stay lifted when all $s0..$s7 are occupied:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative contrast: when `$23 s7` is free over `[birth(ps), death(pd)]`,
  // `local-alloc` would allocate the combined qty `{ps, pd}` to one callee-saved
  // hard reg (`hs == hd`), so `false_pseudo_combine` must break the combine
  // (`pd` demoted to hard `$16 s0` or `ps` demoted to hard `$3 v1`) and pass
  // `VerifyAlterRegRightInverse`.
  {
    const RtlControlFlowGraph cfg = build_cfg(/*occupy_all_callee_saved_gprs=*/false);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    const bool s0_demoted =
        lifted.rpo_blocks[0].instructions[7].find("(set (reg:SI 16 s0)") != std::string::npos;
    const bool v1_demoted =
        lifted.rpo_blocks[0].instructions[6].find("(set (reg:SI 3 v1)") != std::string::npos;
    EXPECT_TRUE(s0_demoted || v1_demoted)
        << "Expected false_pseudo_combine to demote $s0 or $v1 when $s7 is free:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, LocalResolutionLeftToRightSpillCombineAndLowerOrderDeferral) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // 1. `local-alloc.c:1241-1300` (`block_alloc` -> `combine_regs` left-to-right
  //    first-`win` `break`): when two dying same-bank local pseudos (`$a0` and
  //    `$v0`) feed a single-block spilled local `(set (reg:SI 7 a3) (plus:SI
  //    op1 op2))`, ONLY the leftmost dying operand (`op1`) combines into the
  //    spilled dest. When `$a0` (`4`, blocked below by `$v0`/`$v1`) is `op1`
  //    (positive), `$v0` remains uncombined and keeps occupying `$2 v0` in
  //    `regs_live_at`, so overlapping `$3 v1` stays lifted. Swapping to
  //    `(plus:SI $v0 $a0)` (negative) combines `$v0` into the spilled local and
  //    frees `$2 v0`, forcing `$3 v1` to be demoted.
  auto make_spill_combine_cfg = [](bool v0_is_second_operand) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = 32;
    const std::string add_insn =
        v0_is_second_operand
            ? "(set (reg:SI 7 a3) (plus:SI (reg:SI 4 a0) (reg:SI 2 v0))) "
              "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))"
            : "(set (reg:SI 7 a3) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
              "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))";
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:SI 2 v0) (mem/u:SI (reg:SI 16 s0)))",
                    "(set (reg:SI 3 v1) (mem/u:SI (plus:SI (reg:SI 16 s0) (const_int 4))))",
                    "(set (reg:SI 4 a0) (mem/u:SI (plus:SI (reg:SI 16 s0) (const_int 8))))",
                    add_insn,
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 7 a3)) "
                    "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                    "(set (mem:SI (reg:SI 17 s1)) (reg:SI 3 v1)) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 17 s1) (nil)))",
                    "(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 16 s0) (nil)))",
                    "(use (reg/i:SI 2 v0))",
                },
        },
    };
    SyncAllBlocks(&cfg);
    return cfg;
  };
  {
    const RtlControlFlowGraph cfg = make_spill_combine_cfg(/*v0_is_second_operand=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 9u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << "Expected $v1 to stay lifted when $a0 wins combine_regs ahead of $v0:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
  {
    const RtlControlFlowGraph cfg = make_spill_combine_cfg(/*v0_is_second_operand=*/false);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 9u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 3 v1)"), std::string::npos)
        << "Expected $v1 to be demoted when $v0 wins combine_regs into local spill:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. `local-alloc.c:2158-2181` (`HasOverlappingLowerOrderPendingResolution`)
  //    + `flow.c:2506-2515` (`CleanInvalidOrDuplicateRegDeadNotesOnInsn`):
  //    `$v1` (`3`, live `[0, 2]`) has `$v0` free over `[0, 2]` so it needs
  //    resolution; overlapping `$a0` (`4`, live `[1, 4]`) sees `$v0` occupied
  //    from `2..4` and only sees `$v1` (`3 < 4`) free until `$v1` resolves.
  //    Deferring `$a0` lets `$v1` resolve first so `$a0` stays lifted as a pseudo
  //    without spurious double-demotion.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 3 v1) (mem/u:SI (reg:SI 5 a1)))",
                "(set (reg:SI 4 a0) (mem/u:SI (plus:SI (reg:SI 5 a1) (const_int 4)))) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 9))) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                "(set (mem:SI (reg:SI 6 a2)) (reg/i:SI 2 v0)) "
                "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                "(set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (reg:SI 4 a0))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 6u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 4 a0)"), std::string::npos)
        << "Expected higher-order $a0 to be deferred and stay lifted once $v1 resolves:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 3. `reload1.c:2309-2400` (`PseudoTouchesReloadSpillSlot`): in a block that
  //    also holds a reload spill slot `24($sp)`, a non-spill local in `$v1` (`3`)
  //    whose lower-order reg `$v0` (`2`) is free over its own short span coalesces
  //    with an earlier disjoint non-spill `$v1` pseudo (staying lifted as a pseudo)
  //    instead of demoting to hard `$v1`; contrastingly, when the second `$v1`
  //    loads directly from the reload spill slot `24($sp)`, it is NOT coalesced
  //    into the earlier non-spill `$v1` pseudo.
  auto make_spill_coalesce_cfg = [](bool second_v1_loads_spill_slot) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = 32;
    const std::string second_v1_def =
        second_v1_loads_spill_slot
            ? "(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))"
            : "(set (reg:SI 3 v1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4)))) "
              "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))";
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 3 v1) (mem/u:SI (reg:SI 2 v0)))",
                "(set (mem:SI (reg:SI 16 s0)) (reg:SI 3 v1)) (expr_list:REG_DEAD (reg:SI 3 v1) "
                "(nil))",
                "(set (reg:SI 5 a1) (plus:SI (reg:SI 16 s0) (const_int 1)))",
                "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 5 a1)) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                second_v1_def,
                "(set (mem:SI (plus:SI (reg:SI 16 s0) (const_int 4))) (reg:SI 3 v1)) "
                "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                second_v1_loads_spill_slot
                    ? "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 7)))"
                    : "(set (reg/i:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    return cfg;
  };
  {
    const RtlControlFlowGraph cfg = make_spill_coalesce_cfg(/*second_v1_loads_spill_slot=*/false);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 9u);
    auto p1 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[1]);
    auto p5 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[5]);
    ASSERT_TRUE(p1.ok() && p5.ok());
    ASSERT_TRUE(p1->pattern.IsSet() && p5->pattern.IsSet());
    EXPECT_TRUE(p1->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
    EXPECT_TRUE(p5->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
    EXPECT_EQ(p1->pattern.SetDest().RegNo(), p5->pattern.SetDest().RegNo())
        << "Expected second non-spill $v1 local to coalesce with first $v1:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
  {
    const RtlControlFlowGraph cfg = make_spill_coalesce_cfg(/*second_v1_loads_spill_slot=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 9u);
    auto p1 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[1]);
    auto p5 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[5]);
    ASSERT_TRUE(p1.ok() && p5.ok());
    ASSERT_TRUE(p1->pattern.IsSet() && p5->pattern.IsSet());
    EXPECT_NE(p1->pattern.SetDest().RegNo(), p5->pattern.SetDest().RegNo())
        << "Expected spill-slot load $v1 NOT to coalesce into earlier non-spill $v1:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, CrossClassReloadInheritedFprWebAndUnallocatableFpGprCombine) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // 1. `local-alloc.c:700-839, 1004-1007` (`optimize_reg_copy_1`) +
  //    `reload1.c:5186-5257` (`choose_reload_regs` reload inheritance):
  //    When `(set (reg:SF 16 s0) (reg:SF 32 $f0)) (REG_DEAD $f0)` is followed
  //    by an FPR-only consumer reading `$f0` with `$s0` and `$f0` unmodified in
  //    between (positive), `HasLaterCrossClassReloadInheritedFprRead` suppresses
  //    killing `$f0`'s web at the copy so both uses share the same pseudo.
  //    When `$s0` is modified before the later `$f0` read (negative), the
  //    `REG_DEAD $f0` kill IS honored.
  auto make_cross_class_inherit_cfg = [](bool clobber_s0_between) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    RtlBasicBlock b0{
        .id = 0,
        .instructions =
            {
                "(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))",
            },
        .successors = {1},
    };
    std::vector<std::string> b1_insns = {
        "(set (reg:SF 16 s0) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
    };
    if (clobber_s0_between) {
      b1_insns.push_back("(set (reg:SF 16 s0) (reg:SF 46 $f14))");
    }
    b1_insns.push_back(
        "(set (reg:SF 34 $f2) (plus:SF (reg:SF 32 $f0) (reg:SF 44 $f12))) "
        "(expr_list:REG_DEAD (reg:SF 44 $f12) (nil))");
    b1_insns.push_back(
        "(set (mem:SF (reg:SI 5 a1)) (reg:SF 34 $f2)) "
        "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))");
    const size_t call_idx = b1_insns.size();
    b1_insns.push_back("(call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16))");
    b1_insns.push_back(
        "(set (mem:SF (plus:SI (reg:SI 5 a1) (const_int 4))) (reg:SF 16 s0)) "
        "(expr_list:REG_DEAD (reg:SF 16 s0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))");
    b1_insns.push_back(
        "(set (reg/i:SF 32 $f0) (reg:SF 46 $f14)) "
        "(expr_list:REG_DEAD (reg:SF 46 $f14) (nil))");
    b1_insns.push_back("(use (reg/i:SF 32 $f0))");
    RtlBasicBlock b1{.id = 1, .instructions = std::move(b1_insns), .has_label = true};
    cfg.rpo_blocks.push_back(std::move(b0));
    cfg.rpo_blocks.push_back(std::move(b1));
    SyncAllBlocks(&cfg);
    cfg.rpo_blocks[1].rtl_insns[call_idx].has_explicit_call_fusage = true;
    return cfg;
  };
  {
    const RtlControlFlowGraph cfg = make_cross_class_inherit_cfg(/*clobber_s0_between=*/false);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks.size(), 2u);
    auto p0 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[0]);
    auto p1 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[0]);
    auto p2 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[1]);
    ASSERT_TRUE(p0.ok() && p1.ok() && p2.ok());
    const int def_pseudo = p0->pattern.SetDest().RegNo();
    EXPECT_GE(def_pseudo, 72) << lifted.FormatSummary();
    EXPECT_EQ(p1->pattern.SetSrc().RegNo(), def_pseudo) << lifted.FormatSummary();
    EXPECT_EQ(p2->pattern.SetSrc().children[0].RegNo(), def_pseudo)
        << "Expected reload-inherited FPR read to share the same pseudo web as $f0:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
  {
    const RtlControlFlowGraph cfg = make_cross_class_inherit_cfg(/*clobber_s0_between=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks.size(), 2u);
    auto p0 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[0]);
    auto p3 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[2]);
    ASSERT_TRUE(p0.ok() && p3.ok());
    // With `$s0` clobbered in between, `REG_DEAD $f0` killed the web at insn 0,
    // so insn 2's `$f0` read is NOT unified into block 0's pseudo.
    EXPECT_NE(p3->pattern.SetSrc().children[0].RegNo(), p0->pattern.SetDest().RegNo())
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. `local-alloc.c:1525-1538, 1855-1870, 2106, 2121, 2205-2216` + `regs.h:165`:
  //    When a call-crossing `SF` pseudo in `$fp` (`30`) dies in the birth of a
  //    non-call-crossing `SF` pseudo in call-used `$f4` (`36`) while all
  //    callee-saved FPRs (`$f20..$f30`) AND all callee-saved GPRs (`$s0..$s7`)
  //    are live across the span and `combined_refs <= 4 * total_calls` (positive),
  //    `IsUnallocatableCallCrossingFpGprCombine` prevents combining `$fp` and
  //    `$f4` into one unallocatable local qty. When a callee-saved FPR (`$f20`)
  //    is free across the span (negative), the guard returns false.
  auto make_fp_gpr_combine_cfg = [](bool occupy_all_callee_saved) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_pointer_needed = false;
    std::vector<std::string> insns;
    // Define `$fp` (`30`, SF) and all callee-saved GPRs (`$s0..$s7`, 16..23) +
    // callee-saved FPRs (`$f20..$f30`, 52..62 even), optionally omitting `$f20`.
    insns.push_back("(set (reg:SF 30 $fp) (mem/u:SF (reg:SI 4 a0)))");
    for (int s = 16; s <= 23; ++s) {
      insns.push_back("(set (reg:SI " + std::to_string(s) + " s" + std::to_string(s - 16) +
                      ") (mem/u:SI (plus:SI (reg:SI 4 a0) (const_int " +
                      std::to_string((s - 15) * 4) + "))))");
    }
    const int first_cs_fp = occupy_all_callee_saved ? 52 : 54;
    for (int f = first_cs_fp; f <= 62; f += 2) {
      insns.push_back("(set (reg:SF " + std::to_string(f) + " $f" + std::to_string(f - 32) +
                      ") (mem/u:SF (plus:SI (reg:SI 4 a0) (const_int " +
                      std::to_string((f - 32) * 4) + "))))");
    }
    // Call crossed by `$fp` (`30`) and all live callee-saved regs.
    const size_t call_idx = insns.size();
    insns.push_back("(call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16))");
    // `$fp` (`30`) dies in the birth of non-call-crossing `$f4` (`36`, call-used).
    // Keep `$f0`/`$f2` busy across `[birth($f4), death($f4)]` so `$f4` stays lifted.
    insns.push_back("(set (reg:SF 32 $f0) (mem/u:SF (reg:SI 16 s0)))");
    insns.push_back("(set (reg:SF 34 $f2) (mem/u:SF (reg:SI 17 s1)))");
    insns.push_back(
        "(set (reg:SF 36 $f4) (plus:SF (reg:SF 30 $fp) (reg:SF 32 $f0))) "
        "(expr_list:REG_DEAD (reg:SF 30 $fp) (nil))");
    // Use all callee-saved GPRs, callee-saved FPRs, `$f0`, `$f2`, and `$f4`.
    insns.push_back(
        "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 34 $f2))) "
        "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))");
    for (int f = first_cs_fp; f <= 62; f += 2) {
      insns.push_back("(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF " + std::to_string(f) +
                      " $f" + std::to_string(f - 32) + "))) (expr_list:REG_DEAD (reg:SF " +
                      std::to_string(f) + " $f" + std::to_string(f - 32) + ") (nil))");
    }
    for (int s = 16; s <= 23; ++s) {
      insns.push_back("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int " +
                      std::to_string((s - 16) * 4) + "))) (reg:SI " + std::to_string(s) + " s" +
                      std::to_string(s - 16) + ")) (expr_list:REG_DEAD (reg:SI " +
                      std::to_string(s) + " s" + std::to_string(s - 16) + ") (nil))");
    }
    insns.push_back(
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4))) "
        "(expr_list:REG_DEAD (reg:SF 36 $f4) (nil))");
    insns.push_back("(use (reg/i:SF 32 $f0))");
    RtlBasicBlock blk{.id = 0, .instructions = std::move(insns)};
    cfg.rpo_blocks.push_back(std::move(blk));
    SyncAllBlocks(&cfg);
    cfg.rpo_blocks[0].rtl_insns[call_idx].has_explicit_call_fusage = true;
    return cfg;
  };
  {
    const RtlControlFlowGraph cfg = make_fp_gpr_combine_cfg(/*occupy_all_callee_saved=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    // Find the `$fp` birth and the `$f4` birth (right after `call`, `$f0`, `$f2`).
    auto p_fp = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[0]);
    ASSERT_TRUE(p_fp.ok());
    const int fp_reg = p_fp->pattern.SetDest().RegNo();
    size_t f4_birth_idx = 0;
    for (size_t idx = 0; idx < lifted.rpo_blocks[0].instructions.size(); ++idx) {
      if (lifted.rpo_blocks[0].instructions[idx].find("(plus:SF ") != std::string::npos) {
        f4_birth_idx = idx;
        break;
      }
    }
    ASSERT_GT(f4_birth_idx, 0u);
    auto p_f4 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[f4_birth_idx]);
    ASSERT_TRUE(p_f4.ok());
    EXPECT_NE(p_f4->pattern.SetDest().RegNo(), fp_reg)
        << "Expected call-crossing $fp NOT to combine with non-call-crossing $f4 when all "
           "callee-saved FPRs/GPRs are occupied:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
  {
    const RtlControlFlowGraph cfg = make_fp_gpr_combine_cfg(/*occupy_all_callee_saved=*/false);
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, ResolveDeathlessLocalPseudosEnforcesLocalAllocBirthDeathInvariant) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  hooks_.ApplyCompilationProfile(o2_prof);

  // Positive 1: multi-SET `PARALLEL` (`udivmod`) reading `$2` and writing `$2`
  // with no `REG_DEAD $2` coalesces the two webs into one valid local pseudo
  // whose death note is on the final consumer.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(parallel[ (set (reg:SI 1 at) (udiv:SI (reg:SI 5 a1) (reg:SI 2 v0))) "
                "(set (reg:SI 2 v0) (umod:SI (reg:SI 5 a1) (reg:SI 2 v0))) "
                "(clobber (reg:SI 65 lo)) (clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] ) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_UNUSED (reg:SI 1 at) "
                "(expr_list:REG_UNUSED (reg:SI 66 accum) (expr_list:REG_UNUSED (reg:SI 64 hi) "
                "(expr_list:REG_UNUSED (reg:SI 65 lo) (nil))))))",
                "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
    // Coalesced into a single pseudo (reg:SI 72) with REG_DEAD on insn 2.
    EXPECT_NE(lifted.rpo_blocks[0].instructions[0].find("(set (reg:SI 72)"), std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[1].find("(set (reg:SI 72) (umod:SI"),
              std::string::npos)
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 72)"), std::string::npos)
        << lifted.FormatSummary();
  }

  // Positive 2 vs Negative: single-block `$2` read by `(set (reg/i:SI 2 v0) ...)`
  // without `REG_DEAD $2` must be demoted back to hard reg `$2` so `block_alloc`
  // never aborts at `local-alloc.c:2093`; when `REG_DEAD $2` IS present, `$2`
  // remains lifted as a valid single-block pseudo.
  auto build_ret_cfg = [](bool has_dead_v0) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    const std::string ret_notes =
        has_dead_v0 ? "(expr_list:REG_DEAD (reg:SI 8 t0) (expr_list:REG_DEAD (reg:SI 2 v0) (nil)))"
                    : "(expr_list:REG_DEAD (reg:SI 8 t0) (nil))";
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 2))) "
                "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                "(set (reg/i:SI 2 v0) (xor:SI (reg:SI 8 t0) (reg:SI 2 v0))) " + ret_notes,
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    return cfg;
  };
  {
    const RtlControlFlowGraph lifted_demoted =
        InvertAlterReg(build_ret_cfg(/*has_dead_v0=*/false), o2_prof, 72);
    ASSERT_EQ(lifted_demoted.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_NE(lifted_demoted.rpo_blocks[0].instructions[1].find("(set (reg:SI 2 v0)"),
              std::string::npos)
        << lifted_demoted.FormatSummary();
  }
  {
    const RtlControlFlowGraph lifted_kept =
        InvertAlterReg(build_ret_cfg(/*has_dead_v0=*/true), o2_prof, 72);
    ASSERT_EQ(lifted_kept.rpo_blocks[0].instructions.size(), 4u);
    EXPECT_EQ(lifted_kept.rpo_blocks[0].instructions[1].find("(set (reg:SI 2 v0)"),
              std::string::npos)
        << lifted_kept.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, PrecomputePairedLibcallScopesPreservesCalleeSavedClobberAcrossCall) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  auto build_cfg = [](int target_reg, const char* reg_name) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    const std::string reg_str = "(reg:DI " + std::to_string(target_reg) + " " + reg_name + ")";
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(clobber " + reg_str + ")",
                "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"prep_fn\"))) "
                "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                "(set (reg:SI 4 a0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                "(parallel[ (set (reg:DI 2 v0) (call (mem:SI (symbol_ref:SI (\"__divdi3\"))) "
                "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                "(set " + reg_str +
                    " (reg:DI 2 v0)) (expr_list:REG_DEAD (reg:DI 2 v0) "
                    "(expr_list:REG_RETVAL (const_int 0) (nil)))",
                "(set (reg/i:DI 2 v0) " + reg_str + ") (expr_list:REG_DEAD " + reg_str + " (nil))",
                "(use (reg/i:DI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    return cfg;
  };

  // Positive (`optabs.c:2485-2525` + `sched1` hoisting across an unrelated
  // call): a callee-saved `(clobber (reg:DI 16 s0))` survives across the
  // intervening `CALL_INSN`, so `PrecomputePairedLibcallScopes` pairs it with
  // the post-call `REG_RETVAL` into the SAME lifted pseudo (`reg:DI 72`).
  {
    const RtlControlFlowGraph lifted = InvertAlterReg(build_cfg(16, "s0"), o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 7u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[0], "(clobber (reg:DI 72))")
        << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[0].instructions[4].find("(set (reg:DI 72) (reg:DI 2 v0))"),
              std::string::npos)
        << lifted.FormatSummary();
  }

  // Negative: a call-clobbered `(clobber (reg:DI 8 t0))` cannot survive an
  // intervening `CALL_INSN`, so it is flushed at the call and does NOT pair
  // with the post-call `REG_RETVAL` (`instructions[0]` stays hard `8 t0`).
  {
    const RtlControlFlowGraph lifted = InvertAlterReg(build_cfg(8, "t0"), o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 7u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[0], "(clobber (reg:DI 8 t0))")
        << lifted.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, PreservesDuplicateHardRegDeadNotesAndGuardsLiveThroughGlobalCoalesce) {
  const CompilationProfile o1_prof{1, {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"}, {}};

  // 1. Positive (`reload1.c:2309-2326` + `jump.c:452-459`): `InvertAlterReg`
  //    preserves duplicate hard-register `REG_DEAD` notes (e.g. compensation
  //    notes from `.11.jump2`) through scratch demotion so `alter_reg`
  //    right-inverse holds exactly.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = 32;
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                "(set (reg:SI 2 v0) (plus:SI (reg:SI 8 t0) (reg:SI 6 a2))) "
                "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_DEAD (reg:SI 8 t0) "
                "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))))",
                "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
                "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                "(use (reg/i:SI 2 v0))",
            },
    });
    SyncAllBlocks(&cfg);
    hooks_.ApplyCompilationProfile(o1_prof);
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. Positive + Negative (`local-alloc.c:472` + `global.c:1060-1210`):
  //    in `b > 0`, when a live-through multi-block global pseudo holds a higher
  //    `REG_ALLOC_ORDER` slot (`$6 a2`) than two disjoint local pseudos in `$3 v1`
  //    (where `$2 v0` is live across the first `$v1` local so it stays lifted),
  //    coalescing those locals would promote them from `local_alloc` to
  //    `global_alloc` and expose `$3 v1` to the live-through global; therefore
  //    the second `$3 v1` local is demoted instead of coalesced. Conversely,
  //    when no higher-order live-through global exists (only `$2 v0`),
  //    coalescing is safe.
  auto make_two_block_cfg = [](bool include_higher_order_live_through_global) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = 32;
    std::vector<std::string> b0_insns;
    if (include_higher_order_live_through_global) {
      b0_insns.push_back("(set (reg:SI 6 a2) (plus:SI (reg:SI 5 a1) (const_int 5)))");
    } else {
      b0_insns.push_back("(set (reg:SI 7 a3) (plus:SI (reg:SI 5 a1) (const_int 5)))");
    }
    b0_insns.push_back(
        "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions = std::move(b0_insns),
        .successors = {1},
    });
    std::vector<std::string> b1_insns = {
        "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(set (reg:SI 3 v1) (mem/u:SI (reg:SI 2 v0)))",
        "(set (mem:SI (reg:SI 5 a1)) (reg:SI 3 v1)) (expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
        "(set (reg:SI 3 v1) (mem/u:SI (plus:SI (reg:SI 2 v0) (const_int 4)))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
        "(set (mem:SI (plus:SI (reg:SI 5 a1) (const_int 4))) (reg:SI 3 v1)) "
        "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
    };
    if (include_higher_order_live_through_global) {
      b1_insns.push_back(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 6 a2) (reg:SI 5 a1))) "
          "(expr_list:REG_DEAD (reg:SI 6 a2) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))");
    } else {
      b1_insns.push_back(
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
          "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
    }
    b1_insns.push_back("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 1,
        .instructions = std::move(b1_insns),
        .has_label = true,
    });
    SyncAllBlocks(&cfg);
    return cfg;
  };

  {
    const RtlControlFlowGraph lifted = InvertAlterReg(
        make_two_block_cfg(/*include_higher_order_live_through_global=*/true), o1_prof, 72);
    auto p1 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[1]);
    auto p3 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[3]);
    ASSERT_TRUE(p1.ok() && p3.ok());
    EXPECT_TRUE(p1->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
    EXPECT_FALSE(p3->pattern.SetDest().IsPseudoReg())
        << "Expected higher-order live-through global ($a2) to prevent coalescing $v1 locals:\n"
        << lifted.FormatSummary();
  }
  {
    const RtlControlFlowGraph lifted = InvertAlterReg(
        make_two_block_cfg(/*include_higher_order_live_through_global=*/false), o1_prof, 72);
    auto p1 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[1]);
    auto p3 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[3]);
    ASSERT_TRUE(p1.ok() && p3.ok());
    EXPECT_TRUE(p1->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
    EXPECT_TRUE(p3->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
    EXPECT_EQ(p1->pattern.SetDest().RegNo(), p3->pattern.SetDest().RegNo())
        << "Expected absence of higher-order live-through global to allow coalescing $v1 locals:\n"
        << lifted.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, CoalescesDisjointMultiBlockFprPseudosAndPromotesO0DiMdMemStore) {
  // 1. Disjoint multi-block same-reg `FP_REGS` (`32..63`, step 2) coalescing:
  //    web A in `$f2` (`34`, B0 -> B1) overlaps `$f0` (`32`), while disjoint
  //    web B in `$f2` (`34`, B1 -> B3 / B2 -> B3) has `$f0` free. Coalescing
  //    A and B into one pseudo carries A's conflict with `$f0` onto B so
  //    `global_alloc` keeps `$f2` instead of assigning `$f0` (positive).
  {
    const CompilationProfile o2_prof{2, {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}, {}};
    hooks_.ApplyCompilationProfile(o2_prof);
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:DF 32 $f0) (mem/u:DF (reg:SI 4 a0)))",
                    "(set (reg:DF 34 $f2) (mem/u:DF (plus:SI (reg:SI 4 a0) (const_int 8))))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
            .successors = {2, 1},
        },
        RtlBasicBlock{
            .id = 1,
            .instructions =
                {
                    "(set (reg:DF 32 $f0) (plus:DF (reg:DF 32 $f0) (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                    "(set (mem:DF (reg:SI 4 a0)) (reg:DF 32 $f0)) "
                    "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))",
                    "(set (reg:DF 34 $f2) (mem/u:DF (plus:SI (reg:SI 4 a0) (const_int 16))))",
                    "(set (pc) (label_ref L1))",
                },
            .successors = {3},
        },
        RtlBasicBlock{
            .id = 2,
            .instructions =
                {
                    "(set (reg:DF 32 $f0) (mult:DF (reg:DF 32 $f0) (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                    "(set (mem:DF (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:DF 32 $f0)) "
                    "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))",
                    "(set (reg:DF 34 $f2) (mem/u:DF (plus:SI (reg:SI 4 a0) (const_int 24))))",
                },
            .successors = {3},
            .has_label = true,
        },
        RtlBasicBlock{
            .id = 3,
            .instructions =
                {
                    "(set (reg/i:DF 32 $f0) (plus:DF (reg:DF 34 $f2) (reg:DF 44 $f12))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (expr_list:REG_DEAD (reg:DF 44 $f12) "
                    "(nil)))",
                    "(use (reg/i:DF 32 $f0))",
                },
            .has_label = true,
        },
    };
    SyncAllBlocks(&cfg);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    auto p0 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[1]);
    auto p1 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[2]);
    ASSERT_TRUE(p0.ok() && p1.ok());
    ASSERT_TRUE(p0->pattern.SetDest().IsPseudoReg());
    ASSERT_TRUE(p1->pattern.SetDest().IsPseudoReg());
    EXPECT_EQ(p0->pattern.SetDest().RegNo(), p1->pattern.SetDest().RegNo())
        << "Expected disjoint multi-block $f2 webs to coalesce:\n"
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. `-O0` `!gp64` `PromoteNonConflictingSingleBlockMdPseudos` on `mulsidi3`:
  //    `mult:DI -> 64 hi ; (set 10 t2 64 hi) ; (set mem:DI 10 t2)` promotes the
  //    pseudo to `reg_renumber == 64` and demotes `10 t2` back to hard reg
  //    (positive), whereas a non-`MEM` consumer does not promote at `-O0` (negative).
  {
    const CompilationProfile o0_prof{0, {"-O0", "-mips2", "-Wa,-g"}, {}};
    hooks_.ApplyCompilationProfile(o0_prof);
    auto make_o0_mul_cfg = [](bool mem_store) {
      RtlControlFlowGraph cfg;
      cfg.pass = RtlPassKind::kGreg;
      cfg.sideband.frame_pointer_needed = true;
      cfg.sideband.frame_size_bytes = 32;
      cfg.rpo_blocks.push_back(RtlBasicBlock{
          .id = 0,
          .instructions =
              {
                  "(set (reg:SI 2 v0) (mem:SI (reg:SI 30 $fp)))",
                  "(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 4))))",
                  "(parallel[ (set (reg:DI 64 hi) (mult:DI (sign_extend:DI (reg:SI 2 v0)) "
                  "(sign_extend:DI (reg:SI 3 v1)))) (clobber (reg:SI 66 accum)) ] )",
                  "(set (reg:DI 10 t2) (reg:DI 64 hi))",
                  mem_store
                      ? "(set (mem:DI (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg:DI 10 t2))"
                      : "(set (reg:DI 4 a0) (xor:DI (reg:DI 10 t2) (reg:DI 4 a0)))",
                  "(set (reg/i:DI 2 v0) (mem:DI (plus:SI (reg:SI 30 $fp) (const_int 16))))",
                  "(use (reg/i:DI 2 v0))",
              },
      });
      SyncAllBlocks(&cfg);
      return cfg;
    };
    const RtlControlFlowGraph pos =
        InvertAlterReg(make_o0_mul_cfg(/*mem_store=*/true), o0_prof, 72);
    bool found_64_pseudo = false;
    for (const auto& [p, hr] : pos.sideband.reg_renumber) {
      if (hr == 64) {
        found_64_pseudo = true;
      }
    }
    EXPECT_TRUE(found_64_pseudo) << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[3].find("(set (reg:DI 10 t2) (reg:DI "),
              std::string::npos)
        << pos.FormatSummary();

    const RtlControlFlowGraph neg =
        InvertAlterReg(make_o0_mul_cfg(/*mem_store=*/false), o0_prof, 72);
    bool neg_has_64_pseudo = false;
    for (const auto& [p, hr] : neg.sideband.reg_renumber) {
      if (hr == 64) {
        neg_has_64_pseudo = true;
      }
    }
    EXPECT_FALSE(neg_has_64_pseudo) << neg.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest,
       CoalesceDisjointMultiBlockFprRequiresMatchingModeAndUpgradesPartialDiRegDead) {
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};

  // 1. Negative for `CoalesceDisjointMultiBlockSameRegPseudos`
  //    (`config/mips/mips.h:1268-1279`): disjoint multi-block `kDF` (web A) and
  //    `kSF` (web B) pseudos both assigned to `$f2` (`34`) must NOT be coalesced
  //    into one pseudo.
  {
    const CompilationProfile mips1_prof{2, {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}, {}};
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (reg:DF 32 $f0) (mem/u:DF (reg:SI 4 a0)))",
                    "(set (reg:DF 34 $f2) (mem/u:DF (plus:SI (reg:SI 4 a0) (const_int 8))))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
            .successors = {2, 1},
        },
        RtlBasicBlock{
            .id = 1,
            .instructions =
                {
                    "(set (reg:DF 32 $f0) (plus:DF (reg:DF 32 $f0) (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                    "(set (mem:DF (reg:SI 4 a0)) (reg:DF 32 $f0)) "
                    "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))",
                    "(set (reg:SF 34 $f2) (mem/u:SF (plus:SI (reg:SI 4 a0) (const_int 16))))",
                    "(set (pc) (label_ref L1))",
                },
            .successors = {3},
        },
        RtlBasicBlock{
            .id = 2,
            .instructions =
                {
                    "(set (reg:DF 32 $f0) (mult:DF (reg:DF 32 $f0) (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                    "(set (mem:DF (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:DF 32 $f0)) "
                    "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))",
                    "(set (reg:SF 34 $f2) (mem/u:SF (plus:SI (reg:SI 4 a0) (const_int 24))))",
                },
            .successors = {3},
            .has_label = true,
        },
        RtlBasicBlock{
            .id = 3,
            .instructions =
                {
                    "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 44 $f12))) "
                    "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 44 $f12) "
                    "(nil)))",
                    "(use (reg/i:SF 32 $f0))",
                },
            .successors = {},
            .has_label = true,
        },
    };
    for (auto& blk : cfg.rpo_blocks) {
      blk.SyncRtlInsnsFromInstructions();
    }
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, mips1_prof, 72);
    int f2_pseudos = 0;
    for (const auto& [p, hr] : lifted.sideband.reg_renumber) {
      if (hr == 34) {
        ++f2_pseudos;
      }
    }
    EXPECT_GE(f2_pseudos, 2) << lifted.FormatSummary();
  }

  // 2. `RewriteNoteDatumExpr` (`flow.c:2537-2550`): when an insn reads
  //    `(subreg:SI (reg:DI 2 v0) 1)` and simultaneously writes word 2 (`(reg:SI 2 v0)`)
  //    to a distinct pseudo, a partial-word `(REG_DEAD (reg:SI 3 v1))` note is
  //    upgraded to `(REG_DEAD (reg:DI p))` so `p` stays lifted (positive); when
  //    word 2 is NOT overwritten, the partial note is NOT blindly upgraded (negative).
  {
    auto build_cfg = [](bool writes_word2) {
      RtlControlFlowGraph cfg;
      cfg.pass = RtlPassKind::kGreg;
      cfg.rpo_blocks.resize(1);
      cfg.rpo_blocks[0].id = 0;
      const std::string dst = writes_word2 ? "(reg:SI 2 v0)" : "(reg:SI 5 a1)";
      cfg.rpo_blocks[0].instructions = {
          "(set (reg:DI 2 v0) (mem:DI (plus:SI (reg:SI 0 argp) (const_int 16))))",
          "(set (reg:SI 4 a0) (plus:SI (subreg:SI (reg:DI 2 v0) 0) (reg:SI 16 s0)))",
          "(set " + dst +
              " (plus:SI (subreg:SI (reg:DI 2 v0) 1) (reg:SI 17 s1))) "
              "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
          "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) " + dst + "))",
          "(use (reg/i:SI 2 v0))",
      };
      cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
      return cfg;
    };

    const RtlControlFlowGraph pos = InvertAlterReg(build_cfg(/*writes_word2=*/true), o2_prof, 72);
    EXPECT_NE(pos.rpo_blocks[0].instructions[0].find("(set (reg:DI 72)"), std::string::npos)
        << pos.FormatSummary();
    EXPECT_NE(pos.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:DI 72)"), std::string::npos)
        << pos.FormatSummary();

    const RtlControlFlowGraph neg = InvertAlterReg(build_cfg(/*writes_word2=*/false), o2_prof, 72);
    EXPECT_EQ(neg.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:DI 72)"), std::string::npos)
        << neg.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, BaseLiveTicksRecordsUnusedHardRegWritePreventingCrossWriteCoalesce) {
  // Mirrors `global.c:729-767` (`global_conflicts`): `note_stores(...,
  // mark_reg_store)` records birth conflicts for every stored hard register
  // BEFORE `REG_UNUSED` kills it. When two disjoint single-block `$2 v0` webs
  // are separated by `(set (reg:DI 2 v0) (ashiftrt:DI (reg:DI 4 a0) 32))`
  // carrying `(REG_UNUSED (reg:SI 2 v0))` and the second `$2 v0` web reads dying
  // hard regs `$5 a1`/`$3 v1` (`local-alloc.c:1798` `qty_phys_sugg`),
  // `base_live_ticks[2*i+1]` must record `$2 v0` occupied so the two `$2 v0`
  // webs are NOT coalesced across the unused `$2 v0` write (positive). Without
  // the intervening `$2 v0` write (negative), coalescing is permitted.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  hooks_.ApplyCompilationProfile(o2_prof);

  auto make_cfg = [](bool include_unused_v0_di_write) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    std::vector<std::string> insns = {
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 16 s0) (const_int 7)))",
        "(set (reg:SI 16 s0) (and:SI (reg:SI 2 v0) (reg:SI 22 s6))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
        "(set (reg:DI 4 a0) (mem:DI (reg:SI 16 s0)))",
        "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 8)))",
    };
    if (include_unused_v0_di_write) {
      insns.push_back(
          "(parallel[ (set (reg:DI 2 v0) (ashiftrt:DI (reg:DI 4 a0) (const_int 32))) "
          "(clobber (reg:SI 6 a2)) ] ) (expr_list:REG_DEAD (reg:SI 4 a0) "
          "(expr_list:REG_UNUSED (reg:SI 2 v0) (expr_list:REG_UNUSED (reg:SI 6 a2) (nil))))");
      insns.push_back(
          "(set (reg:SI 2 v0) (xor:SI (reg:SI 5 a1) (reg:SI 3 v1))) "
          "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 3 v1) (nil)))");
    } else {
      insns.push_back(
          "(set (reg:SI 2 v0) (xor:SI (reg:SI 5 a1) (reg:SI 4 a0))) "
          "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))");
    }
    insns.push_back(
        "(set (reg:SI 17 s1) (plus:SI (reg:SI 17 s1) (reg:SI 2 v0))) "
        "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
    insns.push_back("(set (reg/i:SI 2 v0) (reg:SI 17 s1))");
    insns.push_back("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks.push_back(RtlBasicBlock{.id = 0, .instructions = std::move(insns)});
    SyncAllBlocks(&cfg);
    return cfg;
  };

  // Positive: intervening `(set (reg:DI 2 v0) ...) (REG_UNUSED (reg:SI 2 v0))`
  // prevents coalescing `[0]` and `[5]` into one pseudo.
  {
    const RtlControlFlowGraph cfg = make_cfg(/*include_unused_v0_di_write=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    auto p0 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[0]);
    auto p5 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[5]);
    ASSERT_TRUE(p0.ok() && p5.ok());
    EXPECT_NE(p0->pattern.SetDest().RegNo(), p5->pattern.SetDest().RegNo())
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative: without the intervening `$2 v0` write, `InvertAlterReg` still
  // produces a valid `alter_reg` preimage.
  {
    const RtlControlFlowGraph cfg = make_cfg(/*include_unused_v0_di_write=*/false);
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
}

TEST_F(InvertAlterRegTest, LocalStackArrayNotTreatedAsReloadSpillSlotPositiveAndNegative) {
  // Mirrors `reload1.c:2352-2358` + `function.c:682-704` (`assign_stack_local(..., -1)`):
  // 1. Positive: a local `s32[2]` array accessing both `0($sp)` and `4($sp)` inside
  //    the same 8-byte slot (`frame_size_bytes = 8`) is NOT a `reload` spill slot,
  //    so `InvertAlterReg` must keep an overlapping `$v1` local lifted rather than
  //    demoting it due to a false local-spill `combine_regs`.
  // 2. Negative (contrast): an upper 8-byte big-endian reload spill slot at `12($sp)`
  //    above the `0($sp)/4($sp)` local array (`frame_size_bytes = 16`) IS still
  //    recognized as a local reload spill slot and demotes overlapping `$v1`.
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};
  hooks_.ApplyCompilationProfile(o2_prof);

  auto make_cfg = [](bool use_upper_spill_slot) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_size_bytes = use_upper_spill_slot ? 16 : 8;
    const std::string slot_expr = use_upper_spill_slot
                                      ? "(mem:SI (plus:SI (reg:SI 29 sp) (const_int 12)))"
                                      : "(mem:SI (plus:SI (reg:SI 29 sp) (const_int 4)))";
    cfg.rpo_blocks = {
        RtlBasicBlock{
            .id = 0,
            .instructions =
                {
                    "(set (mem:SI (reg:SI 29 sp)) (reg:SI 6 a2))",
                    "(set (reg:SI 8 t0) (mem:SI (reg:SI 29 sp)))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 8 t0))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 2)))",
                    "(set (reg:SI 7 a3) (plus:SI (reg:SI 2 v0) (const_int 3))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                    "(set " + slot_expr +
                        " (reg:SI 7 a3)) (expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 3 v1)) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 6 a2) (nil)))",
                    "(set (reg:SI 2 v0) " + slot_expr + ")",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                    "(use (reg/i:SI 2 v0))",
                },
        },
    };
    SyncAllBlocks(&cfg);
    return cfg;
  };

  // Positive: `4($sp)` shares 8-byte slot 0 with `0($sp)` -> NOT a spill slot -> `$v1` stays
  // lifted.
  {
    const RtlControlFlowGraph cfg = make_cfg(/*use_upper_spill_slot=*/false);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 10u);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[3].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // Negative (contrast): `12($sp)` is a valid upper 8-byte big-endian reload spill slot -> `$v1`
  // demoted.
  {
    const RtlControlFlowGraph cfg = make_cfg(/*use_upper_spill_slot=*/true);
    const RtlControlFlowGraph lifted = InvertAlterReg(cfg, o2_prof, 72);
    ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 10u);
    EXPECT_NE(lifted.rpo_blocks[0].instructions[3].find("(set (reg:SI 3 v1)"), std::string::npos)
        << lifted.FormatSummary();
    const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }
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
  const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const FunctionCompilationTrace& trace = **cached_trace;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();
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
    ParsedCompilerFlags snap_flags;
    snap_flags.ApplyOptimizationLevelDefaults(snap.optimize_level);
    if (!snap.c_flags.empty()) {
      snap_flags = ParsedCompilerFlags::Parse(snap.c_flags);
    }
    const bool snap_fp_is_fixed =
        !snap_flags.flag_omit_frame_pointer || snap.post_state.sideband.frame_pointer_needed;
    const RtlControlFlowGraph lifted_snap = InvertAlterReg(snap.post_state, snap_profile, 72);
    ASSERT_FALSE(lifted_snap.rpo_blocks.empty()) << "Empty lifted_snap on " << tc.test_name;
    for (const auto& [pseudo, hard] : lifted_snap.sideband.reg_renumber) {
      EXPECT_GE(pseudo, 72) << tc.test_name;
      EXPECT_TRUE(IsValidRecoveredHardRegHome(hard, snap_fp_is_fixed))
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

  ParsedCompilerFlags fresh_flags;
  fresh_flags.ApplyOptimizationLevelDefaults(trace.stage0_input.profile.optimize_level);
  if (!trace.stage0_input.profile.c_flags.empty()) {
    fresh_flags = ParsedCompilerFlags::Parse(trace.stage0_input.profile.c_flags);
  }
  const bool fresh_fp_is_fixed =
      !fresh_flags.flag_omit_frame_pointer || fresh_greg.sideband.frame_pointer_needed;
  for (const auto& [pseudo, hard] : lifted_pre_alter.sideband.reg_renumber) {
    EXPECT_GE(pseudo, 72) << tc.test_name;
    EXPECT_TRUE(IsValidRecoveredHardRegHome(hard, fresh_fp_is_fixed))
        << "Invalid hard reg " << hard << " in fresh .09.greg for " << tc.test_name;
    ASSERT_TRUE(lifted_pre_alter.sideband.reg_n_refs.contains(pseudo)) << tc.test_name;
    EXPECT_GE(lifted_pre_alter.sideband.reg_n_refs.at(pseudo), 1) << tc.test_name;
    if (hard >= 0) {
      ASSERT_TRUE(lifted_pre_alter.sideband.reg_n_sets.contains(pseudo)) << tc.test_name;
      EXPECT_GE(lifted_pre_alter.sideband.reg_n_sets.at(pseudo), 1)
          << "Pseudo " << pseudo << " (hard " << hard << ") has 0 sets on " << tc.test_name
          << "\nfresh_greg:\n"
          << fresh_greg.FormatSummary() << "\nlifted_pre_alter:\n"
          << lifted_pre_alter.FormatSummary();
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
  const RightInverseResult fresh_ver_res = verifier->VerifyAlterRegRightInverse(fresh_greg);
  EXPECT_TRUE(fresh_ver_res.matches)
      << "Fresh .09.greg -> VerifyAlterRegRightInverse mismatch on " << tc.test_name << ":\n"
      << fresh_ver_res.diff_detail << "\nexpected (.09.greg):\n"
      << fresh_ver_res.expected_summary << "\nactual (.09.greg):\n"
      << fresh_ver_res.actual_summary << "\nlifted pre-alter:\n"
      << lifted_pre_alter.FormatSummary();

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

TEST_F(InvertAlterRegTest, ResolveDeathlessLocalPseudosPreservesIndirectCallTargetPseudo) {
  // Mirrors `calls.c:emit_call_1` + `flow.c:2504-2515`: a local GPR web feeding
  // an indirect `CALL_INSN` callee address `(call (mem:SI (reg:SI 3 v1)) ...)`
  // lacks `(REG_DEAD 3 v1)` on the `CALL_INSN` in Stage 0, yet must remain a
  // pseudo rather than being demoted by `ResolveDeathlessLocalPseudos`.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  hooks_.ApplyCompilationProfile(o2_prof);

  auto make_cfg = [](bool use_in_indirect_call) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    std::vector<std::string> insns = {
        "(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Prep\"))) (const_int 16))) "
        "(clobber (reg:SI 31 ra)) ] )",
        "(set (reg:SI 3 v1) (mem:SI (reg:SI 16 s0))) (expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
        "(set (reg:SI 4 a0) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
    };
    if (use_in_indirect_call) {
      insns.push_back(
          "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (reg:SI 3 v1)) (const_int 16))) "
          "(clobber (reg:SI 31 ra)) ] )");
      insns.push_back("(set (reg/i:SI 2 v0) (reg:SI 2 v0))");
    } else {
      insns.push_back("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 4 a0)))");
    }
    insns.push_back("(use (reg/i:SI 2 v0))");
    cfg.rpo_blocks.push_back(RtlBasicBlock{.id = 0, .instructions = std::move(insns)});
    SyncAllBlocks(&cfg);
    return cfg;
  };

  // Positive: indirect call target `$3 v1` stays lifted as a pseudo (`>= 68`).
  {
    const RtlControlFlowGraph lifted =
        InvertAlterReg(make_cfg(/*use_in_indirect_call=*/true), o2_prof, 72);
    auto p2 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[2]);
    ASSERT_TRUE(p2.ok());
    EXPECT_TRUE(p2->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
  }

  // Negative: deathless non-call consumer without `REG_DEAD` still demotes `$3 v1`.
  {
    const RtlControlFlowGraph lifted =
        InvertAlterReg(make_cfg(/*use_in_indirect_call=*/false), o2_prof, 72);
    auto p2 = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[2]);
    ASSERT_TRUE(p2.ok());
    EXPECT_TRUE(p2->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
    EXPECT_EQ(p2->pattern.SetDest().RegNo(), 3) << lifted.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest,
       GlobalPrefShieldingPrefersSched1AchievableCandidateOverBirthingBreakingCandidate) {
  // Mirrors `sched.c:2469-2549` (`birthing_insn_p`): when global preference
  // shielding for `p_other` (allocated `$f20`, preferring `$f0` via call-return
  // copy `(set $f20 $f0)`) has two conflicting `$f0` pseudos — `q_break` in
  // `B2` whose demotion to `$f0` (`reg_n_sets[32] >= 3`) destroys
  // `birthing_insn_p` and breaks `Im(F_sched1)`, and `q_safe` in `B1` whose
  // demotion keeps `B1` in `Im(F_sched1)` — `-O2` (`RunsSched1()`) selects
  // `q_safe` while `-O1` (`!RunsSched1()`) keeps the default higher-regno
  // candidate `q_break`.
  const CompilationProfile o2_mips1{2, {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}, {}};
  const CompilationProfile o1_mips1{1, {"-O1", "-mips1", "-funsigned-char", "-Wa,-O2"}, {}};

  auto build_cfg = []() {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    // B0: `p_other` (`$f20`) receives call-return `$f0` (`pref = $f0`).
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"InitF\"))) "
                "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                "(set (reg:SF 52 $f20) (reg:SF 32 $f0)) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                "(set (pc) (if_then_else (eq:SI (reg:SI 17 s1) (const_int 0)) "
                "(label_ref L2) (pc)))",
            },
        .successors = {1, 2},
    });
    // B1 -> B3: global `q_safe` (`$f0`, 2 blocks, read in def block) remains
    // `sched1`-achievable when demoted to hard reg `32 $f0`.
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 1,
        .instructions =
            {
                "(set (reg:SF 32 $f0) (mem/u:SF (reg:SI 18 s2)))",
                "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 32 $f0)))",
            },
        .successors = {3},
    });
    // B2 -> B4: global `q_break` (`$f0`, 2 blocks, read in def block, higher
    // regno than `q_safe`) sits in a block whose order (`abs:SF` latency 1
    // before `const_double` load latency 2) is NOT in `Im(F_sched1)` on
    // `-mips1`, so `-O2` skips `q_break` in favor of `q_safe`.
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 2,
        .instructions =
            {
                "(set (reg:SF 34 $f2) (mem/u:SF (reg:SI 18 s2)))",
                "(set (reg:SF 34 $f2) (abs:SF (reg:SF 34 $f2)))",
                "(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1070596096))",
                "(set (reg:SF 52 $f20) (plus:SF (reg:SF 34 $f2) (reg:SF 32 $f0))) "
                "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
            },
        .successors = {4},
    });
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 3,
        .instructions =
            {
                "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 32 $f0))) "
                "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                "(set (pc) (label_ref L5))",
            },
        .successors = {5},
    });
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 4,
        .instructions =
            {
                "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 32 $f0))) "
                "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
            },
        .successors = {5},
    });
    // B5: returns `$f20` via `(reg/i:SF 32 $f0)` (`reg_n_sets[32] >= 3` if
    // either `$f0` candidate is demoted).
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 5,
        .instructions =
            {
                "(set (reg/i:SF 32 $f0) (reg:SF 52 $f20)) "
                "(expr_list:REG_DEAD (reg:SF 52 $f20) (nil))",
                "(use (reg/i:SF 32 $f0))",
            },
        .successors = {},
    });
    SyncAllBlocks(&cfg);
    return cfg;
  };

  // Positive (`-O2`): `q_break` in B2 stays lifted as a pseudo while `q_safe`
  // in B1 shields `$f0`.
  {
    const RtlControlFlowGraph lifted = InvertAlterReg(build_cfg(), o2_mips1, 72);
    auto b1_0 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[0]);
    auto b2_2 = ParsePatternAndNotes(lifted.rpo_blocks[2].instructions[2]);
    ASSERT_TRUE(b1_0.ok());
    ASSERT_TRUE(b2_2.ok());
    EXPECT_TRUE(b1_0->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
    EXPECT_EQ(b1_0->pattern.SetDest().RegNo(), 32) << lifted.FormatSummary();
    EXPECT_TRUE(b2_2->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
  }

  // Negative (`-O1`, `!RunsSched1()`): default tie-break picks the higher
  // pseudo `q_break` in B2 without running `sched1` achievability checks.
  {
    const RtlControlFlowGraph lifted = InvertAlterReg(build_cfg(), o1_mips1, 72);
    auto b1_0 = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[0]);
    auto b2_2 = ParsePatternAndNotes(lifted.rpo_blocks[2].instructions[2]);
    ASSERT_TRUE(b1_0.ok());
    ASSERT_TRUE(b2_2.ok());
    EXPECT_TRUE(b1_0->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
    EXPECT_TRUE(b2_2->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
    EXPECT_EQ(b2_2->pattern.SetDest().RegNo(), 32) << lifted.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, DemotesPostSetupSaveAreasSpillSlotAboveCallerSaveSlot) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2"}, {}};

  // Positive (`reload1.c:658, 1446, 2253, 3499`): an 8-byte aligned big-endian
  // reload spill slot `[sp+28]` allocated AFTER `setup_save_areas` (above a
  // caller-save save/restore slot at `[sp+20]`) is still discovered by
  // `FindProvenReloadSpillSlots`, demoting the no-`REG_DEAD` reload scratch
  // `$f0` (32) back to a hard register.
  auto make_cfg = [&](bool add_reg_dead_on_use) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_pointer_needed = false;
    cfg.sideband.outgoing_args_size = 16;
    cfg.sideband.frame_size_bytes = 16;  // [sp+16..sp+32): [sp+20] caller-save, [sp+28] spill
    cfg.rpo_blocks.push_back(RtlBasicBlock{
        .id = 0,
        .instructions =
            {
                "(set (reg:SF 34 $f2) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))",
                // Caller-save save/restore of `$f2` around a call at `[sp+20]`.
                "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SF 34 $f2))",
                "(parallel[ (set (reg:SF 32 $f0) "
                "(call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16))) "
                "(clobber (reg:SI 31 ra)) ] )",
                "(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 20))))",
                // Post-`setup_save_areas` output-reload spill into `[sp+28]` + input reload.
                "(set (reg:SF 32 $f0) (mult:SF (reg:SF 32 $f0) (reg:SF 34 $f2)))",
                "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 32 $f0))",
                "(set (reg:SF 32 $f0) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))",
                add_reg_dead_on_use
                    ? "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
                      "(expr_list:REG_DEAD (reg:SF 32 $f0) "
                      "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil)))"
                    : "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
                      "(expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
                "(use (reg/i:SF 32 $f0))",
            },
    });
    SyncAllBlocks(&cfg);
    return cfg;
  };

  {
    const RtlControlFlowGraph lifted =
        InvertAlterReg(make_cfg(/*add_reg_dead_on_use=*/false), o2_prof, 72);
    auto load_pn = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[6]);
    ASSERT_TRUE(load_pn.ok());
    EXPECT_TRUE(load_pn->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
    EXPECT_EQ(load_pn->pattern.SetDest().RegNo(), 32) << lifted.FormatSummary();
  }

  // Negative: when the last use carries `(REG_DEAD (reg:SF 32 $f0))`, the load
  // destination is a real pre-reload pseudo rather than an un-noted reload
  // scratch, so it stays lifted as a pseudo.
  {
    const RtlControlFlowGraph lifted =
        InvertAlterReg(make_cfg(/*add_reg_dead_on_use=*/true), o2_prof, 72);
    auto load_pn = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[6]);
    ASSERT_TRUE(load_pn.ok());
    EXPECT_TRUE(load_pn->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
  }
}

TEST_F(InvertAlterRegTest, DemotesMultiUseCalleeSavedGprToFprAndSelfLoopConstDoubleReloads) {
  const CompilationProfile o2_prof{2, {"-O2", "-mips2"}, {}};

  // Exercises BOTH:
  // 1. `reload1.c:5186-5258` (`choose_reload_regs`) multi-use inherited copy
  //    from callee-saved GPR `$s0` (16) -> call-used FPR `$f4` (36) in B0
  //    (2 FP arithmetic uses, no `REG_DEAD`), which registers `$f4` as a
  //    proven call-used FP spill register in `DemoteReloadAndCallerSaveHardScratches`.
  // 2. `reload1.c:517-524, 566-573, 2769-2782, 6583` (`reg_equiv_constant`):
  //    in self-loop block B1 with unique preheader B0, a rematerialized
  //    `const_double:SF` into proven FP spill reg `$f4` (36) is demoted early
  //    into `demoted_reload_scratch_masks` so `DetectLocalResolutionGroupConflicts`
  //    does NOT inject a false `qty_phys_sugg = 36` onto local pseudo `q`
  //    (`B1[1]`, allocated to `32 $f0`).
  auto make_cfg = [](const char* b0_src_gpr, bool is_self_loop) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.rpo_blocks.resize(3);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[0].instructions = {
        std::string("(set (reg:SF 36 $f4) (reg:SF ") + b0_src_gpr + "))",
        "(set (reg:SF 34 $f2) (plus:SF (reg:SF 36 $f4) (reg:SF 44 $f12)))",
        "(set (reg:SF 34 $f2) (mult:SF (reg:SF 34 $f2) (reg:SF 36 $f4)))",
    };
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].successors =
        is_self_loop ? std::vector<size_t>{1, 2} : std::vector<size_t>{2};
    cfg.rpo_blocks[1].instructions = {
        "(set (reg:SF 36 $f4) (const_double:SF (cc0) 0 1070596096))",
        "(set (reg:SF 32 $f0) (plus:SF (reg:SF 36 $f4) (reg:SF 34 $f2)))",
        "(set (reg:SF 34 $f2) (plus:SF (reg:SF 34 $f2) (reg:SF 32 $f0))) "
        "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
        "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))",
        "(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))",
    };
    cfg.rpo_blocks[2].id = 2;
    cfg.rpo_blocks[2].instructions = {
        "(set (reg/i:SF 32 $f0) (reg:SF 34 $f2)) (expr_list:REG_DEAD (reg:SF 34 $f2) (nil))",
        "(use (reg/i:SF 32 $f0))",
    };
    SyncAllBlocks(&cfg);
    return cfg;
  };

  // Positive: multi-use callee-saved GPR `$s0` -> `$f4` in B0 + self-loop B1
  // demotes both reload scratches into `demoted_reload_scratch_masks`, so local
  // pseudo `B1[1]` (`32 $f0`) receives no false `qty_phys_sugg = 36` and stays
  // lifted as a pseudo.
  {
    const RtlControlFlowGraph lifted =
        InvertAlterReg(make_cfg("16 s0", /*is_self_loop=*/true), o2_prof, 72);
    auto b0_copy = ParsePatternAndNotes(lifted.rpo_blocks[0].instructions[0]);
    auto b1_cd = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[0]);
    auto b1_real_q = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[1]);
    ASSERT_TRUE(b0_copy.ok());
    ASSERT_TRUE(b1_cd.ok());
    ASSERT_TRUE(b1_real_q.ok());
    EXPECT_TRUE(b0_copy->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
    EXPECT_EQ(b0_copy->pattern.SetDest().RegNo(), 36) << lifted.FormatSummary();
    EXPECT_TRUE(b1_cd->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
    EXPECT_EQ(b1_cd->pattern.SetDest().RegNo(), 36) << lifted.FormatSummary();
    EXPECT_TRUE(b1_real_q->pattern.SetDest().IsPseudoReg()) << lifted.FormatSummary();
  }

  // Negative 1 (Rule 2a): multi-use copy from call-used GPR `$a0` (4) in B0 is
  // NOT demoted by Rule 2a, so `$f4` is not a proven FP spill reg, B1's
  // `const_double` is not marked in `demoted_reload_scratch_masks`, and it
  // injects false `qty_phys_sugg = 36` onto `B1[1]`, demoting `B1[1]`.
  {
    const RtlControlFlowGraph lifted =
        InvertAlterReg(make_cfg("4 a0", /*is_self_loop=*/true), o2_prof, 72);
    auto b1_real_q = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[1]);
    ASSERT_TRUE(b1_real_q.ok());
    EXPECT_TRUE(b1_real_q->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
  }

  // Negative 2 (Step 4): when B1 is NOT a self-loop, its `const_double` is not
  // marked in `demoted_reload_scratch_masks` even though `$f4` is a proven FP
  // spill reg, so it injects false `qty_phys_sugg = 36` onto `B1[1]`.
  {
    const RtlControlFlowGraph lifted =
        InvertAlterReg(make_cfg("16 s0", /*is_self_loop=*/false), o2_prof, 72);
    auto b1_real_q = ParsePatternAndNotes(lifted.rpo_blocks[1].instructions[1]);
    ASSERT_TRUE(b1_real_q.ok());
    EXPECT_TRUE(b1_real_q->pattern.SetDest().IsHardReg()) << lifted.FormatSummary();
  }
}

INSTANTIATE_TEST_SUITE_P(All4172Cases, InvertAlterRegCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
