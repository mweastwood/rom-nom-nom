#include "core/compiler.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::IsSupersetOf;
using ::testing::SizeIs;

class CompilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "rom_nom_nom_compiler_test";
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);

    ToolchainOptions orig_opts;
    orig_opts.repo_root = test_dir_;
    orig_opts.mode = ToolchainMode::kOriginal;
    original_toolchain_ = Toolchain::CreateForTesting(
        orig_opts, "/tools/gcc", "/tools/kmc-as", "/tools/mips-as", "/tools/mips-ld",
        "/tools/mips-objcopy", "/tools/mips-gcc", "/tools/mips-g++");

    ToolchainOptions modern_opts;
    modern_opts.repo_root = test_dir_;
    modern_opts.mode = ToolchainMode::kModern;
    modern_toolchain_ = Toolchain::CreateForTesting(
        modern_opts, "/tools/gcc", "/tools/kmc-as", "/tools/mips-as", "/tools/mips-ld",
        "/tools/mips-objcopy", "/tools/mips-gcc", "/tools/mips-g++");
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
  Toolchain original_toolchain_{Toolchain::CreateForTesting(ToolchainOptions{})};
  Toolchain modern_toolchain_{Toolchain::CreateForTesting(ToolchainOptions{})};
};

TEST_F(CompilerTest, FindMacroIncLocatesMacroInMacrosDir) {
  std::filesystem::path repo_root = test_dir_ / "repo";
  std::filesystem::path macros_dir = repo_root / "macros";
  std::filesystem::create_directories(macros_dir);

  EXPECT_TRUE(FindMacroInc(repo_root, "harvest-moon-64").empty());
  EXPECT_TRUE(FindMacroInc(repo_root, "").empty());

  std::filesystem::path macro_file = macros_dir / "harvest-moon-64_macro.inc";
  { std::ofstream(macro_file) << "; macro definitions\n"; }

  EXPECT_EQ(FindMacroInc(repo_root, "harvest-moon-64"), macro_file);
  EXPECT_EQ(FindMacroInc(macros_dir, "harvest-moon-64"), macro_file);
}

TEST_F(CompilerTest, CompileCOriginalModeExecutesGccThenKmcAsAndCleansTemp) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  std::filesystem::path src_file = test_dir_ / "src" / "sample.c";
  std::filesystem::path out_obj = test_dir_ / "obj" / "sample.o";
  std::filesystem::path temp_s272 = test_dir_ / "obj" / "sample.s272";
  std::filesystem::path macro_inc = test_dir_ / "macros" / "game_macro.inc";

  CCompileOptions opts{
      .src_file = src_file,
      .out_obj = out_obj,
      .opt_flags = {"-O2", "-mips2", "-Wa,-g"},
      .include_dirs = {test_dir_ / "include"},
      .macro_inc = macro_inc,
  };

  EXPECT_TRUE(compiler.CompileC(opts).ok());

  ASSERT_THAT(executed_processes, SizeIs(2));

  // First process: GCC 2.7.2
  EXPECT_EQ(executed_processes[0].program, "/tools/gcc");
  EXPECT_THAT(executed_processes[0].args, IsSupersetOf(std::vector<std::string>{
                                              "-x",
                                              "c",
                                              "-S",
                                              "-O2",
                                              "-mips2",
                                              "-G",
                                              "0",
                                              absl::StrCat("-I", (test_dir_ / "include").string()),
                                              src_file.string(),
                                              "-o",
                                              temp_s272.string(),
                                          }));

  // Second process: KMC AS
  EXPECT_EQ(executed_processes[1].program, "/tools/kmc-as");
  EXPECT_THAT(executed_processes[1].args, IsSupersetOf(std::vector<std::string>{
                                              "-mips2",
                                              "-EB",
                                              "-G",
                                              "0",
                                              "-N",
                                              "-g",
                                              macro_inc.string(),
                                              temp_s272.string(),
                                              "-o",
                                              out_obj.string(),
                                          }));

  // Temporary .s272 file must be cleaned up
  EXPECT_FALSE(std::filesystem::exists(temp_s272));
}

