#include "lifter/inverse_rtl_to_c_ast_stage.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/symbol_registry.h"
#include "gtest/gtest.h"
#include "lifter/c_emitter.h"
#include "lifter/decompilation_context.h"
#include "lifter/function_loader.h"
#include "lifter/inverse_compiler_pipeline.h"

namespace rom_nom_nom {
namespace {

TEST(InverseRtlToCAstStageTest, FailsPreconditionWhenCfgNotInitialized) {
  auto loaded_or = FunctionLoader::FromWords("EmptyCheck", {0x03e00008, 0x00000000}, 0x80000000);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();
  DecompilationContext ctx(*loaded_or);
  EXPECT_FALSE(InverseRtlToCAstLifter::LiftRtlToCAst(&ctx).ok());
}

TEST(InverseRtlToCAstStageTest, LiftsArithmeticAndMemoryStoreToCAst) {
  // void StoreSum(s32* p, s32 a, s32 b) { *p = a + b; }
  //   0x80000000: addu $a1, $a1, $a2  (0x00a62821)
  //   0x80000004: jr   $ra            (0x03e00008)
  //   0x80000008: sw   $a1, 0($a0)    (0xac850000)
  std::vector<uint32_t> words = {
      0x00a62821,
      0x03e00008,
      0xac850000,
  };
  auto loaded_or = FunctionLoader::FromWords("StoreSum", words, 0x80000000);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  DecompilationContext ctx(*loaded_or);
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();
  ASSERT_TRUE(pipeline.Run(&ctx).ok());
  ASSERT_TRUE(ctx.HasRootRegion());
  ASSERT_TRUE(ctx.HasAst());

  std::string emitted = CEmitter::EmitFunction(ctx.Ast());
  EXPECT_TRUE(absl::StrContains(emitted, "StoreSum(")) << emitted;
  EXPECT_TRUE(absl::StrContains(emitted, "+")) << emitted;
}

TEST(InverseRtlToCAstStageTest, LiftsStructuredIfElseConditionalToCAst) {
  // s32 ClampPositive(s32 a) { return (a > 0) ? a : 0; }
  //   0x80000000: blez $a0, 0x80000010 (0x18800003)
  //   0x80000004: addu $v0, $a0, $zero (0x00801021)
  //   0x80000008: jr   $ra             (0x03e00008)
  //   0x8000000c: nop                  (0x00000000)
  //   0x80000010: jr   $ra             (0x03e00008)
  //   0x80000014: addu $v0, $zero, $zero (0x00001021)
  std::vector<uint32_t> words = {
      0x18800003, 0x00801021, 0x03e00008, 0x00000000, 0x03e00008, 0x00001021,
  };
  auto loaded_or = FunctionLoader::FromWords("ClampPositive", words, 0x80000000);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  DecompilationContext ctx(*loaded_or);
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();
  ASSERT_TRUE(pipeline.Run(&ctx).ok());
  ASSERT_TRUE(ctx.HasAst());

  std::string emitted = CEmitter::EmitFunction(ctx.Ast());
  EXPECT_TRUE(absl::StrContains(emitted, "ClampPositive(")) << emitted;
  EXPECT_TRUE(absl::StrContains(emitted, "if (") || absl::StrContains(emitted, "?")) << emitted;
  EXPECT_TRUE(absl::StrContains(emitted, "return")) << emitted;
}

TEST(InverseRtlToCAstStageTest, LiftsNaturalLoopToCAst) {
  // s32 CountDown(s32 n) { while (n > 0) { n--; } return n; }
  //   0x80000000: blez  $a0, 0x80000014 (0x18800004)
  //   0x80000004: nop                   (0x00000000)
  //   0x80000008: addiu $a0, $a0, -1    (0x2484ffff)
  //   0x8000000c: bgtz  $a0, 0x80000008 (0x1c80fffe)
  //   0x80000010: nop                   (0x00000000)
  //   0x80000014: jr    $ra             (0x03e00008)
  //   0x80000018: addu  $v0, $a0, $zero (0x00801021)
  std::vector<uint32_t> words = {
      0x18800004, 0x00000000, 0x2484ffff, 0x1c80fffe, 0x00000000, 0x03e00008, 0x00801021,
  };
  auto loaded_or = FunctionLoader::FromWords("CountDown", words, 0x80000000);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  DecompilationContext ctx(*loaded_or);
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();
  ASSERT_TRUE(pipeline.Run(&ctx).ok());
  ASSERT_TRUE(ctx.HasAst());

  std::string emitted = CEmitter::EmitFunction(ctx.Ast());
  EXPECT_TRUE(absl::StrContains(emitted, "do {") || absl::StrContains(emitted, "while ("))
      << emitted;
}

TEST(InverseRtlToCAstStageTest, LiftsFunctionCallAndRecoversCalleeReturnUsage) {
  // s32 CallAndAddOne(s32 a) { return Helper(a) + 1; }
  //   0x80000000: addiu $sp, $sp, -24    (0x27bdffe8)
  //   0x80000004: sw    $ra, 20($sp)     (0xafbf0014)
  //   0x80000008: jal   0x80001000       (0x0c000400)
  //   0x8000000c: nop                    (0x00000000)
  //   0x80000010: lw    $ra, 20($sp)     (0x8fbf0014)
  //   0x80000014: addiu $v0, $v0, 1      (0x24420001)
  //   0x80000018: jr    $ra              (0x03e00008)
  //   0x8000001c: addiu $sp, $sp, 24     (0x27bd0018)
  std::vector<uint32_t> words = {
      0x27bdffe8, 0xafbf0014, 0x0c000400, 0x00000000,
      0x8fbf0014, 0x24420001, 0x03e00008, 0x27bd0018,
  };
  auto symbols_or = SymbolIndex::ParseFromTextproto(R"pb(
    symbols { name: "Helper" address: 0x80001000 type: SYMBOL_FUNC }
  )pb");
  ASSERT_TRUE(symbols_or.ok()) << symbols_or.status();

  auto loaded_or = FunctionLoader::FromWords("CallAndAddOne", words, 0x80000000, &*symbols_or);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();

  DecompilationContext ctx(*loaded_or);
  InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();
  ASSERT_TRUE(pipeline.Run(&ctx).ok());
  ASSERT_TRUE(ctx.HasAst());

  std::string emitted = CEmitter::EmitFunction(ctx.Ast());
  EXPECT_TRUE(absl::StrContains(emitted, "Helper(")) << emitted;
  EXPECT_TRUE(absl::StrContains(emitted, "+ 1")) << emitted;
}

}  // namespace
}  // namespace rom_nom_nom
