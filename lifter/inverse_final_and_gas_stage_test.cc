#include "lifter/inverse_final_and_gas_stage.h"

#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/decompilation_context.h"
#include "lifter/function_loader.h"
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

Instruction MakeJal(uint32_t target, uint32_t vram) {
  Instruction inst;
  inst.raw_word = 0x0C000000u;
  inst.opcode = Opcode::kJal;
  inst.target = (target >> 2) & 0x03FFFFFF;
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

Instruction MakeNop(uint32_t vram) {
  Instruction inst;
  inst.opcode = Opcode::kSll;
  inst.rd = Register::kZero;
  inst.rt = Register::kZero;
  inst.vram = vram;
  return inst;
}

TEST(InverseFinalAndGasStageTest, LiftsCfgToRtlTaggingPrologueAndStrippingEpilogue) {
  LoadedFunction func;
  func.name = "test_rtl_prologue_epilogue";
  func.vram = 0x80010000;
  func.instructions = {
      MakeAddiu(Register::kSp, Register::kSp, -40, 0x80010000),
      MakeSw(Register::kRa, 36, Register::kSp, 0x80010004),
      MakeSw(Register::kS0, 32, Register::kSp, 0x80010008),
      MakeSw(Register::kA1, 20, Register::kSp, 0x8001000C),
      MakeJal(0x80020000, 0x80010010),
      MakeNop(0x80010014),
      MakeAddiu(Register::kSp, Register::kSp, 40, 0x80010018),
      MakeJrRa(0x8001001C),
      MakeNop(0x80010020),
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  const RtlControlFlowGraph rtl_cfg =
      InverseFinalAndGasLifter::LiftToRtlCfg(context.Cfg(), &context.FrameLayout().RawAnalysis());
  ASSERT_EQ(rtl_cfg.rpo_blocks.size(), 1u);

  const auto& insns = rtl_cfg.rpo_blocks[0].rtl_insns;
  // Expected in .dbr RTL:
  //   0: addiu $sp, $sp, -40 (prologue)
  //   1: sw $ra, 36($sp)     (prologue)
  //   2: sw $s0, 32($sp)     (prologue)
  //   3: sw $a1, 20($sp)     (body)
  //   4: jal 0x80020000      (body)
  //   (epilogue addiu $sp, 40 and jr $ra are stripped)
  ASSERT_EQ(insns.size(), 5u);
  EXPECT_TRUE(insns[0].is_prologue_insn);
  EXPECT_TRUE(insns[1].is_prologue_insn);
  EXPECT_TRUE(insns[2].is_prologue_insn);
  EXPECT_FALSE(insns[3].is_prologue_insn);
  EXPECT_FALSE(insns[4].is_prologue_insn);
}

TEST(InverseFinalAndGasStageTest, FusesMulSi3AndFormatsBranchesInCanonicalRtl) {
  Instruction mult;
  mult.raw_word = 0x00850018u;
  mult.opcode = Opcode::kMult;
  mult.rs = Register::kA0;
  mult.rt = Register::kA1;
  mult.vram = 0x80010100;

  Instruction mflo;
  mflo.raw_word = 0x00001012u;
  mflo.opcode = Opcode::kMflo;
  mflo.rd = Register::kV0;
  mflo.vram = 0x80010104;

  Instruction nop;
  nop.opcode = Opcode::kSll;
  nop.rd = Register::kZero;
  nop.rt = Register::kZero;
  nop.vram = 0x8001010C;

  LoadedFunction func;
  func.name = "mul_two";
  func.vram = 0x80010100;
  func.instructions = {
      mult,
      mflo,
      MakeJrRa(0x80010108),
      nop,
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  const RtlControlFlowGraph rtl_cfg =
      InverseFinalAndGasLifter::LiftToRtlCfg(context.Cfg(), &context.FrameLayout().RawAnalysis());
  ASSERT_EQ(rtl_cfg.rpo_blocks.size(), 1u);
  ASSERT_EQ(rtl_cfg.rpo_blocks[0].rtl_insns.size(), 1u);
  EXPECT_EQ(rtl_cfg.rpo_blocks[0].rtl_insns[0].idiom_kind, RtlFusedIdiomKind::kMulSi3);
  EXPECT_EQ(rtl_cfg.rpo_blocks[0].rtl_insns[0].machine_instructions.size(), 2u);
}

TEST(InverseFinalAndGasStageTest, LiftsDbrRtlCfgWithNarrowSubregModesAndRegisterFlags) {
  Instruction lh;
  lh.raw_word = 0x84820000u;
  lh.opcode = Opcode::kLh;
  lh.rt = Register::kV0;
  lh.rs = Register::kA0;
  lh.immediate = 0;
  lh.vram = 0x80010200;

  Instruction andi;
  andi.raw_word = 0x3042ffffu;
  andi.opcode = Opcode::kAndi;
  andi.rt = Register::kV0;
  andi.rs = Register::kV0;
  andi.immediate = -1;
  andi.vram = 0x80010208;

  LoadedFunction func;
  func.name = "narrow_halfword_load_and_mask";
  func.vram = 0x80010200;
  func.instructions = {
      lh,
      MakeJrRa(0x80010204),
      andi,
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  const RtlControlFlowGraph dbr_cfg = InverseFinalAndGasLifter::LiftToDbrRtlCfg(
      context.Cfg(), &context.FrameLayout().RawAnalysis(), nullptr, nullptr, func.name);
  ASSERT_EQ(dbr_cfg.rpo_blocks.size(), 1u);
  ASSERT_EQ(dbr_cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(dbr_cfg.rpo_blocks[0].instructions[0],
            "(set (reg:SI 2 v0) (sign_extend:SI (mem:HI (reg:SI 4 a0))))");
  EXPECT_EQ(dbr_cfg.rpo_blocks[0].instructions[1],
            "(set (reg:SI 2 v0) (zero_extend:SI (subreg:HI (reg:SI 2 v0) 0)))");
}

}  // namespace
}  // namespace rom_nom_nom
