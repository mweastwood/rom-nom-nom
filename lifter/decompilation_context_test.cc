#include "lifter/decompilation_context.h"

#include <memory>
#include <vector>

#include "core/c_ast.h"
#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/function_loader.h"
#include "lifter/inverse_compiler_pipeline.h"
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

Instruction MakeNop(uint32_t vram) {
  Instruction inst;
  inst.opcode = Opcode::kSll;
  inst.rd = Register::kZero;
  inst.rt = Register::kZero;
  inst.vram = vram;
  return inst;
}

TEST(DecompilationContextTest, BuildsCfgAndClassifiesStackFrameLayoutRegions) {
  LoadedFunction func;
  func.name = "test_frame_fn";
  func.vram = 0x80010000;
  // Total frame size = 40 bytes:
  //   [0, 16)   : outgoing call argument build area
  //   [16, 32)  : local variable region (e.g. offset 20)
  //   [32, 40)  : saved GPR region ($s0 at 32, $ra at 36)
  //   [40, 56)  : caller-homed argument region ($a0 at 40)
  func.instructions = {
      MakeAddiu(Register::kSp, Register::kSp, -40, 0x80010000),
      MakeSw(Register::kRa, 36, Register::kSp, 0x80010004),
      MakeSw(Register::kS0, 32, Register::kSp, 0x80010008),
      MakeSw(Register::kA0, 40, Register::kSp, 0x8001000C),
      MakeSw(Register::kA1, 20, Register::kSp, 0x80010010),
      MakeJal(0x80020000, 0x80010014),
      MakeNop(0x80010018),
      MakeAddiu(Register::kSp, Register::kSp, 40, 0x8001001C),
      MakeJrRa(0x80010020),
      MakeNop(0x80010024),
  };

  DecompilationContext context(func);
  ASSERT_TRUE(context.BuildCfg().ok());
  ASSERT_TRUE(context.HasCfg());

  const StackFrameLayout& layout = context.FrameLayout();
  EXPECT_EQ(layout.TotalFrameSize(), 40);
  EXPECT_EQ(layout.OutgoingArgsSize(), 16);
  EXPECT_EQ(layout.GpRegSize(), 8);
  EXPECT_EQ(layout.GpRegRounded(), 8);
  EXPECT_EQ(layout.FpRegSize(), 0);
  EXPECT_EQ(layout.FpRegRounded(), 0);
  EXPECT_EQ(layout.LocalVarRegionStart(), 16);
  EXPECT_EQ(layout.LocalVarRegionEnd(), 32);
  EXPECT_EQ(layout.LocalVarSize(), 16);
  EXPECT_EQ(layout.ExactVarSize(), 16);
  EXPECT_EQ(layout.CallerHomingStart(), 40);
  EXPECT_TRUE(layout.HasCalls());

  EXPECT_TRUE(layout.IsOutgoingArgOffset(0));
  EXPECT_TRUE(layout.IsOutgoingArgOffset(12));
  EXPECT_FALSE(layout.IsOutgoingArgOffset(16));

  EXPECT_TRUE(layout.IsLocalVarOffset(20));
  EXPECT_FALSE(layout.IsLocalVarOffset(32));

  EXPECT_TRUE(layout.IsCalleeSavedSpillOffset(32));
  EXPECT_TRUE(layout.IsCalleeSavedSpillOffset(36));
  EXPECT_FALSE(layout.IsCalleeSavedSpillOffset(20));

  EXPECT_TRUE(layout.IsCallerHomedArgOffset(40));
  EXPECT_TRUE(layout.IsCallerHomedArgOffset(52));
  EXPECT_FALSE(layout.IsCallerHomedArgOffset(56));
}

TEST(DecompilationContextTest, TracksAstLifecycle) {
  LoadedFunction func;
  func.name = "simple_leaf";
  func.vram = 0x80010100;
  func.instructions = {
      MakeJrRa(0x80010100),
      MakeNop(0x80010104),
  };

  DecompilationContext context(func);
  EXPECT_FALSE(context.HasAst());

  FunctionDeclaration ast(CType{"void"}, "simple_leaf", {}, std::make_unique<CompoundStatement>());
  context.SetAst(std::move(ast));
  EXPECT_TRUE(context.HasAst());
  EXPECT_EQ(context.Ast().Name(), "simple_leaf");

  auto taken = context.TakeAst();
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(taken->Name(), "simple_leaf");
  EXPECT_FALSE(context.HasAst());
}

