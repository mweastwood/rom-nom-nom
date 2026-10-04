#include "lifter/jump_table.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Optional;

// Helper to construct a mock memory reader from an address map.
MemoryReader MakeMemoryReader(const absl::flat_hash_map<uint32_t, uint32_t>& memory) {
  return [memory](uint32_t vram) -> std::optional<uint32_t> {
    auto it = memory.find(vram);
    if (it != memory.end()) {
      return it->second;
    }
    return std::nullopt;
  };
}

TEST(JumpTableTest, StandardZeroBasedJumpTable) {
  // Assembly:
  // 8005D2FC: sltiu $v0, $v1, 4
  // 8005D300: beqz  $v0, .L8005D400 (target: 0x8005D400)
  // 8005D304: sll   $v0, $v1, 2
  // 8005D308: lui   $at, 0x8012
  // 8005D30C: addu  $at, $at, $v0
  // 8005D310: lw    $v0, -4048($at)
  // 8005D314: jr    $v0
  // 8005D318: nop

  std::vector<Instruction> instructions;

  // sltiu $v0, $v1, 4
  Instruction sltiu;
  sltiu.opcode = Opcode::kSltiu;
  sltiu.rt = Register::kV0;
  sltiu.rs = Register::kV1;
  sltiu.immediate = 4;
  sltiu.vram = 0x8005D2FC;
  instructions.push_back(sltiu);

  // beqz $v0, 0x8005D400
  Instruction beqz;
  beqz.opcode = Opcode::kBeq;
  beqz.rs = Register::kV0;
  beqz.rt = Register::kZero;
  // target = vram + 4 + (imm << 2) => 0x8005D400 - 0x8005D304 = 0xFC => imm = 0x3F (63)
  beqz.immediate = 63;
  beqz.vram = 0x8005D300;
  instructions.push_back(beqz);

  // sll $v0, $v1, 2
  Instruction sll;
  sll.opcode = Opcode::kSll;
  sll.rd = Register::kV0;
  sll.rt = Register::kV1;
  sll.shift_amount = 2;
  sll.vram = 0x8005D304;
  instructions.push_back(sll);

  // lui $at, 0x8012
  Instruction lui;
  lui.opcode = Opcode::kLui;
  lui.rt = Register::kAt;
  lui.immediate = static_cast<int16_t>(0x8012);
  lui.vram = 0x8005D308;
  instructions.push_back(lui);

  // addu $at, $at, $v0
  Instruction addu;
  addu.opcode = Opcode::kAddu;
  addu.rd = Register::kAt;
  addu.rs = Register::kAt;
  addu.rt = Register::kV0;
  addu.vram = 0x8005D30C;
  instructions.push_back(addu);

  // lw $v0, -4048($at)
  Instruction lw;
  lw.opcode = Opcode::kLw;
  lw.rt = Register::kV0;
  lw.rs = Register::kAt;
  lw.immediate = -4048;
  lw.vram = 0x8005D310;
  instructions.push_back(lw);

  // jr $v0
  Instruction jr;
  jr.opcode = Opcode::kJr;
  jr.rs = Register::kV0;
  jr.vram = 0x8005D314;
  instructions.push_back(jr);

  // nop
  Instruction nop;
  nop.opcode = Opcode::kSll;
  nop.rd = Register::kZero;
  nop.rt = Register::kZero;
  nop.shift_amount = 0;
  nop.vram = 0x8005D318;
  instructions.push_back(nop);

  // Table at 0x80120000 - 4048 = 0x8011F030
  absl::flat_hash_map<uint32_t, uint32_t> memory = {
      {0x8011F030, 0x8005D320},
      {0x8011F034, 0x8005D340},
      {0x8011F038, 0x8005D360},
      {0x8011F03C, 0x8005D380},
  };

  auto reader = MakeMemoryReader(memory);
  auto result = JumpTableDetector::DetectAt(instructions, 6, reader);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->switch_vram, 0x8005D314);
  EXPECT_EQ(result->table_vram, 0x8011F030);
  EXPECT_EQ(result->num_cases, 4u);
  EXPECT_EQ(result->lower_bound, 0);
  EXPECT_EQ(result->index_register, Register::kV1);
  EXPECT_EQ(result->default_target_vram, 0x8005D400);

  ASSERT_EQ(result->entries.size(), 4u);
  EXPECT_EQ(result->entries[0].case_value, 0);
  EXPECT_EQ(result->entries[0].target_vram, 0x8005D320);
  EXPECT_EQ(result->entries[1].case_value, 1);
  EXPECT_EQ(result->entries[1].target_vram, 0x8005D340);
  EXPECT_EQ(result->entries[2].case_value, 2);
  EXPECT_EQ(result->entries[2].target_vram, 0x8005D360);
  EXPECT_EQ(result->entries[3].case_value, 3);
  EXPECT_EQ(result->entries[3].target_vram, 0x8005D380);
}

