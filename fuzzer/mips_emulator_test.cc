#include "fuzzer/mips_emulator.h"

#include <cstdint>
#include <limits>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom::fuzzer {
namespace {

TEST(MipsEmulatorTest, ArithmeticAndLogicalInstructions) {
  MipsEmulator emu;
  // 00: addiu $a0, $zero, 42
  // 04: addiu $a1, $zero, 10
  // 08: addu  $v0, $a0, $a1   -> 52
  // 0C: subu  $v1, $a0, $a1   -> 32
  // 10: andi  $t0, $v0, 0x0F   -> 52 & 15 = 4
  // 14: ori   $t1, $v0, 0xF0   -> 52 | 240 = 244
  // 18: jr    $ra
  // 1C: nop
  std::vector<uint32_t> code = {
      0x2404002A,  // addiu $a0, $zero, 42
      0x2405000A,  // addiu $a1, $zero, 10
      0x00851021,  // addu  $v0, $a0, $a1
      0x00851823,  // subu  $v1, $a0, $a1
      0x3048000F,  // andi  $t0, $v0, 0xF
      0x344900F0,  // ori   $t1, $v0, 0xF0
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 52u);
  EXPECT_EQ(res.v1, 32u);
  EXPECT_EQ(emu.GetRegister(Register::kT0), 4u);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 244u);
}

TEST(MipsEmulatorTest, ShiftAndComparisons) {
  MipsEmulator emu;
  // 00: addiu $a0, $zero, -16   (0xFFFFFFF0)
  // 04: sra   $t0, $a0, 2       (0xFFFFFFFC = -4)
  // 08: srl   $t1, $a0, 28      (0x0000000F = 15)
  // 0C: sll   $t2, $t1, 4       (0x000000F0 = 240)
  // 10: slti  $v0, $a0, 0       (true = 1)
  // 14: sltiu $v1, $a0, 0       (false = 0, unsigned compares 0xFFFFFFF0 < 0)
  // 18: jr    $ra
  // 1C: nop
  std::vector<uint32_t> code = {
      0x2404FFF0,  // addiu $a0, $zero, -16
      0x00044083,  // sra   $t0, $a0, 2
      0x00044F02,  // srl   $t1, $a0, 28
      0x00095100,  // sll   $t2, $t1, 4
      0x28820000,  // slti  $v0, $a0, 0
      0x2C830000,  // sltiu $v1, $a0, 0
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetRegister(Register::kT0), 0xFFFFFFFCu);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 15u);
  EXPECT_EQ(emu.GetRegister(Register::kT2), 240u);
  EXPECT_EQ(res.v0, 1u);
  EXPECT_EQ(res.v1, 0u);
}

TEST(MipsEmulatorTest, MemoryStoreAndLoadWithWriteLog) {
  MipsEmulator emu;
  uint32_t target_ram = 0x80100000;
  // 00: lui   $a0, 0x8010
  // 04: addiu $t0, $zero, 0x1234
  // 08: sw    $t0, 0($a0)
  // 0C: addiu $t1, $zero, 0x56
  // 10: sb    $t1, 4($a0)
  // 14: lw    $v0, 0($a0)
  // 18: lbu   $v1, 4($a0)
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x24081234,  // addiu $t0, $zero, 0x1234
      0xAC880000,  // sw    $t0, 0($a0)
      0x24090056,  // addiu $t1, $zero, 0x56
      0xA0890004,  // sb    $t1, 4($a0)
      0x8C820000,  // lw    $v0, 0($a0)
      0x90830004,  // lbu   $v1, 4($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0x1234u);
  EXPECT_EQ(res.v1, 0x56u);

  // Verify write log recorded both store operations
  ASSERT_EQ(res.write_log.size(), 2u);
  EXPECT_EQ(res.write_log[0].address, target_ram);
  EXPECT_EQ(res.write_log[0].value, 0x1234u);
  EXPECT_EQ(res.write_log[0].size, 4u);

  EXPECT_EQ(res.write_log[1].address, target_ram + 4);
  EXPECT_EQ(res.write_log[1].value, 0x56u);
  EXPECT_EQ(res.write_log[1].size, 1u);
}

TEST(MipsEmulatorTest, BranchDelaySlotExecution) {
  MipsEmulator emu;
  // 00: addiu $v0, $zero, 1
  // 04: beq   $zero, $zero, 0x0C (branches to 0x14)
  // 08: addiu $v0, $v0, 10       (executed in delay slot! -> v0 becomes 11)
  // 0C: addiu $v0, $v0, 100      (skipped!)
  // 10: nop
  // 14: jr    $ra
  // 18: addiu $v0, $v0, 5        (delay slot of jr $ra! -> v0 becomes 16)
  std::vector<uint32_t> code = {
      0x24020001,  // addiu $v0, $zero, 1
      0x10000003,  // beq   $zero, $zero, +3 (to 0x14)
      0x2442000A,  // addiu $v0, $v0, 10
      0x24420064,  // addiu $v0, $v0, 100
      0x00000000,  // nop
      0x03E00008,  // jr    $ra
      0x24420005,  // addiu $v0, $v0, 5
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 16u);  // 1 + 10 + 5
}

TEST(MipsEmulatorTest, Abs16ExecutionWithPositiveAndNegativeValues) {
  MipsEmulator emu;
  // Retail Abs16:
  // 00: sll   $v0, $a0, 16
  // 04: sra   $v0, $v0, 16
  // 08: bgez  $v0, 0x0C (.L_end)
  // 0C: nop
  // 10: subu  $v0, $zero, $a0
  // 14: sll   $v0, $v0, 16
  // 18: sra   $v0, $v0, 16
  // .L_end:
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> code = {
      0x00041400,  // sll   $v0, $a0, 16
      0x00021403,  // sra   $v0, $v0, 16
      0x04410004,  // bgez  $v0, .L_end (+4)
      0x00000000,  // nop
      0x00041023,  // subu  $v0, $zero, $a0
      0x00021400,  // sll   $v0, $v0, 16
      0x00021403,  // sra   $v0, $v0, 16
      0x03E00008,  // jr    $ra (.L_end)
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80026850, code));

  // Test with positive value: +50 -> 50
  emu.SetRegister(Register::kA0, 50);
  ExecutionResult pos_res = emu.RunFunction(0x80026850);
  EXPECT_EQ(pos_res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(pos_res.v0, 50u);

  // Test with negative value: -123 -> 123
  emu.SetRegister(Register::kA0, static_cast<uint32_t>(-123));
  ExecutionResult neg_res = emu.RunFunction(0x80026850);
  EXPECT_EQ(neg_res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(neg_res.v0, 123u);
}

TEST(MipsEmulatorTest, InfiniteLoopProtection) {
  MipsEmulator emu;
  // 00: beq $zero, $zero, 0 (infinite branch to itself)
  // 04: nop
  std::vector<uint32_t> code = {
      0x1000FFFF,  // beq $zero, $zero, -1 (points to itself at 00)
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, /*max_steps=*/500);

  EXPECT_EQ(res.status, ExecutionStatus::kMaxStepsReached);
  EXPECT_EQ(res.total_steps, 500u);
}

TEST(MipsEmulatorTest, MultiplyAndDivide) {
  MipsEmulator emu;
  // 00: addiu $a0, $zero, 7
  // 04: addiu $a1, $zero, 3
  // 08: mult  $a0, $a1      (prod = 21)
  // 0C: mflo  $v0           (v0 = 21)
  // 10: div   $a0, $a1      (lo = 2, hi = 1)
  // 14: mflo  $t0           (t0 = 2)
  // 18: mfhi  $t1           (t1 = 1)
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> code = {
      0x24040007,  // addiu $a0, $zero, 7
      0x24050003,  // addiu $a1, $zero, 3
      0x00850018,  // mult  $a0, $a1
      0x00001012,  // mflo  $v0
      0x0085001A,  // div   $a0, $a1
      0x00004012,  // mflo  $t0
      0x00004810,  // mfhi  $t1
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 21u);
  EXPECT_EQ(emu.GetRegister(Register::kT0), 2u);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 1u);
}

TEST(MipsEmulatorTest, CalleeSavedRegistersSnapshot) {
  MipsEmulator emu;
  emu.SetRegister(Register::kS0, 0x1111);
  emu.SetRegister(Register::kS7, 0x7777);
  emu.SetRegister(Register::kSp, 0x80700000);

  CalleeSavedRegisters snapshot = emu.GetCalleeSavedRegisters();
  EXPECT_EQ(snapshot.s0, 0x1111u);
  EXPECT_EQ(snapshot.s7, 0x7777u);
  EXPECT_EQ(snapshot.sp, 0x80700000u);

  CalleeSavedRegisters other = snapshot;
  EXPECT_EQ(snapshot, other);

  other.s0 = 0x2222;
  EXPECT_FALSE(snapshot == other);
}

TEST(MipsEmulatorTest, MemoryFaultHandling) {
  MipsEmulator emu;
  // Load from invalid address 0x10000000 (outside 8MB RDRAM)
  // 00: lui $at, 0x1000
  // 04: lw  $v0, 0($at)
  // 08: jr  $ra
  // 0C: nop
  std::vector<uint32_t> code = {
      0x3C011000,  // lui $at, 0x1000
      0x8C220000,  // lw  $v0, 0($at)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kMemoryFault);
}

TEST(MipsEmulatorTest, ZeroRegisterImmutability) {
  MipsEmulator emu;
  // Attempting to write to $zero using multiple instructions
  // 00: addiu $zero, $zero, 99
  // 04: ori   $zero, $zero, 0xFF
  // 08: sll   $zero, $zero, 5
  // 0C: jr    $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x24000063,  // addiu $zero, $zero, 99
      0x340000FF,  // ori   $zero, $zero, 0xFF
      0x00000140,  // sll   $zero, $zero, 5
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetRegister(Register::kZero), 0u);
}

TEST(MipsEmulatorTest, BranchLikelyTakenExecutesDelaySlot) {
  MipsEmulator emu;
  // 00: addiu $v0, $zero, 10
  // 04: addiu $t0, $zero, 1
  // 08: bnel  $t0, $zero, +2 (taken to 0x14)
  // 0C: addiu $v0, $v0, 5    (executed in delay slot! -> v0 becomes 15)
  // 10: addiu $v0, $v0, 100  (skipped)
  // 14: jr    $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x2402000A,  // addiu $v0, $zero, 10
      0x24080001,  // addiu $t0, $zero, 1
      0x55000002,  // bnel  $t0, $zero, +2 (to 0x14)
      0x24420005,  // addiu $v0, $v0, 5
      0x24420064,  // addiu $v0, $v0, 100
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 15u);
}

TEST(MipsEmulatorTest, BranchLikelyNotTakenAnnulsDelaySlot) {
  MipsEmulator emu;
  // 00: addiu $v0, $zero, 10
  // 04: bnel  $zero, $zero, +2 (not taken!)
  // 08: addiu $v0, $v0, 5      (ANNULLED! Not executed!)
  // 0C: addiu $v0, $v0, 2      (falls through to here -> v0 becomes 12)
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> code = {
      0x2402000A,  // addiu $v0, $zero, 10
      0x54000002,  // bnel  $zero, $zero, +2 (not taken)
      0x24420005,  // addiu $v0, $v0, 5 (annulled)
      0x24420002,  // addiu $v0, $v0, 2
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 12u);  // 10 + 2 (5 was annulled!)
}

