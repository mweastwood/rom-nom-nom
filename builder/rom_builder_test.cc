#include "builder/rom_builder.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "core/compiler.h"
#include "core/game_config.pb.h"
#include "core/process.h"
#include "core/sha1.h"
#include "core/symbol_registry.h"
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
  rom_end: 15
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
  rom_end: 15
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

TEST_F(RomBuilderTest, BuildPadsRomToCartridgeMediaSize) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(asm_dir_ / "entry.s", ".text\nnop");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"pb(
    game_name: "test_game"
    basename: "test_game"
    sha1: "0000000000000000000000000000000000000000"
    segments {
      name: "boot"
      rom_start: 0x0
      rom_end: 64
      type: SEGMENT_CODE
      subsegments { name: "boot" rom_start: 0x0 type: SUBSEGMENT_C }
    }
  )pb");

  std::filesystem::path output_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path output_rom = test_dir_ / "out" / "test_game.z64";

  auto mock_process_runner =
      [&](const ProcessOptions& process_options) -> absl::StatusOr<ProcessResult> {
    if (process_options.args.size() >= 4 && process_options.args[1] == "binary" &&
        process_options.args[3] == output_rom.string()) {
      std::filesystem::create_directories(output_rom.parent_path());
      std::ofstream rom_output(output_rom, std::ios::binary);
      std::string initial_content(16, '\xAA');
      rom_output.write(initial_content.data(), initial_content.size());
    }
    ProcessResult process_result;
    process_result.exit_code = 0;
    return process_result;
  };

  Compiler compiler(toolchain_, mock_process_runner);

  RomBuilderOptions builder_options{
      .game_name = "test_game",
      .config_path = config_path,
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = output_elf,
      .out_rom = output_rom,
      .is_test = false,
      .prefer_c = false,
  };

  RomBuilder rom_builder(toolchain_, builder_options, compiler);
  auto build_result_or = rom_builder.Build();
  ASSERT_TRUE(build_result_or.ok()) << build_result_or.status();

  // Verify ROM was unconditionally padded from 16 bytes to the expected 64 bytes
  std::error_code error_code;
  EXPECT_EQ(std::filesystem::file_size(output_rom, error_code), uint64_t{64});

  // Verify the initial 16 bytes are intact and trailing 48 bytes are zero padding
  std::ifstream rom_input(output_rom, std::ios::binary);
  std::string file_content((std::istreambuf_iterator<char>(rom_input)),
                           std::istreambuf_iterator<char>());
  ASSERT_EQ(file_content.size(), 64ULL);
  EXPECT_EQ(file_content.substr(0, 16), std::string(16, '\xAA'));
  EXPECT_EQ(file_content.substr(16), std::string(48, '\0'));
}

TEST_F(RomBuilderTest, BuildIncludesLiftedSymbolsScriptFromSrcDir) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(asm_dir_ / "entry.s", ".text\nnop");

  std::filesystem::path lifted_symbols_script =
      CreateFile(src_dir_ / "lifted_symbols.ld", "lifted_sym = 0x80010000;\n");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"pb(
    game_name: "test_game"
    basename: "test_game"
    sha1: "0000000000000000000000000000000000000000"
    segments {
      name: "boot"
      rom_start: 0x0
      rom_end: 16
      type: SEGMENT_CODE
      subsegments { name: "boot" rom_start: 0x0 type: SUBSEGMENT_C }
    }
  )pb");

  std::filesystem::path output_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path output_rom = test_dir_ / "out" / "test_game.z64";

  bool link_command_included_lifted_symbols = false;
  auto mock_process_runner =
      [&](const ProcessOptions& process_options) -> absl::StatusOr<ProcessResult> {
    for (const auto& argument : process_options.args) {
      if (argument == lifted_symbols_script.string()) {
        link_command_included_lifted_symbols = true;
      }
    }
    if (process_options.args.size() >= 4 && process_options.args[1] == "binary" &&
        process_options.args[3] == output_rom.string()) {
      std::filesystem::create_directories(output_rom.parent_path());
      std::ofstream rom_output(output_rom, std::ios::binary);
      std::string initial_content(16, '\xBB');
      rom_output.write(initial_content.data(), initial_content.size());
    }
    ProcessResult process_result;
    process_result.exit_code = 0;
    return process_result;
  };

  Compiler compiler(toolchain_, mock_process_runner);

  RomBuilderOptions builder_options{
      .game_name = "test_game",
      .config_path = config_path,
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = output_elf,
      .out_rom = output_rom,
      .is_test = false,
      .prefer_c = false,
  };

  RomBuilder rom_builder(toolchain_, builder_options, compiler);
  auto build_result_or = rom_builder.Build();
  ASSERT_TRUE(build_result_or.ok()) << build_result_or.status();
  EXPECT_TRUE(link_command_included_lifted_symbols);
}

