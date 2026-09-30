#include "splitter/linker_script.h"

#include <string>
#include <string_view>

#include "absl/strings/match.h"
#include "gtest/gtest.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

constexpr std::string_view kSymbolsProtoText = R"pb(
  entries { name: "g_boot_flag" address: 0x80025C00 type: SYMBOL_DATA }
  entries { name: "MainProc" address: 0x80026000 type: SYMBOL_FUNC }
)pb";

constexpr std::string_view kConfigProtoText = R"pb(
  game_name: "test-game"
  sha1: "0000000000000000000000000000000000000000"
  basename: "test-game"
  segments { name: "header" type: SEGMENT_HEADER rom_start: 0x0 rom_end: 0x40 }
  segments {
    name: "entry"
    type: SEGMENT_CODE
    rom_start: 0x1000
    rom_end: 0x1060
    vram: 0x80025C00
    subsegments { name: "entry_asm" type: SUBSEGMENT_ASM rom_start: 0x1000 }
  }
  segments {
    name: "main"
    type: SEGMENT_CODE
    rom_start: 0x1060
    rom_end: 0x2000
    subsegments { name: "boot" type: SUBSEGMENT_C rom_start: 0x1060 }
    subsegments { name: "13F0" type: SUBSEGMENT_ASM rom_start: 0x13F0 }
    subsegments { name: "data/EBE10.data" type: SUBSEGMENT_DATA rom_start: 0x1800 }
  }
)pb";

TEST(LinkerScriptTest, GenerateSymbolsScript) {
  auto symbols_or = SymbolIndex::ParseFromTextproto(kSymbolsProtoText);
  ASSERT_TRUE(symbols_or.ok());

  LinkerScriptGenerator generator;
  std::string script = generator.GenerateSymbolsScript(*symbols_or);

  EXPECT_EQ(script, R"(/* Auto-generated global symbol definitions */
g_boot_flag = 0x80025C00;
MainProc = 0x80026000;
)");
}

TEST(LinkerScriptTest, GenerateHardwareRegsScript) {
  LinkerScriptGenerator generator;
  std::string hw_regs = generator.GenerateHardwareRegsScript();

  EXPECT_TRUE(absl::StrContains(hw_regs, "SP_STATUS_REG = 0xA4040010;"));
  EXPECT_TRUE(absl::StrContains(hw_regs, "MI_INTR_MASK_REG = 0xA430000C;"));
  EXPECT_TRUE(absl::StrContains(hw_regs, "VI_CURRENT_REG = 0xA4400010;"));
}

TEST(LinkerScriptTest, GenerateMainScriptMatchesExpectedOutput) {
  auto config_or = ParseSplitConfig(kConfigProtoText);
  ASSERT_TRUE(config_or.ok());

  LinkerScriptGenerator generator;
  auto script_or = generator.GenerateMainScript(*config_or);
  ASSERT_TRUE(script_or.ok());

  EXPECT_EQ(*script_or, R"(SECTIONS
{
    HIDDEN(__romPos = 0);
    header_ROM_START = __romPos;
    header_VRAM = ADDR(.header);
    .header : AT(header_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        header_DATA_START = .;
        build/test-game/asm/test-game/header.o(.data);
        header_DATA_END = .;
        header_DATA_SIZE = ABSOLUTE(header_DATA_END - header_DATA_START);
    }
    __romPos += SIZEOF(.header);
    header_ROM_END = __romPos;
    header_VRAM_END = .;

    entry_ROM_START = __romPos;
    entry_VRAM = ADDR(.entry);
    .entry 0x80025C00 : AT(entry_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        entry_TEXT_START = .;
        build/test-game/asm/test-game/entry_asm.o(.text);
        . = ALIGN(., 16);
        entry_TEXT_END = .;
        entry_TEXT_SIZE = ABSOLUTE(entry_TEXT_END - entry_TEXT_START);
        entry_DATA_START = .;
        build/test-game/asm/test-game/entry_asm.o(.data);
        . = ALIGN(., 16);
        entry_DATA_END = .;
        entry_DATA_SIZE = ABSOLUTE(entry_DATA_END - entry_DATA_START);
        entry_RODATA_START = .;
        build/test-game/asm/test-game/entry_asm.o(.rodata);
        . = ALIGN(., 16);
        entry_RODATA_END = .;
        entry_RODATA_SIZE = ABSOLUTE(entry_RODATA_END - entry_RODATA_START);
    }
    entry_bss_VRAM = ADDR(.entry_bss);
    .entry_bss (NOLOAD) : SUBALIGN(16)
    {
        FILL(0x00000000);
        entry_BSS_START = .;
        build/test-game/asm/test-game/entry_asm.o(.bss);
        . = ALIGN(., 16);
        entry_BSS_END = .;
        entry_BSS_SIZE = ABSOLUTE(entry_BSS_END - entry_BSS_START);
    }
    __romPos += SIZEOF(.entry);
    __romPos = ALIGN(__romPos, 16);
    . = ALIGN(., 16);
    entry_ROM_END = __romPos;
    entry_VRAM_END = .;

    main_ROM_START = __romPos;
    main_VRAM = ADDR(.main);
    .main entry_VRAM_END : AT(main_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        main_TEXT_START = .;
        build/test-game/src/c/test-game/boot.o(.text);
        build/test-game/asm/test-game/13F0.o(.text);
        . = ALIGN(., 16);
        main_TEXT_END = .;
        main_TEXT_SIZE = ABSOLUTE(main_TEXT_END - main_TEXT_START);
        main_DATA_START = .;
        build/test-game/src/c/test-game/boot.o(.data);
        build/test-game/asm/test-game/13F0.o(.data);
        build/test-game/asm/test-game/data/EBE10.data.o(.data);
        . = ALIGN(., 16);
        main_DATA_END = .;
        main_DATA_SIZE = ABSOLUTE(main_DATA_END - main_DATA_START);
        main_RODATA_START = .;
        build/test-game/src/c/test-game/boot.o(.rodata);
        build/test-game/asm/test-game/13F0.o(.rodata);
        . = ALIGN(., 16);
        main_RODATA_END = .;
        main_RODATA_SIZE = ABSOLUTE(main_RODATA_END - main_RODATA_START);
    }
    main_bss_VRAM = ADDR(.main_bss);
    .main_bss (NOLOAD) : SUBALIGN(16)
    {
        FILL(0x00000000);
        main_BSS_START = .;
        build/test-game/src/c/test-game/boot.o(.bss);
        build/test-game/asm/test-game/13F0.o(.bss);
        . = ALIGN(., 16);
        main_BSS_END = .;
        main_BSS_SIZE = ABSOLUTE(main_BSS_END - main_BSS_START);
    }
    __romPos += SIZEOF(.main);
    __romPos = ALIGN(__romPos, 16);
    . = ALIGN(., 16);
    main_ROM_END = __romPos;
    main_VRAM_END = .;

    /DISCARD/ :
    {
        *(*);
    }
}
)");
}

