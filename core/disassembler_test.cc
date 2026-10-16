#include "core/disassembler.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "core/symbol_registry.h"
#include "gtest/gtest.h"

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

TEST(DisassemblerTest, DefaultDoesNotEmitRelocations) {
  // 80700000: lui   $a0, 0x8020
  // 80700004: lw    $v0, 0x1234($a0)
  // 80700008: jr    $ra
  // 8070000C:  nop
  std::vector<uint8_t> code;
  AppendWord(code, 0x3C048020);  // lui $a0, 0x8020
  AppendWord(code, 0x8C821234);  // lw $v0, 0x1234($a0)
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop

  DisassemblerOptions options;
  options.emit_line_comments = false;
  options.emit_function_framing = false;
  options.emit_relocations = false;
  Disassembler disasm(nullptr, options);

  auto func_or = disasm.DisassembleFunction(code, 0x80700000);
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  EXPECT_EQ(func_or->emitted_assembly,
            "  lui   $a0, 0x8020\n"
            "  lw    $v0, 4660($a0)\n"
            "  jr    $ra\n"
            "   nop\n");
}

TEST(DisassemblerTest, EmitsSynthesizedRelocationsWhenEnabled) {
  // 80700000: lui   $a0, 0x8020
  // 80700004: lw    $v0, 0x1234($a0)
  // 80700008: jr    $ra
  // 8070000C:  nop
  std::vector<uint8_t> code;
  AppendWord(code, 0x3C048020);  // lui $a0, 0x8020
  AppendWord(code, 0x8C821234);  // lw $v0, 0x1234($a0)
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop

  DisassemblerOptions options;
  options.emit_line_comments = false;
  options.emit_function_framing = false;
  options.emit_relocations = true;
  Disassembler disasm(nullptr, options);

  auto func_or = disasm.DisassembleFunction(code, 0x80700000);
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  EXPECT_EQ(func_or->emitted_assembly,
            "  lui   $a0, %hi(D_80201234)\n"
            "  lw    $v0, %lo(D_80201234)($a0)\n"
            "  jr    $ra\n"
            "   nop\n");
}

TEST(DisassemblerTest, EmitsNamedRelocationsWithSymbolRegistry) {
  constexpr std::string_view kSymbols = R"pb(
    entries { name: "g_target_data" address: 0x80201234 type: SYMBOL_DATA }
  )pb";
  auto symbols_or = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(symbols_or.ok());

  // 80700000: lui   $a0, 0x8020
  // 80700004: sw    $v0, 0x1234($a0)
  // 80700008: jr    $ra
  // 8070000C:  nop
  std::vector<uint8_t> code;
  AppendWord(code, 0x3C048020);  // lui $a0, 0x8020
  AppendWord(code, 0xAC821234);  // sw $v0, 0x1234($a0)
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop

  DisassemblerOptions options;
  options.emit_line_comments = false;
  options.emit_function_framing = false;
  options.emit_relocations = true;
  Disassembler disasm(&*symbols_or, options);

  auto func_or = disasm.DisassembleFunction(code, 0x80700000);
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  EXPECT_EQ(func_or->emitted_assembly,
            "  lui   $a0, %hi(g_target_data)\n"
            "  sw    $v0, %lo(g_target_data)($a0)\n"
            "  jr    $ra\n"
            "   nop\n");
}

