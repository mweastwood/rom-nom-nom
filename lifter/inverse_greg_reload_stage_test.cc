#include "lifter/inverse_greg_reload_stage.h"

#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/decompilation_context.h"
#include "lifter/function_loader.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

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

TEST(InverseGregReloadStageTest, FoldsInputReloadInstructionsIntoLregRtlCfg) {
  auto make_mtc1 = [](Register rt, FpRegister fs, uint32_t vram) {
    Instruction inst;
    inst.raw_word = 0x44800000u;
    inst.opcode = Opcode::kMtc1;
    inst.rt = rt;
    inst.fs = fs;
    inst.vram = vram;
    return inst;
  };
  auto make_mul_s = [](FpRegister fd, FpRegister fs, FpRegister ft, uint32_t vram) {
    Instruction inst;
    inst.raw_word = 0x46000002u;
    inst.opcode = Opcode::kMulS;
    inst.fd = fd;
    inst.fs = fs;
    inst.ft = ft;
    inst.vram = vram;
    return inst;
  };
  auto make_add_s = [](FpRegister fd, FpRegister fs, FpRegister ft, uint32_t vram) {
    Instruction inst;
    inst.raw_word = 0x46000000u;
    inst.opcode = Opcode::kAddS;
    inst.fd = fd;
    inst.fs = fs;
    inst.ft = ft;
    inst.vram = vram;
    return inst;
  };

  LoadedFunction func;
  func.name = "stage4_lreg_reload_fold_fn";
  func.vram = 0x80010500;
  // Mirrors `ScaleAndOffsetFloat(s32 unused, f32 x, f32 scale, f32 offset)`:
  // In `.lreg`, `(reg:SF 5 a1)`, `(reg:SF 6 a2)`, `(reg:SF 7 a3)` appear directly inside `mul.s`
  // and `add.s` (2 RTL instructions). In `.greg`, `reload1.c:emit_reload_insns` inserts 3
  // `RELOAD_FOR_INPUT` `mtc1` instructions into `$f2`/`$f4` (5 RTL instructions).
  func.instructions = {
      make_mtc1(Register::kA1, FpRegister::kF2, 0x80010500),
      make_mtc1(Register::kA2, FpRegister::kF4, 0x80010504),
      make_mul_s(FpRegister::kF0, FpRegister::kF2, FpRegister::kF4, 0x80010508),
      make_mtc1(Register::kA3, FpRegister::kF2, 0x8001050C),
      make_add_s(FpRegister::kF0, FpRegister::kF0, FpRegister::kF2, 0x80010510),
      MakeJrRa(0x80010514),
      MakeNop(0x80010518),
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  ControlFlowGraph cfg = context.Cfg();
  CfgAnalysisManager analyses(&cfg);
  CfgDeReorg::UnscheduleDelaySlotsOnly(&cfg, &analyses);

  RtlControlFlowGraph rtl_cfg =
      InverseFinalAndGasLifter::LiftToRtlCfg(cfg, &context.FrameLayout().RawAnalysis());
  ASSERT_EQ(rtl_cfg.rpo_blocks.size(), 1u);
  EXPECT_EQ(rtl_cfg.rpo_blocks[0].instructions.size(), 5u);

  InverseGregReloadLifter::InvertGregToLregRtlCfg(&rtl_cfg, &context.FrameLayout().RawAnalysis());
  ASSERT_EQ(rtl_cfg.rpo_blocks.size(), 1u);
  ASSERT_EQ(rtl_cfg.rpo_blocks[0].instructions.size(), 2u);
}

TEST(InverseGregReloadStageTest, StripsHighpartMultiplyMfhiReloads) {
  Instruction mult;
  mult.opcode = Opcode::kMult;
  mult.rs = Register::kA0;
  mult.rt = Register::kV0;

  Instruction mfhi;
  mfhi.opcode = Opcode::kMfhi;
  mfhi.rd = Register::kV0;

  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  RtlInsn r_mult;
  r_mult.canonical_pattern = mult.Disassemble();
  r_mult.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  r_mult.machine_instructions = {mult};
  cfg.rpo_blocks[0].AddInsn(r_mult);

  RtlInsn r_mfhi;
  r_mfhi.canonical_pattern = mfhi.Disassemble();
  r_mfhi.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  r_mfhi.machine_instructions = {mfhi};
  cfg.rpo_blocks[0].AddInsn(r_mfhi);

  InverseGregReloadLifter::InvertGregToLregRtlCfg(&cfg);
  ASSERT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns[0].machine_instructions[0].opcode, Opcode::kMult);
}

}  // namespace
}  // namespace rom_nom_nom