TEST_F(CompilerTest, CompileCOriginalModeAbortsWhenGccFails) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 1;
    res.stderr_output = "compiler fatal error";
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  std::filesystem::path src_file = test_dir_ / "src" / "sample.c";
  std::filesystem::path out_obj = test_dir_ / "obj" / "sample.o";

  CCompileOptions opts{
      .src_file = src_file,
      .out_obj = out_obj,
      .opt_flags = {"-O2"},
  };

  auto status = compiler.CompileC(opts);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("compiler fatal error"));

  // KMC AS must not be executed
  EXPECT_THAT(executed_processes, SizeIs(1));
}

TEST_F(CompilerTest, CompileCOriginalModeAbortsWhenKmcAsFails) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    if (executed_processes.size() == 1) {
      res.exit_code = 0;
    } else {
      res.exit_code = 1;
      res.stderr_output = "assembler syntax error";
    }
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  std::filesystem::path src_file = test_dir_ / "src" / "sample.c";
  std::filesystem::path out_obj = test_dir_ / "obj" / "sample.o";

  CCompileOptions opts{
      .src_file = src_file,
      .out_obj = out_obj,
      .opt_flags = {"-O2"},
  };

  auto status = compiler.CompileC(opts);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("assembler syntax error"));

  EXPECT_THAT(executed_processes, SizeIs(2));
}

TEST_F(CompilerTest, CompileCModernModeExecutesModernGccDirectly) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(modern_toolchain_, mock_runner);

  std::filesystem::path src_file = test_dir_ / "src" / "sample.c";
  std::filesystem::path out_obj = test_dir_ / "obj" / "sample.o";

  CCompileOptions opts{
      .src_file = src_file,
      .out_obj = out_obj,
      .include_dirs = {test_dir_ / "include"},
  };

  EXPECT_TRUE(compiler.CompileC(opts).ok());

  ASSERT_THAT(executed_processes, SizeIs(1));
  EXPECT_EQ(executed_processes[0].program, "/tools/mips-gcc");
  EXPECT_THAT(executed_processes[0].args, IsSupersetOf(std::vector<std::string>{
                                              "-c",
                                              "-march=vr4300",
                                              "-mabi=32",
                                              "-EB",
                                              "-fno-PIC",
                                              "-mno-abicalls",
                                              "-ffreestanding",
                                              "-DMODERN_TOOLCHAIN=1",
                                              "-DNON_MATCHING=1",
                                              src_file.string(),
                                              "-o",
                                              out_obj.string(),
                                          }));
}

TEST_F(CompilerTest, CompileCppModernModeExecutesModernGxx) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(modern_toolchain_, mock_runner);

  std::filesystem::path src_file = test_dir_ / "src" / "sample.cc";
  std::filesystem::path out_obj = test_dir_ / "obj" / "sample.o";

  CppCompileOptions opts{
      .src_file = src_file,
      .out_obj = out_obj,
  };

  EXPECT_TRUE(compiler.CompileCpp(opts).ok());

  ASSERT_THAT(executed_processes, SizeIs(1));
  EXPECT_EQ(executed_processes[0].program, "/tools/mips-g++");
  EXPECT_THAT(executed_processes[0].args, IsSupersetOf(std::vector<std::string>{
                                              "-c",
                                              "-march=vr4300",
                                              "-mabi=32",
                                              "-EB",
                                              "-fno-PIC",
                                              "-mno-abicalls",
                                              "-ffreestanding",
                                              src_file.string(),
                                              "-o",
                                              out_obj.string(),
                                          }));
}

TEST_F(CompilerTest, CompileCppRejectsOriginalModeToolchain) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  CppCompileOptions opts{
      .src_file = test_dir_ / "sample.cc",
      .out_obj = test_dir_ / "sample.o",
  };

  auto status = compiler.CompileCpp(opts);
  EXPECT_EQ(status.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_THAT(executed_processes, IsEmpty());
}

TEST_F(CompilerTest, AssembleExecutesGnuAs) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  std::filesystem::path src_file = test_dir_ / "asm" / "sample.s";
  std::filesystem::path out_obj = test_dir_ / "obj" / "sample.o";

  AssemblyOptions opts{
      .src_file = src_file,
      .out_obj = out_obj,
      .include_dirs = {test_dir_ / "asm"},
  };

  EXPECT_TRUE(compiler.Assemble(opts).ok());

  ASSERT_THAT(executed_processes, SizeIs(1));
  EXPECT_EQ(executed_processes[0].program, "/tools/mips-as");
  EXPECT_THAT(executed_processes[0].args, IsSupersetOf(std::vector<std::string>{
                                              "-march=vr4300",
                                              "-mabi=32",
                                              "-EB",
                                              absl::StrCat("-I", (test_dir_ / "asm").string()),
                                              src_file.string(),
                                              "-o",
                                              out_obj.string(),
                                          }));
}