TEST(DisassemblerTest, UsesExternalRelocationTracker) {
  constexpr std::string_view kSymbols = R"pb(
    entries { name: "g_custom_table" address: 0x80205000 type: SYMBOL_DATA }
  )pb";
  auto symbols_or = SymbolIndex::ParseFromTextproto(kSymbols);
  ASSERT_TRUE(symbols_or.ok());

  std::vector<uint8_t> code;
  AppendWord(code, 0x3C048020);  // lui $a0, 0x8020
  AppendWord(code, 0x24845000);  // addiu $a0, $a0, 0x5000
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop

  RelocationTracker tracker(&*symbols_or);
  ASSERT_TRUE(tracker.AnalyzeCode(code, 0x80700000).ok());

  DisassemblerOptions options;
  options.emit_line_comments = false;
  options.emit_function_framing = false;
  options.emit_relocations = false;  // Relies on external tracker
  Disassembler disasm(&*symbols_or, options);
  disasm.SetRelocationTracker(&tracker);

  auto func_or = disasm.DisassembleFunction(code, 0x80700000);
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  EXPECT_EQ(func_or->emitted_assembly,
            "  lui   $a0, %hi(g_custom_table)\n"
            "  addiu $a0, $a0, %lo(g_custom_table)\n"
            "  jr    $ra\n"
            "   nop\n");

  // Single instruction disassembly also honors the tracker
  EXPECT_EQ(disasm.DisassembleInstruction(0x3C048020, 0x80700000),
            "lui   $a0, %hi(g_custom_table)");
  EXPECT_EQ(disasm.DisassembleInstruction(0x24845000, 0x80700004),
            "addiu $a0, $a0, %lo(g_custom_table)");
}

TEST(DisassemblerTest, DisassembleFunctionWithMultipleReturns) {
  // Construct a function with an early return skipped by a forward branch:
  // 80700000: bne   $a0, $0, .L80700010 (+3)
  // 80700004:  nop
  // 80700008: jr    $ra                    (early return)
  // 8070000C:  addiu $v0, $0, 1
  // 80700010: addiu $v0, $0, 2             (.L80700010)
  // 80700014: jr    $ra                    (final return)
  // 80700018:  nop
  std::vector<uint8_t> code;
  AppendWord(code, 0x14800003);  // bne $a0, $0, +3 (target: 0x80700010)
  AppendWord(code, 0x00000000);  // nop
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x24020001);  // addiu $v0, $0, 1
  AppendWord(code, 0x24020002);  // addiu $v0, $0, 2
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop

  DisassemblerOptions options;
  options.emit_line_comments = false;
  options.emit_function_framing = false;
  Disassembler disasm(nullptr, options);

  auto func_or = disasm.DisassembleFunction(code, 0x80700000);
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  const auto& func = *func_or;
  EXPECT_EQ(func.vram_start, 0x80700000u);
  EXPECT_EQ(func.vram_end, 0x8070001Cu);
  EXPECT_EQ(func.Size(), 0x1Cu);
  EXPECT_EQ(func.instructions.size(), 7u);

  // Verify that .L80700010 label was emitted
  EXPECT_NE(func.emitted_assembly.find(".L80700010:\n"), std::string::npos);
  EXPECT_EQ(func.emitted_assembly,
            "  bnez   $a0, .L80700010\n"
            "   nop\n"
            "  jr    $ra\n"
            "   addiu $v0, $zero, 1\n"
            ".L80700010:\n"
            "  addiu $v0, $zero, 2\n"
            "  jr    $ra\n"
            "   nop\n");
}

TEST(DisassemblerTest, PreScanCodeDiscoversJalEntrypoints) {
  // Construct two functions in a single buffer:
  // Func 1 (0x80700000):
  // 80700000: jal   0x80700010 (target: Func 2)
  // 80700004:  nop
  // 80700008: jr    $ra
  // 8070000C:  nop
  // Func 2 (0x80700010):
  // 80700010: jr    $ra
  // 80700014:  nop
  std::vector<uint8_t> code;
  AppendWord(code, 0x0C000000 | (0x00700010 >> 2));  // jal 0x80700010
  AppendWord(code, 0x00000000);                      // nop
  AppendWord(code, 0x03E00008);                      // jr $ra
  AppendWord(code, 0x00000000);                      // nop
  AppendWord(code, 0x03E00008);                      // jr $ra
  AppendWord(code, 0x00000000);                      // nop

  DisassemblerOptions options;
  options.emit_line_comments = false;
  options.emit_function_framing = false;
  Disassembler disasm(nullptr, options);

  auto funcs_or = disasm.DisassembleAllFunctions(code, 0x80700000);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();

  const auto& funcs = *funcs_or;
  ASSERT_EQ(funcs.size(), 2u);
  EXPECT_EQ(funcs[0].name, "func_80700000");
  EXPECT_EQ(funcs[0].vram_start, 0x80700000u);
  EXPECT_EQ(funcs[0].vram_end, 0x80700010u);

  EXPECT_EQ(funcs[1].name, "func_80700010");
  EXPECT_EQ(funcs[1].vram_start, 0x80700010u);
  EXPECT_EQ(funcs[1].vram_end, 0x80700018u);
}

