#include "core/mips.h"

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(MipsTest, RegisterNames) {
  EXPECT_EQ(RegisterName(Register::kZero), "$zero");
  EXPECT_EQ(RegisterName(Register::kAt), "$at");
  EXPECT_EQ(RegisterName(Register::kV0), "$v0");
  EXPECT_EQ(RegisterName(Register::kA0), "$a0");
  EXPECT_EQ(RegisterName(Register::kT0), "$t0");
  EXPECT_EQ(RegisterName(Register::kS0), "$s0");
  EXPECT_EQ(RegisterName(Register::kSp), "$sp");
  EXPECT_EQ(RegisterName(Register::kRa), "$ra");

  EXPECT_EQ(FpRegisterName(FpRegister::kF0), "$f0");
  EXPECT_EQ(FpRegisterName(FpRegister::kF12), "$f12");
  EXPECT_EQ(FpRegisterName(FpRegister::kF31), "$f31");
}

TEST(MipsTest, DecodeNop) {
  // 0x00000000 = sll $zero, $zero, 0 = nop
  auto res = DecodeInstruction(0x00000000, 0x80025C00);
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->IsNop());
  EXPECT_EQ(res->Disassemble(), "nop");
}

TEST(MipsTest, DecodeRTypeArithmetic) {
  // addu $t0, $a0, $a1 -> 0x00854021
  // rs=4 ($a0), rt=5 ($a1), rd=8 ($t0), funct=0x21 (addu)
  auto res = DecodeInstruction(0x00854021, 0x80025C00);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->opcode, Opcode::kAddu);
  ASSERT_TRUE(res->rd.has_value());
  EXPECT_EQ(*res->rd, Register::kT0);
  ASSERT_TRUE(res->rs.has_value());
  EXPECT_EQ(*res->rs, Register::kA0);
  ASSERT_TRUE(res->rt.has_value());
  EXPECT_EQ(*res->rt, Register::kA1);
  EXPECT_EQ(res->Disassemble(), "addu  $t0, $a0, $a1");
  EXPECT_FALSE(res->IsBranch());
  EXPECT_FALSE(res->IsJump());
}

TEST(MipsTest, DecodeRTypeShift) {
  // sll $t0, $t1, 4 -> rs=0, rt=9 ($t1), rd=8 ($t0), sa=4, funct=0x00
  // (0 << 21) | (9 << 16) | (8 << 11) | (4 << 6) | 0x00 = 0x00094100
  auto res = DecodeInstruction(0x00094100, 0x80025C00);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->opcode, Opcode::kSll);
  EXPECT_EQ(*res->rd, Register::kT0);
  EXPECT_EQ(*res->rt, Register::kT1);
  EXPECT_EQ(res->shift_amount, 4);
  EXPECT_EQ(res->Disassemble(), "sll   $t0, $t1, 4");
}

TEST(MipsTest, DecodeITypeImmediateAndStack) {
  // addiu $sp, $sp, -32 (-0x20 = 0xFFE0)
  // op=0x09, rs=29 ($sp), rt=29 ($sp), imm=0xFFE0 -> 0x27BDFFE0
  auto res = DecodeInstruction(0x27BDFFE0, 0x80025C00);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->opcode, Opcode::kAddiu);
  EXPECT_EQ(*res->rs, Register::kSp);
  EXPECT_EQ(*res->rt, Register::kSp);
  EXPECT_EQ(res->immediate, -32);
  EXPECT_EQ(res->Disassemble(), "addiu $sp, $sp, -32");
}

TEST(MipsTest, DecodeLogicalImmediateUnsigned) {
  // andi $a0, $a0, 0xFFFF (op=0x0C, rs=4, rt=4, imm=0xFFFF -> 0x3084FFFF)
  auto res = DecodeInstruction(0x3084FFFF, 0x80025C00);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->opcode, Opcode::kAndi);
  EXPECT_EQ(res->UnsignedImmediate(), 0xFFFFu);
  EXPECT_EQ(res->immediate, -1);
  EXPECT_EQ(res->Disassemble(), "andi  $a0, $a0, 0xFFFF");
}

TEST(MipsTest, DecodeLui) {
  // lui $t0, 0x8004 -> op=0x0F, rs=0, rt=8 ($t0), imm=0x8004 -> 0x3C088004
  auto res = DecodeInstruction(0x3C088004, 0x80025C00);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->opcode, Opcode::kLui);
  EXPECT_EQ(*res->rt, Register::kT0);
  EXPECT_EQ(static_cast<uint16_t>(res->immediate), 0x8004);
  EXPECT_EQ(res->Disassemble(), "lui   $t0, 0x8004");
}