TEST(MipsEmulatorTest, VariableShiftsAndNor) {
  MipsEmulator emu;
  // 00: addiu $a0, $zero, 1
  // 04: addiu $a1, $zero, 4
  // 08: sllv  $t0, $a0, $a1   (1 << 4 = 16)
  // 0C: srlv  $t1, $t0, $a1   (16 >> 4 = 1)
  // 10: nor   $v0, $a0, $zero (~1 = 0xFFFFFFFE)
  // 14: jr    $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x24040001,  // addiu $a0, $zero, 1
      0x24050004,  // addiu $a1, $zero, 4
      0x00A44004,  // sllv  $t0, $a0, $a1
      0x00A84846,  // srlv  $t1, $t0, $a1
      0x00801027,  // nor   $v0, $a0, $zero
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetRegister(Register::kT0), 16u);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 1u);
  EXPECT_EQ(res.v0, 0xFFFFFFFEu);
}

TEST(MipsEmulatorTest, DivisionByZeroSafe) {
  MipsEmulator emu;
  // 00: addiu $a0, $zero, 10
  // 04: div   $a0, $zero  (division by zero should not crash process)
  // 08: divu  $a0, $zero
  // 0C: jr    $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x2404000A,  // addiu $a0, $zero, 10
      0x0080001A,  // div   $a0, $zero
      0x0080001B,  // divu  $a0, $zero
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
}

TEST(MipsEmulatorTest, JalrIndirectCallAndLink) {
  MipsEmulator emu;
  // Non-leaf function saving $ra, calling subroutine via jalr, and returning:
  // 00: lui   $t0, 0x8000
  // 04: ori   $t0, $t0, 0x101C (subroutine at 0x8000101C)
  // 08: sw    $ra, 0($sp)      (save top-level return sentinel)
  // 0C: jalr  $ra, $t0         (call subroutine, link return address 0x1014 into $ra)
  // 10: addiu $a0, $zero, 25   (delay slot: arg = 25)
  // 14: lw    $ra, 0($sp)      (restore top-level return sentinel)
  // 18: jr    $ra              (return to top-level caller)
  // 1C: addu  $v0, $a0, $a0    (subroutine: v0 = 25 + 25 = 50)
  // 20: jr    $ra              (subroutine returns to caller at 0x1014)
  // 24: nop
  std::vector<uint32_t> code = {
      0x3C088000,  // lui   $t0, 0x8000
      0x3508101C,  // ori   $t0, $t0, 0x101C
      0xAFBF0000,  // sw    $ra, 0($sp)
      0x0100F809,  // jalr  $ra, $t0
      0x24040019,  // addiu $a0, $zero, 25
      0x8FBF0000,  // lw    $ra, 0($sp)
      0x03E00008,  // jr    $ra
      0x00841021,  // addu  $v0, $a0, $a0
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  emu.SetRegister(Register::kSp, 0x80101000);  // Initialize stack pointer
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 50u);
}

TEST(MipsEmulatorTest, CalleeSavedPreservationAndStackFrame) {
  MipsEmulator emu;
  // Simulate full C prologue / epilogue saving and restoring $s0-$s1:
  // 00: addiu $sp, $sp, -32
  // 04: sw    $s0, 24($sp)
  // 08: sw    $s1, 20($sp)
  // 0C: addiu $s0, $zero, 0xAAAA (scratch work)
  // 10: addiu $s1, $zero, 0xBBBB
  // 14: addu  $v0, $s0, $s1
  // 18: lw    $s1, 20($sp)
  // 1C: lw    $s0, 24($sp)
  // 20: jr    $ra
  // 24: addiu $sp, $sp, 32
  std::vector<uint32_t> code = {
      0x27BDFFE0,  // addiu $sp, $sp, -32
      0xAFB00018,  // sw    $s0, 24($sp)
      0xAFB10014,  // sw    $s1, 20($sp)
      0x2410AAAA,  // addiu $s0, $zero, 0xAAAA
      0x2411BBBB,  // addiu $s1, $zero, 0xBBBB
      0x02111021,  // addu  $v0, $s0, $s1
      0x8FB10014,  // lw    $s1, 20($sp)
      0x8FB00018,  // lw    $s0, 24($sp)
      0x03E00008,  // jr    $ra
      0x27BD0020,  // addiu $sp, $sp, 32
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));

  // Initialize caller state
  emu.SetRegister(Register::kSp, 0x80700000);
  emu.SetRegister(Register::kS0, 0x12345678);
  emu.SetRegister(Register::kS1, 0x9ABCDEF0);
  CalleeSavedRegisters pre_call = emu.GetCalleeSavedRegisters();

  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, static_cast<uint32_t>(0xFFFF6665));  // 0xFFFF... + 0xFFFF...

  CalleeSavedRegisters post_call = emu.GetCalleeSavedRegisters();
  EXPECT_EQ(pre_call, post_call);
  EXPECT_EQ(emu.GetRegister(Register::kS0), 0x12345678u);
  EXPECT_EQ(emu.GetRegister(Register::kS1), 0x9ABCDEF0u);
  EXPECT_EQ(emu.GetRegister(Register::kSp), 0x80700000u);
}

TEST(MipsEmulatorTest, UnalignedLwlAndLwr) {
  MipsEmulator emu;
  uint32_t ram = 0x80100000;
  // Write two words to RAM:
  // [0x80100000] = 0x11223344
  // [0x80100004] = 0x55667788
  ASSERT_TRUE(emu.Write32(ram, 0x11223344));
  ASSERT_TRUE(emu.Write32(ram + 4, 0x55667788));

  // 00: lui $a0, 0x8010
  // 04: lwl $v0, 1($a0)  (loads 22, 33, 44 into top 24 bits)
  // 08: lwr $v0, 4($a0)  (loads 55 into bottom 8 bits)
  // 0C: jr  $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui $a0, 0x8010
      0x88820001,  // lwl $v0, 1($a0)
      0x98820004,  // lwr $v0, 4($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0x22334455u);
}

TEST(MipsEmulatorTest, UnalignedSwlAndSwr) {
  MipsEmulator emu;
  uint32_t ram = 0x80100000;
  ASSERT_TRUE(emu.Write32(ram, 0x11223344));
  ASSERT_TRUE(emu.Write32(ram + 4, 0x55667788));
  emu.ClearWriteLog();

  // 00: lui $a0, 0x8010
  // 04: lui $t0, 0xAABB
  // 08: ori $t0, $t0, 0xCCDD  ($t0 = 0xAABBCCDD)
  // 0C: swl $t0, 1($a0)       (stores AA, BB, CC into bytes 1, 2, 3)
  // 10: swr $t0, 4($a0)       (stores DD into byte 4)
  // 14: jr  $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui $a0, 0x8010
      0x3C08AABB,  // lui $t0, 0xAABB
      0x3508CCDD,  // ori $t0, $t0, 0xCCDD
      0xA8880001,  // swl $t0, 1($a0)
      0xB8880004,  // swr $t0, 4($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);

  uint32_t word0 = 0;
  uint32_t word1 = 0;
  ASSERT_TRUE(emu.Read32(ram, &word0));
  ASSERT_TRUE(emu.Read32(ram + 4, &word1));
  EXPECT_EQ(word0, 0x11AABBCCu);
  EXPECT_EQ(word1, 0xDD667788u);
}

TEST(MipsEmulatorTest, SignedIntegerOverflowAddAndSubTraps) {
  MipsEmulator emu;
  // 00: lui  $a0, 0x7FFF
  // 04: ori  $a0, $a0, 0xFFFF (0x7FFFFFFF = INT32_MAX)
  // 08: addi $t0, $a0, 1      (overflows! traps!)
  // 0C: jr   $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x3C047FFF,  // lui  $a0, 0x7FFF
      0x3484FFFF,  // ori  $a0, $a0, 0xFFFF
      0x20880001,  // addi $t0, $a0, 1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kIntegerOverflow);
}

TEST(MipsEmulatorTest, UnsignedWraparoundNeverTraps) {
  MipsEmulator emu;
  // 00: addiu $a0, $zero, -1  (0xFFFFFFFF)
  // 04: addiu $v0, $a0, 1     (0xFFFFFFFF + 1 = 0x00000000, wraps with NO trap!)
  // 08: subu  $v1, $v0, 1     (0x00000000 - 1 = 0xFFFFFFFF, wraps with NO trap!)
  // 0C: jr    $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x2404FFFF,  // addiu $a0, $zero, -1
      0x24820001,  // addiu $v0, $a0, 1
      0x2443FFFF,  // addiu $v1, $v0, -1
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0u);
  EXPECT_EQ(res.v1, 0xFFFFFFFFu);
}

TEST(MipsEmulatorTest, DivisionSignedOverflowMinIntNoCrash) {
  MipsEmulator emu;
  // INT32_MIN / -1 must not crash host with SIGFPE
  // 00: lui  $a0, 0x8000      (0x80000000 = INT32_MIN)
  // 04: addiu $a1, $zero, -1  (0xFFFFFFFF = -1)
  // 08: div  $a0, $a1         (0x80000000 / -1)
  // 0C: mflo $v0              (v0 = 0x80000000)
  // 10: mfhi $v1              (v1 = 0)
  // 14: jr   $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x3C048000,  // lui  $a0, 0x8000
      0x2405FFFF,  // addiu $a1, $zero, -1
      0x0085001A,  // div  $a0, $a1
      0x00001012,  // mflo $v0
      0x00001810,  // mfhi $v1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0x80000000u);
  EXPECT_EQ(res.v1, 0u);
}

TEST(MipsEmulatorTest, MisalignedWordLoadFaults) {
  MipsEmulator emu;
  // Standard lw at misaligned address 0x80100001 must fault
  // 00: lui $a0, 0x8010
  // 04: lw  $v0, 1($a0)
  // 08: jr  $ra
  // 0C: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui $a0, 0x8010
      0x8C820001,  // lw  $v0, 1($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kMemoryFault);
}

TEST(MipsEmulatorTest, SignVsZeroExtensionLoads) {
  MipsEmulator emu;
  uint32_t ram = 0x80100000;
  // Write 0x80 to byte 0 and 0x8000 to halfword 2:
  // [0x80100000] = 0x80008000
  ASSERT_TRUE(emu.Write32(ram, 0x80008000));

  // 00: lui $a0, 0x8010
  // 04: lb  $t0, 0($a0)  (0x80 -> sign-extended to 0xFFFFFF80)
  // 08: lbu $t1, 0($a0)  (0x80 -> zero-extended to 0x00000080)
  // 0C: lh  $t2, 2($a0)  (0x8000 -> sign-extended to 0xFFFF8000)
  // 10: lhu $t3, 2($a0)  (0x8000 -> zero-extended to 0x00008000)
  // 14: jr  $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui $a0, 0x8010
      0x80880000,  // lb  $t0, 0($a0)
      0x90890000,  // lbu $t1, 0($a0)
      0x848A0002,  // lh  $t2, 2($a0)
      0x948B0002,  // lhu $t3, 2($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetRegister(Register::kT0), 0xFFFFFF80u);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 0x00000080u);
  EXPECT_EQ(emu.GetRegister(Register::kT2), 0xFFFF8000u);
  EXPECT_EQ(emu.GetRegister(Register::kT3), 0x00008000u);
}

TEST(MipsEmulatorTest, BreakAndSyscallTraps) {
  MipsEmulator emu;
  // 00: break
  std::vector<uint32_t> break_code = {0x0000000D};
  ASSERT_TRUE(emu.LoadWords(0x80001000, break_code));
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kBreakTrap);

  // 00: syscall
  std::vector<uint32_t> syscall_code = {0x0000000C};
  ASSERT_TRUE(emu.LoadWords(0x80001000, syscall_code));
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kSyscallTrap);

  // sync instruction should execute as a no-op
  // 00: sync
  // 04: jr   $ra
  // 08: nop
  std::vector<uint32_t> sync_code = {0x0000000F, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, sync_code));
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
}

