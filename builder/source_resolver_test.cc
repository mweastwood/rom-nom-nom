#include "builder/source_resolver.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::SizeIs;

auto MatchResolved(const std::filesystem::path& path, SourceType type, const std::string& stem) {
  return AllOf(Field(&ResolvedSource::source_path, Eq(path)),
               Field(&ResolvedSource::type, Eq(type)),
               Field(&ResolvedSource::object_stem, Eq(stem)));
}

class SourceResolverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "rom_nom_nom_source_resolver_test";
    std::filesystem::remove_all(test_dir_);
    src_dir_ = test_dir_ / "src";
    asm_dir_ = test_dir_ / "asm";
    assets_dir_ = test_dir_ / "assets";

    std::filesystem::create_directories(src_dir_ / "c");
    std::filesystem::create_directories(src_dir_ / "cc");
    std::filesystem::create_directories(asm_dir_ / "nonmatchings");
    std::filesystem::create_directories(assets_dir_);
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
  std::filesystem::path src_dir_;
  std::filesystem::path asm_dir_;
  std::filesystem::path assets_dir_;
};

TEST_F(SourceResolverTest, ResolvesCSourceFile) {
  std::filesystem::path boot_c = CreateFile(src_dir_ / "c" / "boot.c", "void boot() {}");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/src/boot.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(boot_c, SourceType::kC, "boot"));
}

TEST_F(SourceResolverTest, ResolvesCppSourceFile) {
  std::filesystem::path main_cc = CreateFile(src_dir_ / "cc" / "main.cc", "int main() {}");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/src/main.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(main_cc, SourceType::kCpp, "main"));
}

TEST_F(SourceResolverTest, ResolvesAssemblyFile) {
  std::filesystem::path entry_s = CreateFile(asm_dir_ / "entry.s", ".text\nnop");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/asm/entry.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(entry_s, SourceType::kAssembly, "entry"));
}

TEST_F(SourceResolverTest, ResolvesBinaryAssetFile) {
  std::filesystem::path font_bin = CreateFile(assets_dir_ / "font.bin", "\x00\x01\x02");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/assets/font.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(font_bin, SourceType::kBinary, "font"));
}

TEST_F(SourceResolverTest, SubsegmentHintAsmTakesPrecedenceOverPartialC) {
  // When a C file exists but splitter subsegment is still asm, /asm/ hint must select asm
  std::filesystem::path partial_c = CreateFile(src_dir_ / "c" / "render.c");
  std::filesystem::path asm_render = CreateFile(asm_dir_ / "render.s");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/asm/render.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(asm_render, SourceType::kAssembly, "render"));
}

TEST_F(SourceResolverTest, SubsegmentHintSrcTakesPrecedenceOverAsm) {
  std::filesystem::path c_render = CreateFile(src_dir_ / "c" / "render.c");
  CreateFile(asm_dir_ / "render.s");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/src/render.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(c_render, SourceType::kC, "render"));
}

TEST_F(SourceResolverTest, SubsegmentHintAssetsTakesPrecedenceOverBinaryCandidates) {
  std::filesystem::path bin_logo = CreateFile(assets_dir_ / "logo.bin");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/assets/logo.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(bin_logo, SourceType::kBinary, "logo"));
}

TEST_F(SourceResolverTest, FallbackPrioritySelectsCFirstWhenNoHintGiven) {
  std::filesystem::path boot_c = CreateFile(src_dir_ / "c" / "boot.c");
  CreateFile(asm_dir_ / "boot.s");
  CreateFile(assets_dir_ / "boot.bin");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  // No distinct directory hint in obj_ref
  auto resolved_or = resolver.Resolve("boot.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(boot_c, SourceType::kC, "boot"));
}

TEST_F(SourceResolverTest, FallbackPrioritySelectsAsmBeforeBinaryWhenNoHintGiven) {
  CreateFile(assets_dir_ / "table.bin");
  std::filesystem::path table_s = CreateFile(asm_dir_ / "table.s");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("table.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(table_s, SourceType::kAssembly, "table"));
}

TEST_F(SourceResolverTest, HandlesMissingDirectoriesWithoutError) {
  SourceResolver resolver({
      .src_dir = test_dir_ / "nonexistent_src",
      .asm_dir = test_dir_ / "nonexistent_asm",
      .assets_dir = test_dir_ / "nonexistent_assets",
  });

  auto resolved_or = resolver.Resolve("anything.o");
  EXPECT_EQ(resolved_or.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(SourceResolverTest, ResolveAllProcessesMultipleObjectsInOrder) {
  std::filesystem::path c_boot = CreateFile(src_dir_ / "c" / "boot.c");
  std::filesystem::path asm_entry = CreateFile(asm_dir_ / "entry.s");
  std::filesystem::path bin_font = CreateFile(assets_dir_ / "font.bin");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  std::vector<std::string> obj_refs = {
      "build/game/src/boot.o",
      "build/game/asm/entry.o",
      "build/game/assets/font.o",
  };

  auto results_or = resolver.ResolveAll(obj_refs);
  ASSERT_TRUE(results_or.ok()) << results_or.status();

  EXPECT_THAT(*results_or, ElementsAre(MatchResolved(c_boot, SourceType::kC, "boot"),
                                       MatchResolved(asm_entry, SourceType::kAssembly, "entry"),
                                       MatchResolved(bin_font, SourceType::kBinary, "font")));
}

TEST_F(SourceResolverTest, ReturnsNotFoundWhenNoCandidateMatches) {
  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/src/missing.o");
  EXPECT_EQ(resolved_or.status().code(), absl::StatusCode::kNotFound);
  EXPECT_THAT(resolved_or.status().message(), HasSubstr("missing.o"));
}

TEST_F(SourceResolverTest, ValidatesInvalidInputs) {
  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  EXPECT_EQ(resolver.Resolve("").status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(resolver.Resolve("/").status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(SourceResolverTest, IgnoresNonmatchingsDirectoryInAsmDir) {
  CreateFile(asm_dir_ / "nonmatchings" / "boot" / "main.s");
  std::filesystem::path c_main = CreateFile(src_dir_ / "c" / "game" / "main.c");

  SourceResolver resolver({.src_dir = src_dir_, .asm_dir = asm_dir_, .assets_dir = assets_dir_});

  auto resolved_or = resolver.Resolve("build/game/src/main.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(c_main, SourceType::kC, "main"));
}

TEST_F(SourceResolverTest, PreferCOverridesAssemblyHintWhenCCandidateExists) {
  std::filesystem::path asm_entry = CreateFile(asm_dir_ / "entry.s");
  std::filesystem::path c_entry = CreateFile(src_dir_ / "entry.c");

  SourceResolver resolver({
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .assets_dir = assets_dir_,
      .prefer_c = true,
  });

  auto resolved_or = resolver.Resolve("build/game/asm/entry.o");
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();
  EXPECT_THAT(*resolved_or, MatchResolved(c_entry, SourceType::kC, "entry"));
}

}  // namespace
}  // namespace rom_nom_nom