TEST(JumpTableTest, OffsetLowerBoundJumpTable) {
  // Assembly:
  // 8009D940: addiu $v1, $s0, -5
  // 8009D944: sltiu $v0, $v1, 3
  // 8009D948: beqz  $v0, .L8009DADC
  // 8009D94C: sll   $v0, $v1, 2
  // 8009D950: lui   $at, 0x8012
  // 8009D954: addu  $at, $at, $v0
  // 8009D958: lw    $v0, 4144($at)
  // 8009D95C: jr    $v0
  // 8009D960: nop

  std::vector<Instruction> instructions;

  Instruction addiu;
  addiu.opcode = Opcode::kAddiu;
  addiu.rt = Register::kV1;
  addiu.rs = Register::kS0;
  addiu.immediate = -5;
  addiu.vram = 0x8009D940;
  instructions.push_back(addiu);

  Instruction sltiu;
  sltiu.opcode = Opcode::kSltiu;
  sltiu.rt = Register::kV0;
  sltiu.rs = Register::kV1;
  sltiu.immediate = 3;
  sltiu.vram = 0x8009D944;
  instructions.push_back(sltiu);

  Instruction beqz;
  beqz.opcode = Opcode::kBeq;
  beqz.rs = Register::kV0;
  beqz.rt = Register::kZero;
  beqz.immediate = 100;
  beqz.vram = 0x8009D948;
  instructions.push_back(beqz);

  Instruction sll;
  sll.opcode = Opcode::kSll;
  sll.rd = Register::kV0;
  sll.rt = Register::kV1;
  sll.shift_amount = 2;
  sll.vram = 0x8009D94C;
  instructions.push_back(sll);

  Instruction lui;
  lui.opcode = Opcode::kLui;
  lui.rt = Register::kAt;
  lui.immediate = static_cast<int16_t>(0x8012);
  lui.vram = 0x8009D950;
  instructions.push_back(lui);

  Instruction addu;
  addu.opcode = Opcode::kAddu;
  addu.rd = Register::kAt;
  addu.rs = Register::kAt;
  addu.rt = Register::kV0;
  addu.vram = 0x8009D954;
  instructions.push_back(addu);

  Instruction lw;
  lw.opcode = Opcode::kLw;
  lw.rt = Register::kV0;
  lw.rs = Register::kAt;
  lw.immediate = 4144;
  lw.vram = 0x8009D958;
  instructions.push_back(lw);

  Instruction jr;
  jr.opcode = Opcode::kJr;
  jr.rs = Register::kV0;
  jr.vram = 0x8009D95C;
  instructions.push_back(jr);

  uint32_t table_addr = 0x80120000 + 4144;
  absl::flat_hash_map<uint32_t, uint32_t> memory = {
      {table_addr, 0x8009D970},
      {table_addr + 4, 0x8009D990},
      {table_addr + 8, 0x8009D9B0},
  };

  auto reader = MakeMemoryReader(memory);
  auto result = JumpTableDetector::DetectAt(instructions, 7, reader);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->lower_bound, 5);
  EXPECT_EQ(result->index_register, Register::kS0);
  EXPECT_EQ(result->num_cases, 3u);

  ASSERT_EQ(result->entries.size(), 3u);
  EXPECT_EQ(result->entries[0].case_value, 5);
  EXPECT_EQ(result->entries[1].case_value, 6);
  EXPECT_EQ(result->entries[2].case_value, 7);
}