TEST(DisassemblerTest, KnownEntrypointsAccessors) {
  Disassembler disasm;
  EXPECT_TRUE(disasm.KnownEntrypoints().empty());

  disasm.RegisterFunctionEntrypoint(0x80025C00);
  EXPECT_EQ(disasm.KnownEntrypoints().size(), 1u);
  EXPECT_TRUE(disasm.KnownEntrypoints().contains(0x80025C00));

  absl::flat_hash_set<uint32_t> custom_entrypoints = {0x80026000, 0x80027000};
  disasm.SetKnownEntrypoints(custom_entrypoints);
  EXPECT_EQ(disasm.KnownEntrypoints().size(), 2u);
  EXPECT_TRUE(disasm.KnownEntrypoints().contains(0x80026000));
  EXPECT_TRUE(disasm.KnownEntrypoints().contains(0x80027000));
  EXPECT_FALSE(disasm.KnownEntrypoints().contains(0x80025C00));
}

TEST(DisassemblerTest, ForwardBranchToReturnDelaySlotDoesNotPrematurelyEndFunction) {
  // 80700000: bnez  $a0, .L8070000C (target: 80700004 + 2*4 = 8070000C)
  // 80700004:  nop
  // 80700008: jr    $ra
  // 8070000C:  addiu $v0, $zero, 2  <- branch target (.L8070000C, delay slot of jr $ra)
  // 80700010: jr    $ra
  // 80700014:  nop
  std::vector<uint8_t> code;
  AppendWord(code, 0x14800002);  // bne $a0, $0, +2 -> 8070000C
  AppendWord(code, 0x00000000);  // nop
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x24020002);  // addiu $v0, $zero, 2 (target of bne, delay slot of return)
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop

  DisassemblerOptions options;
  options.emit_line_comments = false;
  options.emit_function_framing = false;
  Disassembler disasm(nullptr, options);

  auto func_or = disasm.DisassembleFunction(code, 0x80700000);
  ASSERT_TRUE(func_or.ok()) << func_or.status();

  const auto& func = *func_or;
  EXPECT_EQ(func.vram_start, 0x80700000u);
  EXPECT_GE(func.vram_end, 0x80700014u);
  EXPECT_NE(func.emitted_assembly.find(".L8070000C:"), std::string::npos);
}

TEST(DisassemblerTest, IdentifiesTrailingPaddingAndOmitsFunctionFraming) {
  // Construct code slice:
  // Function: jr $ra; nop (8 bytes)
  // Followed by trailing padding: andi $v0, $t9, 0xFFFF (4 bytes)
  std::vector<uint8_t> code;
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop
  AppendWord(code, 0x3322FFFF);  // andi $v0, $t9, 0xFFFF (trailing padding word)

  Disassembler disasm;
  auto funcs_or = disasm.DisassembleAllFunctions(code, 0x80110A00);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();

  const auto& funcs = *funcs_or;
  ASSERT_EQ(funcs.size(), 2u);

  // First block is a legitimate function
  EXPECT_TRUE(funcs[0].is_function);
  EXPECT_EQ(funcs[0].vram_start, 0x80110A00u);
  EXPECT_EQ(funcs[0].Size(), 8u);
  EXPECT_NE(funcs[0].emitted_assembly.find(".ent func_80110A00"), std::string::npos);
  EXPECT_NE(funcs[0].emitted_assembly.find(".end func_80110A00"), std::string::npos);

  // Second block is trailing padding (< 8 bytes, no return, no callers)
  EXPECT_FALSE(funcs[1].is_function);
  EXPECT_EQ(funcs[1].vram_start, 0x80110A08u);
  EXPECT_EQ(funcs[1].Size(), 4u);
  EXPECT_EQ(funcs[1].emitted_assembly.find(".ent"), std::string::npos);
  EXPECT_EQ(funcs[1].emitted_assembly.find(".end"), std::string::npos);
  EXPECT_NE(funcs[1].emitted_assembly.find("andi  $v0, $t9, 0xFFFF"), std::string::npos);
}