TEST_F(RomBuilderTest, UsesOutElfStemForIsolatedObjectDirectoryAndLinkerScript) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(asm_dir_ / "entry.s", ".text\nnop");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"pb(
    game_name: "test_game"
    basename: "test_game"
    sha1: "0000000000000000000000000000000000000000"
    segments {
      name: "boot"
      rom_start: 0x0
      rom_end: 16
      type: SEGMENT_CODE
      subsegments { name: "boot" rom_start: 0x0 type: SUBSEGMENT_C }
    }
  )pb");

  std::filesystem::path custom_elf = test_dir_ / "out" / "test_game_lifted_c.elf";
  std::filesystem::path custom_rom = test_dir_ / "out" / "test_game_lifted_c.z64";

  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    if (opts.args.size() >= 4 && opts.args[1] == "binary" && opts.args[3] == custom_rom.string()) {
      std::filesystem::create_directories(custom_rom.parent_path());
      std::ofstream(custom_rom, std::ios::binary).write("1234567890123456", 16);
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
      .out_elf = custom_elf,
      .out_rom = custom_rom,
  };

  RomBuilder builder(toolchain_, opts, compiler);
  auto result_or = builder.Build();
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  std::filesystem::path expected_obj_dir = test_dir_ / "out" / "_test_game_lifted_c_objs";
  EXPECT_TRUE(std::filesystem::exists(expected_obj_dir));
  EXPECT_TRUE(std::filesystem::exists(expected_obj_dir / "test_game_lifted_c.ld"));
}

TEST_F(RomBuilderTest, SkipsUndefinedAutoScriptsWhenLiftedSymbolsPresent) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(asm_dir_ / "entry.s", ".text\nnop");

  std::filesystem::path lifted_symbols =
      CreateFile(src_dir_ / "lifted_symbols.ld", "lifted_sym = 0x80010000;\n");
  std::filesystem::path undef_syms =
      CreateFile(build_dir_ / "undefined_syms_auto.test_game.ld", "undef_sym = 0x80020000;\n");
  std::filesystem::path undef_funcs =
      CreateFile(build_dir_ / "undefined_funcs_auto.test_game.ld", "undef_func = 0x80030000;\n");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"pb(
    game_name: "test_game"
    basename: "test_game"
    sha1: "0000000000000000000000000000000000000000"
    segments {
      name: "boot"
      rom_start: 0x0
      rom_end: 16
      type: SEGMENT_CODE
      subsegments { name: "boot" rom_start: 0x0 type: SUBSEGMENT_C }
    }
  )pb");

  std::filesystem::path out_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path out_rom = test_dir_ / "out" / "test_game.z64";

  std::vector<std::string> link_args;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    for (size_t i = 0; i + 1 < opts.args.size(); ++i) {
      if (opts.args[i] == "-o" && opts.args[i + 1] == out_elf.string()) {
        link_args = opts.args;
        break;
      }
    }
    if (opts.args.size() >= 4 && opts.args[1] == "binary" && opts.args[3] == out_rom.string()) {
      std::filesystem::create_directories(out_rom.parent_path());
      std::ofstream(out_rom, std::ios::binary).write("1234567890123456", 16);
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
  };

  RomBuilder builder(toolchain_, opts, compiler);
  auto result_or = builder.Build();
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  EXPECT_THAT(link_args, ::testing::Contains(lifted_symbols.string()));
  EXPECT_THAT(link_args, ::testing::Not(::testing::Contains(undef_syms.string())));
  EXPECT_THAT(link_args, ::testing::Not(::testing::Contains(undef_funcs.string())));
}

