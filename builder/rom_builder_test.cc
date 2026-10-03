#include "builder/rom_builder.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "core/compiler.h"
#include "core/process.h"
#include "core/sha1.h"
#include "core/toolchain.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::AllOf;
using ::testing::Eq;
using ::testing::Field;
using ::testing::HasSubstr;

class RomBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "rom_nom_nom_rom_builder_test";
    std::filesystem::remove_all(test_dir_);

    src_dir_ = test_dir_ / "src";
    asm_dir_ = test_dir_ / "asm";
    build_dir_ = test_dir_ / "build";
    assets_dir_ = test_dir_ / "assets";

    std::filesystem::create_directories(src_dir_ / "c" / "test_game");
    std::filesystem::create_directories(asm_dir_);
    std::filesystem::create_directories(build_dir_);
    std::filesystem::create_directories(assets_dir_);

    ToolchainOptions tc_opts;
    tc_opts.repo_root = test_dir_;
    tc_opts.mode = ToolchainMode::kOriginal;
    toolchain_ = Toolchain::CreateForTesting(tc_opts);
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
  std::filesystem::path build_dir_;
  std::filesystem::path assets_dir_;
  Toolchain toolchain_{Toolchain::CreateForTesting(ToolchainOptions{})};
};

TEST_F(RomBuilderTest, ComputeFileSha1ComputesAccurateDigest) {
  std::filesystem::path file = CreateFile(test_dir_ / "empty.bin", "");
  auto sha1_or = ComputeFileSha1(file);
  ASSERT_TRUE(sha1_or.ok()) << sha1_or.status();
  // SHA-1 of empty string is da39a3ee5e6b4b0d3255bfef95601890afd80709
  EXPECT_EQ(*sha1_or, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_F(RomBuilderTest, BuildSuccessWithBitExactMatch) {
  // Set up source files
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(asm_dir_ / "entry.s", ".text\nnop");

  // Main linker script
  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
    .entry : { build/asm/entry.o(.text*); }
}
)");
  CreateFile(build_dir_ / "symbols.ld", "g_sym = 0x80000000;\n");

  std::filesystem::path out_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path out_rom = test_dir_ / "out" / "test_game.z64";

  std::string expected_rom_content = "ROM_BINARY_DATA";
  Sha1 sha;
  sha.Update(expected_rom_content);
  std::string expected_sha1 = sha.FinalizeHex();

  // Create retail ROM file to verify bit-exact
  std::filesystem::path retail_rom = CreateFile(test_dir_ / "retail.z64", expected_rom_content);

  // Config with expected sha1 and flags
  std::filesystem::path config_path =
      CreateFile(test_dir_ / "config.textproto", absl::StrCat(R"(
game_name: "test_game"
sha1: ")",
                                                              expected_sha1, R"("
basename: "test_game"
c_flags {
  key: "boot"
  value { flags: "-O0" }
}
c_flags {
  key: "default"
  value { flags: "-O2" }
}
segments {
  name: "boot"
  type: SEGMENT_CODE
  rom_start: 0
  rom_end: 0x1000
}
)"));

  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    // When objcopy is called to extract out_rom, write the expected ROM content
    if (opts.args.size() >= 4 && opts.args[1] == "binary" && opts.args[3] == out_rom.string()) {
      std::ofstream(out_rom, std::ios::binary) << expected_rom_content;
    }
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(toolchain_, mock_runner);

  RomBuilderOptions opts{
      .game_name = "test_game",
      .config_path = config_path,
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = out_elf,
      .out_rom = out_rom,
      .toolchain_mode = ToolchainMode::kOriginal,
      .verify_rom = retail_rom,
      .is_test = true,
  };

  RomBuilder builder(toolchain_, opts, compiler);

  auto result_or = builder.Build();
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  EXPECT_THAT(*result_or, AllOf(Field(&RomBuildResult::success, Eq(true)),
                                Field(&RomBuildResult::sha1_matches, Eq(true)),
                                Field(&RomBuildResult::byte_matches, Eq(true)),
                                Field(&RomBuildResult::built_sha1, Eq(expected_sha1)),
                                Field(&RomBuildResult::total_objects, Eq(2u))));
}

TEST_F(RomBuilderTest, BuildSuccessPureAssemblyWhenSrcDirEmpty) {
  // Even if a C file exists in src, empty src_dir means builder resolves from asm
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "syntax error in C file");
  CreateFile(asm_dir_ / "boot.s", ".text\nnop");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");
  CreateFile(build_dir_ / "symbols.ld", "g_sym = 0x80000000;\n");

  std::filesystem::path out_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path out_rom = test_dir_ / "out" / "test_game.z64";

  std::string expected_rom_content = "ROM_BINARY_DATA";
  Sha1 sha;
  sha.Update(expected_rom_content);
  std::string expected_sha1 = sha.FinalizeHex();

  std::filesystem::path retail_rom = CreateFile(test_dir_ / "retail.z64", expected_rom_content);

  std::filesystem::path config_path =
      CreateFile(test_dir_ / "config.textproto", absl::StrCat(R"(
game_name: "test_game"
sha1: ")",
                                                              expected_sha1, R"("
basename: "test_game"
segments {
  name: "boot"
  type: SEGMENT_CODE
  rom_start: 0
  rom_end: 0x1000
}
)"));

  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    if (opts.args.size() >= 4 && opts.args[1] == "binary" && opts.args[3] == out_rom.string()) {
      std::ofstream(out_rom, std::ios::binary) << expected_rom_content;
    }
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(toolchain_, mock_runner);

  RomBuilderOptions opts{
      .game_name = "test_game",
      .config_path = config_path,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      // src_dir is left empty!
      .out_elf = out_elf,
      .out_rom = out_rom,
      .toolchain_mode = ToolchainMode::kOriginal,
      .verify_rom = retail_rom,
      .is_test = true,
  };

  RomBuilder builder(toolchain_, opts, compiler);

  auto result_or = builder.Build();
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->success);
  EXPECT_EQ(result_or->total_objects, 1u);
}