TEST(MipsEmulatorTest, BranchInBranchDelaySlotIllegal) {
  MipsEmulator emu;
  // Branch placed directly inside delay slot of another branch:
  // 00: beq  $zero, $zero, +2
  // 04: beq  $zero, $zero, +2  (ILLEGAL: branch in delay slot)
  // 08: nop
  // 0C: jr   $ra
  // 10: nop
  std::vector<uint32_t> illegal_code = {
      0x10000002,  // beq $zero, $zero, +2
      0x10000002,  // beq $zero, $zero, +2 (in delay slot)
      0x00000000,  // nop
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, illegal_code));
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kInvalidOpcode);
}

TEST(MipsEmulatorTest, FpuDataMovementAndArithmetic) {
  MipsEmulator emu;
  // $a0 = 4.0f (0x40800000)
  // $a1 = 2.0f (0x40000000)
  // 00: mtc1    $a0, $f0
  // 04: mtc1    $a1, $f1
  // 08: add.s   $f2, $f0, $f1   (4.0 + 2.0 = 6.0)
  // 0C: sub.s   $f3, $f0, $f1   (4.0 - 2.0 = 2.0)
  // 10: mul.s   $f4, $f0, $f1   (4.0 * 2.0 = 8.0)
  // 14: div.s   $f5, $f0, $f1   (4.0 / 2.0 = 2.0)
  // 18: sqrt.s  $f6, $f0        (sqrt(4.0) = 2.0)
  // 1C: neg.s   $f7, $f0        (-4.0)
  // 20: abs.s   $f8, $f7        (abs(-4.0) = 4.0)
  // 24: mov.s   $f9, $f0        (4.0)
  // 28: mfc1    $v0, $f2        (move 6.0f bits to $v0)
  // 2C: jr      $ra
  // 30: nop
  std::vector<uint32_t> code = {
      0x44840000,  // mtc1   $a0, $f0
      0x44850800,  // mtc1   $a1, $f1
      0x46010080,  // add.s  $f2, $f0, $f1
      0x460100C1,  // sub.s  $f3, $f0, $f1
      0x46010102,  // mul.s  $f4, $f0, $f1
      0x46010143,  // div.s  $f5, $f0, $f1
      0x46000184,  // sqrt.s $f6, $f0
      0x460001C7,  // neg.s  $f7, $f0
      0x46003A05,  // abs.s  $f8, $f7
      0x46000246,  // mov.s  $f9, $f0
      0x44021000,  // mfc1   $v0, $f2
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 0x40800000);  // 4.0f
  emu.SetRegister(Register::kA1, 0x40000000);  // 2.0f

  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF2), 6.0f);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF3), 2.0f);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF4), 8.0f);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF5), 2.0f);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF6), 2.0f);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF7), -4.0f);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF8), 4.0f);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF9), 4.0f);
  EXPECT_EQ(res.v0, 0x40C00000u);  // 6.0f IEEE 754 bits
}

TEST(MipsEmulatorTest, FpuConversions) {
  MipsEmulator emu;
  // 00: mtc1       $a0, $f0    ($a0 = 42 integer)
  // 04: cvt.s.w    $f1, $f0    (convert 42 -> 42.0f)
  // 08: mtc1       $a1, $f2    ($a1 = 42.9f bits: 0x422B999A)
  // 0C: trunc.w.s  $f3, $f2    (truncate 42.9f -> 42 integer)
  // 10: mfc1       $v0, $f3    (move integer 42 to $v0)
  // 14: jr         $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x44840000,  // mtc1       $a0, $f0
      0x46800060,  // cvt.s.w    $f1, $f0
      0x44851000,  // mtc1       $a1, $f2
      0x460010CD,  // trunc.w.s  $f3, $f2
      0x44021800,  // mfc1       $v0, $f3
      0x03E00008,  // jr         $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 42);          // integer 42
  emu.SetRegister(Register::kA1, 0x422B999A);  // 42.9f

  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF1), 42.0f);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF3), 42u);
  EXPECT_EQ(res.v0, 42u);
}

TEST(MipsEmulatorTest, FpuComparisonsAndBranches) {
  MipsEmulator emu;
  // 00: mtc1    $a0, $f0     ($f0 = 2.0f)
  // 04: mtc1    $a1, $f1     ($f1 = 4.0f)
  // 08: c.lt.s  $f0, $f1     (2.0 < 4.0 -> true)
  // 0C: bc1t    .Ltaken      (branch taken)
  // 10: addiu   $v0, $zero, 1 (delay slot: v0 = 1)
  // 14: addiu   $v0, $v0, 100 (skipped)
  // .Ltaken (18):
  // 18: c.eq.s  $f0, $f1     (2.0 == 4.0 -> false)
  // 1C: bc1f    .Ldone       (branch taken because condition is false)
  // 20: addiu   $v0, $v0, 2  (delay slot: v0 += 2 -> v0 = 3)
  // 24: addiu   $v0, $v0, 200 (skipped)
  // .Ldone (28):
  // 28: jr      $ra
  // 2C: nop
  std::vector<uint32_t> code = {
      0x44840000,  // mtc1   $a0, $f0
      0x44850800,  // mtc1   $a1, $f1
      0x4601003C,  // c.lt.s $f0, $f1
      0x45010002,  // bc1t   +2 (target: 0x80001018)
      0x24020001,  // addiu  $v0, $zero, 1
      0x24420064,  // addiu  $v0, $v0, 100 (skipped)
      0x46010032,  // c.eq.s $f0, $f1
      0x45000002,  // bc1f   +2 (target: 0x80001028)
      0x24420002,  // addiu  $v0, $v0, 2
      0x244200C8,  // addiu  $v0, $v0, 200 (skipped)
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 0x40000000);  // 2.0f
  emu.SetRegister(Register::kA1, 0x40800000);  // 4.0f

  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 3u);  // 1 + 2
}

TEST(MipsEmulatorTest, FpuLoadsAndStores) {
  MipsEmulator emu;
  uint32_t ram = 0x80100000;
  // Write 3.14159f (0x40490FD0) into RAM
  ASSERT_TRUE(emu.Write32(ram, 0x40490FD0));

  // 00: lui     $a0, 0x8010
  // 04: lwc1    $f0, 0($a0)
  // 08: swc1    $f0, 4($a0)
  // 0C: jr      $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0xC4800000,  // lwc1  $f0, 0($a0)
      0xE4800004,  // swc1  $f0, 4($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_FLOAT_EQ(emu.GetFpRegister(FpRegister::kF0), 3.14159f);

  uint32_t copied = 0;
  ASSERT_TRUE(emu.Read32(ram + 4, &copied));
  EXPECT_EQ(copied, 0x40490FD0u);

  // Misaligned lwc1 must fault
  std::vector<uint32_t> misaligned_code = {
      0x3C048010,  // lui   $a0, 0x8010
      0xC4800001,  // lwc1  $f0, 1($a0) (unaligned!)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80002000, misaligned_code));
  EXPECT_EQ(emu.RunFunction(0x80002000).status, ExecutionStatus::kMemoryFault);
}

TEST(MipsEmulatorTest, RunFunctionReturnsF0Float) {
  MipsEmulator emu;
  // Function returning 123.456f in $f0
  // 00: lui   $a0, 0x42F6
  // 04: ori   $a0, $a0, 0xE979  (0x42F6E979 = 123.456f)
  // 08: mtc1  $a0, $f0
  // 0C: jr    $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x3C0442F6,  // lui   $a0, 0x42F6
      0x3484E979,  // ori   $a0, $a0, 0xE979
      0x44840000,  // mtc1  $a0, $f0
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_FLOAT_EQ(res.f0, 123.456f);
  EXPECT_EQ(res.f0_bits, 0x42F6E979u);
}

TEST(MipsEmulatorTest, BltzalAndBgezalBranchAndLink) {
  MipsEmulator emu;
  // Test 1: bltzal taken (a0 = -5)
  // 00: bltzal  $a0, +2          (taken to 0x1010, links ra = 0x1008)
  // 04: addiu   $v0, $zero, 1    (delay slot: v0 = 1)
  // 08: addiu   $v0, $v0, 100    (skipped)
  // 0C: addiu   $v0, $v0, 100    (skipped)
  // 10: jr      $ra              (callee returns to 0x1008)
  // 14: nop
  std::vector<uint32_t> code = {
      0x04900002,  // bltzal $a0, +2 (target: 0x80001010)
      0x24020001,  // addiu  $v0, $zero, 1
      0x24420064,  // addiu  $v0, $v0, 100
      0x24420064,  // addiu  $v0, $v0, 100
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetPc(0x80001000);
  emu.SetRegister(Register::kA0, static_cast<uint32_t>(-5));
  // Call Step:
  // Step 1: executes bltzal, sets $ra = 0x80001008
  EXPECT_EQ(emu.Step(), ExecutionStatus::kRunning);
  EXPECT_EQ(emu.GetRegister(Register::kRa), 0x80001008u);

  // Test 2: bltzal NOT taken (a0 = 5) still unconditionally updates $ra = PC + 8
  MipsEmulator emu2;
  ASSERT_TRUE(emu2.LoadWords(0x80001000, code));
  emu2.SetPc(0x80001000);
  emu2.SetRegister(Register::kA0, 5);
  EXPECT_EQ(emu2.Step(), ExecutionStatus::kRunning);
  EXPECT_EQ(emu2.GetRegister(Register::kRa), 0x80001008u);  // Unconditionally linked!

  // Test 3: bltzal with rs == 31 is architecturally illegal
  MipsEmulator emu3;
  std::vector<uint32_t> illegal_code = {0x07F00002};  // bltzal $ra, +2
  ASSERT_TRUE(emu3.LoadWords(0x80001000, illegal_code));
  emu3.SetPc(0x80001000);
  EXPECT_EQ(emu3.Step(), ExecutionStatus::kInvalidOpcode);
}

TEST(MipsEmulatorTest, UnalignedPcInstructionFetchFaults) {
  MipsEmulator emu;
  // Jump to misaligned address 0x80001005:
  // 00: lui   $t0, 0x8000
  // 04: ori   $t0, $t0, 0x1005 (unaligned target)
  // 08: jr    $t0
  // 0C: addiu $v0, $zero, 42   (delay slot executes normally)
  std::vector<uint32_t> code = {
      0x3C088000,  // lui   $t0, 0x8000
      0x35081005,  // ori   $t0, $t0, 0x1005
      0x01000008,  // jr    $t0
      0x2402002A,  // addiu $v0, $zero, 42 (delay slot)
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  // Hardware executes delay slot ($v0 becomes 42) then raises AdEL on fetch
  EXPECT_EQ(res.status, ExecutionStatus::kMemoryFault);
  EXPECT_EQ(emu.GetRegister(Register::kV0), 42u);
}

TEST(MipsEmulatorTest, NullPointerDereferenceFaults) {
  MipsEmulator emu;
  // 00: lw    $v0, 0($zero) (NULL pointer dereference)
  // 04: jr    $ra
  // 08: nop
  std::vector<uint32_t> code = {
      0x8C020000,  // lw $v0, 0($zero)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kMemoryFault);
}

TEST(MipsEmulatorTest, Ldc1AndSdc1EvenRegisterPairing) {
  MipsEmulator emu;
  // Initialize double 3.141592653589793 into $f20 / $f21
  double pi = 3.141592653589793;
  emu.SetFpDouble(FpRegister::kF20, pi);
  EXPECT_DOUBLE_EQ(emu.GetFpDouble(FpRegister::kF20), pi);

  // Address 0x80100000 is 8-byte aligned
  // 00: sdc1  $f20, 0($a0)   (save $f20/$f21 to 0x80100000)
  // 04: ldc1  $f22, 0($a0)   (load into $f22/$f23 from 0x80100000)
  // 08: jr    $ra
  // 0C: nop
  std::vector<uint32_t> code = {
      0xF4940000,  // sdc1 $f20, 0($a0)
      0xD4960000,  // ldc1 $f22, 0($a0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 0x80100000);
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_DOUBLE_EQ(emu.GetFpDouble(FpRegister::kF22), pi);
  EXPECT_EQ(emu.GetFpDoubleBits(FpRegister::kF22), emu.GetFpDoubleBits(FpRegister::kF20));
  // Verify 8-byte write logged
  ASSERT_EQ(res.write_log.size(), 1u);
  EXPECT_EQ(res.write_log[0].size, 8);
  EXPECT_EQ(res.write_log[0].address, 0x80100000u);
}

TEST(MipsEmulatorTest, Ldc1AndSdc1MisalignedFaults) {
  MipsEmulator emu;
  // Address 0x80100004 is 4-byte aligned, but NOT 8-byte aligned for ldc1
  // 00: ldc1  $f2, 4($a0)
  // 04: jr    $ra
  // 08: nop
  std::vector<uint32_t> code = {
      0xD4820004,  // ldc1 $f2, 4($a0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 0x80100000);
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kMemoryFault);
}

TEST(MipsEmulatorTest, Ldc1OddRegisterFails) {
  MipsEmulator emu;
  // ldc1 with odd register $f1 is architecturally invalid under Status.FR=0
  // 00: ldc1  $f1, 0($a0)
  // 04: jr    $ra
  // 08: nop
  std::vector<uint32_t> code = {
      0xD4810000,  // ldc1 $f1, 0($a0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 0x80100000);
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kInvalidOpcode);
}

TEST(MipsEmulatorTest, DoublePrecisionArithmetic) {
  MipsEmulator emu;
  emu.SetFpDouble(FpRegister::kF2, 10.5);
  emu.SetFpDouble(FpRegister::kF4, 2.5);

  // 00: add.d  $f0, $f2, $f4
  // 04: jr     $ra
  // 08: nop
  std::vector<uint32_t> code_add = {
      0x46241000,  // add.d $f0, $f2, $f4
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_add));
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_DOUBLE_EQ(res.f0_double, 13.0);
  EXPECT_DOUBLE_EQ(emu.GetFpDouble(FpRegister::kF0), 13.0);

  // sub.d
  std::vector<uint32_t> code_sub = {0x46241001, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_sub));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, 8.0);

  // mul.d
  std::vector<uint32_t> code_mul = {0x46241002, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_mul));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, 26.25);

  // div.d
  std::vector<uint32_t> code_div = {0x46241003, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_div));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, 4.2);

  // sqrt.d
  emu.SetFpDouble(FpRegister::kF2, 64.0);
  std::vector<uint32_t> code_sqrt = {0x46201004, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_sqrt));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, 8.0);

  // abs.d
  emu.SetFpDouble(FpRegister::kF2, -42.75);
  std::vector<uint32_t> code_abs = {0x46201005, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_abs));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, 42.75);

  // mov.d
  emu.SetFpDouble(FpRegister::kF2, 123.456);
  std::vector<uint32_t> code_mov = {0x46201006, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_mov));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, 123.456);

  // neg.d
  std::vector<uint32_t> code_neg = {0x46201007, 0x03E00008, 0x00000000};
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_neg));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, -123.456);
}