TEST_F(RomBuilderTest, GeneratesSymbolsFromTextprotoWhenSymbolsLdAbsent) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot() {}");
  CreateFile(asm_dir_ / "entry.s", ".text\nnop");

  std::filesystem::path symbols_proto = CreateFile(test_dir_ / "symbols.textproto", R"pb(
    entries { name: "D_80002000" address: 0x80002000 type: SYMBOL_DATA }
    entries { name: "g_inner_data" address: 0x80002010 type: SYMBOL_DATA }
  )pb");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); }
}
)");

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"pb(
    game_name: "test_game"
    basename: "test_game"
    sha1: "0000000000000000000000000000000000000000"
    segments {
      name: "code"
      rom_start: 0x0
      rom_end: 0x4000
      vram: 0x80000000
      type: SEGMENT_CODE
      subsegments { name: "boot" rom_start: 0x0 type: SUBSEGMENT_C }
    }
  )pb");

  std::filesystem::path out_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path out_rom = test_dir_ / "out" / "test_game.z64";

  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    if (opts.args.size() >= 4 && opts.args[1] == "binary" && opts.args[3] == out_rom.string()) {
      std::filesystem::create_directories(out_rom.parent_path());
      std::ofstream(out_rom, std::ios::binary).write("1234567890123456", 16);
    }
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(toolchain_, mock_runner);

  RomBuilderOptions opts{
      .game_name = "test_game",
      .config_path = config_path,
      .symbols_path = symbols_proto,
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = out_elf,
      .out_rom = out_rom,
  };

  RomBuilder builder(toolchain_, opts, compiler);
  auto result_or = builder.Build();
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  std::filesystem::path generated_symbols = test_dir_ / "out" / "_test_game_objs" / "symbols.ld";
  ASSERT_TRUE(std::filesystem::exists(generated_symbols));

  std::ifstream ifs(generated_symbols);
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

  EXPECT_THAT(content, HasSubstr("PROVIDE(D_80002000 = 0x80002000);"));
  EXPECT_THAT(content, HasSubstr("PROVIDE(g_inner_data = 0x80002010);"));
}

