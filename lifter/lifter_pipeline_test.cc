#include "lifter/lifter_pipeline.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/c_ast.h"
#include "core/mips.h"
#include "differ/target_extractor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/function_loader.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

TEST(LifterPipelineTest, EndToEndLinearFunction) {
  // Simple leaf function:
  // 0x00: addu $v0, $a0, $a1
  // 0x04: jr $ra
  // 0x08: nop
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  auto loaded_or = FunctionLoader::FromWords("AddTwo", words, 0x80025CB4);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  LifterPipelineOptions options;
  options.includes = {"common.h", "types.h"};
  options.format_with_clang = false;

  LifterPipeline pipeline(options);
  auto result_or = pipeline.DecompileFunction(*loaded_or);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  EXPECT_EQ(result_or->function_name, "AddTwo");
  EXPECT_EQ(result_or->vram, 0x80025CB4);
  EXPECT_EQ(result_or->instruction_count, 3u);
  ASSERT_THAT(result_or->ast, NotNull());

  EXPECT_THAT(result_or->c_code, HasSubstr("#include \"common.h\""));
  EXPECT_THAT(result_or->c_code, HasSubstr("#include \"types.h\""));
  EXPECT_THAT(result_or->c_code, HasSubstr("s32 AddTwo(s32 arg0, s32 arg1) {"));
  EXPECT_THAT(result_or->c_code, HasSubstr("s32 v0;"));
  EXPECT_THAT(result_or->c_code, HasSubstr("v0 = (arg0 + arg1);"));
  EXPECT_THAT(result_or->c_code, HasSubstr("return v0;"));
}