TEST(MipsEmulatorTest, DoublePrecisionConversions) {
  MipsEmulator emu;

  // cvt.s.d $f0, $f2 (double to single)
  emu.SetFpDouble(FpRegister::kF2, 3.141592653589793);
  std::vector<uint32_t> code_cvt_sd = {
      0x46201020,  // cvt.s.d $f0, $f2
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_cvt_sd));
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_FLOAT_EQ(res.f0, 3.1415927f);

  // cvt.d.s $f0, $f2 (single to double)
  emu.SetFpRegister(FpRegister::kF2, 2.5f);
  std::vector<uint32_t> code_cvt_ds = {
      0x46001021,  // cvt.d.s $f0, $f2
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_cvt_ds));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, 2.5);

  // cvt.d.w $f0, $f2 (32-bit int in $f2 to double)
  emu.SetFpBits(FpRegister::kF2, static_cast<uint32_t>(-500));
  std::vector<uint32_t> code_cvt_dw = {
      0x46801021,  // cvt.d.w $f0, $f2
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_cvt_dw));
  res = emu.RunFunction(0x80001000);
  EXPECT_DOUBLE_EQ(res.f0_double, -500.0);

  // trunc.w.d $f0, $f2 (double to 32-bit int)
  emu.SetFpDouble(FpRegister::kF2, 123.85);
  std::vector<uint32_t> code_trunc = {
      0x4620100D,  // trunc.w.d $f0, $f2
      0x03E00008,  // jr        $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_trunc));
  res = emu.RunFunction(0x80001000);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 123u);

  // trunc.w.d with negative value
  emu.SetFpDouble(FpRegister::kF2, -456.9);
  res = emu.RunFunction(0x80001000);
  EXPECT_EQ(static_cast<int32_t>(emu.GetFpBits(FpRegister::kF0)), -456);

  // trunc.w.d with NaN clamps to 0x7FFFFFFF
  emu.SetFpDouble(FpRegister::kF2, std::numeric_limits<double>::quiet_NaN());
  res = emu.RunFunction(0x80001000);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 0x7FFFFFFFu);

  // trunc.w.d with overflow clamps
  emu.SetFpDouble(FpRegister::kF2, 1e20);
  res = emu.RunFunction(0x80001000);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 0x7FFFFFFFu);

  emu.SetFpDouble(FpRegister::kF2, -1e20);
  res = emu.RunFunction(0x80001000);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 0x80000000u);
}

TEST(MipsEmulatorTest, DoublePrecisionComparisonsAndBranches) {
  MipsEmulator emu;
  emu.SetFpDouble(FpRegister::kF2, 10.0);
  emu.SetFpDouble(FpRegister::kF4, 20.0);

  // 00: c.lt.d $f2, $f4   (10.0 < 20.0 -> true)
  // 04: bc1t   target (+2 instructions -> 0x10)
  // 08: addiu  $v0, $zero, 1   (delay slot executed: v0 = 1)
  // 0C: addiu  $v0, $zero, 2   (skipped)
  // 10: jr     $ra
  // 14: nop
  std::vector<uint32_t> code = {
      0x4624103C,  // c.lt.d $f2, $f4
      0x45010002,  // bc1t   +2
      0x24020001,  // addiu  $v0, $zero, 1
      0x24020002,  // addiu  $v0, $zero, 2
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_TRUE(emu.GetFpuCondition());
  EXPECT_EQ(res.v0, 1u);

  // Test c.eq.d (10.0 == 20.0 -> false)
  std::vector<uint32_t> code_eq = {
      0x46241032,  // c.eq.d $f2, $f4
      0x45000002,  // bc1f   +2 (taken because false)
      0x24020005,  // addiu  $v0, $zero, 5 (delay slot)
      0x24020006,  // addiu  $v0, $zero, 6 (skipped)
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_eq));
  res = emu.RunFunction(0x80001000);
  EXPECT_FALSE(emu.GetFpuCondition());
  EXPECT_EQ(res.v0, 5u);

  // Test c.le.d (10.0 <= 20.0 -> true)
  std::vector<uint32_t> code_le = {
      0x4624103E,  // c.le.d $f2, $f4
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_le));
  res = emu.RunFunction(0x80001000);
  EXPECT_TRUE(emu.GetFpuCondition());
}

TEST(MipsEmulatorTest, DoublePrecisionOddRegistersRaiseInvalidOpcode) {
  MipsEmulator emu;

  // add.d with odd fd ($f1): 0x46240840
  // add.d $f1, $f2, $f4 -> fd=1 (odd)
  std::vector<uint32_t> code_odd_fd = {
      0x46241040,  // add.d $f1, $f2, $f4
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_odd_fd));
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kInvalidOpcode);

  // cvt.s.d with odd fs ($f1):
  // cvt.s.d $f0, $f1 -> fs=1 (odd)
  std::vector<uint32_t> code_odd_fs = {
      0x46200820,  // cvt.s.d $f0, $f1
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_odd_fs));
  res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kInvalidOpcode);

  // c.eq.d with odd ft ($f3):
  std::vector<uint32_t> code_odd_ft = {
      0x46231032,  // c.eq.d $f2, $f3
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code_odd_ft));
  res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kInvalidOpcode);
}

TEST(MipsEmulatorTest, JalExternalCallInterception) {
  MipsEmulator emu;
  emu.SetRegister(Register::kSp, 0x801F0000);
  emu.SetInterceptExternalCalls(true);
  emu.SetCodeBounds(0x80001000, 0x80001028);

  std::vector<uint32_t> code = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0010,  // 04: sw    $ra, 16($sp)
      0x2404002A,  // 08: addiu $a0, $zero, 42
      0x0C014000,  // 0C: jal   0x80050000 (external call)
      0x24050063,  // 10: addiu $a1, $zero, 99 (delay slot)
      0x00451021,  // 14: addu  $v0, $v0, $a1 (v0 was 0, so v0 becomes 99)
      0x8FBF0010,  // 18: lw    $ra, 16($sp)
      0x27BD0018,  // 1C: addiu $sp, $sp, 24
      0x03E00008,  // 20: jr    $ra
      0x00000000,  // 24: nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 99u);
  ASSERT_EQ(res.call_log.size(), 1u);
  EXPECT_EQ(res.call_log[0].target_address, 0x80050000u);
  EXPECT_EQ(res.call_log[0].a0, 42u);
  EXPECT_EQ(res.call_log[0].a1, 99u);
}

TEST(MipsEmulatorTest, JalrExternalCallInterception) {
  MipsEmulator emu;
  emu.SetRegister(Register::kSp, 0x801F0000);
  emu.SetInterceptExternalCalls(true);
  emu.SetCodeBounds(0x80001000, 0x80001028);

  std::vector<uint32_t> code = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0010,  // 04: sw    $ra, 16($sp)
      0x3C088006,  // 08: lui   $t0, 0x8006
      0x2404007B,  // 0C: addiu $a0, $zero, 123
      0x0100F809,  // 10: jalr  $t0 (external call to 0x80060000)
      0x240501C8,  // 14: addiu $a1, $zero, 456 (delay slot)
      0x8FBF0010,  // 18: lw    $ra, 16($sp)
      0x27BD0018,  // 1C: addiu $sp, $sp, 24
      0x03E00008,  // 20: jr    $ra
      0x00000000,  // 24: nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  ASSERT_EQ(res.call_log.size(), 1u);
  EXPECT_EQ(res.call_log[0].target_address, 0x80060000u);
  EXPECT_EQ(res.call_log[0].a0, 123u);
  EXPECT_EQ(res.call_log[0].a1, 456u);
}

TEST(MipsEmulatorTest, JalInternalCallNotIntercepted) {
  MipsEmulator emu;
  emu.SetRegister(Register::kSp, 0x801F0000);
  emu.SetInterceptExternalCalls(true);
  emu.SetCodeBounds(0x80001000, 0x80001028);

  std::vector<uint32_t> code = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0010,  // 04: sw    $ra, 16($sp)
      0x0C000407,  // 08: jal   0x8000101C (internal call to local helper)
      0x2404000A,  // 0C: addiu $a0, $zero, 10
      0x8FBF0010,  // 10: lw    $ra, 16($sp)
      0x27BD0018,  // 14: addiu $sp, $sp, 24
      0x03E00008,  // 18: jr    $ra
      0x24820005,  // 1C: addiu $v0, $a0, 5 (helper: returns a0 + 5)
      0x03E00008,  // 20: jr    $ra
      0x00000000,  // 24: nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);

  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 15u);
  EXPECT_TRUE(res.call_log.empty());
}

