#include "lifter/module_symbol_scanner.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/game_config.h"
#include "core/symbol_registry.h"
#include "core/target_extractor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/function_loader.h"
#include "lifter/lifter_pipeline.h"

namespace rom_nom_nom {
namespace {

TEST(ModuleSymbolScannerTest, DiscoverAllCodeModulesFiltersToCodeSegmentCSubsegments) {
  std::string proto_cfg = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "code"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x3000
  vram: 0x80020000
  subsegments { name: "mod_c" type: SUBSEGMENT_C rom_start: 0x1000 }
  subsegments { name: "mod_asm" type: SUBSEGMENT_ASM rom_start: 0x1400 }
  subsegments { name: "mod_data" type: SUBSEGMENT_DATA rom_start: 0x1800 }
  subsegments { name: "mod_bss" type: SUBSEGMENT_BSS vram: 0x80030000 }
}
segments {
  name: "assets"
  type: SEGMENT_BIN
  rom_start: 0x3000
  rom_end: 0x4000
}
)";
  auto cfg_or = ParseGameConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::vector<std::string> modules = ModuleSymbolScanner::DiscoverAllCodeModules(*cfg_or);
  EXPECT_THAT(modules, ::testing::ElementsAre("mod_c"));
}

TEST(ModuleSymbolScannerTest, DiscoverFunctionsInModuleResolvesSymbolRangeAndSynthesizesFallback) {
  std::string proto_cfg = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "code"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x2000
  vram: 0x80020000
  subsegments { name: "src/mod_a" type: SUBSEGMENT_C rom_start: 0x1000 }
  subsegments { name: "src/mod_empty" type: SUBSEGMENT_C rom_start: 0x1800 }
}
)";
  auto cfg_or = ParseGameConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries { name: "FuncA1" address: 0x80020000 type: SYMBOL_FUNC }
entries { name: "FuncA2" address: 0x80020100 type: SYMBOL_FUNC }
)";
  auto sym_idx_or = SymbolIndex::ParseFromTextproto(proto_syms);
  ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();

  auto test_dir = std::filesystem::temp_directory_path() / "module_symbol_scanner_unit_test";
  std::filesystem::remove_all(test_dir);

  // Lookup by filename stem "mod_a" resolves FuncA1 and FuncA2
  auto funcs_a = ModuleSymbolScanner::DiscoverFunctionsInModule("mod_a", *cfg_or, *sym_idx_or,
                                                                test_dir, "test-game", "");
  ASSERT_TRUE(funcs_a.ok()) << funcs_a.status();
  EXPECT_THAT(*funcs_a, ::testing::ElementsAre("FuncA1", "FuncA2"));

  // Module with no registered function symbols synthesizes start_vram symbol name
  auto funcs_empty = ModuleSymbolScanner::DiscoverFunctionsInModule(
      "mod_empty", *cfg_or, *sym_idx_or, test_dir, "test-game", "");
  ASSERT_TRUE(funcs_empty.ok()) << funcs_empty.status();
  EXPECT_THAT(*funcs_empty, ::testing::ElementsAre("func_80020800"));

  // Unknown module returns NotFoundError
  auto funcs_missing = ModuleSymbolScanner::DiscoverFunctionsInModule(
      "does_not_exist", *cfg_or, *sym_idx_or, test_dir, "test-game", "");
  EXPECT_FALSE(funcs_missing.ok());
}

TEST(ModuleSymbolScannerTest, GetFunctionsInModuleFromStandaloneAsmFile) {
  std::filesystem::path test_dir =
      std::filesystem::temp_directory_path() / "lifter_standalone_asm_test";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir / "asm");

  // Create standalone assembly file test_module.s
  std::filesystem::path asm_file = test_dir / "asm" / "test_module.s";
  {
    std::ofstream out(asm_file);
    out << "# Test assembly module with standalone functions\n"
        << ".text\n"
        << "\t.ent FuncFromEnt\n"
        << "glabel FuncFromEnt\n"
        << "\tjr $ra\n"
        << "\tnop\n"
        << "\t.end FuncFromEnt\n\n"
        << "  glabel FuncFromGlabelOnly\n"
        << "\tjr $ra\n"
        << "\tnop\n\n"
        << "\t.ent FuncFromEntOnly\n"
        << "\tjr $ra\n"
        << "\tnop\n"
        << "\t.end FuncFromEntOnly\n";
  }

  std::string proto_cfg = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "code"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x2000
  vram: 0x80020000
  subsegments {
    name: "test_module"
    type: SUBSEGMENT_C
    rom_start: 0x1000
    vram: 0x80020000
  }
}
)";
  auto cfg_or = ParseGameConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries {
  name: "FallbackFunc"
  address: 0x80020000
  type: SYMBOL_FUNC
}
)";
  auto sym_idx_or = SymbolIndex::ParseFromTextproto(proto_syms);
  ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();

  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = test_dir;
  extractor_opts.game_name = "test-game";

  auto extractor = std::make_unique<TargetExtractor>(extractor_opts, &(*cfg_or), &(*sym_idx_or));
  auto loader = std::make_unique<FunctionLoader>(std::move(extractor));

  LifterPipelineOptions options;
  options.asm_dir = test_dir / "asm";
  LifterPipeline pipeline(options, std::move(loader));

  auto funcs_or = pipeline.GetFunctionsInModule("test_module");
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  EXPECT_THAT(*funcs_or,
              ::testing::ElementsAre("FuncFromEnt", "FuncFromGlabelOnly", "FuncFromEntOnly"));

  std::filesystem::remove_all(test_dir);
}