TEST(DecompilationContextTest, LiftsCfgToRtlTaggingPrologueAndStrippingEpilogue) {
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
  context.LiftCfgToRtl();
  ASSERT_TRUE(context.HasRtlCfg());
  ASSERT_EQ(context.RtlCfg().rpo_blocks.size(), 1u);

  const auto& insns = context.RtlCfg().rpo_blocks[0].rtl_insns;
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

TEST(DecompilationContextTest, InverseCompilerPipelineExecutesAllStagesInOrderAndProducesCAst) {
  LoadedFunction func;
  func.name = "add_imm";
  func.vram = 0x80010200;
  func.instructions = {
      MakeJrRa(0x80010200),
      MakeAddiu(Register::kV0, Register::kA0, 7, 0x80010204),
  };

  DecompilationContext context(func);
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();

  std::vector<InverseStageId> executed_stages;
  size_t stage0_rtl_insns = 0;
  ASSERT_TRUE(pipeline
                  .Run(&context,
                       [&](const InverseStageDescriptor& desc, const DecompilationContext& ctx) {
                         executed_stages.push_back(desc.id);
                         if (desc.id == InverseStageId::kStage00FinalAndGas) {
                           ASSERT_TRUE(ctx.HasRtlCfg());
                           ASSERT_EQ(ctx.RtlCfg().rpo_blocks.size(), 1u);
                           stage0_rtl_insns = ctx.RtlCfg().rpo_blocks[0].rtl_insns.size();
                         }
                       })
                  .ok());

  ASSERT_EQ(executed_stages.size(), 14u);
  EXPECT_EQ(executed_stages.front(), InverseStageId::kStage00FinalAndGas);
  EXPECT_EQ(executed_stages[1], InverseStageId::kStage01DbrDelaySlot);
  EXPECT_EQ(executed_stages[2], InverseStageId::kStage02Jump2CrossJump);
  EXPECT_EQ(executed_stages.back(), InverseStageId::kStage13RtlToCAst);
  EXPECT_EQ(stage0_rtl_insns, 1u);
  ASSERT_TRUE(context.HasAst());
  EXPECT_EQ(context.Ast().Name(), "add_imm");
}

TEST(DecompilationContextTest, Stage2RecoversSched2CallerArgMoveBeforeFirstCall) {
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
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();

  RtlControlFlowGraph stage1_rtl;
  RtlControlFlowGraph stage2_rtl;
  ASSERT_TRUE(pipeline
                  .Run(&context,
                       [&](const InverseStageDescriptor& desc, const DecompilationContext& ctx) {
                         if (desc.id == InverseStageId::kStage01DbrDelaySlot) {
                           stage1_rtl = ctx.RtlCfg();
                         } else if (desc.id == InverseStageId::kStage02Jump2CrossJump) {
                           stage2_rtl = ctx.RtlCfg();
                         }
                       })
                  .ok());

  ASSERT_EQ(stage1_rtl.rpo_blocks.size(), 1u);
  ASSERT_EQ(stage2_rtl.rpo_blocks.size(), 1u);
  // Stage 1 (.jump2) has 6 RTL insns: sp-alloc, sw ra, sw s0, s0=a0, jal, v0=v0+s0.
  // Stage 2 (.sched2) restores a0=s0 before the call, yielding 7 RTL insns.
  EXPECT_EQ(stage1_rtl.rpo_blocks[0].instructions.size(), 6u);
  ASSERT_EQ(stage2_rtl.rpo_blocks[0].instructions.size(), 7u);
  EXPECT_EQ(stage2_rtl.rpo_blocks[0].instructions[4], "addu  $a0, $s0, $zero");
}

TEST(DecompilationContextTest, Stage3StripsPrologueAndRestoresGregSelfMoves) {
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
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();

  RtlControlFlowGraph stage2_rtl;
  RtlControlFlowGraph stage3_rtl;
  ASSERT_TRUE(pipeline
                  .Run(&context,
                       [&](const InverseStageDescriptor& desc, const DecompilationContext& ctx) {
                         if (desc.id == InverseStageId::kStage02Jump2CrossJump) {
                           stage2_rtl = ctx.RtlCfg();
                         } else if (desc.id == InverseStageId::kStage03Sched2PostReload) {
                           stage3_rtl = ctx.RtlCfg();
                         }
                       })
                  .ok());

  ASSERT_EQ(stage2_rtl.rpo_blocks.size(), 1u);
  ASSERT_EQ(stage3_rtl.rpo_blocks.size(), 1u);
  // Stage 2 (.sched2) has 7 RTL insns (3 prologue + 4 body).
  // Stage 3 (.greg) strips the 3 prologue insns and restores 2 parameter self-moves (`a0`, `a1`),
  // yielding 6 RTL insns.
  EXPECT_EQ(stage2_rtl.rpo_blocks[0].instructions.size(), 7u);
  ASSERT_EQ(stage3_rtl.rpo_blocks[0].instructions.size(), 6u);
  EXPECT_EQ(stage3_rtl.rpo_blocks[0].instructions[0], "addu  $a0, $a0, $zero");
  EXPECT_EQ(stage3_rtl.rpo_blocks[0].instructions[1], "addu  $a1, $a1, $zero");
}

TEST(DecompilationContextTest, Stage4FoldsReloadInstructionsIntoLregRtlCfg) {
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
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();

  RtlControlFlowGraph stage3_rtl;
  RtlControlFlowGraph stage4_rtl;
  ASSERT_TRUE(pipeline
                  .Run(&context,
                       [&](const InverseStageDescriptor& desc, const DecompilationContext& ctx) {
                         if (desc.id == InverseStageId::kStage03Sched2PostReload) {
                           stage3_rtl = ctx.RtlCfg();
                         } else if (desc.id == InverseStageId::kStage04GregReload) {
                           stage4_rtl = ctx.RtlCfg();
                         }
                       })
                  .ok());

  ASSERT_EQ(stage3_rtl.rpo_blocks.size(), 1u);
  ASSERT_EQ(stage4_rtl.rpo_blocks.size(), 1u);
  EXPECT_EQ(stage3_rtl.rpo_blocks[0].instructions.size(), 5u);
  ASSERT_EQ(stage4_rtl.rpo_blocks[0].instructions.size(), 2u);
}

TEST(DecompilationContextTest, Stage6FoldsSplitLargeIntPairsIntoCombineRtlCfg) {
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
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();

  RtlControlFlowGraph stage5_rtl;
  RtlControlFlowGraph stage6_rtl;
  ASSERT_TRUE(pipeline
                  .Run(&context,
                       [&](const InverseStageDescriptor& desc, const DecompilationContext& ctx) {
                         if (desc.id == InverseStageId::kStage05LregLocalAlloc) {
                           stage5_rtl = ctx.RtlCfg();
                         } else if (desc.id == InverseStageId::kStage06Sched1PreAlloc) {
                           stage6_rtl = ctx.RtlCfg();
                         }
                       })
                  .ok());

  ASSERT_EQ(stage5_rtl.rpo_blocks.size(), 1u);
  ASSERT_EQ(stage6_rtl.rpo_blocks.size(), 1u);
  EXPECT_EQ(stage5_rtl.rpo_blocks[0].rtl_insns.size(), 3u);
  ASSERT_EQ(stage6_rtl.rpo_blocks[0].rtl_insns.size(), 2u);
  EXPECT_EQ(stage6_rtl.rpo_blocks[0].rtl_insns[0].idiom_kind,
            RtlFusedIdiomKind::kSplitLargeIntHiLo);
  EXPECT_EQ(stage6_rtl.rpo_blocks[0].rtl_insns[0].machine_instructions.size(), 2u);
}

}  // namespace
}  // namespace rom_nom_nom