TEST(MipsEmulatorTest, MmioRegisterReadAndWriteMocking) {
  MipsEmulator emu;

  // SP_STATUS (0xA4040010): reading returns 1 (halted)
  uint32_t sp_status = 0;
  ASSERT_TRUE(emu.Read32(0xA4040010, &sp_status));
  EXPECT_EQ(sp_status, 1u);

  // MI_VERSION (0xA4300004): reading returns 0x02020102
  uint32_t mi_version = 0;
  ASSERT_TRUE(emu.Read32(0xA4300004, &mi_version));
  EXPECT_EQ(mi_version, 0x02020102u);

  // VI_CURRENT (0xA4400010): scanline advances on each read
  uint32_t vi_line0 = 0;
  uint32_t vi_line1 = 0;
  ASSERT_TRUE(emu.Read32(0xA4400010, &vi_line0));
  ASSERT_TRUE(emu.Read32(0xA4400010, &vi_line1));
  EXPECT_NE(vi_line0, vi_line1);

  // Writing to MMIO register does not fault and reads back
  ASSERT_TRUE(emu.Write32(0xA4600000, 0x12345678));
  uint32_t pi_dram = 0;
  ASSERT_TRUE(emu.Read32(0xA4600000, &pi_dram));
  EXPECT_EQ(pi_dram, 0x12345678u);
}

TEST(MipsEmulatorTest, MmioPollingLoopTerminatesOnStatus) {
  // Loop: polls SP_STATUS (0xA4040010) waiting for halt bit (bit 0)
  // 00: lui  $t0, 0xA404
  // 04: lw   $v0, 16($t0)      ; read SP_STATUS (0xA4040010)
  // 08: andi $v0, $v0, 1       ; check halt bit
  // 0C: beqz $v0, -3 (0x80001004) ; wait while not halted
  // 10: nop
  // 14: jr   $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x3C08A404,  // lui  $t0, 0xA404
      0x8D020010,  // lw   $v0, 16($t0)
      0x30420001,  // andi $v0, $v0, 1
      0x1040FFFD,  // beqz $v0, -3 (0x80001004)
      0x00000000,  // nop
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  MipsEmulator emu;
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 1u);
  EXPECT_LT(res.total_steps, 10u);
}

TEST(MipsEmulatorTest, FloatingPointControlRegistersCfc1Ctc1) {
  // Test CFC1 / CTC1 with FCR31 and FCR0:
  // 00: cfc1  $v0, $0      (read FCR0 -> 0x00000B00)
  // 04: addiu $t0, $zero, 1
  // 08: sll   $t0, $t0, 23 (bit 23 = FPU condition)
  // 0C: ctc1  $t0, $31     (write FCR31 with condition bit set)
  // 10: cfc1  $v1, $31     (read FCR31)
  // 14: jr    $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x44420000,  // cfc1  $v0, $0
      0x24080001,  // addiu $t0, $zero, 1
      0x000845C0,  // sll   $t0, $t0, 23
      0x44C8F800,  // ctc1  $t0, $31
      0x4443F800,  // cfc1  $v1, $31
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  MipsEmulator emu;
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0x00000B00u);
  EXPECT_EQ(res.v1, (1u << 23));
  EXPECT_TRUE(emu.GetFpuCondition());
}

TEST(MipsEmulatorTest, ReadHookInterceptsUninitializedMemory) {
  MipsEmulator emu;
  // 00: lui  $a0, 0x8010
  // 04: lw   $v0, 0($a0)
  // 08: jr   $ra
  // 0C: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui  $a0, 0x8010
      0x8C820000,  // lw   $v0, 0($a0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };
  emu.SetReadHook([](uint32_t vram, size_t size, bool is_fp) -> std::optional<uint64_t> {
    if (vram == 0x80100000 && size == 4) {
      return 0x12345678u;
    }
    return std::nullopt;
  });
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0x12345678u);
}

TEST(MipsEmulatorTest, ReadHookDoesNotOverrideWrittenMemory) {
  MipsEmulator emu;
  // 00: lui   $a0, 0x8010
  // 04: addiu $t0, $zero, 42
  // 08: sw    $t0, 0($a0)
  // 0C: lw    $v0, 0($a0)
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x2408002A,  // addiu $t0, $zero, 42
      0xAC880000,  // sw    $t0, 0($a0)
      0x8C820000,  // lw    $v0, 0($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  emu.SetReadHook([](uint32_t vram, size_t size, bool is_fp) -> std::optional<uint64_t> {
    if (vram == 0x80100000) {
      return 0xDEADBEEFu;
    }
    return std::nullopt;
  });
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 42u);
}

TEST(MipsEmulatorTest, CacheInstructionIsNoOp) {
  MipsEmulator emu;
  // 00: cache 0x14, 0($zero)
  // 04: addiu $v0, $zero, 1
  // 08: jr    $ra
  // 0C: nop
  std::vector<uint32_t> code = {
      0xBC140000,  // cache 0x14, 0($zero)
      0x24020001,  // addiu $v0, $zero, 1
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 1u);
}

TEST(MipsEmulatorTest, LoadDoubleAndStoreDouble) {
  MipsEmulator emu;
  // 00: lui   $a0, 0x8010
  // 04: addiu $t0, $zero, 0x1234
  // 08: sll   $t0, $t0, 16
  // 0C: ori   $t0, $t0, 0x5678
  // 10: sd    $t0, 0($a0)
  // 14: ld    $v0, 0($a0)
  // 18: jr    $ra
  // 1C: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x24081234,  // addiu $t0, $zero, 0x1234
      0x00084400,  // sll   $t0, $t0, 16
      0x35085678,  // ori   $t0, $t0, 0x5678
      0xFC880000,  // sd    $t0, 0($a0)
      0xDC820000,  // ld    $v0, 0($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0x12345678u);
}

TEST(MipsEmulatorTest, StackRelativeWriteTrackingAndRollback) {
  MipsEmulator emu;
  // 00: lui   $a0, 0x8010
  // 04: addiu $t0, $zero, 111
  // 08: sw    $t0, 0($sp)       (base_reg = $sp, is_stack_relative = true)
  // 0C: addiu $t1, $zero, 222
  // 10: sw    $t1, 0($a0)       (base_reg = $a0, is_stack_relative = false)
  // 14: jr    $ra
  // 18: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x2408006F,  // addiu $t0, $zero, 111
      0xAFA80000,  // sw    $t0, 0($sp)
      0x240900DE,  // addiu $t1, $zero, 222
      0xAC890000,  // sw    $t1, 0($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  emu.SetRegister(Register::kSp, 0x80010000);
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);

  const auto& writes = emu.GetWriteLog();
  ASSERT_GE(writes.size(), 2u);
  EXPECT_TRUE(writes[0].is_stack_relative);
  EXPECT_EQ(writes[0].base_reg, 29);
  EXPECT_FALSE(writes[1].is_stack_relative);
  EXPECT_EQ(writes[1].base_reg, 4);

  // Test rollback restores previous memory
  uint32_t val = 0;
  ASSERT_TRUE(emu.Read32(0x80100000, &val));
  EXPECT_EQ(val, 222u);

  emu.RollbackWrites(code, 0x80001000);
  ASSERT_TRUE(emu.Read32(0x80100000, &val));
  EXPECT_EQ(val, 0u);
}

TEST(MipsEmulatorTest, ReadHookAllSizesAndFloatingPointDistinction) {
  MipsEmulator emu;
  // 00: lui   $a0, 0x8010
  // 04: lbu   $t0, 0($a0)      (1-byte read, is_fp = false)
  // 08: lhu   $t1, 4($a0)      (2-byte read, is_fp = false)
  // 0C: lw    $t2, 8($a0)      (4-byte read, is_fp = false)
  // 10: ld    $t3, 16($a0)     (8-byte read, is_fp = false)
  // 14: lwc1  $f0, 24($a0)     (4-byte read, is_fp = true)
  // 18: ldc1  $f2, 32($a0)     (8-byte read, is_fp = true)
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x90880000,  // lbu   $t0, 0($a0)
      0x94890004,  // lhu   $t1, 4($a0)
      0x8C8A0008,  // lw    $t2, 8($a0)
      0xDC8B0010,  // ld    $t3, 16($a0)
      0xC4800018,  // lwc1  $f0, 24($a0)
      0xD4820020,  // ldc1  $f2, 32($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  struct HookInvocation {
    uint32_t vram;
    size_t size;
    bool is_fp;
  };
  std::vector<HookInvocation> invocations;

  emu.SetReadHook([&](uint32_t vram, size_t size, bool is_fp) -> std::optional<uint64_t> {
    invocations.push_back({vram, size, is_fp});
    if (size == 1) return 0xABu;
    if (size == 2) return 0xCDEFu;
    if (size == 4) return 0x11223344u;
    if (size == 8) return 0x0123456789ABCDEFULL;
    return std::nullopt;
  });

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);

  ASSERT_EQ(invocations.size(), 6u);
  EXPECT_EQ(invocations[0].size, 1u);
  EXPECT_FALSE(invocations[0].is_fp);
  EXPECT_EQ(invocations[1].size, 2u);
  EXPECT_FALSE(invocations[1].is_fp);
  EXPECT_EQ(invocations[2].size, 4u);
  EXPECT_FALSE(invocations[2].is_fp);
  EXPECT_EQ(invocations[3].size, 8u);
  EXPECT_FALSE(invocations[3].is_fp);
  EXPECT_EQ(invocations[4].size, 4u);
  EXPECT_TRUE(invocations[4].is_fp);
  EXPECT_EQ(invocations[5].size, 8u);
  EXPECT_TRUE(invocations[5].is_fp);

  EXPECT_EQ(emu.GetRegister(Register::kT0), 0xABu);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 0xCDEFu);
  EXPECT_EQ(emu.GetRegister(Register::kT2), 0x11223344u);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 0x11223344u);
}

TEST(MipsEmulatorTest, ReadHookWasWrittenGranularByteTracking) {
  MipsEmulator emu;
  // Store a single byte at 0x80100002 (offset 2 within the 4-byte word at 0x80100000)
  // 00: lui   $a0, 0x8010
  // 04: addiu $t0, $zero, 0x77
  // 08: sb    $t0, 2($a0)
  // 0C: lw    $v0, 0($a0)
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x24080077,  // addiu $t0, $zero, 0x77
      0xA0880002,  // sb    $t0, 2($a0)
      0x8C820000,  // lw    $v0, 0($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  bool hook_called = false;
  emu.SetReadHook([&](uint32_t vram, size_t size, bool is_fp) -> std::optional<uint64_t> {
    if (vram == 0x80100000) {
      hook_called = true;
      return 0xDEADBEEFu;
    }
    return std::nullopt;
  });

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  // The hook should NOT have been called because byte 2 was written
  EXPECT_FALSE(hook_called);
  // In big-endian, byte offset 2 in a 32-bit word is (val >> 8) & 0xFF
  EXPECT_EQ(res.v0 & 0x0000FF00u, 0x00007700u);
}

