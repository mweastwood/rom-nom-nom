#include "core/compiler_flags.h"

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(CompilerFlagsTest, ParsesFiveStandardProfilesAndStagePredicates) {
  const ParsedCompilerFlags o0 = ParsedCompilerFlags::Parse("-O0 -Wa,-g");
  EXPECT_EQ(o0.opt_level, 0);
  EXPECT_FALSE(o0.RunsDbrSchedule());
  EXPECT_FALSE(o0.RunsSched1());
  EXPECT_FALSE(o0.RunsSched2());
  EXPECT_FALSE(o0.SplitsMultiInsnMovesBeforeDbr());
  EXPECT_FALSE(o0.RunsCse2());
  EXPECT_FALSE(o0.RunsOptimizerPasses());
  EXPECT_FALSE(o0.GasOptimizesAndReordersInsns());
  EXPECT_EQ(o0.ProfileKey(), "O0_WaG");

  const ParsedCompilerFlags o1 = ParsedCompilerFlags::Parse("-O1 -Wa,-g");
  EXPECT_EQ(o1.opt_level, 1);
  EXPECT_TRUE(o1.RunsDbrSchedule());
  EXPECT_FALSE(o1.RunsSched1());
  EXPECT_FALSE(o1.RunsSched2());
  EXPECT_FALSE(o1.SplitsMultiInsnMovesBeforeDbr());
  EXPECT_FALSE(o1.RunsCse2());
  EXPECT_TRUE(o1.RunsOptimizerPasses());
  EXPECT_FALSE(o1.GasOptimizesAndReordersInsns());
  EXPECT_EQ(o1.ProfileKey(), "O1_WaG");

  const ParsedCompilerFlags o2 = ParsedCompilerFlags::Parse("-O2 -Wa,-g");
  EXPECT_EQ(o2.opt_level, 2);
  EXPECT_TRUE(o2.RunsDbrSchedule());
  EXPECT_TRUE(o2.RunsSched1());
  EXPECT_TRUE(o2.RunsSched2());
  EXPECT_TRUE(o2.SplitsMultiInsnMovesBeforeDbr());
  EXPECT_TRUE(o2.RunsCse2());
  EXPECT_FALSE(o2.flag_inline_functions);
  EXPECT_FALSE(o2.GasOptimizesAndReordersInsns());
  EXPECT_EQ(o2.ProfileKey(), "O2_WaG");

  const ParsedCompilerFlags o2_wa_o2 = ParsedCompilerFlags::Parse("-O2 -Wa,-O2");
  EXPECT_EQ(o2_wa_o2.opt_level, 2);
  EXPECT_EQ(o2_wa_o2.gas_opt_level, 2);
  EXPECT_FALSE(o2_wa_o2.gas_debug);
  EXPECT_TRUE(o2_wa_o2.GasOptimizesAndReordersInsns());
  EXPECT_EQ(o2_wa_o2.ProfileKey(), "O2_WaO2");

  const ParsedCompilerFlags o3 = ParsedCompilerFlags::Parse("-O3 -Wa,-g");
  EXPECT_EQ(o3.opt_level, 3);
  EXPECT_TRUE(o3.RunsDbrSchedule());
  EXPECT_TRUE(o3.SplitsMultiInsnMovesBeforeDbr());
  EXPECT_TRUE(o3.flag_inline_functions);
  EXPECT_EQ(o3.ProfileKey(), "O3_WaG");
}

TEST(CompilerFlagsTest, RespectsExplicitPassAndTargetFlagOverrides) {
  const ParsedCompilerFlags custom = ParsedCompilerFlags::Parse(
      "-O2 -fno-delayed-branch -fno-schedule-insns -fno-schedule-insns2 "
      "-fno-omit-frame-pointer -unsigned-char -mips2 -mcpu=r4000 -G 8 -Wa,-O2,-g");
  EXPECT_EQ(custom.opt_level, 2);
  EXPECT_FALSE(custom.RunsDbrSchedule());
  EXPECT_FALSE(custom.RunsSched1());
  EXPECT_FALSE(custom.RunsSched2());
  EXPECT_FALSE(custom.SplitsMultiInsnMovesBeforeDbr());
  EXPECT_FALSE(custom.flag_omit_frame_pointer);
  EXPECT_FALSE(custom.signed_char);
  EXPECT_EQ(custom.mips_isa, 2);
  EXPECT_EQ(custom.mcpu, "r4000");
  EXPECT_EQ(custom.small_data_threshold, 8);
  EXPECT_TRUE(custom.gas_debug);
  EXPECT_FALSE(custom.GasOptimizesAndReordersInsns());
}

}  // namespace
}  // namespace rom_nom_nom
