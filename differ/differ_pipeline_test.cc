#include "differ/differ_pipeline.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;

class DifferPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "differ_pipeline_test";
    std::filesystem::remove_all(test_dir_);

    std::filesystem::create_directories(test_dir_ / "config");
    std::filesystem::create_directories(test_dir_ / "symbols");
    std::filesystem::create_directories(test_dir_ / "asm" / "test-game");
    std::filesystem::create_directories(test_dir_ / "src" / "c" / "test-game");

    // 1. Create config
    std::ofstream cfg(test_dir_ / "config" / "test-game.textproto");
    cfg << R"pb(
      game_name: "test-game"
      basename: "test-game"
      segments {
        name: "code"
        type: SEGMENT_CODE
        rom_start: 0x1000
        rom_end: 0x5000
        vram: 0x80020000
      }
    )pb";

    // 2. Create symbols
    std::ofstream syms(test_dir_ / "symbols" / "test-game.textproto");
    syms << R"pb(
      entries { name: "TargetFunc" address: 0x80021000 type: SYMBOL_FUNC }
      entries { name: "idle" address: 0x80021010 type: SYMBOL_FUNC }
    )pb";

    // 3. Create nonmatching assembly for TargetFunc
    std::ofstream asm_file(test_dir_ / "asm" / "test-game" / "TargetFunc.s");
    asm_file << R"(glabel TargetFunc
/* 80021000 27BDFFE0 */  addiu $sp, $sp, -32
/* 80021004 AFBF001C */  sw    $ra, 28($sp)
/* 80021008 03E00008 */  jr    $ra
/* 8002100C 27BD0020 */  addiu $sp, $sp, 32
endlabel TargetFunc
)";

    // 4. Create C source defining idle()
    std::ofstream c_file(test_dir_ / "src" / "c" / "test-game" / "boot.c");
    c_file << R"(#include <stddef.h>

void idle(void) {
  // idle implementation
}
)";
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
};

TEST_F(DifferPipelineTest, FindsSourceFileForKnownFunction) {
  DifferPipeline pipeline({
      .game_name = "test-game",
      .repo_root = test_dir_,
  });

  auto file = pipeline.FindSourceFile("idle", "idle");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->filename().string(), "boot.c");

  auto not_found = pipeline.FindSourceFile("NonExistent_Func", "NonExistent_Func");
  EXPECT_FALSE(not_found.has_value());
}

TEST_F(DifferPipelineTest, HandlesFunctionNotYetInC) {
  DifferPipeline pipeline({
      .game_name = "test-game",
      .repo_root = test_dir_,
      .format_options = {.use_color = false},
  });

  auto result_or = pipeline.Diff("TargetFunc");
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  const auto& res = *result_or;
  EXPECT_EQ(res.canonical_name, "TargetFunc");
  EXPECT_FALSE(res.source_file.has_value());
  EXPECT_EQ(res.target_instructions_count, 4u);
  EXPECT_EQ(res.compiled_instructions_count, 0u);
  EXPECT_FALSE(res.is_bit_exact);

  EXPECT_THAT(res.formatted_output, HasSubstr("Function: TargetFunc"));
  EXPECT_THAT(res.formatted_output, HasSubstr("Target:   4 instructions"));
  EXPECT_THAT(res.formatted_output, HasSubstr("Current:  0 instructions"));
  EXPECT_THAT(res.formatted_output, HasSubstr("Not yet in C"));
  EXPECT_THAT(res.formatted_output, HasSubstr("[MISSING]"));
  EXPECT_THAT(res.formatted_output, HasSubstr("addiu $sp, $sp, -32"));
  EXPECT_THAT(res.formatted_output, HasSubstr("jr    $ra"));
}

}  // namespace
}  // namespace rom_nom_nom