TEST_F(RomBuilderTest, BuildFailsWhenCompilationFails) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");

  std::filesystem::path out_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path out_rom = test_dir_ / "out" / "test_game.z64";

  auto failing_runner = [](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    ProcessResult res;
    res.exit_code = 1;
    res.stderr_output = "compiler error";
    return res;
  };

  Compiler compiler(toolchain_, failing_runner);

  RomBuilderOptions opts{
      .game_name = "test_game",
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = out_elf,
      .out_rom = out_rom,
  };

  RomBuilder builder(toolchain_, opts, compiler);
  auto result_or = builder.Build();
  EXPECT_FALSE(result_or.ok());
  EXPECT_THAT(result_or.status().message(), HasSubstr("compiler error"));
}

TEST_F(RomBuilderTest, BuildFailsVerificationInTestModeWhenSha1Mismatches) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");

  std::filesystem::path out_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path out_rom = test_dir_ / "out" / "test_game.z64";

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"(
game_name: "test_game"
sha1: "0000000000000000000000000000000000000000"
basename: "test_game"
segments {
  name: "boot"
  type: SEGMENT_CODE
  rom_start: 0
  rom_end: 0x1000
}
)");

  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    if (opts.args.size() >= 4 && opts.args[1] == "binary" && opts.args[3] == out_rom.string()) {
      std::ofstream(out_rom, std::ios::binary) << "DIFFERENT_DATA";
    }
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(toolchain_, mock_runner);

  RomBuilderOptions opts{
      .game_name = "test_game",
      .config_path = config_path,
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = out_elf,
      .out_rom = out_rom,
      .is_test = true,
  };

  RomBuilder builder(toolchain_, opts, compiler);
  auto result_or = builder.Build();
  EXPECT_FALSE(result_or.ok());
  EXPECT_THAT(result_or.status().message(), HasSubstr("Verification failed"));
}

TEST_F(RomBuilderTest, ValidatesEmptyRequiredOptions) {
  Compiler compiler(toolchain_);

  RomBuilder b1(toolchain_, {.game_name = "", .out_elf = "a.elf", .out_rom = "a.z64"}, compiler);
  EXPECT_EQ(b1.Build().status().code(), absl::StatusCode::kInvalidArgument);

  RomBuilder b2(toolchain_, {.game_name = "game", .out_elf = "", .out_rom = "a.z64"}, compiler);
  EXPECT_EQ(b2.Build().status().code(), absl::StatusCode::kInvalidArgument);

  RomBuilder b3(toolchain_, {.game_name = "game", .out_elf = "a.elf", .out_rom = ""}, compiler);
  EXPECT_EQ(b3.Build().status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace rom_nom_nom
