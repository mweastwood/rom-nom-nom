#include "fuzzer/mips_emulator.h"

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

}  // namespace
}  // namespace rom_nom_nom::fuzzer