TEST(JumpTableTest, HoistedBaseRegisterJumpTable) {
  // Hoisted base setup at function start:
  // 80040420: lui   $s7, 0x8012
  // 80040424: addiu $s7, $s7, -4544
  // ...
  // 800404B0: sltiu $v0, $v1, 2
  // 800404B4: beqz  $v0, .Ldefault
  // 800404B8: sll   $v0, $v1, 2
  // 800404BC: addu  $v0, $v0, $s7
  // 800404C0: lw    $v0, 0($v0)
  // 800404C4: jr    $v0

  std::vector<Instruction> instructions;

  Instruction lui;
  lui.opcode = Opcode::kLui;
  lui.rt = Register::kS7;
  lui.immediate = static_cast<int16_t>(0x8012);
  lui.vram = 0x80040420;
  instructions.push_back(lui);

  Instruction addiu;
  addiu.opcode = Opcode::kAddiu;
  addiu.rt = Register::kS7;
  addiu.rs = Register::kS7;
  addiu.immediate = -4544;
  addiu.vram = 0x80040424;
  instructions.push_back(addiu);

  Instruction sltiu;
  sltiu.opcode = Opcode::kSltiu;
  sltiu.rt = Register::kV0;
  sltiu.rs = Register::kV1;
  sltiu.immediate = 2;
  sltiu.vram = 0x800404B0;
  instructions.push_back(sltiu);

  Instruction beqz;
  beqz.opcode = Opcode::kBeq;
  beqz.rs = Register::kV0;
  beqz.rt = Register::kZero;
  beqz.immediate = 20;
  beqz.vram = 0x800404B4;
  instructions.push_back(beqz);

  Instruction sll;
  sll.opcode = Opcode::kSll;
  sll.rd = Register::kV0;
  sll.rt = Register::kV1;
  sll.shift_amount = 2;
  sll.vram = 0x800404B8;
  instructions.push_back(sll);

  Instruction addu;
  addu.opcode = Opcode::kAddu;
  addu.rd = Register::kV0;
  addu.rs = Register::kV0;
  addu.rt = Register::kS7;
  addu.vram = 0x800404BC;
  instructions.push_back(addu);

  Instruction lw;
  lw.opcode = Opcode::kLw;
  lw.rt = Register::kV0;
  lw.rs = Register::kV0;
  lw.immediate = 0;
  lw.vram = 0x800404C0;
  instructions.push_back(lw);

  Instruction jr;
  jr.opcode = Opcode::kJr;
  jr.rs = Register::kV0;
  jr.vram = 0x800404C4;
  instructions.push_back(jr);

  uint32_t table_addr = 0x80120000 - 4544;  // 0x8011EE40
  absl::flat_hash_map<uint32_t, uint32_t> memory = {
      {table_addr, 0x800404D0},
      {table_addr + 4, 0x800404F0},
  };

  auto reader = MakeMemoryReader(memory);
  auto tables = JumpTableDetector::DetectJumpTables(instructions, reader);

  ASSERT_EQ(tables.size(), 1u);
  EXPECT_EQ(tables[0].table_vram, 0x8011EE40);
  EXPECT_EQ(tables[0].num_cases, 2u);
  EXPECT_EQ(tables[0].entries[0].target_vram, 0x800404D0);
  EXPECT_EQ(tables[0].entries[1].target_vram, 0x800404F0);
}

TEST(JumpTableTest, IgnoresFunctionReturn) {
  // jr $ra must never be treated as a jump table.
  Instruction jr_ra;
  jr_ra.opcode = Opcode::kJr;
  jr_ra.rs = Register::kRa;
  jr_ra.vram = 0x80025CAC;

  std::vector<Instruction> instructions = {jr_ra};
  auto reader = MakeMemoryReader({});
  auto result = JumpTableDetector::DetectAt(instructions, 0, reader);

  EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace rom_nom_nom