TEST(MipsTest, DecodeMemoryLoadsAndStores) {
  // lw $v0, 16($sp) -> op=0x23, rs=29 ($sp), rt=2 ($v0), imm=16 -> 0x8FA20010
  auto load = DecodeInstruction(0x8FA20010, 0x80025C00);
  ASSERT_TRUE(load.ok());
  EXPECT_EQ(load->opcode, Opcode::kLw);
  EXPECT_TRUE(load->IsLoad());
  EXPECT_FALSE(load->IsStore());
  EXPECT_EQ(load->Disassemble(), "lw    $v0, 16($sp)");

  // sw $ra, 28($sp) -> op=0x2B, rs=29 ($sp), rt=31 ($ra), imm=28 -> 0xAFBF001C
  auto store = DecodeInstruction(0xAFBF001C, 0x80025C00);
  ASSERT_TRUE(store.ok());
  EXPECT_EQ(store->opcode, Opcode::kSw);
  EXPECT_TRUE(store->IsStore());
  EXPECT_FALSE(store->IsLoad());
  EXPECT_EQ(store->Disassemble(), "sw    $ra, 28($sp)");
}

TEST(MipsTest, DecodeJumpAndCall) {
  // jal 0x800266C0 at PC 0x80025C00
  // target = (0x800266C0 & 0x0FFFFFFF) >> 2 = 0x000266C0 >> 2 = 0x000099B0
  // op=0x03 -> (3 << 26) | 0x000099B0 = 0x0C0099B0
  auto jal = DecodeInstruction(0x0C0099B0, 0x80025C00);
  ASSERT_TRUE(jal.ok());
  EXPECT_EQ(jal->opcode, Opcode::kJal);
  EXPECT_TRUE(jal->IsJump());
  EXPECT_TRUE(jal->IsCall());
  EXPECT_TRUE(jal->HasDelaySlot());
  EXPECT_EQ(jal->JumpTarget(), 0x800266C0);
  EXPECT_EQ(jal->Disassemble(), "jal   0x800266C0");

  // jr $ra -> op=0x00, rs=31 ($ra), funct=0x08 -> 0x03E00008
  auto ret = DecodeInstruction(0x03E00008, 0x80025C00);
  ASSERT_TRUE(ret.ok());
  EXPECT_EQ(ret->opcode, Opcode::kJr);
  EXPECT_TRUE(ret->IsJump());
  EXPECT_TRUE(ret->IsReturn());
  EXPECT_TRUE(ret->HasDelaySlot());
  EXPECT_EQ(ret->Disassemble(), "jr    $ra");
}

TEST(MipsTest, DecodeBranchAndTargetCalculation) {
  // beq $a0, $zero, offset +8 words (offset = 8 instructions = +32 bytes)
  // At PC 0x80025C00: target is 0x80025C00 + 4 + (8 << 2) = 0x80025C24
  // op=0x04, rs=4 ($a0), rt=0 ($zero), imm=8 -> 0x10800008
  auto branch = DecodeInstruction(0x10800008, 0x80025C00);
  ASSERT_TRUE(branch.ok());
  EXPECT_EQ(branch->opcode, Opcode::kBeq);
  EXPECT_TRUE(branch->IsBranch());
  EXPECT_FALSE(branch->IsBranchLikely());
  EXPECT_TRUE(branch->HasDelaySlot());
  EXPECT_EQ(branch->BranchTarget(), 0x80025C24);
  EXPECT_EQ(branch->Disassemble(), "beq   $a0, $zero, .L80025C24");

  // beql $s0, $s1, offset -4 words
  // op=0x14, rs=16, rt=17, imm=0xFFFC (-4) -> 0x5211FFFC
  auto beql = DecodeInstruction(0x5211FFFC, 0x80025C10);
  ASSERT_TRUE(beql.ok());
  EXPECT_EQ(beql->opcode, Opcode::kBeql);
  EXPECT_TRUE(beql->IsBranch());
  EXPECT_TRUE(beql->IsBranchLikely());
  EXPECT_EQ(beql->BranchTarget(), 0x80025C10 + 4 + (-4 * 4));
}