TEST(MipsEmulatorTest, FloatingPointDoubleRegisterPairingAndLdc1Sdc1) {
  MipsEmulator emu;
  // 00: lui   $a0, 0x8010
  // 04: ldc1  $f2, 0($a0)
  // 08: sdc1  $f2, 8($a0)
  // 0C: jr    $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0xD4820000,  // ldc1  $f2, 0($a0)
      0xF4820008,  // sdc1  $f2, 8($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Pre-load double at 0x80100000
  double pi = 3.141592653589793;
  uint64_t pi_bits = 0;
  std::memcpy(&pi_bits, &pi, sizeof(double));
  ASSERT_TRUE(emu.Write64(0x80100000, pi_bits));

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);

  // Verify GetFpDouble
  EXPECT_DOUBLE_EQ(emu.GetFpDouble(FpRegister::kF2), pi);

  // Verify odd register ($f3) holds MSW and even register ($f2) holds LSW
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF3), static_cast<uint32_t>(pi_bits >> 32));
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF2), static_cast<uint32_t>(pi_bits & 0xFFFFFFFF));

  // Verify write to 0x80100008
  uint64_t stored_bits = 0;
  ASSERT_TRUE(emu.Read64(0x80100008, &stored_bits));
  EXPECT_EQ(stored_bits, pi_bits);
}

TEST(MipsEmulatorTest, RollbackMultipleWritesToSameAddressReverseOrder) {
  MipsEmulator emu;
  // Initialize address 0x80100000 with 0xAAAA5555
  ASSERT_TRUE(emu.Write32(0x80100000, 0xAAAA5555));
  emu.ClearWriteLog();

  // 00: lui   $a0, 0x8010
  // 04: addiu $t0, $zero, 1
  // 08: sw    $t0, 0($a0)
  // 0C: addiu $t0, $zero, 2
  // 10: sw    $t0, 0($a0)
  // 14: addiu $t0, $zero, 3
  // 18: sw    $t0, 0($a0)
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x24080001,  // addiu $t0, $zero, 1
      0xAC880000,  // sw    $t0, 0($a0)
      0x24080002,  // addiu $t0, $zero, 2
      0xAC880000,  // sw    $t0, 0($a0)
      0x24080003,  // addiu $t0, $zero, 3
      0xAC880000,  // sw    $t0, 0($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);

  uint32_t val = 0;
  ASSERT_TRUE(emu.Read32(0x80100000, &val));
  EXPECT_EQ(val, 3u);

  // Rollback writes
  emu.RollbackWrites(code, 0x80001000);
  ASSERT_TRUE(emu.Read32(0x80100000, &val));
  EXPECT_EQ(val, 0xAAAA5555u);
}

TEST(MipsEmulatorTest, StackRelativeBaseRegDetectionSpAndFp) {
  MipsEmulator emu;
  // 00: lui   $a0, 0x8010
  // 04: addiu $t0, $zero, 1
  // 08: sb    $t0, 0($sp)       (base_reg = 29, stack_relative = true)
  // 0C: sh    $t0, 4($fp)       (base_reg = 30, stack_relative = true)
  // 10: sw    $t0, 8($s0)       (base_reg = 16, stack_relative = false)
  // 14: sw    $t0, 12($gp)      (base_reg = 28, stack_relative = false)
  // 18: swc1  $f0, 16($sp)      (base_reg = 29, stack_relative = true)
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x24080001,  // addiu $t0, $zero, 1
      0xA3A80000,  // sb    $t0, 0($sp)   (op 0x28, rs=29, rt=8)
      0xA7C80004,  // sh    $t0, 4($fp)   (op 0x29, rs=30, rt=8)
      0xAE080008,  // sw    $t0, 8($s0)   (op 0x2B, rs=16, rt=8)
      0xAF88000C,  // sw    $t0, 12($gp)  (op 0x2B, rs=28, rt=8)
      0xE7A00010,  // swc1  $f0, 16($sp)  (op 0x39, rs=29, ft=0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  emu.SetRegister(Register::kSp, 0x80010000);
  emu.SetRegister(Register::kFp, 0x80010100);
  emu.SetRegister(Register::kS0, 0x80010200);
  emu.SetRegister(Register::kGp, 0x80010300);

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);

  const auto& writes = emu.GetWriteLog();
  ASSERT_EQ(writes.size(), 5u);

  // 0: sb via $sp (29)
  EXPECT_EQ(writes[0].base_reg, 29);
  EXPECT_TRUE(writes[0].is_stack_relative);

  // 1: sh via $fp (30)
  EXPECT_EQ(writes[1].base_reg, 30);
  EXPECT_TRUE(writes[1].is_stack_relative);

  // 2: sw via $s0 (16)
  EXPECT_EQ(writes[2].base_reg, 16);
  EXPECT_FALSE(writes[2].is_stack_relative);

  // 3: sw via $gp (28)
  EXPECT_EQ(writes[3].base_reg, 28);
  EXPECT_FALSE(writes[3].is_stack_relative);

  // 4: swc1 via $sp (29)
  EXPECT_EQ(writes[4].base_reg, 29);
  EXPECT_TRUE(writes[4].is_stack_relative);
}

TEST(MipsEmulatorTest, DiagnosticsReportingOnMemoryFaults) {
  MipsEmulator emu;
  // 00: lui  $a0, 0x8010
  // 04: lw   $v0, 1($a0)      (Unaligned 4-byte read at 0x80100001)
  // 08: jr   $ra
  // 0C: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui  $a0, 0x8010
      0x8C820001,  // lw   $v0, 1($a0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kMemoryFault);
  EXPECT_NE(res.error_message.find("LW unaligned addr=0x80100001"), std::string::npos);
}

TEST(MipsEmulatorTest, ExternalCallInterceptionWithArgumentsAndSentinel) {
  MipsEmulator emu;
  // Code bounds: 0x80001000 .. 0x80001040
  // 00: addiu $a0, $zero, 10
  // 04: addiu $a1, $zero, 20
  // 08: addiu $a2, $zero, 30
  // 0C: addu  $s0, $ra, $zero   (save initial $ra sentinel)
  // 10: jal   0x80005000       (outside code bounds)
  // 14: addiu $a3, $zero, 40   (delay slot sets $a3)
  // 18: addu  $v0, $v0, $a0    (resumes here after call, $v0 is 0 from call return)
  // 1C: addu  $ra, $s0, $zero   (restore initial $ra sentinel)
  // 20: jr    $ra
  // 24: nop
  std::vector<uint32_t> code = {
      0x2404000A,  // addiu $a0, $zero, 10
      0x24050014,  // addiu $a1, $zero, 20
      0x2406001E,  // addiu $a2, $zero, 30
      0x03E08021,  // addu  $s0, $ra, $zero
      0x0C001400,  // jal   0x80005000 (target: 0x80005000)
      0x24070028,  // addiu $a3, $zero, 40
      0x00441021,  // addu  $v0, $v0, $a0
      0x0200F821,  // addu  $ra, $s0, $zero
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  emu.SetInterceptExternalCalls(true);
  emu.SetCodeBounds(0x80001000, 0x80001040);

  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000, 100);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 10u);

  const auto& calls = emu.GetCallLog();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].target_address, 0x80005000u);
  EXPECT_EQ(calls[0].a0, 10u);
  EXPECT_EQ(calls[0].a1, 20u);
  EXPECT_EQ(calls[0].a2, 30u);
  EXPECT_EQ(calls[0].a3, 40u);
}

TEST(MipsEmulatorTest, MmioRejectsNonKsegAddresses) {
  MipsEmulator emu;
  uint32_t val = 0;
  // KUSEG address 0x04040010 has the same low 29 bits as SP_STATUS, but is NOT in KSEG0/KSEG1
  EXPECT_FALSE(emu.Read32(0x04040010, &val));
  EXPECT_FALSE(emu.Write32(0x04040010, 1));
  // KSEG1 address 0xA4040010 is valid MMIO
  EXPECT_TRUE(emu.Read32(0xA4040010, &val));
}

TEST(MipsEmulatorTest, Cop1RoundTiesToEvenAndCvtWAndComparisons) {
  MipsEmulator emu;
  // round.w.s $f0, $f2 (0x4600100C)
  std::vector<uint32_t> round_code = {
      0x4600100C,  // round.w.s $f0, $f2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, round_code));
  emu.SetFpRegister(FpRegister::kF2, 2.5f);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 2u);

  emu.SetFpRegister(FpRegister::kF2, 3.5f);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 4u);

  // 2147483520.0f is the largest float < 2^31; should not overflow to 0x7FFFFFFF
  emu.SetFpRegister(FpRegister::kF2, 2147483520.0f);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 2147483520u);

  // cvt.w.s $f0, $f2 (0x46001024) with FCR31 rounding mode 1 (trunc)
  std::vector<uint32_t> cvt_ws_code = {
      0x24080001,  // addiu $t0, $zero, 1 (RM = trunc)
      0x44C8F800,  // ctc1  $t0, $31
      0x46001024,  // cvt.w.s $f0, $f2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, cvt_ws_code));
  emu.SetFpRegister(FpRegister::kF2, 7.9f);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 7u);

  // C.ULT.S ($f2, $f4) -> funct 0x3D (0x4604103D)
  std::vector<uint32_t> cmp_code = {
      0x4604103D,  // c.ult.s $f2, $f4
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, cmp_code));
  emu.SetFpRegister(FpRegister::kF2, std::numeric_limits<float>::quiet_NaN());
  emu.SetFpRegister(FpRegister::kF4, 1.0f);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_TRUE(emu.GetFpuCondition());
}

TEST(MipsEmulatorTest, RollbackWritesClearsWrittenPhysAddrs) {
  MipsEmulator emu;
  emu.SetReadHook([](uint32_t vram, size_t /*size*/, bool /*is_fp*/) -> std::optional<uint64_t> {
    if (vram == 0x80100000) {
      return 0xDEADBEEFu;
    }
    return std::nullopt;
  });
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x2408002A,  // addiu $t0, $zero, 42
      0xAC880000,  // sw    $t0, 0($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  uint32_t val = 0;
  ASSERT_TRUE(emu.Read32(0x80100000, &val));
  EXPECT_EQ(val, 42u);

  emu.RollbackWrites(code, 0x80001000);
  ASSERT_TRUE(emu.Read32(0x80100000, &val));
  EXPECT_EQ(val, 0xDEADBEEFu);
}

TEST(MipsEmulatorTest, DoublewordAddAndSubImmediateAndRegister) {
  MipsEmulator emu;
  // 00: daddi  $t0, $a0, 10        (0x6088000A) -> 42 + 10 = 52
  // 04: daddiu $t1, $a1, 1         (0x64A90001) -> 0x7FFFFFFF + 1 = 0x80000000 (no trap)
  // 08: dadd   $t2, $t0, $a0       (0x0104502C) -> 52 + 42 = 94
  // 0C: daddu  $t3, $a1, $a0       (0x00A4582D) -> 0x7FFFFFFF + 42 = 0x80000029 (no trap)
  // 10: dsub   $t4, $t2, $a0       (0x0144602E) -> 94 - 42 = 52
  // 14: dsubu  $v0, $t1, $a0       (0x0124102F) -> 0x80000000 - 42 = 0x7FFFFFD6 (no trap)
  // 18: jr     $ra
  // 1C: nop
  std::vector<uint32_t> code = {
      0x6088000A,  // daddi  $t0, $a0, 10
      0x64A90001,  // daddiu $t1, $a1, 1
      0x0104502C,  // dadd   $t2, $t0, $a0
      0x00A4582D,  // daddu  $t3, $a1, $a0
      0x0144602E,  // dsub   $t4, $t2, $a0
      0x0124102F,  // dsubu  $v0, $t1, $a0
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 42);
  emu.SetRegister(Register::kA1, 0x7FFFFFFFu);
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetRegister(Register::kT0), 52u);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 0x80000000u);
  EXPECT_EQ(emu.GetRegister(Register::kT2), 94u);
  EXPECT_EQ(emu.GetRegister(Register::kT3), 0x80000029u);
  EXPECT_EQ(emu.GetRegister(Register::kT4), 52u);
  EXPECT_EQ(res.v0, 0x7FFFFFD6u);

  // Verify DADDI signed overflow trap
  std::vector<uint32_t> daddi_ovf = {
      0x60820001,  // daddi $v0, $a0, 1
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, daddi_ovf));
  emu.SetRegister(Register::kA0, 0x7FFFFFFFu);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kIntegerOverflow);

  // Verify DADD signed overflow trap
  std::vector<uint32_t> dadd_ovf = {
      0x0085102C,  // dadd $v0, $a0, $a1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, dadd_ovf));
  emu.SetRegister(Register::kA0, 0x7FFFFFFFu);
  emu.SetRegister(Register::kA1, 1u);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kIntegerOverflow);

  // Verify DSUB signed overflow trap
  std::vector<uint32_t> dsub_ovf = {
      0x0085102E,  // dsub $v0, $a0, $a1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, dsub_ovf));
  emu.SetRegister(Register::kA0, 0x80000000u);
  emu.SetRegister(Register::kA1, 1u);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kIntegerOverflow);
}

