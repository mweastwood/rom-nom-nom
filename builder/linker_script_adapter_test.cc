#include "builder/linker_script_adapter.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

constexpr std::string_view kSampleLdScript = R"(
OUTPUT_ARCH(mips)
SECTIONS
{
    __romPos = 0;
    _bootSegmentRomStart = __romPos;
    .boot 0x80000400 : AT(__romPos)
    {
        build/harvest-moon-64/src/boot.o(.text*);
        build/harvest-moon-64/src/boot.o(.data*);
        build/harvest-moon-64/src/boot.o(.rodata*);
    }

    .main 0x80026000 : AT(__romPos)
    {
        build/harvest-moon-64/src/main.o(.text*);
        build/harvest-moon-64/asm/data.o(.data*);
    }
}
)";

class LinkerScriptAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "rom_nom_nom_linker_script_test";
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path CreateFile(const std::filesystem::path& path,
                                   std::string_view content = "") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path);
    ofs << content;
    return path;
  }

  std::filesystem::path test_dir_;
};

TEST_F(LinkerScriptAdapterTest, ParsesAndExtractsUniqueSortedObjectReferences) {
  auto adapter_or = LinkerScriptAdapter::Parse(kSampleLdScript);
  ASSERT_TRUE(adapter_or.ok()) << adapter_or.status();

  EXPECT_THAT(adapter_or->ObjectReferences(),
              ElementsAre("build/harvest-moon-64/asm/data.o", "build/harvest-moon-64/src/boot.o",
                          "build/harvest-moon-64/src/main.o"));
}

TEST_F(LinkerScriptAdapterTest, RewritesObjectPathsToTargetDirectory) {
  auto adapter_or = LinkerScriptAdapter::Parse(kSampleLdScript);
  ASSERT_TRUE(adapter_or.ok()) << adapter_or.status();

  std::filesystem::path target_obj_dir = "/isolated/_objs";
  std::string rewritten = adapter_or->RewriteObjectPaths(target_obj_dir);

  EXPECT_THAT(rewritten, HasSubstr("/isolated/_objs/boot.o(.text*);"));
  EXPECT_THAT(rewritten, HasSubstr("/isolated/_objs/boot.o(.data*);"));
  EXPECT_THAT(rewritten, HasSubstr("/isolated/_objs/main.o(.text*);"));
  EXPECT_THAT(rewritten, HasSubstr("/isolated/_objs/data.o(.data*);"));

  EXPECT_THAT(rewritten, Not(HasSubstr("build/harvest-moon-64/")));
}

TEST_F(LinkerScriptAdapterTest, RewritesObjectPathsWithCustomMapping) {
  auto adapter_or = LinkerScriptAdapter::Parse(kSampleLdScript);
  ASSERT_TRUE(adapter_or.ok()) << adapter_or.status();

  std::string rewritten =
      adapter_or->RewriteObjectPathsWithMapping([](std::string_view orig) -> std::filesystem::path {
        std::filesystem::path p(orig);
        return std::filesystem::path("/custom") / p.stem();
      });

  EXPECT_THAT(rewritten, HasSubstr("/custom/boot(.text*);"));
  EXPECT_THAT(rewritten, HasSubstr("/custom/main(.text*);"));
  EXPECT_THAT(rewritten, HasSubstr("/custom/data(.data*);"));
}

TEST_F(LinkerScriptAdapterTest, LoadReadsAndParsesFromFileOnDisk) {
  std::filesystem::path script_path = CreateFile(test_dir_ / "game.ld", kSampleLdScript);

  auto adapter_or = LinkerScriptAdapter::Load(script_path);
  ASSERT_TRUE(adapter_or.ok()) << adapter_or.status();

  EXPECT_THAT(adapter_or->ObjectReferences(), SizeIs(3));
}

TEST_F(LinkerScriptAdapterTest, LoadReturnsNotFoundForNonexistentFile) {
  auto adapter_or = LinkerScriptAdapter::Load(test_dir_ / "nonexistent.ld");
  EXPECT_EQ(adapter_or.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(LinkerScriptAdapterTest, FindMainScriptFindsScriptMatchingGameName) {
  std::filesystem::path game_ld = CreateFile(test_dir_ / "harvest-moon-64.ld");
  CreateFile(test_dir_ / "other.ld");

  auto script_or = LinkerScriptAdapter::FindMainScript(test_dir_, "harvest-moon-64");
  ASSERT_TRUE(script_or.ok()) << script_or.status();
  EXPECT_EQ(*script_or, game_ld);
}

TEST_F(LinkerScriptAdapterTest, FindMainScriptSkipsSymbolsLdAndFindsPrimaryScript) {
  CreateFile(test_dir_ / "symbols.ld");
  CreateFile(test_dir_ / "hardware_regs.ld");
  std::filesystem::path primary_ld = CreateFile(test_dir_ / "my_game.ld");

  auto script_or = LinkerScriptAdapter::FindMainScript(test_dir_, "unmatched_name");
  ASSERT_TRUE(script_or.ok()) << script_or.status();
  EXPECT_EQ(*script_or, primary_ld);
}

TEST_F(LinkerScriptAdapterTest, FindMainScriptFailsWhenNoPrimaryScriptExists) {
  CreateFile(test_dir_ / "symbols.ld");
  CreateFile(test_dir_ / "hardware_regs.ld");

  auto script_or = LinkerScriptAdapter::FindMainScript(test_dir_, "unmatched_name");
  EXPECT_EQ(script_or.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(LinkerScriptAdapterTest, DiscoverAuxiliaryScriptsFindsAllAuxiliaryFiles) {
  std::filesystem::path symbols_ld = CreateFile(test_dir_ / "symbols.ld");
  std::filesystem::path hw_regs = CreateFile(test_dir_ / "nested" / "hardware_regs.ld");
  std::filesystem::path undef_syms = CreateFile(test_dir_ / "undefined_syms_auto.txt");
  std::filesystem::path undef_funcs = CreateFile(test_dir_ / "undefined_funcs_auto.txt");

  AuxiliaryScripts aux = LinkerScriptAdapter::DiscoverAuxiliaryScripts(test_dir_);

  EXPECT_EQ(aux.symbols_ld, symbols_ld);
  EXPECT_EQ(aux.hardware_regs_ld, hw_regs);
  EXPECT_EQ(aux.undefined_syms_auto, undef_syms);
  EXPECT_EQ(aux.undefined_funcs_auto, undef_funcs);

  EXPECT_THAT(aux.ToList(), ElementsAre(symbols_ld, hw_regs, undef_syms, undef_funcs));
}

TEST_F(LinkerScriptAdapterTest, DiscoverAuxiliaryScriptsHandlesMissingFilesGracefully) {
  AuxiliaryScripts aux = LinkerScriptAdapter::DiscoverAuxiliaryScripts(test_dir_);
  EXPECT_THAT(aux.ToList(), IsEmpty());
}

}  // namespace
}  // namespace rom_nom_nom
