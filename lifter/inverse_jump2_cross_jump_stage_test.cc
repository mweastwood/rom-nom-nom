#include "lifter/inverse_jump2_cross_jump_stage.h"

#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/decompilation_context.h"
#include "lifter/function_loader.h"
#include "lifter/inverse_final_and_gas_stage.h"
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

TEST(InverseJump2CrossJumpStageTest, RecoversSched2CallerArgMoveBeforeFirstCall) {
  LoadedFunction func;
  func.name = "caller_arg_move_fn";
  func.vram = 0x80010300;
  // Function saves $a0 in $s0, calls helper($a0), then uses $s0 after the call.
  // In `.jump2`, `jump.c:delete_noop_moves` deletes `(set (reg:SI 4 a0) (reg:SI 16 s0))` before
  // the call because `$a0` still holds the incoming argument value; Stage 2 (`.sched2`) restores
  // that move instruction.
  func.instructions = {
      MakeAddiu(Register::kSp, Register::kSp, -24, 0x80010300),
      MakeSw(Register::kRa, 20, Register::kSp, 0x80010304),
      MakeSw(Register::kS0, 16, Register::kSp, 0x80010308),
      MakeJal(0x80020000, 0x8001030C),
      MakeAddu(Register::kS0, Register::kA0, Register::kZero, 0x80010310),
      MakeAddu(Register::kV0, Register::kV0, Register::kS0, 0x80010314),
      MakeLw(Register::kRa, 20, Register::kSp, 0x80010318),
      MakeLw(Register::kS0, 16, Register::kSp, 0x8001031C),
      MakeJrRa(0x80010320),
      MakeAddiu(Register::kSp, Register::kSp, 24, 0x80010324),
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  ControlFlowGraph cfg = context.Cfg();
  CfgAnalysisManager analyses(&cfg);
  CfgDeReorg::UnscheduleDelaySlotsOnly(&cfg, &analyses);

  RtlControlFlowGraph stage1_rtl =
      InverseFinalAndGasLifter::LiftToRtlCfg(cfg, &context.FrameLayout().RawAnalysis());
  RtlControlFlowGraph stage2_rtl =
      InverseJump2CrossJumpLifter::LiftToSched2RtlCfg(cfg, &context.FrameLayout().RawAnalysis());

  ASSERT_EQ(stage1_rtl.rpo_blocks.size(), 1u);
  ASSERT_EQ(stage2_rtl.rpo_blocks.size(), 1u);
  EXPECT_EQ(stage1_rtl.rpo_blocks[0].instructions.size(), 6u);
  ASSERT_EQ(stage2_rtl.rpo_blocks[0].instructions.size(), 7u);
  EXPECT_EQ(stage2_rtl.rpo_blocks[0].instructions[4], "addu  $a0, $s0, $zero");

  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse("-O2 -Wa,-g");
  RtlControlFlowGraph dbr_rtl = InverseFinalAndGasLifter::LiftToDbrRtlCfg(
      context.Cfg(), &context.FrameLayout().RawAnalysis(), nullptr, nullptr, func.name, &flags);
  RtlControlFlowGraph jump2_rtl = InverseFinalAndGasLifter::LiftToJump2RtlCfg(
      cfg, &dbr_rtl, &context.FrameLayout().RawAnalysis(), nullptr, nullptr, func.name, &flags);
  RtlControlFlowGraph sched2_sexpr_rtl = InverseJump2CrossJumpLifter::LiftToSched2RtlCfg(
      cfg, &context.FrameLayout().RawAnalysis(), &jump2_rtl, nullptr, nullptr, func.name, &flags);
  ASSERT_EQ(sched2_sexpr_rtl.rpo_blocks.size(), 1u);
  ASSERT_EQ(sched2_sexpr_rtl.rpo_blocks[0].instructions.size(), 7u);
  EXPECT_EQ(sched2_sexpr_rtl.rpo_blocks[0].instructions[4], "(set (reg:SI 4 a0) (reg:SI 16 s0))");
}

}  // namespace
}  // namespace rom_nom_nom