TEST(MipsEmulatorTest, DoublewordMultiplyAndDivideInstructions) {
  MipsEmulator emu;
  // Test DMULT, DMULTU, DDIV, DDIVU
  // 00: dmult  $a0, $a1  (0x0085001C)
  // 04: mflo   $t0       (0x00004012)
  // 08: mfhi   $t1       (0x00004810)
  // 0C: dmultu $a2, $a3  (0x00C7001D)
  // 10: mflo   $t2       (0x00005012)
  // 14: mfhi   $t3       (0x00005810)
  // 18: ddiv   $a0, $a1  (0x0085001E)
  // 1C: mflo   $t4       (0x00006012)
  // 20: mfhi   $t5       (0x00006810)
  // 24: ddivu  $a2, $a3  (0x00C7001F)
  // 28: mflo   $v0       (0x00001012)
  // 2C: mfhi   $v1       (0x00001810)
  // 30: jr     $ra
  // 34: nop
  std::vector<uint32_t> code = {
      0x0085001C,  // dmult  $a0, $a1
      0x00004012,  // mflo   $t0
      0x00004810,  // mfhi   $t1
      0x00C7001D,  // dmultu $a2, $a3
      0x00005012,  // mflo   $t2
      0x00005810,  // mfhi   $t3
      0x0085001E,  // ddiv   $a0, $a1
      0x00006012,  // mflo   $t4
      0x00006810,  // mfhi   $t5
      0x00C7001F,  // ddivu  $a2, $a3
      0x00001012,  // mflo   $v0
      0x00001810,  // mfhi   $v1
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, static_cast<uint32_t>(-100000));
  emu.SetRegister(Register::kA1, 30000u);
  emu.SetRegister(Register::kA2, 0x80000005u);
  emu.SetRegister(Register::kA3, 2u);

  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);

  int64_t signed_prod = -100000LL * 30000LL;
  EXPECT_EQ(emu.GetRegister(Register::kT0), static_cast<uint32_t>(signed_prod & 0xFFFFFFFF));
  EXPECT_EQ(emu.GetRegister(Register::kT1), static_cast<uint32_t>(signed_prod >> 32));

  uint64_t unsigned_prod = 0x80000005ULL * 2ULL;
  EXPECT_EQ(emu.GetRegister(Register::kT2), static_cast<uint32_t>(unsigned_prod & 0xFFFFFFFF));
  EXPECT_EQ(emu.GetRegister(Register::kT3), static_cast<uint32_t>(unsigned_prod >> 32));

  EXPECT_EQ(emu.GetRegister(Register::kT4), static_cast<uint32_t>(-3));      // -100000 / 30000
  EXPECT_EQ(emu.GetRegister(Register::kT5), static_cast<uint32_t>(-10000));  // -100000 % 30000
  EXPECT_EQ(res.v0, 0x40000002u);                                            // 0x80000005 / 2
  EXPECT_EQ(res.v1, 1u);                                                     // 0x80000005 % 2

  // Verify DDIV INT32_MIN / -1 and division by zero safety
  std::vector<uint32_t> edge_code = {
      0x0085001E,  // ddiv  $a0, $a1
      0x00001012,  // mflo  $v0
      0x00001810,  // mfhi  $v1
      0x0080001E,  // ddiv  $a0, $zero
      0x0080001F,  // ddivu $a0, $zero
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, edge_code));
  emu.SetRegister(Register::kA0, 0x80000000u);
  emu.SetRegister(Register::kA1, 0xFFFFFFFFu);
  ExecutionResult edge_res = emu.RunFunction(0x80001000);
  EXPECT_EQ(edge_res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(edge_res.v0, 0x80000000u);
  EXPECT_EQ(edge_res.v1, 0u);
}

TEST(MipsEmulatorTest, DoublewordShiftInstructions) {
  MipsEmulator emu;
  // Test DSLLV, DSRLV, DSRAV, DSLL, DSRL, DSRA, DSLL32, DSRL32, DSRA32
  // $a0 = 0xFFFFFFF0 (-16)
  // $a1 = 4 (shift < 32)
  // $a2 = 36 (shift >= 32, uses 6-bit mask 0x3F)
  std::vector<uint32_t> code = {
      // Variable shifts (< 32)
      0x00A44014,  // dsllv  $t0, $a0, $a1 (0xFFFFFFF0 << 4 = 0xFFFFFF00)
      0x00A44816,  // dsrlv  $t1, $a0, $a1 (0xFFFFFFF0 >> 4 = 0x0FFFFFFF)
      0x00A45017,  // dsrav  $t2, $a0, $a1 (-16 >> 4 = -1 = 0xFFFFFFFF)
      // Variable shifts (>= 32)
      0x00C45814,  // dsllv  $t3, $a0, $a2 (uint64(0xFFFFFFF0) << 36 -> low 32 bits = 0)
      0x00C46016,  // dsrlv  $t4, $a0, $a2 (uint64(0xFFFFFFF0) >> 36 = 0)
      0x00C46817,  // dsrav  $t5, $a0, $a2 (int64(-16) >> 36 = -1 = 0xFFFFFFFF)
      // Immediate shifts (sa = 4)
      0x00047138,  // dsll   $t6, $a0, 4   (0xFFFFFF00)
      0x0004793A,  // dsrl   $t7, $a0, 4   (0x0FFFFFFF)
      0x0004C13B,  // dsra   $t8, $a0, 4   (0xFFFFFFFF)
      // 32+sa shifts (sa = 2 -> shift by 34)
      0x0004C8BC,  // dsll32 $t9, $a0, 2   (low 32 bits = 0)
      0x000410BE,  // dsrl32 $v0, $a0, 2   (uint64(0xFFFFFFF0) >> 34 = 0)
      0x000418BF,  // dsra32 $v1, $a0, 2   (int64(-16) >> 34 = 0xFFFFFFFF)
      0x03E00008,  // jr     $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  emu.SetRegister(Register::kA0, 0xFFFFFFF0u);
  emu.SetRegister(Register::kA1, 4u);
  emu.SetRegister(Register::kA2, 36u);

  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetRegister(Register::kT0), 0xFFFFFF00u);
  EXPECT_EQ(emu.GetRegister(Register::kT1), 0x0FFFFFFFu);
  EXPECT_EQ(emu.GetRegister(Register::kT2), 0xFFFFFFFFu);
  EXPECT_EQ(emu.GetRegister(Register::kT3), 0u);
  EXPECT_EQ(emu.GetRegister(Register::kT4), 0u);
  EXPECT_EQ(emu.GetRegister(Register::kT5), 0xFFFFFFFFu);
  EXPECT_EQ(emu.GetRegister(Register::kT6), 0xFFFFFF00u);
  EXPECT_EQ(emu.GetRegister(Register::kT7), 0x0FFFFFFFu);
  EXPECT_EQ(emu.GetRegister(Register::kT8), 0xFFFFFFFFu);
  EXPECT_EQ(emu.GetRegister(Register::kT9), 0u);
  EXPECT_EQ(res.v0, 0u);
  EXPECT_EQ(res.v1, 0xFFFFFFFFu);
}

TEST(MipsEmulatorTest, LwuDmfc1AndDmtc1Instructions) {
  MipsEmulator emu;
  ASSERT_TRUE(emu.Write32(0x80100000, 0x89ABCDEFu));
  emu.ClearWriteLog();

  // 00: lui   $a0, 0x8010       (0x3C048010)
  // 04: lwu   $v0, 0($a0)       (0x9C820000) -> loads 0x89ABCDEF
  // 08: dmtc1 $v0, $f2          (0x44A21000) -> sets $f2=0x89ABCDEF, $f3=0
  // 0C: dmfc1 $v1, $f2          (0x44231000) -> reads $f2=0x89ABCDEF into $v1
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> code = {
      0x3C048010,  // lui   $a0, 0x8010
      0x9C820000,  // lwu   $v0, 0($a0)
      0x44A21000,  // dmtc1 $v0, $f2
      0x44231000,  // dmfc1 $v1, $f2
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  emu.SetFpBits(FpRegister::kF3, 0xFFFFFFFFu);  // Should be cleared by dmtc1
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 0x89ABCDEFu);
  EXPECT_EQ(res.v1, 0x89ABCDEFu);
  EXPECT_EQ(emu.GetFpDoubleBits(FpRegister::kF2), 0x0000000089ABCDEFULL);

  // LWU unaligned address must fault
  std::vector<uint32_t> unaligned_lwu = {
      0x3C048010,  // lui $a0, 0x8010
      0x9C820002,  // lwu $v0, 2($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, unaligned_lwu));
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kMemoryFault);

  // DMFC1 / DMTC1 with odd FP register must return kInvalidOpcode
  std::vector<uint32_t> odd_dmfc1 = {
      0x44221800,  // dmfc1 $v0, $f3
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, odd_dmfc1));
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kInvalidOpcode);

  std::vector<uint32_t> odd_dmtc1 = {
      0x44A21800,  // dmtc1 $v0, $f3
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, odd_dmtc1));
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kInvalidOpcode);
}

