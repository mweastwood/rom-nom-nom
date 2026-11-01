#include "lifter/inverse_sched1_pre_alloc_stage.h"

#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/decompilation_context.h"
#include "lifter/function_loader.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

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

Instruction MakeNop(uint32_t vram) {
  Instruction inst;
  inst.opcode = Opcode::kSll;
  inst.rd = Register::kZero;
  inst.rt = Register::kZero;
  inst.vram = vram;
  return inst;
}

TEST(InverseSched1PreAllocStageTest, FoldsSplitLargeIntPairsIntoCombineRtlCfg) {
  auto make_lui = [](Register rt, uint16_t imm16, uint32_t vram) {
    Instruction inst;
    inst.raw_word = 0x3c000000u | (static_cast<uint32_t>(rt) << 16) | imm16;
    inst.opcode = Opcode::kLui;
    inst.rt = rt;
    inst.immediate = static_cast<int16_t>(imm16);
    inst.vram = vram;
    return inst;
  };
  auto make_ori = [](Register rt, Register rs, uint16_t imm16, uint32_t vram) {
    Instruction inst;
    inst.raw_word =
        0x34000000u | (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) | imm16;
    inst.opcode = Opcode::kOri;
    inst.rt = rt;
    inst.rs = rs;
    inst.immediate = static_cast<int16_t>(imm16);
    inst.vram = vram;
    return inst;
  };

  LoadedFunction func;
  func.name = "stage6_combine_split_large_int_fn";
  func.vram = 0x80010600;
  // Mirrors `ArithmeticLargeImmediate(s32 a)` (`a + 0x12345678`):
  // In `.combine`, `(set (reg:SI 81) (const_int 305419896))` is a single RTL instruction.
  // In `.sched`, `sched.c:schedule_insns` calls `try_split` -> `gen_split_156` to split `large_int`
  // into `lui $v0, 0x1234` and `ori $v0, $v0, 0x5678`.
  func.instructions = {
      make_lui(Register::kV0, 0x1234u, 0x80010600),
      make_ori(Register::kV0, Register::kV0, 0x5678u, 0x80010604),
      MakeAddu(Register::kV0, Register::kA0, Register::kV0, 0x80010608),
      MakeJrRa(0x8001060C),
      MakeNop(0x80010610),
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  ControlFlowGraph cfg = context.Cfg();
  CfgAnalysisManager analyses(&cfg);
  CfgDeReorg::UnscheduleDelaySlotsOnly(&cfg, &analyses);

  RtlControlFlowGraph rtl_cfg =
      InverseFinalAndGasLifter::LiftToRtlCfg(cfg, &context.FrameLayout().RawAnalysis());
  ASSERT_EQ(rtl_cfg.rpo_blocks.size(), 1u);
  EXPECT_EQ(rtl_cfg.rpo_blocks[0].rtl_insns.size(), 3u);

  InverseSched1PreAllocLifter::InvertSchedToCombineRtlCfg(&rtl_cfg);
  ASSERT_EQ(rtl_cfg.rpo_blocks.size(), 1u);
  ASSERT_EQ(rtl_cfg.rpo_blocks[0].rtl_insns.size(), 2u);
  EXPECT_EQ(rtl_cfg.rpo_blocks[0].rtl_insns[0].idiom_kind, RtlFusedIdiomKind::kSplitLargeIntHiLo);
  EXPECT_EQ(rtl_cfg.rpo_blocks[0].rtl_insns[0].machine_instructions.size(), 2u);
}

TEST(InverseSched1PreAllocStageTest, DoesNotFoldAcrossInterveningReadOfHighHalf) {
  Instruction lui;
  lui.opcode = Opcode::kLui;
  lui.rt = Register::kV0;
  lui.immediate = 0x1000;

  Instruction addu = MakeAddu(Register::kA0, Register::kA0, Register::kV0, 0x80010604);

  Instruction ori;
  ori.opcode = Opcode::kOri;
  ori.rt = Register::kV0;
  ori.rs = Register::kV0;
  ori.immediate = 0x0001;

  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  for (const Instruction& inst : {lui, addu, ori}) {
    RtlInsn r;
    r.canonical_pattern = inst.Disassemble();
    r.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
    r.machine_instructions = {inst};
    cfg.rpo_blocks[0].AddInsn(r);
  }

  // Because `addu` reads `$v0` before `ori` modifies `$v0`, `lui` and `ori` are not a split
  // `large_int` pair and must remain separate instructions.
  InverseSched1PreAllocLifter::InvertSchedToCombineRtlCfg(&cfg);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 3u);
}

}  // namespace
}  // namespace rom_nom_nom
