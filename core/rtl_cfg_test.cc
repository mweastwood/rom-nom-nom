#include "core/rtl_cfg.h"

#include <string>
#include <vector>

#include "core/mips.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(RtlCfgTest, AllRtlPassesAndExtensionsAreCompleteAndOrdered) {
  const auto passes = AllRtlPasses();
  ASSERT_EQ(passes.size(), 13u);
  EXPECT_EQ(passes.front(), RtlPassKind::kRtl);
  EXPECT_EQ(passes.back(), RtlPassKind::kDbr);

  EXPECT_EQ(RtlPassExtension(RtlPassKind::kRtl), ".rtl");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kJump), ".jump");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kCse), ".cse");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kLoop), ".loop");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kCse2), ".cse2");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kFlow), ".flow");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kCombine), ".combine");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kSched), ".sched");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kLreg), ".lreg");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kGreg), ".greg");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kSched2), ".sched2");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kJump2), ".jump2");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kDbr), ".dbr");
}

TEST(RtlCfgTest, CanonicalizeRtlPatternCollapsesWhitespace) {
  EXPECT_EQ(CanonicalizeRtlPattern("(set\n  (reg:SI 2 v0)\t\t(const_int 42))"),
            "(set (reg:SI 2 v0) (const_int 42))");
}

TEST(RtlCfgTest, SyncRtlInsnsPreservesMachineInstructionsAcrossPatternRewrites) {
  RtlBasicBlock blk;
  Instruction inst;
  inst.vram = 0x80001000u;
  inst.opcode = Opcode::kAddiu;
  inst.rt = Register::kV0;
  inst.rs = Register::kA0;
  inst.immediate = 5;

  RtlInsn rinsn;
  rinsn.canonical_pattern = "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 5)))";
  rinsn.vram = inst.vram;
  rinsn.machine_instructions = {inst};
  blk.AddInsn(rinsn);

  blk.instructions[0] = "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 5)))";
  blk.SyncRtlInsnsFromInstructions();

  ASSERT_EQ(blk.rtl_insns.size(), 1u);
  ASSERT_EQ(blk.rtl_insns[0].machine_instructions.size(), 1u);
  EXPECT_EQ(blk.rtl_insns[0].machine_instructions[0].vram, 0x80001000u);
  EXPECT_FALSE(blk.rtl_insns[0].is_synthetic_rtl_expansion);
}

TEST(RtlCfgTest, SyncAllBlocksPreservingMachineInsnsRecoversSplitBlocks) {
  RtlBasicBlock orig_blk;
  Instruction inst0;
  inst0.vram = 0x80001000u;
  Instruction inst1;
  inst1.vram = 0x80001004u;

  RtlInsn r0;
  r0.canonical_pattern = "(set (reg:SI 2 v0) (const_int 1))";
  r0.vram = inst0.vram;
  r0.machine_instructions = {inst0};
  orig_blk.AddInsn(r0);

  RtlInsn r1;
  r1.canonical_pattern = "(return)";
  r1.vram = inst1.vram;
  r1.machine_instructions = {inst1};
  orig_blk.AddInsn(r1);

  RtlControlFlowGraph split_cfg;
  split_cfg.rpo_blocks.resize(2);
  split_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 1))");
  split_cfg.rpo_blocks[1].AddPatternOnly("(return)");

  split_cfg.SyncAllBlocksPreservingMachineInsns({orig_blk});
  EXPECT_EQ(split_cfg.TotalMachineInsnCount(), 2u);
  EXPECT_EQ(split_cfg.rpo_blocks[0].id, 0u);
  EXPECT_EQ(split_cfg.rpo_blocks[1].id, 1u);
}

TEST(RtlCfgTest, CompareAgainstAndFormatSummaryVerifyTopologyAndPatterns) {
  RtlControlFlowGraph cfg_a;
  cfg_a.rpo_blocks.resize(2);
  cfg_a.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  cfg_a.rpo_blocks[0].successors = {1};
  cfg_a.rpo_blocks[1].AddPatternOnly("(return)");

  EXPECT_EQ(cfg_a.FormatSummary(),
            "B0 (-> [1]): (set (reg:SI 2 v0) (const_int 1))\n"
            "B1 (-> []): (return)");

  RtlControlFlowGraph cfg_b = cfg_a;
  const RtlCfgPassComparison exact_cmp = cfg_a.CompareAgainst(cfg_b);
  EXPECT_TRUE(exact_cmp.MatchesExactly());
  EXPECT_TRUE(exact_cmp.MatchesTopologyAndInsnCounts());

  cfg_b.rpo_blocks[0].instructions[0] = "(set (reg:SI 2 v0) (const_int 2))";
  const RtlCfgPassComparison pat_mismatch = cfg_a.CompareAgainst(cfg_b);
  EXPECT_TRUE(pat_mismatch.MatchesTopologyAndInsnCounts());
  EXPECT_FALSE(pat_mismatch.MatchesExactly());
  EXPECT_FALSE(pat_mismatch.mismatch_detail.empty());

  cfg_b.rpo_blocks[0].successors = {};
  const RtlCfgPassComparison edge_mismatch = cfg_a.CompareAgainst(cfg_b);
  EXPECT_FALSE(edge_mismatch.MatchesTopologyAndInsnCounts());
  EXPECT_FALSE(edge_mismatch.successors_match);
}

TEST(RtlCfgTest, RecomputeRtlCfgRpoReordersBlocksAndPrunesUnreachable) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(5);
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (const_int 0))");
  cfg.rpo_blocks[0].successors = {2, 1};

  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  cfg.rpo_blocks[1].successors = {4};

  cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 2 v0) (const_int 2))");
  cfg.rpo_blocks[2].successors = {4};

  cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 2 v0) (const_int 99))");
  cfg.rpo_blocks[3].successors = {4};

  cfg.rpo_blocks[4].AddPatternOnly("(return)");

  RecomputeRtlCfgRpo(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[0].id, 0u);
  EXPECT_EQ(cfg.rpo_blocks[0].predecessors, std::vector<size_t>{});
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[1].id, 1u);
  EXPECT_EQ(cfg.rpo_blocks[1].predecessors, std::vector<size_t>{0});
  EXPECT_EQ(cfg.rpo_blocks[1].successors, std::vector<size_t>{3});
  EXPECT_EQ(cfg.rpo_blocks[2].id, 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].predecessors, std::vector<size_t>{0});
  EXPECT_EQ(cfg.rpo_blocks[2].successors, std::vector<size_t>{3});
  EXPECT_EQ(cfg.rpo_blocks[3].id, 3u);
  EXPECT_EQ(cfg.rpo_blocks[3].predecessors, (std::vector<size_t>{1, 2}));
}

}  // namespace
}  // namespace rom_nom_nom
