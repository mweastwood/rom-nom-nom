#include "lifter/inverse_combine_stage.h"

#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

TEST(InverseCombineStageTest, UncombinesEntryAssignParmsAndReturnValueCopy) {
  Instruction addu;
  addu.raw_word = 0x00851021u;
  addu.opcode = Opcode::kAddu;
  addu.rd = Register::kV0;
  addu.rs = Register::kA0;
  addu.rt = Register::kA1;

  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  RtlInsn r;
  r.canonical_pattern = addu.Disassemble();
  r.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  r.machine_instructions = {addu};
  cfg.rpo_blocks[0].AddInsn(r);

  // In `.combine`, `ArithmeticAddTwo(s32 a, s32 b)` is a single `addu $v0, $a0, $a1` instruction.
  // In `.flow`, `assign_parms` emits 2 entry copies (`$a0 -> reg72`, `$a1 -> reg73`), the addition
  // into `reg74`, and the return value copy `reg74 -> $v0` (4 instructions total).
  InverseCombineLifter::InvertCombineToFlowRtlCfg(&cfg);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.size(), 4u);
}

TEST(InverseCombineStageTest, UncombinesSignedAndUnsignedSubwordLoadsInNonEntryBlock) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(2);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[1].predecessors = {0};

  // Entry block B0: `addu $a0, $a0, $zero` + conditional branch
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))");

  // Non-entry block B1: `lh $v1, 0($a0)` (+2 in .flow), `lhu $a1, 2($a0)` (+1 in .flow),
  // `addu $v0, $v1, $a1` (+1 return copy in .flow).
  cfg.rpo_blocks[1].AddPatternOnly("lh    $v1, 0($a0)");
  cfg.rpo_blocks[1].AddPatternOnly("lhu   $a1, 2($a0)");
  cfg.rpo_blocks[1].AddPatternOnly("addu  $v0, $v1, $a1");

  InverseCombineLifter::InvertCombineToFlowRtlCfg(&cfg);
  ASSERT_EQ(cfg.rpo_blocks.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[0].rtl_insns.size(), 2u);
  // 3 original instructions + 2 (lh sign_extend) + 1 (lhu zero_extend) + 1 ($v0 return copy) = 7
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 7u);
}

TEST(InverseCombineStageTest, UncombinesSymbolIndexedAccessAndDivisionWithTrap) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(2);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[1].predecessors = {0};

  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");
  cfg.rpo_blocks[0].AddPatternOnly("addu  $a1, $a1, $zero");

  // B1 has a `kGasSymbolIndexedAccess` word load (+2 in .flow) and a `kDivisionWithTrap` divmod
  // extracting both `mflo` and `mfhi` (+2 in .flow), plus a store `sw $v0, 0($a0)`.
  RtlInsn sym_lw;
  sym_lw.canonical_pattern = "lw    $v1, 0($v0)";
  sym_lw.idiom_kind = RtlFusedIdiomKind::kGasSymbolIndexedAccess;
  cfg.rpo_blocks[1].AddInsn(sym_lw);

  Instruction div_inst;
  div_inst.raw_word = 0x0065001Au;
  div_inst.opcode = Opcode::kDiv;
  div_inst.rs = Register::kV1;
  div_inst.rt = Register::kA1;
  Instruction mflo_inst;
  mflo_inst.raw_word = 0x00001012u;
  mflo_inst.opcode = Opcode::kMflo;
  mflo_inst.rd = Register::kV0;
  Instruction mfhi_inst;
  mfhi_inst.raw_word = 0x00003010u;
  mfhi_inst.opcode = Opcode::kMfhi;
  mfhi_inst.rd = Register::kA2;

  RtlInsn div_trap;
  div_trap.canonical_pattern = "div   $zero, $v1, $a1";
  div_trap.idiom_kind = RtlFusedIdiomKind::kDivisionWithTrap;
  div_trap.machine_instructions = {div_inst, mflo_inst, mfhi_inst};
  cfg.rpo_blocks[1].AddInsn(div_trap);

  cfg.rpo_blocks[1].AddPatternOnly("addu  $v1, $v0, $a2");
  cfg.rpo_blocks[1].AddPatternOnly("sw    $v1, 0($a0)");

  InverseCombineLifter::InvertCombineToFlowRtlCfg(&cfg);
  // 4 original instructions + 2 (kGasSymbolIndexedAccess) + 2 (divmod mflo+mfhi) = 8
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 8u);
}

TEST(InverseCombineStageTest, UncombinesCallArgAndReturnRegisterMoves) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(2);
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[1].predecessors = {0};

  cfg.rpo_blocks[0].AddPatternOnly("addu  $a0, $a0, $zero");

  // B1 computes `$a0 = $a0 + 1` (coalesced arg copy -> +1), calls `jal`, and uses `$v0` in
  // `addiu $v0, $v0, 5` (coalesced call-result copy -> +1, plus exit return copy -> +1).
  cfg.rpo_blocks[1].AddPatternOnly("addiu $a0, $a0, 1");
  cfg.rpo_blocks[1].AddPatternOnly("jal   0x80040000");
  cfg.rpo_blocks[1].AddPatternOnly("addiu $v0, $v0, 5");

  InverseCombineLifter::InvertCombineToFlowRtlCfg(&cfg);
  // 3 original instructions + 1 (arg $a0 copy) + 1 (call $v0 result copy) + 1 (exit $v0 copy) = 6
  EXPECT_EQ(cfg.rpo_blocks[1].rtl_insns.size(), 6u);
}

}  // namespace
}  // namespace rom_nom_nom