TEST_F(RomBuilderTest, LoadsModuleCFlagsFromTuningCacheFileAndTunedCFlagsTextproto) {
  CreateFile(src_dir_ / "c" / "test_game" / "boot.c", "void boot(void) {}");
  CreateFile(src_dir_ / "c" / "test_game" / "audio.c", "void audio(void) {}");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .boot : { build/src/boot.o(.text*); build/src/audio.o(.text*); }
}
)");

  // Relative tuning_cache_file sets module_c_flags for both "boot" and "audio"
  CreateFile(test_dir_ / "config" / "test_game.tuning.textproto", R"pb(
    game_name: "test_game"
    module_c_flags {
      key: "boot"
      value { flags: "-O2" flags: "-Wa,-O2" }
    }
    module_c_flags {
      key: "audio"
      value { flags: "-O1" flags: "-Wa,-g" }
    }
  )pb");

  // Optional src_dir/tuned_c_flags.textproto overrides "audio"
  CreateFile(src_dir_ / "tuned_c_flags.textproto", R"pb(
    game_name: "test_game"
    module_c_flags {
      key: "audio"
      value { flags: "-O3" flags: "-Wa,-O2" }
    }
  )pb");

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"pb(
    game_name: "test_game"
    basename: "test_game"
    sha1: "0000000000000000000000000000000000000000"
    tuning_cache_file: "config/test_game.tuning.textproto"
    c_flags {
      key: "default"
      value { flags: "-O0" }
    }
    segments {
      name: "code"
      rom_start: 0x0
      rom_end: 0x10
      vram: 0x80000000
      type: SEGMENT_CODE
      subsegments { name: "boot" rom_start: 0x0 type: SUBSEGMENT_C }
      subsegments { name: "audio" rom_start: 0x8 type: SUBSEGMENT_C }
    }
  )pb");

  std::filesystem::path output_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path output_rom = test_dir_ / "out" / "test_game.z64";

  std::vector<std::string> boot_compiler_arguments;
  std::vector<std::string> audio_compiler_arguments;
  auto mock_runner = [&](const ProcessOptions& process_options) -> absl::StatusOr<ProcessResult> {
    for (const auto& argument : process_options.args) {
      if (argument == (src_dir_ / "c" / "test_game" / "boot.c").string()) {
        boot_compiler_arguments = process_options.args;
      }
      if (argument == (src_dir_ / "c" / "test_game" / "audio.c").string()) {
        audio_compiler_arguments = process_options.args;
      }
    }
    if (process_options.args.size() >= 4 && process_options.args[1] == "binary" &&
        process_options.args[3] == output_rom.string()) {
      std::filesystem::create_directories(output_rom.parent_path());
      std::ofstream(output_rom, std::ios::binary).write("1234567890123456", 16);
    }
    ProcessResult process_result;
    process_result.exit_code = 0;
    return process_result;
  };

  Compiler compiler(toolchain_, mock_runner);
  RomBuilderOptions builder_options{
      .game_name = "test_game",
      .config_path = config_path,
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = output_elf,
      .out_rom = output_rom,
  };

  RomBuilder builder(toolchain_, builder_options, compiler);
  auto result_or = builder.Build();
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  EXPECT_THAT(boot_compiler_arguments, ::testing::Contains("-O2"));
  EXPECT_THAT(audio_compiler_arguments, ::testing::Contains("-O3"));
}