TEST(DisassemblerTest, SmallBlockWithExplicitSymbolPreservesFunctionFraming) {
  // 4-byte block with explicit symbol registered as SYMBOL_FUNC
  std::vector<uint8_t> code;
  AppendWord(code, 0x24020001);  // addiu $v0, $zero, 1

  SymbolRegistry registry;
  auto* entry = registry.add_entries();
  entry->set_name("SpecialSmallFunc");
  entry->set_address(0x80700000);
  entry->set_type(SYMBOL_FUNC);

  auto symbols_or = SymbolIndex::FromProto(std::move(registry));
  ASSERT_TRUE(symbols_or.ok());

  Disassembler disasm(&*symbols_or);
  auto funcs_or = disasm.DisassembleAllFunctions(code, 0x80700000);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();

  const auto& funcs = *funcs_or;
  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_TRUE(funcs[0].is_function);
  EXPECT_EQ(funcs[0].name, "SpecialSmallFunc");
  EXPECT_NE(funcs[0].emitted_assembly.find(".ent SpecialSmallFunc"), std::string::npos);
  EXPECT_NE(funcs[0].emitted_assembly.find(".end SpecialSmallFunc"), std::string::npos);
}

TEST(DisassemblerTest, SmallBlockWithKnownEntrypointPreservesFunctionFraming) {
  // 4-byte block registered in known_entrypoints_
  std::vector<uint8_t> code;
  AppendWord(code, 0x24020001);  // addiu $v0, $zero, 1

  Disassembler disasm;
  disasm.RegisterFunctionEntrypoint(0x80700000);

  auto funcs_or = disasm.DisassembleAllFunctions(code, 0x80700000);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();

  const auto& funcs = *funcs_or;
  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_TRUE(funcs[0].is_function);
  EXPECT_NE(funcs[0].emitted_assembly.find(".ent func_80700000"), std::string::npos);
  EXPECT_NE(funcs[0].emitted_assembly.find(".end func_80700000"), std::string::npos);
}

TEST(DisassemblerTest, TrailingPaddingAfterAlignmentNops) {
  // Function: jr $ra; nop (8 bytes)
  // Followed by 2 alignment NOPs (8 bytes of zeros) -> total function size 16 bytes
  // Followed by 1 trailing padding word: 0x3322FFFF (4 bytes)
  std::vector<uint8_t> code;
  AppendWord(code, 0x03E00008);  // jr $ra
  AppendWord(code, 0x00000000);  // nop (delay slot)
  AppendWord(code, 0x00000000);  // alignment nop
  AppendWord(code, 0x00000000);  // alignment nop
  AppendWord(code, 0x3322FFFF);  // trailing padding

  Disassembler disasm;
  auto funcs_or = disasm.DisassembleAllFunctions(code, 0x80700000);
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();

  const auto& funcs = *funcs_or;
  ASSERT_EQ(funcs.size(), 2u);

  // Function absorbs alignment NOPs
  EXPECT_TRUE(funcs[0].is_function);
  EXPECT_EQ(funcs[0].vram_start, 0x80700000u);
  EXPECT_EQ(funcs[0].Size(), 16u);

  // Trailing word is padding without function framing
  EXPECT_FALSE(funcs[1].is_function);
  EXPECT_EQ(funcs[1].vram_start, 0x80700010u);
  EXPECT_EQ(funcs[1].Size(), 4u);
  EXPECT_EQ(funcs[1].emitted_assembly.find(".ent"), std::string::npos);
  EXPECT_EQ(funcs[1].emitted_assembly.find(".end"), std::string::npos);
}

}  // namespace
}  // namespace rom_nom_nom
