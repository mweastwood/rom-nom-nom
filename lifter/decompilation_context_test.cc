#include "lifter/decompilation_context.h"

#include <memory>
#include <vector>

#include "core/c_ast.h"
#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/function_loader.h"

namespace rom_nom_nom {
namespace {

Instruction MakeAddiu(Register rt, Register rs, int16_t imm, uint32_t vram) {
  Instruction inst;
  inst.opcode = Opcode::kAddiu;
  inst.rt = rt;
  inst.rs = rs;
  inst.immediate = imm;
  inst.vram = vram;
  return inst;
}

Instruction MakeSw(Register rt, int16_t offset, Register base, uint32_t vram) {
  Instruction inst;
  inst.opcode = Opcode::kSw;
  inst.rt = rt;
  inst.rs = base;
  inst.immediate = offset;
  inst.vram = vram;
  return inst;
}

Instruction MakeJal(uint32_t target, uint32_t vram) {
  Instruction inst;
  inst.opcode = Opcode::kJal;
  inst.target = (target >> 2) & 0x03FFFFFF;
  inst.vram = vram;
  return inst;
}

Instruction MakeJrRa(uint32_t vram) {
  Instruction inst;
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

}  // namespace
}  // namespace rom_nom_nom
