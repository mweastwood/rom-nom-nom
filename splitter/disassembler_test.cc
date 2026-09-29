#include "splitter/disassembler.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "gtest/gtest.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

// Helper to encode a 32-bit big-endian instruction into a byte vector.
void AppendWord(std::vector<uint8_t>& buf, uint32_t word) {
  buf.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(word & 0xFF));
}

constexpr std::string_view kSymbolsProtoText = R"pb(
  entries { name: "MyFunction" address: 0x80700000 type: SYMBOL_FUNC }
  entries { name: "CalleeFunction" address: 0x80701000 type: SYMBOL_FUNC }
)pb";

TEST(DisassemblerTest, DisassembleSingleInstruction) {
  Disassembler disasm;
  // addiu $sp, $sp, -32 (0x27BDFFE0)
  std::string text = disasm.DisassembleInstruction(0x27BDFFE0, 0x80700000);
  EXPECT_EQ(text, "addiu $sp, $sp, -32");
}

TEST(DisassemblerTest, DisassembleFunctionWithBranchesAndDelaySlots) {
  auto symbols_or = SymbolIndex::ParseFromTextproto(kSymbolsProtoText);
  ASSERT_TRUE(symbols_or.ok());

  // Construct a small test function:
  // 80700000: addiu $sp, $sp, -32
  // 80700004: sw    $ra, 28($sp)
  // 80700008: beqz  $a0, .L80700018 (+3 instructions)
  // 8070000C:  nop
  // 80700010: jal   CalleeFunction (0x80701000)
  // 80700014:  nop
  // 80700018: lw    $ra, 28($sp)  <- branch target (.L80700018)
  // 8070001C: jr    $ra
  // 80700020:  addiu $sp, $sp, 32
  // 80700024: nop (trailing alignment)
  std::vector<uint8_t> code;
  AppendWord(code, 0x27BDFFE0);  // addiu $sp, $sp, -32
  AppendWord(code, 0xAFBF001C);  // sw $ra, 28($sp)
  AppendWord(code, 0x10800003);  // beq $a0, $0, +3 (target: 8070000C + 3*4 = 80700018)
  AppendWord(code, 0x00000000);  // nop
  // jal 0x80701000: target26 = (0x80701000 & 0x0FFFFFFF) >> 2 = 0x01C0400
  AppendWord(code, 0x0C000000 | 0x01C0400);  // jal CalleeFunction
  AppendWord(code, 0x00000000);              // nop
  AppendWord(code, 0x8FBF001C);              // lw $ra, 28($sp)
  AppendWord(code, 0x03E00008);              // jr $ra
  AppendWord(code, 0x27BD0020);              // addiu $sp, $sp, 32
  AppendWord(code, 0x00000000);              // nop (trailing padding)

  DisassemblerOptions options;
  options.emit_line_comments = true;
  options.emit_function_framing = true;
  Disassembler disasm(&*symbols_or, options);

  auto func_or = disasm.DisassembleFunction(code, 0x80700000);
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  const auto& func = *func_or;
  EXPECT_EQ(func.name, "MyFunction");
  EXPECT_EQ(func.vram_start, 0x80700000u);
  EXPECT_EQ(func.vram_end, 0x80700028u);
  EXPECT_EQ(func.Size(), 0x28u);
  EXPECT_EQ(func.instructions.size(), 10u);

  EXPECT_EQ(func.emitted_assembly, R"(.set noat
.set noreorder

.globl MyFunction
.ent MyFunction
MyFunction:
/* 80700000 27BDFFE0 */  addiu $sp, $sp, -32
/* 80700004 AFBF001C */  sw    $ra, 28($sp)
/* 80700008 10800003 */  beqz   $a0, .L80700018
/* 8070000C 00000000 */   nop
/* 80700010 0C1C0400 */  jal    CalleeFunction
/* 80700014 00000000 */   nop
.L80700018:
/* 80700018 8FBF001C */  lw    $ra, 28($sp)
/* 8070001C 03E00008 */  jr    $ra
/* 80700020 27BD0020 */   addiu $sp, $sp, 32
/* 80700024 00000000 */  nop
.end MyFunction
)");
}

TEST(DisassemblerTest, DisassembleAllFunctionsSplitsConsecutiveFunctions) {
  // Construct 2 back-to-back functions:
  // Func 1 (16 bytes):
  // 80700000: addiu $sp, $sp, -16
  // 80700004: jr    $ra
  // 80700008:  addiu $sp, $sp, 16
  // 8070000C: nop (padding)
  //
  // Func 2 (12 bytes):
  // 80700010: jr    $ra
  // 80700014:  nop
  // 80700018: nop (padding)
  std::vector<uint8_t> code;
  // Func 1
  AppendWord(code, 0x27BDFFF0);  // addiu $sp, $sp, -16
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x27BD0010);  // addiu $sp, $sp, 16
  AppendWord(code, 0x00000000);  // nop
  // Func 2
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop
  AppendWord(code, 0x00000000);  // nop

  Disassembler disasm;
  auto funcs_or = disasm.DisassembleAllFunctions(code, 0x80700000);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();

  const auto& funcs = *funcs_or;
  ASSERT_EQ(funcs.size(), 2u);

  EXPECT_EQ(funcs[0].emitted_assembly, R"(.set noat
.set noreorder

.globl func_80700000
.ent func_80700000
func_80700000:
/* 80700000 27BDFFF0 */  addiu $sp, $sp, -16
/* 80700004 03E00008 */  jr    $ra
/* 80700008 27BD0010 */   addiu $sp, $sp, 16
/* 8070000C 00000000 */  nop
.end func_80700000
)");

  EXPECT_EQ(funcs[1].emitted_assembly, R"(.set noat
.set noreorder

.globl func_80700010
.ent func_80700010
func_80700010:
/* 80700010 03E00008 */  jr    $ra
/* 80700014 00000000 */   nop
/* 80700018 00000000 */  nop
.end func_80700010
)");
}

TEST(DisassemblerTest, DisassembleRangeMonolithic) {
  std::vector<uint8_t> code;
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop

  Disassembler disasm;
  auto text_or = disasm.DisassembleRange(code, 0x80700000);
  ASSERT_TRUE(text_or.ok());

  EXPECT_EQ(*text_or, R"(.set noat
.set noreorder

.globl func_80700000
.ent func_80700000
func_80700000:
/* 80700000 03E00008 */  jr    $ra
/* 80700004 00000000 */   nop
.end func_80700000

)");
}

TEST(DisassemblerTest, RejectEmptyBuffer) {
  std::vector<uint8_t> empty;
  Disassembler disasm;
  EXPECT_FALSE(disasm.DisassembleFunction(empty, 0x80700000).ok());
}

}  // namespace
}  // namespace rom_nom_nom
