#include "lifter/lifter_pipeline.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/c_ast.h"
#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/function_loader.h"

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

}  // namespace
}  // namespace rom_nom_nom