TEST_F(RomBuilderTest, BuildCompilesCModuleWithPerFunctionFlagsUsingAssemblySplicing) {
  CreateFile(src_dir_ / "c" / "test_game" / "math.c", R"(
int Add(int a, int b) { return a + b; }
int Sub(int a, int b) { return a - b; }
)");

  CreateFile(build_dir_ / "test_game.ld", R"(
SECTIONS
{
    .math : { build/src/math.o(.text*); }
}
)");
  CreateFile(build_dir_ / "symbols.ld", "");

  CreateFile(src_dir_ / "tuned_c_flags.textproto", R"pb(
    game_name: "test_game"
    module_c_flags {
      key: "math"
      value { flags: "-O2" flags: "-mips2" flags: "-mcpu=r4000" flags: "-Wa,-O2" }
    }
    entries {
      function_name: "Sub"
      module_name: "math"
      c_flags: "-O1"
      c_flags: "-mips2"
      c_flags: "-mcpu=r4000"
      c_flags: "-Wa,-g"
    }
  )pb");

  std::filesystem::path config_path = CreateFile(test_dir_ / "config.textproto", R"pb(
    game_name: "test_game"
    basename: "test_game"
    sha1: "0000000000000000000000000000000000000000"
    c_flags {
      key: "default"
      value { flags: "-O2" flags: "-mips2" flags: "-mcpu=r4000" flags: "-Wa,-O2" }
    }
    segments {
      name: "code"
      rom_start: 0x0
      rom_end: 0x10
      vram: 0x80000000
      type: SEGMENT_CODE
      subsegments { name: "math" rom_start: 0x0 type: SUBSEGMENT_C }
    }
  )pb");

  std::filesystem::path output_elf = test_dir_ / "out" / "test_game.elf";
  std::filesystem::path output_rom = test_dir_ / "out" / "test_game.z64";

  std::vector<std::vector<std::string>> gcc_invocations;
  std::string captured_spliced_assembly;

  auto mock_runner = [&](const ProcessOptions& process_options) -> absl::StatusOr<ProcessResult> {
    if (process_options.program == toolchain_.Gcc272().string()) {
      gcc_invocations.push_back(process_options.args);
      for (size_t i = 0; i + 1 < process_options.args.size(); ++i) {
        if (process_options.args[i] == "-o") {
          std::filesystem::path out_s(process_options.args[i + 1]);
          bool is_o1 = false;
          for (const auto& arg : process_options.args) {
            if (arg == "-O1") is_o1 = true;
          }
          std::ofstream ofs(out_s);
          if (is_o1) {
            ofs << R"(	.text
	.align	2
	.globl	Add
	.type	 Add,@function
	.ent	Add
Add:
.L1:
	addu_o1	$2,$4,$5
	j	$31
	.end	Add
.Lfe1:
	.size	 Add,.Lfe1-Add
	.align	2
	.globl	Sub
	.type	 Sub,@function
	.ent	Sub
Sub:
.L1:
	subu_o1	$2,$4,$5
	j	$31
	.end	Sub
.Lfe2:
	.size	 Sub,.Lfe2-Sub
)";
          } else {
            ofs << R"(	.text
	.align	2
	.globl	Add
	.type	 Add,@function
	.ent	Add
Add:
.L1:
	addu_o2	$2,$4,$5
	j	$31
	.end	Add
.Lfe1:
	.size	 Add,.Lfe1-Add
	.align	2
	.globl	Sub
	.type	 Sub,@function
	.ent	Sub
Sub:
.L1:
	subu_o2	$2,$4,$5
	j	$31
	.end	Sub
.Lfe2:
	.size	 Sub,.Lfe2-Sub
)";
          }
        }
      }
    } else if (process_options.program == toolchain_.KmcAs().string()) {
      for (const auto& arg : process_options.args) {
        if (absl::EndsWith(arg, ".spliced.s272")) {
          std::ifstream ifs(arg);
          captured_spliced_assembly.assign((std::istreambuf_iterator<char>(ifs)),
                                           std::istreambuf_iterator<char>());
        }
      }
    }

    if (process_options.args.size() >= 4 && process_options.args[1] == "binary" &&
        process_options.args[3] == output_rom.string()) {
      std::filesystem::create_directories(output_rom.parent_path());
      std::ofstream(output_rom, std::ios::binary).write("1234567890123456", 16);
    }

    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(toolchain_, mock_runner);
  RomBuilderOptions builder_options{
      .game_name = "test_game",
      .config_path = config_path,
      .src_dir = src_dir_,
      .asm_dir = asm_dir_,
      .build_dir = build_dir_,
      .assets_dir = assets_dir_,
      .out_elf = output_elf,
      .out_rom = output_rom,
  };

  RomBuilder builder(toolchain_, builder_options, compiler);
  auto result_or = builder.Build();
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  ASSERT_EQ(gcc_invocations.size(), 2u);
  EXPECT_THAT(gcc_invocations[0], ::testing::Contains("-O2"));
  EXPECT_THAT(gcc_invocations[1], ::testing::Contains("-O1"));

  EXPECT_THAT(captured_spliced_assembly, ::testing::HasSubstr("addu_o2"));
  EXPECT_THAT(captured_spliced_assembly, ::testing::HasSubstr("subu_o1"));
  EXPECT_THAT(captured_spliced_assembly, ::testing::Not(::testing::HasSubstr("subu_o2")));

  EXPECT_THAT(captured_spliced_assembly, ::testing::HasSubstr(".L_sp_Sub_1"));

  EXPECT_THAT(captured_spliced_assembly, ::testing::HasSubstr(".set\tnomove"));
  EXPECT_THAT(captured_spliced_assembly, ::testing::HasSubstr(".set\tmove"));
}

}  // namespace
}  // namespace rom_nom_nom
