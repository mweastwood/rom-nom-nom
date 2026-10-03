#include "lifter/function_loader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/mips.h"
#include "differ/target_extractor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ::testing::NotNull;
using ::testing::SizeIs;

TEST(FunctionLoaderTest, FromWordsSuccess) {
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  auto loaded_or = FunctionLoader::FromWords("TestAdd", words, 0x80020000);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  EXPECT_EQ(loaded_or->name, "TestAdd");
  EXPECT_EQ(loaded_or->vram, 0x80020000);
  EXPECT_THAT(loaded_or->instructions, SizeIs(3));
  EXPECT_EQ(loaded_or->instructions[0].opcode, Opcode::kAddu);
  EXPECT_TRUE(loaded_or->instructions[1].IsReturn());
}

TEST(FunctionLoaderTest, LoadFunctionViaExtractor) {
  auto test_dir = std::filesystem::temp_directory_path() / "func_loader_test";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir / "config");
  std::filesystem::create_directories(test_dir / "symbols");
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
}
)";
  auto cfg_or = ParseSplitConfig(proto_cfg);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();

  std::string proto_syms = R"(
entries {
  name: "MyFunc"
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

  // Create dummy ROM binary
  std::vector<uint8_t> rom(0x2000, 0);
  // Place "jr $ra" at rom_start 0x1000: 0x03E00008
  rom[0x1000] = 0x03;
  rom[0x1001] = 0xE0;
  rom[0x1002] = 0x00;
  rom[0x1003] = 0x08;
  // nop: 0x00000000
  std::filesystem::path rom_path = test_dir / "roms" / "test-game.z64";
  std::ofstream rom_file(rom_path, std::ios::binary);
  rom_file.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  rom_file.close();

  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = test_dir;
  extractor_opts.game_name = "test-game";

  auto extractor = std::make_unique<TargetExtractor>(extractor_opts, &(*cfg_or), &(*sym_idx_or));

  FunctionLoader loader(std::move(extractor));
  auto loaded_or = loader.LoadFunction("MyFunc");
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  EXPECT_EQ(loaded_or->name, "MyFunc");
  EXPECT_EQ(loaded_or->vram, 0x80020000);
  EXPECT_THAT(loaded_or->instructions, SizeIs(2));
  EXPECT_TRUE(loaded_or->instructions[0].IsReturn());

  std::filesystem::remove_all(test_dir);
}

TEST(FunctionLoaderTest, LoadFunctionViaExplicitPaths) {
  auto test_dir = std::filesystem::temp_directory_path() / "func_loader_paths_test";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  std::filesystem::path cfg_path = test_dir / "custom_config.textproto";
  std::ofstream cfg_file(cfg_path);
  cfg_file << R"(
game_name: "custom-game"
sha1: "0123456789abcdef0123456789abcdef01234567"
basename: "custom-game"
segments {
  name: "code"
  type: SEGMENT_CODE
  rom_start: 0x1000
  rom_end: 0x2000
  vram: 0x80020000
}
)";
  cfg_file.close();

  std::filesystem::path sym_path = test_dir / "custom_symbols.textproto";
  std::ofstream sym_file(sym_path);
  sym_file << R"(
entries {
  name: "CustomFunc"
  address: 0x80020000
  type: SYMBOL_FUNC
}
entries {
  name: "CustomNext"
  address: 0x80020008
  type: SYMBOL_FUNC
}
)";
  sym_file.close();

  std::vector<uint8_t> rom(0x2000, 0);
  rom[0x1000] = 0x03;
  rom[0x1001] = 0xE0;
  rom[0x1002] = 0x00;
  rom[0x1003] = 0x08;
  std::filesystem::path rom_path = test_dir / "custom_rom.z64";
  std::ofstream rom_file(rom_path, std::ios::binary);
  rom_file.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  rom_file.close();

  FunctionLoaderOptions loader_opts;
  loader_opts.repo_root = test_dir;
  loader_opts.game_name = "custom-game";
  loader_opts.config_path = cfg_path;
  loader_opts.symbols_path = sym_path;
  loader_opts.rom_path = rom_path;

  auto loader_or = FunctionLoader::Create(loader_opts);
  ASSERT_TRUE(loader_or.ok()) << loader_or.status();

  auto loaded_or = (*loader_or)->LoadFunction("CustomFunc");
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  EXPECT_EQ(loaded_or->name, "CustomFunc");
  EXPECT_EQ(loaded_or->vram, 0x80020000);
  EXPECT_THAT(loaded_or->instructions, SizeIs(2));

  std::filesystem::remove_all(test_dir);
}

}  // namespace
}  // namespace rom_nom_nom
