#include "core/toolchain.h"

#include <filesystem>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::IsSupersetOf;
using ::testing::Not;
using ::testing::StartsWith;

TEST(ToolchainTest, FindExecutableInPathFindsSystemBinaries) {
  EXPECT_FALSE(FindExecutableInPath("sh").empty());
  EXPECT_FALSE(FindExecutableInPath("ls").empty());
  EXPECT_TRUE(FindExecutableInPath("definitely_not_a_real_binary_12345").empty());
}

TEST(ToolchainTest, DiscoversOriginalToolchainSuccessfully) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  const Toolchain& toolchain = *toolchain_or;
  EXPECT_EQ(toolchain.Mode(), ToolchainMode::kOriginal);
  EXPECT_FALSE(toolchain.Gcc272().empty());
  EXPECT_TRUE(std::filesystem::exists(toolchain.Gcc272()));
  EXPECT_FALSE(toolchain.KmcAs().empty());
  EXPECT_TRUE(std::filesystem::exists(toolchain.KmcAs()));
  EXPECT_FALSE(toolchain.GnuAs().empty());
  EXPECT_TRUE(std::filesystem::exists(toolchain.GnuAs()));
  EXPECT_FALSE(toolchain.GnuLd().empty());
  EXPECT_TRUE(std::filesystem::exists(toolchain.GnuLd()));
  EXPECT_FALSE(toolchain.GnuObjcopy().empty());
  EXPECT_TRUE(std::filesystem::exists(toolchain.GnuObjcopy()));
}

TEST(ToolchainTest, BuildsGcc272CompileArgsCorrectly) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  std::vector<std::string> opt_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g,-V"};
  std::vector<std::filesystem::path> include_dirs = {"src", "asm"};
  std::filesystem::path src = "src/c/game/boot.c";
  std::filesystem::path out_s = "build/boot.s272";

  auto args = toolchain_or->BuildGcc272CompileArgs(opt_flags, include_dirs, src, out_s);

  // Verifies -Wa, is stripped from compiler invocation
  EXPECT_THAT(args, Each(Not(StartsWith("-Wa,"))));

  // Verifies required flags and arguments are present using gMock matchers
  EXPECT_THAT(args, IsSupersetOf(std::vector<std::string>{
                        "-x",
                        "c",
                        "-S",
                        "-O2",
                        "-mips2",
                        "-mcpu=r4000",
                        "-G",
                        "0",
                        "-Isrc",
                        "-Iasm",
                        src.string(),
                        "-o",
                        out_s.string(),
                    }));

  // Verifies prefix arguments safely
  ASSERT_GE(args.size(), 4u);
  EXPECT_THAT(args[0], StartsWith("-B"));
  EXPECT_EQ(args[1], "-x");
  EXPECT_EQ(args[2], "c");
  EXPECT_EQ(args[3], "-S");
}

TEST(ToolchainTest, BuildsKmcAsArgsCorrectly) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  std::vector<std::string> opt_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g,-V"};
  std::vector<std::filesystem::path> include_dirs = {"asm"};
  std::filesystem::path macro_inc = "macros/harvest-moon-64_macro.inc";
  std::filesystem::path in_s = "build/boot.s272";
  std::filesystem::path out_obj = "build/boot.o";

  auto args = toolchain_or->BuildKmcAsArgs(opt_flags, include_dirs, macro_inc, in_s, out_obj);

  EXPECT_THAT(args, IsSupersetOf(std::vector<std::string>{
                        "-mcpu=r4000",
                        "-mips2",
                        "-EB",
                        "-G",
                        "0",
                        "-N",
                        "-g",
                        "-V",
                        "-Iasm",
                        macro_inc.string(),
                        in_s.string(),
                        "-o",
                        out_obj.string(),
                    }));
}

TEST(ToolchainTest, BuildsKmcAsArgsWithO0Default) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  std::vector<std::string> opt_flags = {"-O0"};
  std::vector<std::filesystem::path> include_dirs = {};
  auto args = toolchain_or->BuildKmcAsArgs(opt_flags, include_dirs, {}, "in.s", "out.o");

  EXPECT_THAT(args, Contains("-O0"));
}

TEST(ToolchainTest, BuildsGnuAsArgsCorrectly) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  std::vector<std::filesystem::path> include_dirs = {"asm", "build"};
  auto args = toolchain_or->BuildGnuAsArgs(include_dirs, "asm/controller.s", "build/controller.o");

  EXPECT_THAT(args, IsSupersetOf(std::vector<std::string>{
                        "-march=vr4300",
                        "-mabi=32",
                        "-EB",
                        "-Iasm",
                        "-Ibuild",
                        "asm/controller.s",
                        "-o",
                        "build/controller.o",
                    }));
}

TEST(ToolchainTest, BuildsObjcopyBinaryArgsCorrectly) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  auto args = toolchain_or->BuildObjcopyBinaryArgs("assets/header.bin", "build/header.o");
  EXPECT_THAT(args, ElementsAre("-I", "binary", "-O", "elf32-tradbigmips", "-B", "mips",
                                "assets/header.bin", "build/header.o"));
}

TEST(ToolchainTest, BuildsExtractRomArgsCorrectly) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  auto args = toolchain_or->BuildExtractRomArgs("build/game.elf", "build/game.z64");
  EXPECT_THAT(args, ElementsAre("-O", "binary", "build/game.elf", "build/game.z64"));
}

TEST(ToolchainTest, BuildsLdArgsCorrectly) {
  ToolchainOptions options;
  auto toolchain_or = Toolchain::Discover(options);
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  auto args = toolchain_or->BuildLdArgs({}, "build/game.ld", "build/game.elf");
  EXPECT_THAT(args,
              ElementsAre("-T", "build/game.ld", "--no-check-sections", "-o", "build/game.elf"));
}

}  // namespace
}  // namespace rom_nom_nom