TEST(LifterPipelineTest, EndToEndBranchFunction) {
  // Branching function:
  // 0x00: bne $a0, $zero, .L1 (0x10)
  // 0x04: nop
  // 0x08: addiu $v0, $zero, 10
  // 0x0C: jr $ra
  // 0x10: nop
  // 0x14: addiu $v0, $zero, 20
  // 0x18: jr $ra
  // 0x1C: nop
  std::vector<uint32_t> words = {
      0x14800003,  // bne $a0, $zero, 0x10 (.L1)
      0x00000000,  // nop
      0x2402000A,  // addiu $v0, $zero, 10
      0x03E00008,  // jr $ra
      0x00000000,  // nop
      0x24020014,  // addiu $v0, $zero, 20
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  auto loaded_or = FunctionLoader::FromWords("GetBranchValue", words, 0x80025D00);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  LifterPipelineOptions options;
  options.format_with_clang = false;

  LifterPipeline pipeline(options);
  auto result_or = pipeline.DecompileFunction(*loaded_or);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  EXPECT_EQ(result_or->function_name, "GetBranchValue");
  EXPECT_THAT(result_or->c_code, HasSubstr("GetBranchValue(s32 arg0) {"));
  EXPECT_THAT(result_or->c_code, HasSubstr("if ("));
  EXPECT_THAT(result_or->c_code, HasSubstr("return v0;"));
}

TEST(LifterPipelineTest, EndToEndSwitchFunction) {
  std::vector<uint32_t> words = {
      0x2C820002,  // 00: sltiu $v0, $a0, 2
      0x1040000A,  // 04: beqz  $v0, 0x30 (.L_default / join)
      0x00041080,  // 08: sll   $v0, $a0, 2
      0x3C018002,  // 0C: lui   $at, 0x8002
      0x00220821,  // 10: addu  $at, $at, $v0
      0x8C221000,  // 14: lw    $v0, 0x1000($at)
      0x00400008,  // 18: jr    $v0
      0x00000000,  // 1C: nop
      0x24020064,  // 20: addiu $v0, $zero, 100 (.L_case0)
      0x0800974C,  // 24: j     0x80025D30 (join)
      0x00000000,  // 28: nop
      0x240200C8,  // 2C: addiu $v0, $zero, 200 (.L_case1)
      0x03E00008,  // 30: jr    $ra (join)
      0x00000000,  // 34: nop
  };

  uint32_t table_addr = 0x80021000;
  absl::flat_hash_map<uint32_t, uint32_t> memory = {
      {table_addr, 0x80025D20},
      {table_addr + 4, 0x80025D2C},
  };
  auto memory_reader = [memory](uint32_t vram) -> std::optional<uint32_t> {
    auto it = memory.find(vram);
    if (it != memory.end()) return it->second;
    return std::nullopt;
  };

  auto loaded_or = FunctionLoader::FromWords("GetSwitchValue", words, 0x80025D00, nullptr, nullptr,
                                             memory_reader);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  LifterPipelineOptions options;
  options.format_with_clang = false;

  LifterPipeline pipeline(options);
  auto result_or = pipeline.DecompileFunction(*loaded_or);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  EXPECT_EQ(result_or->function_name, "GetSwitchValue");
  EXPECT_THAT(result_or->c_code, HasSubstr("switch ("));
  EXPECT_THAT(result_or->c_code, HasSubstr("case 0:"));
  EXPECT_THAT(result_or->c_code, HasSubstr("case 1:"));
  EXPECT_THAT(result_or->c_code, HasSubstr("break;"));
  EXPECT_THAT(result_or->c_code, HasSubstr("return v0;"));
}

TEST(LifterPipelineTest, EndToEndSwitchFunctionWithoutMemoryReaderGracefullySkips) {
  std::vector<uint32_t> words = {
      0x2C820002,  // 00: sltiu $v0, $a0, 2
      0x1040000A,  // 04: beqz  $v0, 0x30
      0x00041080,  // 08: sll   $v0, $a0, 2
      0x3C018002,  // 0C: lui   $at, 0x8002
      0x00220821,  // 10: addu  $at, $at, $v0
      0x8C221000,  // 14: lw    $v0, 0x1000($at)
      0x00400008,  // 18: jr    $v0
      0x00000000,  // 1C: nop
      0x03E00008,  // 20: jr    $ra
      0x00000000,  // 24: nop
  };

  // No memory_reader provided
  auto loaded_or = FunctionLoader::FromWords("GetSwitchNoMem", words, 0x80025D00);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  LifterPipelineOptions options;
  options.format_with_clang = false;

  LifterPipeline pipeline(options);
  auto result_or = pipeline.DecompileFunction(*loaded_or);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_EQ(result_or->function_name, "GetSwitchNoMem");
}

TEST(LifterPipelineTest, EmptyInstructionsFails) {
  LoadedFunction empty_func;
  empty_func.name = "Empty";

  LifterPipeline pipeline({});
  auto result_or = pipeline.DecompileFunction(empty_func);
  EXPECT_FALSE(result_or.ok());
}

TEST(LifterPipelineTest, DecompileFunctionsEmptyListFails) {
  LifterPipeline pipeline({});
  auto result_or = pipeline.DecompileFunctions({});
  EXPECT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(LifterPipelineTest, GetAllModulesUninitializedFails) {
  LifterPipeline pipeline({});
  auto modules_or = pipeline.GetAllModules();
  EXPECT_FALSE(modules_or.ok());
  EXPECT_EQ(modules_or.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(LifterPipelineTest, GetAllModulesAndDecompileAllModules) {
  auto test_dir = std::filesystem::temp_directory_path() / "lifter_all_modules_test";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir / "config");
  std::filesystem::create_directories(test_dir / "roms");

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
    name: "mod_alpha"
    type: SUBSEGMENT_C
    rom_start: 0x1000
    vram: 0x80020000
  }
}
)";
  auto cfg_or = ParseSplitConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries {
  name: "AlphaFunc"
  address: 0x80020000
  type: SYMBOL_FUNC
}
entries {
  name: "NextFunc"
  address: 0x80020008
  type: SYMBOL_FUNC
}
)";
  auto sym_idx_or = SymbolIndex::ParseFromTextproto(proto_syms);
  ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();

  std::vector<uint8_t> rom(0x2000, 0);
  // jr $ra (0x03E00008)
  rom[0x1000] = 0x03;
  rom[0x1001] = 0xE0;
  rom[0x1002] = 0x00;
  rom[0x1003] = 0x08;

  std::filesystem::path rom_path = test_dir / "roms" / "test-game.z64";
  std::ofstream rom_file(rom_path, std::ios::binary);
  rom_file.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  rom_file.close();

  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = test_dir;
  extractor_opts.game_name = "test-game";

  auto extractor = std::make_unique<TargetExtractor>(extractor_opts, &(*cfg_or), &(*sym_idx_or));
  auto loader = std::make_unique<FunctionLoader>(std::move(extractor));

  LifterPipelineOptions options;
  options.repo_root = test_dir;
  options.game_name = "test-game";
  options.format_with_clang = false;

  LifterPipeline pipeline(options, std::move(loader));

  auto modules_or = pipeline.GetAllModules();
  ASSERT_TRUE(modules_or.ok()) << modules_or.status();
  EXPECT_THAT(*modules_or, ::testing::ElementsAre("mod_alpha"));

  std::filesystem::path out_dir = test_dir / "lifted";
  auto files_or = pipeline.DecompileAllModules(out_dir);
  ASSERT_TRUE(files_or.ok()) << files_or.status();
  EXPECT_THAT(*files_or, ::testing::SizeIs(1));

  std::filesystem::path alpha_c = out_dir / "mod_alpha.c";
  EXPECT_TRUE(std::filesystem::exists(alpha_c));

  std::ifstream ifs(alpha_c);
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  EXPECT_THAT(content, HasSubstr("AlphaFunc"));

  std::filesystem::remove_all(test_dir);
}