TEST(LinkerScriptTest, NumericSegmentNameSanitizedAndBssSubsegmentHandled) {
  constexpr std::string_view kNumericConfigText = R"pb(
    game_name: "test-game"
    sha1: "0000000000000000000000000000000000000000"
    basename: "test-game"
    segments {
      name: "main"
      type: SEGMENT_CODE
      rom_start: 0x1000
      rom_end: 0x2000
      vram: 0x80000000
      subsegments { name: "code" type: SUBSEGMENT_ASM rom_start: 0x1000 }
      subsegments { name: "FF5C0" type: SUBSEGMENT_BSS vram: 0x80010000 }
    }
    segments { name: "3F1B0" type: SEGMENT_BIN rom_start: 0x2000 rom_end: 0x3000 }
  )pb";
  auto config_or = ParseSplitConfig(kNumericConfigText);
  ASSERT_TRUE(config_or.ok()) << config_or.status();

  LinkerScriptOptions options;
  options.base_build_dir = "build";
  LinkerScriptGenerator generator(options);

  auto script_or = generator.GenerateMainScript(*config_or);
  ASSERT_TRUE(script_or.ok()) << script_or.status();

  // BSS subsegment inside main should resolve to data/FF5C0.bss.o
  EXPECT_TRUE(
      absl::StrContains(*script_or, "build/test-game/asm/test-game/data/FF5C0.bss.o(.bss);"));

  // Numeric segment 3F1B0 should have section ._3F1B0 and symbol _3F1B0_ROM_START
  EXPECT_TRUE(absl::StrContains(*script_or, "_3F1B0_ROM_START = __romPos;"));
  EXPECT_TRUE(absl::StrContains(*script_or, "._3F1B0"));
  EXPECT_TRUE(absl::StrContains(*script_or, "build/test-game/assets/test-game/3F1B0.o(.data);"));
}

TEST(LinkerScriptTest, ResolveObjectPathPrefixHandling) {
  Segment seg;
  seg.set_name("main");
  seg.set_type(SEGMENT_CODE);

  Subsegment sub;
  sub.set_name("boot");
  sub.set_type(SUBSEGMENT_C);

  // 1. Default base_build_dir ("build") appends game_name: "build/test-game/src/c/test-game/boot.o"
  LinkerScriptOptions opt_default;
  opt_default.base_build_dir = "build";
  LinkerScriptGenerator gen_default(opt_default);
  EXPECT_EQ(gen_default.ResolveObjectPath("test-game", seg, &sub),
            "build/test-game/src/c/test-game/boot.o");

  // 2. base_build_dir already ending in game_name does not duplicate it:
  LinkerScriptOptions opt_game_dir;
  opt_game_dir.base_build_dir = "build/test-game";
  LinkerScriptGenerator gen_game_dir(opt_game_dir);
  EXPECT_EQ(gen_game_dir.ResolveObjectPath("test-game", seg, &sub),
            "build/test-game/src/c/test-game/boot.o");

  // 3. Empty base_build_dir uses game_name directly:
  LinkerScriptOptions opt_empty;
  opt_empty.base_build_dir = "";
  LinkerScriptGenerator gen_empty(opt_empty);
  EXPECT_EQ(gen_empty.ResolveObjectPath("test-game", seg, &sub),
            "test-game/src/c/test-game/boot.o");
}

}  // namespace
}  // namespace rom_nom_nom
