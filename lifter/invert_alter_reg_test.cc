#include "lifter/invert_alter_reg.h"

#include <string>
#include <vector>

#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

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

// Subclasses `SubfunctionInverseProvider` directly (never `ToySubfunctionInverses`)
// so that `RightInverseVerifier` exercises only the production `InvertAlterReg`
// implementation.
class ProductionAlterRegProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     int pseudo_regno) const override {
    return ::rom_nom_nom::lifter::InvertAlterReg(allocated_cfg_x, pseudo_regno);
  }
};

class CorruptedAlterRegProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     int pseudo_regno) const override {
    RtlControlFlowGraph bad = ::rom_nom_nom::lifter::InvertAlterReg(allocated_cfg_x, pseudo_regno);
    // Corrupt `reg_renumber` to map `pseudo_regno` to `$t0` (`8`) instead of `$v0` (`2`).
    bad.sideband.reg_renumber[pseudo_regno] = 8;
    return bad;
  }
};

class InvertAlterRegTest : public ::testing::Test {
 protected:
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
              "(return)",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 4u);
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
                  "(return)",
              },
          .successors = {},
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
              "(return)",
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

TEST_F(InvertAlterRegTest, ReversesSingleStackSlotSpillToNegativeRegRenumber) {
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
              "(return)",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.sideband.frame_size_bytes, 0);
  ASSERT_TRUE(lifted.sideband.reg_renumber.contains(72));
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), -1);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1], "(set (reg/i:SI 2 v0) (reg:SI 72))");

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;

  SubfunctionBoundarySnapshot snap;
  snap.subfunction_id = SubfunctionId::kAlterReg;
  snap.optimize_level = 2;
  snap.pre_state = lifted;
  snap.post_state = cfg;
  const RightInverseResult boundary_res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;
}

TEST_F(InvertAlterRegTest, ReversesMultipleContiguousStackSlotSpillsInOrder) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_size_bytes = 16;
  cfg.sideband.frame_pointer_needed = true;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 4))) "
              "(plus:SI (reg:SI 4 a0) (const_int 1)))",
              "(set (mem:DF (plus:SI (reg:SI 30 $fp) (const_int 8))) (reg:DF 44 $f12))",
              "(set (reg/i:SI 2 v0) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 4))))",
              "(use (reg/i:SI 2 v0))",
              "(return)",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.sideband.frame_size_bytes, 0);
  ASSERT_EQ(lifted.sideband.reg_renumber.size(), 2u);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(72), -1);
  EXPECT_EQ(lifted.sideband.reg_renumber.at(73), -1);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[1], "(set (reg:DF 73) (reg:DF 44 $f12))");
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[2], "(set (reg/i:SI 2 v0) (reg:SI 72))");

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertAlterRegTest, PreservesStructOrVolatileOrAddressTakenFpStackSlots) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_size_bytes = 8;
  cfg.sideband.frame_pointer_needed = true;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (mem/s:SI (plus:SI (reg:SI 30 $fp) (const_int 4))) (reg:SI 4 a0))",
              "(set (reg:SI 2 v0) (plus:SI (reg:SI 30 $fp) (const_int 4)))",
              "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
              "(use (reg/i:SI 2 v0))",
              "(return)",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.sideband.frame_size_bytes, 8);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (mem/s:SI (plus:SI (reg:SI 30 $fp) (const_int 4))) (reg:SI 4 a0))");

  const RightInverseResult res = verifier_.VerifyAlterRegRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertAlterRegTest, PreservesFcr31StatusHardRegister67) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.push_back(RtlBasicBlock{
      .id = 0,
      .instructions =
          {
              "(set (reg:CC 67 $fcr31) (eq:CC (reg:SF 44 $f12) (reg:SF 46 $f14)))",
              "(set (reg:SI 2 v0) (const_int 1))",
              "(use (reg/i:SI 2 v0))",
          },
  });
  SyncAllBlocks(&cfg);

  const RtlControlFlowGraph lifted = InvertAlterReg(cfg, 72);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:CC 67 $fcr31) (eq:CC (reg:SF 44 $f12) (reg:SF 46 $f14)))");
  EXPECT_FALSE(lifted.sideband.reg_renumber.contains(67));
  for (const auto& [pseudo, hard] : lifted.sideband.reg_renumber) {
    EXPECT_GE(pseudo, 72);
    EXPECT_NE(hard, 67);
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

  // Reject malformed sequence.
  RtlControlFlowGraph bad_seq_cfg = valid_cfg;
  bad_seq_cfg.rpo_blocks[0].instructions = {
      "(sequence [(insn (set (reg:SI 2 v0) (const_int 1)))])"};
  EXPECT_TRUE(InvertAlterReg(bad_seq_cfg).rpo_blocks.empty());
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
              "(return)",
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
  GccContext ctx_;
  FunctionHooks hooks_{&ctx_};
  ProductionAlterRegProvider provider_;
  RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_P(InvertAlterRegCorpusTest, All2480CasesSatisfyRightInverseLaw) {
  const RoundTripTestCase& tc = GetParam();
  const FunctionCompilationTrace trace = hooks_.TraceForwardCompilation(tc);
  ASSERT_FALSE(trace.subfunction_snapshots.empty())
      << "Failed to capture snapshots for " << tc.test_name;

  bool found_alter_reg = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kAlterReg) {
      continue;
    }
    found_alter_reg = true;
    const RightInverseResult res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "SubfunctionId::kAlterReg right-inverse mismatch on "
                             << tc.test_name << ": " << res.diff_detail << "\nexpected:\n"
                             << res.expected_summary << "\nactual:\n"
                             << res.actual_summary;
  }
  EXPECT_TRUE(found_alter_reg) << "Missing SubfunctionId::kAlterReg snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(All2480Cases, InvertAlterRegCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