TEST(LifterPipelineTest, DecompileAllModulesCopiesHeadersAndLiftsSources) {
  auto test_dir = std::filesystem::temp_directory_path() / "lifter_headers_lift_test";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir / "src" / "c" / "test-game");
  std::filesystem::create_directories(test_dir / "roms");

  // Create handwritten header and source
  std::ofstream hdr(test_dir / "src" / "c" / "test-game" / "mod_alpha.h");
  hdr << "// Handwritten header\n";
  hdr.close();

  std::ofstream src(test_dir / "src" / "c" / "test-game" / "mod_alpha.c");
  src << "// Handwritten source\n";
  src.close();

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
    name: "mod_alpha"
    type: SUBSEGMENT_C
    rom_start: 0x1000
    vram: 0x80020000
  }
}
)";
  auto cfg_or = ParseSplitConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries {
  name: "AlphaFunc"
  address: 0x80020000
  type: SYMBOL_FUNC
}
)";
  auto sym_idx_or = SymbolIndex::ParseFromTextproto(proto_syms);
  ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();

  std::vector<uint8_t> rom(0x2000, 0);
  // jal 0x80054321 (0x0C0150C8)
  rom[0x1000] = 0x0C;
  rom[0x1001] = 0x01;
  rom[0x1002] = 0x50;
  rom[0x1003] = 0xC8;
  // jr $ra (0x03E00008)
  rom[0x1004] = 0x03;
  rom[0x1005] = 0xE0;
  rom[0x1006] = 0x00;
  rom[0x1007] = 0x08;

  std::filesystem::path rom_path = test_dir / "roms" / "test-game.z64";
  std::ofstream rom_file(rom_path, std::ios::binary);
  rom_file.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  rom_file.close();

  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = test_dir;
  extractor_opts.game_name = "test-game";

  auto extractor = std::make_unique<TargetExtractor>(extractor_opts, &(*cfg_or), &(*sym_idx_or));
  auto loader = std::make_unique<FunctionLoader>(std::move(extractor));

  LifterPipelineOptions options;
  options.repo_root = test_dir;
  options.game_name = "test-game";
  options.format_with_clang = false;

  LifterPipeline pipeline(options, std::move(loader));

  std::filesystem::path out_dir = test_dir / "lifted";
  auto files_or = pipeline.DecompileAllModules(out_dir);
  ASSERT_TRUE(files_or.ok()) << files_or.status();
  EXPECT_THAT(*files_or, ::testing::SizeIs(1));

  // Handwritten headers should NOT be copied into lifted output
  EXPECT_FALSE(std::filesystem::exists(out_dir / "mod_alpha.h"));

  // Purely generated standard types.h should be generated
  EXPECT_TRUE(std::filesystem::exists(out_dir / "types.h"));
  std::ifstream types_input_stream(out_dir / "types.h");
  std::string types_file_content((std::istreambuf_iterator<char>(types_input_stream)),
                                 std::istreambuf_iterator<char>());
  EXPECT_THAT(types_file_content, HasSubstr("typedef signed int s32;"));

  // Verify lifted_symbols.ld was generated and contains unresolved func_80054320
  EXPECT_TRUE(std::filesystem::exists(out_dir / "lifted_symbols.ld"));
  std::ifstream symbols_input_stream(out_dir / "lifted_symbols.ld");
  std::string symbols_content((std::istreambuf_iterator<char>(symbols_input_stream)),
                              std::istreambuf_iterator<char>());
  EXPECT_THAT(symbols_content, HasSubstr("func_80054320 = 0x80054320;"));

  EXPECT_TRUE(std::filesystem::exists(out_dir / "mod_alpha.c"));

  std::ifstream module_source_stream(out_dir / "mod_alpha.c");
  std::string module_source_content((std::istreambuf_iterator<char>(module_source_stream)),
                                    std::istreambuf_iterator<char>());
  EXPECT_THAT(module_source_content, HasSubstr("AlphaFunc"));
  EXPECT_THAT(module_source_content, Not(HasSubstr("// Handwritten source")));
  EXPECT_THAT(module_source_content, Not(HasSubstr("mod_alpha.h")));

  std::filesystem::remove_all(test_dir);
}

