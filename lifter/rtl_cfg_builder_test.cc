#include "lifter/rtl_cfg_builder.h"

#include <string>
#include <vector>

#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

Instruction MakeAddiu(Register rt, Register rs, int16_t imm) {
  Instruction inst;
  inst.raw_word = 0x24000000u;
  inst.opcode = Opcode::kAddiu;
  inst.rt = rt;
  inst.rs = rs;
  inst.immediate = imm;
  return inst;
}

RtlInsn MakeSingleRtlInsn(const Instruction& inst) {
  RtlInsn out;
  out.canonical_pattern = inst.Disassemble();
  out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  out.machine_instructions = {inst};
  return out;
}

TEST(RtlCfgBuilderTest, FormatsRegisterNamesAndIdentifiesSelfMoves) {
  EXPECT_EQ(GprName(Register::kZero), "$0");
  EXPECT_EQ(GprName(Register::kV0), "v0");
  EXPECT_EQ(GprName(Register::kA0), "a0");
  EXPECT_EQ(GprName(Register::kS0), "s0");
  EXPECT_EQ(GprName(Register::kSp), "sp");
  EXPECT_EQ(FprName(FpRegister::kF0), "$f0");
  EXPECT_EQ(FprName(FpRegister::kF12), "$f12");

  const RtlInsn gpr_sm = MakeSelfMoveRtlInsn("a1");
  std::string sm_reg;
  EXPECT_TRUE(IsGotSelfMoveInsn(gpr_sm, &sm_reg));
  EXPECT_EQ(sm_reg, "a1");

  const RtlInsn fpr_sm = MakeSelfMoveRtlInsn("$f14");
  EXPECT_TRUE(IsGotSelfMoveInsn(fpr_sm, &sm_reg));
  EXPECT_EQ(sm_reg, "$f14");

  const RtlInsn cross_move = MakeRegMoveRtlInsn(Register::kA0, Register::kS0);
  EXPECT_FALSE(IsGotSelfMoveInsn(cross_move, &sm_reg));
}

TEST(RtlCfgBuilderTest, SplicesInstructionsBeforeTrailingJumpAndSynchronizesVectors) {
  RtlBasicBlock dst;
  dst.AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kA0, Register::kA0, 1)));
  dst.AddInsn(MakeUncondJumpRtlInsn());
  EXPECT_TRUE(EndsWithUncondJump(dst));

  RtlBasicBlock src;
  src.AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kV0, Register::kZero, 42)));
  src.AddInsn(MakeUncondJumpRtlInsn());

  InsertBlockInsnsBeforeJump(&dst, src);
  ASSERT_EQ(dst.instructions.size(), 3u);
  ASSERT_EQ(dst.rtl_insns.size(), 3u);
  EXPECT_EQ(dst.instructions[0], "addiu $a0, $a0, 1");
  EXPECT_EQ(dst.instructions[1], "addiu $v0, $zero, 42");
  EXPECT_EQ(dst.instructions[2], "(set (pc) (label_ref))");
  EXPECT_TRUE(EndsWithUncondJump(dst));

  EraseInsnAt(&dst, 0);
  ASSERT_EQ(dst.instructions.size(), 2u);
  ASSERT_EQ(dst.rtl_insns.size(), 2u);
  EXPECT_TRUE(RtlInsnDefinesReturnReg(dst.rtl_insns[0]));
}

TEST(RtlCfgBuilderTest, RecomputesReversePostOrderAndPredecessorsDroppingUnreachableBlocks) {
  // Block 0 branches to 2 (taken) and 1 (fallthrough); Block 3 is unreachable.
  // Both 1 and 2 jump to Block 4 (exit).
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(5);
  cfg.rpo_blocks[0].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kT0, Register::kZero, 0)));
  cfg.rpo_blocks[0].successors = {2, 1};

  cfg.rpo_blocks[1].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kV0, Register::kZero, 1)));
  cfg.rpo_blocks[1].successors = {4};

  cfg.rpo_blocks[2].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kV0, Register::kZero, 2)));
  cfg.rpo_blocks[2].successors = {4};

  cfg.rpo_blocks[3].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kV0, Register::kZero, 99)));
  cfg.rpo_blocks[3].successors = {4};

  RecomputeRtlCfgRpo(&cfg);

  // Unreachable block 3 is pruned; remaining 4 blocks are ordered in RPO from block 0.
  ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[0].predecessors, std::vector<size_t>{});
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[1].predecessors, std::vector<size_t>{0});
  EXPECT_EQ(cfg.rpo_blocks[1].successors, std::vector<size_t>{3});
  EXPECT_EQ(cfg.rpo_blocks[2].predecessors, std::vector<size_t>{0});
  EXPECT_EQ(cfg.rpo_blocks[2].successors, std::vector<size_t>{3});
  EXPECT_EQ(cfg.rpo_blocks[3].predecessors, (std::vector<size_t>{1, 2}));
}

}  // namespace
}  // namespace rom_nom_nom