TEST(ModuleSymbolScannerTest, GetFunctionsInModulePrefersNonmatchingsOverStandaloneAsm) {
  std::filesystem::path test_dir =
      std::filesystem::temp_directory_path() / "lifter_pref_nonmatchings_test";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir / "asm" / "nonmatchings" / "test_module");

  // Standalone file with glabel that should be ignored when nonmatchings dir exists with .s files
  std::filesystem::path asm_file = test_dir / "asm" / "test_module.s";
  {
    std::ofstream out(asm_file);
    out << "glabel IgnoredStandaloneFunc\n";
  }

  // Nonmatching file in subdir
  std::filesystem::path nonmatching_file =
      test_dir / "asm" / "nonmatchings" / "test_module" / "func_80010000.s";
  {
    std::ofstream out(nonmatching_file);
    out << "glabel func_80010000\n";
  }

  std::string proto_cfg = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "code"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x2000
  vram: 0x80020000
  subsegments {
    name: "test_module"
    type: SUBSEGMENT_C
    rom_start: 0x1000
    vram: 0x80020000
  }
}
)";
  auto cfg_or = ParseGameConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries {
  name: "FallbackFunc"
  address: 0x80020000
  type: SYMBOL_FUNC
}
)";
  auto sym_idx_or = SymbolIndex::ParseFromTextproto(proto_syms);
  ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();

  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = test_dir;
  extractor_opts.game_name = "test-game";

  auto extractor = std::make_unique<TargetExtractor>(extractor_opts, &(*cfg_or), &(*sym_idx_or));
  auto loader = std::make_unique<FunctionLoader>(std::move(extractor));

  LifterPipelineOptions options;
  options.asm_dir = test_dir / "asm";
  LifterPipeline pipeline(options, std::move(loader));

  auto funcs_or = pipeline.GetFunctionsInModule("test_module");
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  EXPECT_THAT(*funcs_or, ::testing::ElementsAre("func_80010000"));

  std::filesystem::remove_all(test_dir);
}

TEST(ModuleSymbolScannerTest, GetFunctionsInModuleFallsBackToConfigWhenNoAsmFiles) {
  std::filesystem::path test_dir =
      std::filesystem::temp_directory_path() / "lifter_fallback_config_test";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir / "asm");

  std::string proto_cfg = R"(
game_name: "test-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "test-game"
segments {
  name: "code"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x2000
  vram: 0x80020000
  subsegments {
    name: "test_module"
    type: SUBSEGMENT_C
    rom_start: 0x1000
    vram: 0x80020000
  }
}
)";
  auto cfg_or = ParseGameConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries {
  name: "FallbackFunc"
  address: 0x80020000
  type: SYMBOL_FUNC
}
)";
  auto sym_idx_or = SymbolIndex::ParseFromTextproto(proto_syms);
  ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();

  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = test_dir;
  extractor_opts.game_name = "test-game";

  auto extractor = std::make_unique<TargetExtractor>(extractor_opts, &(*cfg_or), &(*sym_idx_or));
  auto loader = std::make_unique<FunctionLoader>(std::move(extractor));

  LifterPipelineOptions options;
  options.asm_dir = test_dir / "asm";
  LifterPipeline pipeline(options, std::move(loader));

  auto funcs_or = pipeline.GetFunctionsInModule("test_module");
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  EXPECT_THAT(*funcs_or, ::testing::ElementsAre("FallbackFunc"));

  std::filesystem::remove_all(test_dir);
}

}  // namespace
}  // namespace rom_nom_nom