TEST(LifterPipelineTest, BuildTranslationUnitPopulatesSymbolIndexAndFunctions) {
  std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "lifter_tu_test";
  std::filesystem::create_directories(test_dir / "roms");

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
    name: "mod_alpha"
    type: SUBSEGMENT_C
    rom_start: 0x1000
    vram: 0x80020000
  }
}
)";
  auto cfg_or = ParseSplitConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries {
  name: "AlphaFunc"
  address: 0x80020000
  type: SYMBOL_FUNC
}
entries {
  name: "BetaFunc"
  address: 0x80020010
  type: SYMBOL_FUNC
}
)";
  auto sym_idx_or = SymbolIndex::ParseFromTextproto(proto_syms);
  ASSERT_TRUE(sym_idx_or.ok()) << sym_idx_or.status();

  std::vector<uint8_t> rom(0x2000, 0);
  // AlphaFunc at 0x1000: jr $ra; nop
  rom[0x1000] = 0x03;
  rom[0x1001] = 0xE0;
  rom[0x1002] = 0x00;
  rom[0x1003] = 0x08;
  // BetaFunc at 0x1010: jr $ra; nop
  rom[0x1010] = 0x03;
  rom[0x1011] = 0xE0;
  rom[0x1012] = 0x00;
  rom[0x1013] = 0x08;

  std::filesystem::path rom_path = test_dir / "roms" / "test-game.z64";
  std::ofstream rom_file(rom_path, std::ios::binary);
  rom_file.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  rom_file.close();

  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = test_dir;
  extractor_opts.game_name = "test-game";

  auto extractor = std::make_unique<TargetExtractor>(extractor_opts, &(*cfg_or), &(*sym_idx_or));
  auto loader = std::make_unique<FunctionLoader>(std::move(extractor));

  LifterPipelineOptions options;
  options.repo_root = test_dir;
  options.game_name = "test-game";
  options.includes = {"custom.h"};

  LifterPipeline pipeline(options, std::move(loader));

  auto tu_or = pipeline.BuildTranslationUnit({"AlphaFunc", "BetaFunc"}, {"custom.h"});
  ASSERT_TRUE(tu_or.ok()) << tu_or.status();

  EXPECT_EQ(tu_or->includes, std::vector<std::string>{"custom.h"});
  ASSERT_NE(tu_or->symbol_index, nullptr);
  EXPECT_TRUE(tu_or->symbol_index->HasName("AlphaFunc"));
  EXPECT_TRUE(tu_or->symbol_index->HasName("BetaFunc"));
  ASSERT_THAT(tu_or->functions, ::testing::SizeIs(2));
  EXPECT_EQ(tu_or->functions[0].Name(), "AlphaFunc");
  EXPECT_EQ(tu_or->functions[1].Name(), "BetaFunc");

  std::filesystem::remove_all(test_dir);
}

}  // namespace
}  // namespace rom_nom_nom
