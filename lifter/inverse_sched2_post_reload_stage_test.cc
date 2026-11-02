#include "lifter/inverse_sched2_post_reload_stage.h"

#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/decompilation_context.h"
#include "lifter/function_loader.h"
#include "lifter/inverse_jump2_cross_jump_stage.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

Instruction MakeAddiu(Register rt, Register rs, int16_t imm, uint32_t vram) {
  Instruction inst;
  inst.raw_word = 0x24000000u;
  inst.opcode = Opcode::kAddiu;
  inst.rt = rt;
  inst.rs = rs;
  inst.immediate = imm;
  inst.vram = vram;
  return inst;
}

Instruction MakeSw(Register rt, int16_t offset, Register base, uint32_t vram) {
  Instruction inst;
  inst.raw_word = 0xAC000000u;
  inst.opcode = Opcode::kSw;
  inst.rt = rt;
  inst.rs = base;
  inst.immediate = offset;
  inst.vram = vram;
  return inst;
}

Instruction MakeLw(Register rt, int16_t offset, Register base, uint32_t vram) {
  Instruction inst;
  inst.raw_word = 0x8C000000u;
  inst.opcode = Opcode::kLw;
  inst.rt = rt;
  inst.rs = base;
  inst.immediate = offset;
  inst.vram = vram;
  return inst;
}

Instruction MakeJal(uint32_t target, uint32_t vram) {
  Instruction inst;
  inst.raw_word = 0x0C000000u;
  inst.opcode = Opcode::kJal;
  inst.target = (target >> 2) & 0x03FFFFFF;
  inst.vram = vram;
  return inst;
}

Instruction MakeAddu(Register rd, Register rs, Register rt, uint32_t vram) {
  Instruction inst;
  inst.raw_word = 0x00000021u;
  inst.opcode = Opcode::kAddu;
  inst.rd = rd;
  inst.rs = rs;
  inst.rt = rt;
  inst.vram = vram;
  return inst;
}

Instruction MakeJrRa(uint32_t vram) {
  Instruction inst;
  inst.raw_word = 0x03E00008u;
  inst.opcode = Opcode::kJr;
  inst.rs = Register::kRa;
  inst.vram = vram;
  return inst;
}

TEST(InverseSched2PostReloadStageTest, StripsPrologueAndRestoresGregSelfMoves) {
  LoadedFunction func;
  func.name = "stage3_greg_recovery_fn";
  func.vram = 0x80010400;
  // Non-leaf function reading $a1 twice before the call (in `addu $s0, $a0, $a1` and
  // `addu $a0, $a0, $a1`). Between `.greg` and `.sched2`, `thread_prologue_and_epilogue_insns`
  // inserts 3 prologue instructions (`addiu $sp, -24`, `sw $ra`, `sw $s0`) and `schedule_insns`
  // deletes the incoming-parameter self-moves (`a0 = a0`, `a1 = a1`). Stage 3 (`.greg`) strips the
  // 3 prologue instructions and restores both `.greg` self-moves.
  func.instructions = {
      MakeAddiu(Register::kSp, Register::kSp, -24, 0x80010400),
      MakeSw(Register::kRa, 20, Register::kSp, 0x80010404),
      MakeSw(Register::kS0, 16, Register::kSp, 0x80010408),
      MakeAddu(Register::kS0, Register::kA0, Register::kA1, 0x8001040C),
      MakeJal(0x80020000, 0x80010410),
      MakeAddu(Register::kA0, Register::kA0, Register::kA1, 0x80010414),
      MakeAddu(Register::kV0, Register::kV0, Register::kS0, 0x80010418),
      MakeLw(Register::kRa, 20, Register::kSp, 0x8001041C),
      MakeLw(Register::kS0, 16, Register::kSp, 0x80010420),
      MakeJrRa(0x80010424),
      MakeAddiu(Register::kSp, Register::kSp, 24, 0x80010428),
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  ControlFlowGraph cfg = context.Cfg();
  CfgAnalysisManager analyses(&cfg);
  CfgDeReorg::UnscheduleDelaySlotsOnly(&cfg, &analyses);

  RtlControlFlowGraph stage2_rtl =
      InverseJump2CrossJumpLifter::LiftToSched2RtlCfg(cfg, &context.FrameLayout().RawAnalysis());
  RtlControlFlowGraph stage3_rtl = stage2_rtl;
  InverseSched2PostReloadLifter::InvertSched2ToGregRtlCfg(
      &stage3_rtl, &context.FrameLayout().RawAnalysis(), context.Symbols(), context.FunctionName());

  ASSERT_EQ(stage2_rtl.rpo_blocks.size(), 1u);
  ASSERT_EQ(stage3_rtl.rpo_blocks.size(), 1u);
  EXPECT_EQ(stage2_rtl.rpo_blocks[0].instructions.size(), 7u);
  ASSERT_EQ(stage3_rtl.rpo_blocks[0].instructions.size(), 4u);

  InverseSched2PostReloadLifter::RestoreGregSelfMovesAndFormatAsMipsRtl(
      &stage3_rtl, context.Symbols(), context.FunctionName());
  ASSERT_EQ(stage3_rtl.rpo_blocks[0].instructions.size(), 6u);
  EXPECT_EQ(stage3_rtl.rpo_blocks[0].instructions[0], "addu  $a0, $a0, $zero");
  EXPECT_EQ(stage3_rtl.rpo_blocks[0].instructions[1], "addu  $a1, $a1, $zero");
}

}  // namespace
}  // namespace rom_nom_nom
