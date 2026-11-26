#include "lifter/stage0_test_utils.h"

#include <cstdint>
#include <vector>

#include "core/rtl_cfg.h"
#include "gtest/gtest.h"
#include "lifter/pass00_final_and_gas.h"

namespace rom_nom_nom::lifter {
namespace {

Stage0FunctionInput MakePureAluInput() {
  Stage0FunctionInput input;
  input.machine_words = {
      0x00851021u,  // addu $v0, $a0, $a1
      0x03e00008u,  // jr $ra
      0x24420001u,  // addiu $v0, $v0, 1
  };
  input.profile.optimize_level = 2;
  input.profile.c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  return input;
}

TEST(Stage0TestUtilsTest, PureAluRelativeBranchFunctionIsNotDistinctAndLiftsIdentically) {
  Stage0FunctionInput reloc = MakePureAluInput();
  // Relative branch (`beq $a0, $zero, +3` -> index 4) has no relocations, tables, or `j`/`jal`.
  reloc.machine_words = {
      0x10800003u,  // beq $a0, $zero, +3 (target = word index 4)
      0x00851021u,  // addu $v0, $a0, $a1
      0x03e00008u,  // jr $ra
      0x24420001u,  // addiu $v0, $v0, 1
      0x03e00008u,  // jr $ra
      0x00851023u,  // subu $v0, $a0, $a1
  };
  const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
  EXPECT_EQ(linked.base_vram, 0x80020000u);
  EXPECT_EQ(linked.gp_vram, 0x80060000u);
  EXPECT_FALSE(HasDistinctLinkedRomRepresentation(reloc, linked));

  const RtlControlFlowGraph lifted_reloc = InvertFinalAndGasFunction(reloc);
  const RtlControlFlowGraph lifted_linked = InvertFinalAndGasFunction(linked);
  ASSERT_FALSE(lifted_reloc.rpo_blocks.empty());
  EXPECT_TRUE(lifted_linked.CompareAgainst(lifted_reloc).MatchesExactly());
  EXPECT_EQ(lifted_linked.sideband, lifted_reloc.sideband);
}

TEST(Stage0TestUtilsTest, RelocationsJumpTablesConstantPoolsAndGpDataAreDistinct) {
  // 1. External relocation (`kMips26` `jal`).
  {
    Stage0FunctionInput reloc = MakePureAluInput();
    reloc.machine_words = {0x0c000000u, 0x00000000u, 0x03e00008u, 0x00000000u};
    reloc.relocations = {{0u, Stage0RelocKind::kMips26, "callee", 0}};
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
    EXPECT_TRUE(HasDistinctLinkedRomRepresentation(reloc, linked));
    EXPECT_TRUE(linked.relocations.empty());
    EXPECT_FALSE(linked.vram_to_symbol.empty());
  }

  // 2. Jump table.
  {
    Stage0FunctionInput reloc = MakePureAluInput();
    reloc.jump_tables = {{"$L5", {0u, 1u}, 0u}};
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
    EXPECT_TRUE(HasDistinctLinkedRomRepresentation(reloc, linked));
    ASSERT_EQ(linked.jump_tables.size(), 1u);
    EXPECT_NE(linked.jump_tables[0].table_vram, 0u);
    EXPECT_TRUE(linked.jump_tables[0].label_name.empty());
  }

  // 3. Constant pool entry.
  {
    Stage0FunctionInput reloc = MakePureAluInput();
    reloc.constant_pool = {{"$LC0", ".rdata", 8u, {0x3ff00000u, 0x00000000u}, 0u}};
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
    EXPECT_TRUE(HasDistinctLinkedRomRepresentation(reloc, linked));
    ASSERT_EQ(linked.constant_pool.size(), 1u);
    EXPECT_NE(linked.constant_pool[0].entry_vram, 0u);
    EXPECT_TRUE(linked.constant_pool[0].label_name.empty());
  }

  // 4. `$gp` small-data symbol sizes in profile.
  {
    Stage0FunctionInput reloc = MakePureAluInput();
    reloc.profile.gp_symbol_sizes["g_small"] = 4u;
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
    EXPECT_TRUE(HasDistinctLinkedRomRepresentation(reloc, linked));
    EXPECT_TRUE(linked.profile.gp_symbol_sizes.empty());
  }
}

TEST(Stage0TestUtilsTest, UnrelocatedJalOrJAndEmptyCFlagsAreAlwaysDistinct) {
  // 1. Unrelocated `jal 0x0` (`0x0c000000u`): `invert_gas_macro.cc:2302-2317`
  //    rewrites unrelocated `jal` differently under `is_linked_rom`.
  {
    Stage0FunctionInput reloc = MakePureAluInput();
    reloc.machine_words = {
        0x0c000000u,  // jal 0x0 (unrelocated)
        0x00000000u,  // nop
        0x03e00008u,  // jr $ra
        0x00000000u,  // nop
    };
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
    EXPECT_EQ(reloc.machine_words, linked.machine_words);
    EXPECT_TRUE(HasDistinctLinkedRomRepresentation(reloc, linked));
  }

  // 2. Unrelocated `j 0x0` (`0x08000000u`).
  {
    Stage0FunctionInput reloc = MakePureAluInput();
    reloc.machine_words = {
        0x08000002u,  // j word 2
        0x00000000u,  // nop
        0x03e00008u,  // jr $ra
        0x00000000u,  // nop
    };
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
    EXPECT_NE(reloc.machine_words, linked.machine_words);
    EXPECT_TRUE(HasDistinctLinkedRomRepresentation(reloc, linked));
  }

  // 3. Empty `c_flags`: `invert_gas_macro.cc:2153-2156` takes the pure-word
  //    fast path only when `base_vram == 0 && c_flags.empty()`.
  {
    Stage0FunctionInput reloc = MakePureAluInput();
    reloc.profile.c_flags.clear();
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(reloc);
    EXPECT_TRUE(HasDistinctLinkedRomRepresentation(reloc, linked));
  }
}

}  // namespace
}  // namespace rom_nom_nom::lifter