TEST(MipsTest, DecodeFPUInstructions) {
  // add.s $f0, $f12, $f14
  // op=0x11, fmt=16 (rs=0x10), ft=14, fs=12, fd=0, funct=0x00
  // (0x11 << 26) | (0x10 << 21) | (14 << 16) | (12 << 11) | (0 << 6) | 0x00
  // 0x460E6000
  auto add_s = DecodeInstruction(0x460E6000, 0x80025C00);
  ASSERT_TRUE(add_s.ok());
  EXPECT_EQ(add_s->opcode, Opcode::kAddS);
  EXPECT_EQ(add_s->Disassemble(), "add.s $f0, $f12, $f14");

  // c.lt.s $f12, $f14 -> funct=0x3C
  // 0x460E603C
  auto c_lt_s = DecodeInstruction(0x460E603C, 0x80025C00);
  ASSERT_TRUE(c_lt_s.ok());
  EXPECT_EQ(c_lt_s->opcode, Opcode::kCLtS);
  EXPECT_EQ(c_lt_s->Disassemble(), "c.lt.s $f12, $f14");

  // lwc1 $f0, 4($sp) -> op=0x31, rs=29, rt=0, imm=4 -> 0xC7A00004
  auto lwc1 = DecodeInstruction(0xC7A00004, 0x80025C00);
  ASSERT_TRUE(lwc1.ok());
  EXPECT_EQ(lwc1->opcode, Opcode::kLwc1);
  EXPECT_TRUE(lwc1->IsLoad());
  EXPECT_EQ(lwc1->Disassemble(), "lwc1  $f0, 4($sp)");
}

TEST(MipsTest, DecodeFromByteSpan) {
  // 0x27BDFFE0 in big-endian bytes
  std::vector<uint8_t> bytes = {0x27, 0xBD, 0xFF, 0xE0};
  auto res = DecodeInstruction(absl::MakeConstSpan(bytes), 0x80025C00);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->opcode, Opcode::kAddiu);
  EXPECT_EQ(res->immediate, -32);

  // Less than 4 bytes should return error
  std::vector<uint8_t> short_bytes = {0x27, 0xBD};
  auto err = DecodeInstruction(absl::MakeConstSpan(short_bytes));
  EXPECT_FALSE(err.ok());
}

TEST(MipsTest, FormatBranchPseudoInstructions) {
  // beq $zero, $zero, target -> b target
  uint32_t b_word = (0x04 << 26) | (0 << 21) | (0 << 16) | 5;
  auto b_inst = DecodeInstruction(b_word, 0x80025C00);
  ASSERT_TRUE(b_inst.ok());
  EXPECT_EQ(FormatBranch(*b_inst, ".L1"), "b      .L1");

  // beq $v0, $zero, target -> beqz $v0, target
  uint32_t beqz_word = (0x04 << 26) | (2 << 21) | (0 << 16) | 5;
  auto beqz_inst = DecodeInstruction(beqz_word, 0x80025C00);
  ASSERT_TRUE(beqz_inst.ok());
  EXPECT_EQ(FormatBranch(*beqz_inst, ".L1"), "beqz   $v0, .L1");

  // bne $v0, $zero, target -> bnez $v0, target
  uint32_t bnez_word = (0x05 << 26) | (2 << 21) | (0 << 16) | 5;
  auto bnez_inst = DecodeInstruction(bnez_word, 0x80025C00);
  ASSERT_TRUE(bnez_inst.ok());
  EXPECT_EQ(FormatBranch(*bnez_inst, ".L1"), "bnez   $v0, .L1");

  // beq $v0, $v1, target -> beq $v0, $v1, target
  uint32_t beq_word = (0x04 << 26) | (2 << 21) | (3 << 16) | 5;
  auto beq_inst = DecodeInstruction(beq_word, 0x80025C00);
  ASSERT_TRUE(beq_inst.ok());
  EXPECT_EQ(FormatBranch(*beq_inst, ".L1"), "beq    $v0, $v1, .L1");

  // Fallback for non-branch instruction
  uint32_t nop_word = 0x00000000;
  auto nop_inst = DecodeInstruction(nop_word, 0x80025C00);
  ASSERT_TRUE(nop_inst.ok());
  EXPECT_EQ(FormatBranch(*nop_inst, ".L1"), "nop");
}

TEST(MipsTest, DecodeSequence) {
  std::vector<uint32_t> words = {
      0x00854021,  // addu $t0, $a0, $a1
      0x00000000,  // nop
      0x03E00008,  // jr $ra
      0x00000000,  // nop (delay slot)
  };
  auto seq_or = DecodeSequence(words, 0x80025C00);
  ASSERT_TRUE(seq_or.ok());
  EXPECT_EQ(seq_or->size(), 4);
  EXPECT_EQ((*seq_or)[0].vram, 0x80025C00);
  EXPECT_EQ((*seq_or)[0].opcode, Opcode::kAddu);
  EXPECT_EQ((*seq_or)[1].vram, 0x80025C04);
  EXPECT_TRUE((*seq_or)[1].IsNop());
  EXPECT_EQ((*seq_or)[2].vram, 0x80025C08);
  EXPECT_TRUE((*seq_or)[2].IsReturn());
  EXPECT_EQ((*seq_or)[3].vram, 0x80025C0C);
  EXPECT_TRUE((*seq_or)[3].IsNop());
}

}  // namespace
}  // namespace rom_nom_nom