TEST(MipsEmulatorTest, BltzallAndBgezallTakenAndUntakenAnnulmentAndLink) {
  MipsEmulator emu;
  // Test BLTZALL (major=0x01, rt=0x12):
  // 00: addu    $s0, $ra, $zero   (0x03E08021) save sentinel
  // 04: addiu   $v0, $zero, 10    (0x2402000A)
  // 08: bltzall $a0, +2           (0x04920002) target = 0x80001014, link = 0x80001010
  // 0C: addiu   $v0, $v0, 5       (0x24420005) delay slot (executed iff taken)
  // 10: addiu   $v0, $v0, 100     (0x24420064) fallthrough (executed iff untaken)
  // 14: addu    $v1, $ra, $zero   (0x03E01821) record linked $ra into $v1
  // 18: addu    $ra, $s0, $zero   (0x0200F821) restore sentinel
  // 1C: jr      $ra               (0x03E00008)
  // 20: nop                       (0x00000000)
  std::vector<uint32_t> bltzall_code = {
      0x03E08021,  // addu    $s0, $ra, $zero
      0x2402000A,  // addiu   $v0, $zero, 10
      0x04920002,  // bltzall $a0, +2
      0x24420005,  // addiu   $v0, $v0, 5
      0x24420064,  // addiu   $v0, $v0, 100
      0x03E01821,  // addu    $v1, $ra, $zero
      0x0200F821,  // addu    $ra, $s0, $zero
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, bltzall_code));

  // BLTZALL taken ($a0 = -1)
  emu.SetRegister(Register::kA0, 0xFFFFFFFFu);
  ExecutionResult res_taken = emu.RunFunction(0x80001000);
  EXPECT_EQ(res_taken.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res_taken.v0, 15u);          // 10 + 5 (delay slot executed, fallthrough skipped)
  EXPECT_EQ(res_taken.v1, 0x80001010u);  // $ra linked to branch_pc + 8

  // BLTZALL untaken ($a0 = 0)
  emu.SetRegister(Register::kA0, 0u);
  ExecutionResult res_untaken = emu.RunFunction(0x80001000);
  EXPECT_EQ(res_untaken.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res_untaken.v0, 110u);         // 10 + 100 (delay slot annulled!)
  EXPECT_EQ(res_untaken.v1, 0x80001010u);  // $ra still unconditionally linked to branch_pc + 8

  // Test BGEZALL (major=0x01, rt=0x13):
  std::vector<uint32_t> bgezall_code = bltzall_code;
  bgezall_code[2] = 0x04930002;  // bgezall $a0, +2
  ASSERT_TRUE(emu.LoadWords(0x80001000, bgezall_code));

  // BGEZALL taken ($a0 = 0)
  emu.SetRegister(Register::kA0, 0u);
  ExecutionResult bgez_taken = emu.RunFunction(0x80001000);
  EXPECT_EQ(bgez_taken.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(bgez_taken.v0, 15u);
  EXPECT_EQ(bgez_taken.v1, 0x80001010u);

  // BGEZALL untaken ($a0 = -1)
  emu.SetRegister(Register::kA0, 0xFFFFFFFFu);
  ExecutionResult bgez_untaken = emu.RunFunction(0x80001000);
  EXPECT_EQ(bgez_untaken.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(bgez_untaken.v0, 110u);
  EXPECT_EQ(bgez_untaken.v1, 0x80001010u);
}

TEST(MipsEmulatorTest, CvtWSAndCvtWDAllRoundingModesOddFdRejectionAndPostRoundClamping) {
  MipsEmulator emu;
  // cvt.w.s $f0, $f2 (0x46001024)
  std::vector<uint32_t> cvt_ws_code = {
      0x46001024,  // cvt.w.s $f0, $f2
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, cvt_ws_code));

  struct RoundingCase {
    uint32_t rm;
    double pos_val;
    int32_t expected_pos;
    double neg_val;
    int32_t expected_neg;
  };
  const std::vector<RoundingCase> cases = {
      {0, 2.5, 2, -3.5, -4},  // RN (round to nearest, ties to even)
      {1, 2.9, 2, -2.9, -2},  // RZ (round toward zero / truncate)
      {2, 2.1, 3, -2.9, -2},  // RP (round toward +infinity / ceil)
      {3, 2.9, 2, -2.1, -3},  // RM (round toward -infinity / floor)
  };

  for (const auto& tc : cases) {
    emu.SetFcr31(tc.rm);
    emu.SetFpRegister(FpRegister::kF2, static_cast<float>(tc.pos_val));
    EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(static_cast<int32_t>(emu.GetFpBits(FpRegister::kF0)), tc.expected_pos)
        << "CVT.W.S pos failed for RM=" << tc.rm;

    emu.SetFcr31(tc.rm);
    emu.SetFpRegister(FpRegister::kF2, static_cast<float>(tc.neg_val));
    EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(static_cast<int32_t>(emu.GetFpBits(FpRegister::kF0)), tc.expected_neg)
        << "CVT.W.S neg failed for RM=" << tc.rm;
  }

  // cvt.w.d $f0, $f2 (0x46201024)
  std::vector<uint32_t> cvt_wd_code = {
      0x46201024,  // cvt.w.d $f0, $f2
      0x03E00008,  // jr      $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, cvt_wd_code));
  for (const auto& tc : cases) {
    emu.SetFcr31(tc.rm);
    emu.SetFpDouble(FpRegister::kF2, tc.pos_val);
    EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(static_cast<int32_t>(emu.GetFpBits(FpRegister::kF0)), tc.expected_pos)
        << "CVT.W.D pos failed for RM=" << tc.rm;

    emu.SetFcr31(tc.rm);
    emu.SetFpDouble(FpRegister::kF2, tc.neg_val);
    EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(static_cast<int32_t>(emu.GetFpBits(FpRegister::kF0)), tc.expected_neg)
        << "CVT.W.D neg failed for RM=" << tc.rm;
  }

  // Test clamping AFTER rounding:
  // ROUND.W.D $f0, $f2 (0x4620100C) on -2147483648.4 rounds to -2147483648.0 -> 0x80000000
  // and on 2147483647.4 rounds to 2147483647.0 -> 0x7FFFFFFF, while 2147483647.6 rounds to
  // 2147483648.0 -> clamps to 0x7FFFFFFF.
  std::vector<uint32_t> round_wd_code = {
      0x4620100C,  // round.w.d $f0, $f2
      0x03E00008,  // jr        $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, round_wd_code));
  emu.SetFpDouble(FpRegister::kF2, -2147483648.4);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 0x80000000u);

  emu.SetFpDouble(FpRegister::kF2, 2147483647.4);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 0x7FFFFFFFu);

  // CEIL.W.D $f0, $f2 (0x4620100E) on -2147483648.9 rounds up to -2147483648.0 -> 0x80000000
  std::vector<uint32_t> ceil_wd_code = {
      0x4620100E,  // ceil.w.d $f0, $f2
      0x03E00008,  // jr       $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, ceil_wd_code));
  emu.SetFpDouble(FpRegister::kF2, -2147483648.9);
  EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(emu.GetFpBits(FpRegister::kF0), 0x80000000u);

  // Reject odd destination register fd (!IsEvenFpRegister(fd)) for .D float-to-word instructions
  // fd = $f1 (sa = 1 -> 1 << 6 = 0x40), fs = $f2 (rd = 2 -> 2 << 11 = 0x1000)
  for (uint32_t funct : {0x0Cu, 0x0Du, 0x0Eu, 0x0Fu, 0x24u}) {
    uint32_t inst = (0x11u << 26) | (0x11u << 21) | (2u << 11) | (1u << 6) | funct;
    std::vector<uint32_t> odd_fd_code = {inst, 0x03E00008, 0x00000000};
    ASSERT_TRUE(emu.LoadWords(0x80001000, odd_fd_code));
    EXPECT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kInvalidOpcode)
        << "Expected kInvalidOpcode for odd fd on funct=0x" << std::hex << funct;
  }
}

TEST(MipsEmulatorTest, All16Cop1ComparisonPredicatesSingleAndDouble) {
  MipsEmulator emu;
  const float qnan_f = std::numeric_limits<float>::quiet_NaN();
  const double qnan_d = std::numeric_limits<double>::quiet_NaN();

  for (uint32_t funct = 0x30; funct <= 0x3F; ++funct) {
    bool match_unordered = (funct & 1) != 0;
    bool match_equal = (funct & 2) != 0;
    bool match_less = (funct & 4) != 0;

    // Single-precision: fmt = 0x10, fs = $f2 (2), ft = $f4 (4)
    uint32_t inst_s = (0x11u << 26) | (0x10u << 21) | (4u << 16) | (2u << 11) | funct;
    std::vector<uint32_t> code_s = {inst_s, 0x03E00008, 0x00000000};
    ASSERT_TRUE(emu.LoadWords(0x80001000, code_s));

    // 1. Less: 1.0f < 2.0f
    emu.SetFpRegister(FpRegister::kF2, 1.0f);
    emu.SetFpRegister(FpRegister::kF4, 2.0f);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(emu.GetFpuCondition(), match_less) << "S (<) funct=0x" << std::hex << funct;

    // 2. Equal: 2.0f == 2.0f
    emu.SetFpRegister(FpRegister::kF2, 2.0f);
    emu.SetFpRegister(FpRegister::kF4, 2.0f);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(emu.GetFpuCondition(), match_equal) << "S (==) funct=0x" << std::hex << funct;

    // 3. Greater: 3.0f > 2.0f
    emu.SetFpRegister(FpRegister::kF2, 3.0f);
    emu.SetFpRegister(FpRegister::kF4, 2.0f);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_FALSE(emu.GetFpuCondition()) << "S (>) funct=0x" << std::hex << funct;

    // 4. Unordered: NaN vs 2.0f
    emu.SetFpRegister(FpRegister::kF2, qnan_f);
    emu.SetFpRegister(FpRegister::kF4, 2.0f);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(emu.GetFpuCondition(), match_unordered) << "S (NaN) funct=0x" << std::hex << funct;

    // Double-precision: fmt = 0x11, fs = $f2 (2), ft = $f4 (4)
    uint32_t inst_d = (0x11u << 26) | (0x11u << 21) | (4u << 16) | (2u << 11) | funct;
    std::vector<uint32_t> code_d = {inst_d, 0x03E00008, 0x00000000};
    ASSERT_TRUE(emu.LoadWords(0x80001000, code_d));

    // 1. Less: 1.0 < 2.0
    emu.SetFpDouble(FpRegister::kF2, 1.0);
    emu.SetFpDouble(FpRegister::kF4, 2.0);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(emu.GetFpuCondition(), match_less) << "D (<) funct=0x" << std::hex << funct;

    // 2. Equal: 2.0 == 2.0
    emu.SetFpDouble(FpRegister::kF2, 2.0);
    emu.SetFpDouble(FpRegister::kF4, 2.0);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(emu.GetFpuCondition(), match_equal) << "D (==) funct=0x" << std::hex << funct;

    // 3. Greater: 3.0 > 2.0
    emu.SetFpDouble(FpRegister::kF2, 3.0);
    emu.SetFpDouble(FpRegister::kF4, 2.0);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_FALSE(emu.GetFpuCondition()) << "D (>) funct=0x" << std::hex << funct;

    // 4. Unordered: NaN vs 2.0
    emu.SetFpDouble(FpRegister::kF2, qnan_d);
    emu.SetFpDouble(FpRegister::kF4, 2.0);
    ASSERT_EQ(emu.RunFunction(0x80001000).status, ExecutionStatus::kHaltedReturn);
    EXPECT_EQ(emu.GetFpuCondition(), match_unordered) << "D (NaN) funct=0x" << std::hex << funct;
  }
}

TEST(MipsEmulatorTest, RollbackWritesRestoresOverwrittenCodeWords) {
  MipsEmulator emu;
  // Code at 0x80001000..0x80001010:
  // 00: addiu $v0, $zero, 77
  // 04: sw    $zero, 0($a0)   (overwrites code word at $a0 = 0x80001000)
  // 08: jr    $ra
  // 0C: nop
  std::vector<uint32_t> code = {
      0x2402004D,  // addiu $v0, $zero, 77
      0xAC800000,  // sw    $zero, 0($a0)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  ASSERT_TRUE(emu.LoadWords(0x80001000, code));
  // Corrupt the second word via ClearMemory before Write8 so previous_value alone wouldn't restore
  // the full code span unless LoadWords(code_vram, code) is triggered by code_touched.
  emu.ClearMemory(0x8000100C, 4);
  ASSERT_TRUE(emu.Write8(0x80001003, 0x00));

  uint32_t word0 = 0;
  ASSERT_TRUE(emu.Read32(0x80001000, &word0));
  EXPECT_EQ(word0, 0x24020000u);

  emu.RollbackWrites(code, 0x80001000);
  ASSERT_TRUE(emu.Read32(0x80001000, &word0));
  EXPECT_EQ(word0, 0x2402004Du);

  // Re-running the function after rollback should execute the restored instruction
  emu.SetRegister(Register::kA0, 0x80001000u);
  ExecutionResult res = emu.RunFunction(0x80001000);
  EXPECT_EQ(res.status, ExecutionStatus::kHaltedReturn);
  EXPECT_EQ(res.v0, 77u);
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