TEST_F(CompilerTest, ConvertBinaryExecutesObjcopy) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  std::filesystem::path src_file = test_dir_ / "assets" / "sample.bin";
  std::filesystem::path out_obj = test_dir_ / "obj" / "sample.o";

  BinaryAssetOptions opts{
      .src_file = src_file,
      .out_obj = out_obj,
  };

  EXPECT_TRUE(compiler.ConvertBinary(opts).ok());

  ASSERT_THAT(executed_processes, SizeIs(1));
  EXPECT_EQ(executed_processes[0].program, "/tools/mips-objcopy");
  EXPECT_THAT(executed_processes[0].args, IsSupersetOf(std::vector<std::string>{
                                              "-I",
                                              "binary",
                                              "-O",
                                              "elf32-tradbigmips",
                                              "-B",
                                              "mips",
                                              src_file.string(),
                                              out_obj.string(),
                                          }));
}

TEST_F(CompilerTest, LinkExecutesGnuLd) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  std::filesystem::path script1 = test_dir_ / "symbols.ld";
  std::filesystem::path main_script = test_dir_ / "game.ld";
  std::filesystem::path out_elf = test_dir_ / "game.elf";

  LinkOptions opts{
      .script_paths = {script1},
      .main_ld_script = main_script,
      .out_elf = out_elf,
  };

  EXPECT_TRUE(compiler.Link(opts).ok());

  ASSERT_THAT(executed_processes, SizeIs(1));
  EXPECT_EQ(executed_processes[0].program, "/tools/mips-ld");
  EXPECT_THAT(executed_processes[0].args, IsSupersetOf(std::vector<std::string>{
                                              "-T",
                                              script1.string(),
                                              "-T",
                                              main_script.string(),
                                              "--no-check-sections",
                                              "-o",
                                              out_elf.string(),
                                          }));
}

TEST_F(CompilerTest, ExtractRomExecutesObjcopy) {
  std::vector<ProcessOptions> executed_processes;
  auto mock_runner = [&](const ProcessOptions& opts) -> absl::StatusOr<ProcessResult> {
    executed_processes.push_back(opts);
    ProcessResult res;
    res.exit_code = 0;
    return res;
  };

  Compiler compiler(original_toolchain_, mock_runner);

  std::filesystem::path in_elf = test_dir_ / "game.elf";
  std::filesystem::path out_rom = test_dir_ / "game.z64";

  ExtractRomOptions opts{
      .in_elf = in_elf,
      .out_rom = out_rom,
  };

  EXPECT_TRUE(compiler.ExtractRom(opts).ok());

  ASSERT_THAT(executed_processes, SizeIs(1));
  EXPECT_EQ(executed_processes[0].program, "/tools/mips-objcopy");
  EXPECT_THAT(executed_processes[0].args, IsSupersetOf(std::vector<std::string>{
                                              "-O",
                                              "binary",
                                              in_elf.string(),
                                              out_rom.string(),
                                          }));
}

TEST_F(CompilerTest, ValidatesInputPaths) {
  Compiler compiler(original_toolchain_);

  EXPECT_EQ(compiler.CompileC({.src_file = "", .out_obj = "a.o"}).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(compiler.CompileC({.src_file = "a.c", .out_obj = ""}).code(),
            absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(compiler.Assemble({.src_file = "", .out_obj = "a.o"}).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(compiler.Assemble({.src_file = "a.s", .out_obj = ""}).code(),
            absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(compiler.ConvertBinary({.src_file = "", .out_obj = "a.o"}).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(compiler.ConvertBinary({.src_file = "a.bin", .out_obj = ""}).code(),
            absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(compiler.Link({.main_ld_script = "", .out_elf = "a.elf"}).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(compiler.Link({.main_ld_script = "game.ld", .out_elf = ""}).code(),
            absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(compiler.ExtractRom({.in_elf = "", .out_rom = "a.z64"}).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(compiler.ExtractRom({.in_elf = "a.elf", .out_rom = ""}).code(),
            absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace rom_nom_nom
