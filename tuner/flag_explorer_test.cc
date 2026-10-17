#include "tuner/flag_explorer.h"

#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_join.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(FlagExplorerTest, BaseFlagsAlwaysFirstAndDeduplicated) {
  std::vector<std::string> base = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  auto profiles = FlagExplorer::GenerateCandidateFlags(base);
  ASSERT_FALSE(profiles.empty());
  EXPECT_EQ(profiles.front(), base);

  absl::flat_hash_set<std::string> seen;
  for (const auto& p : profiles) {
    EXPECT_TRUE(seen.insert(absl::StrJoin(p, " ")).second);
  }
}

TEST(FlagExplorerTest, RespectsMaxProfilesLimit) {
  FlagExplorerOptions opts;
  opts.max_profiles = 5;
  auto profiles = FlagExplorer::GenerateCandidateFlags({"-O2"}, opts);
  EXPECT_EQ(profiles.size(), 5u);
}

TEST(FlagExplorerTest, GeneratesAssemblerAndOptLevelVariations) {
  FlagExplorerOptions opts;
  opts.max_profiles = 0;
  auto profiles =
      FlagExplorer::GenerateCandidateFlags({"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}, opts);

  bool found_o1 = false;
  bool found_o0 = false;
  bool found_wa_o2 = false;
  bool found_omit_fp = false;

  for (const auto& p : profiles) {
    std::string joined = absl::StrJoin(p, " ");
    if (joined == "-O1 -mips2 -mcpu=r4000 -Wa,-g") found_o1 = true;
    if (joined == "-O0 -mips2 -mcpu=r4000 -Wa,-g") found_o0 = true;
    if (joined == "-O2 -mips2 -mcpu=r4000 -Wa,-O2") found_wa_o2 = true;
    if (joined == "-O2 -mips2 -mcpu=r4000 -Wa,-g -fomit-frame-pointer") found_omit_fp = true;
  }

  EXPECT_TRUE(found_o1);
  EXPECT_TRUE(found_o0);
  EXPECT_TRUE(found_wa_o2);
  EXPECT_TRUE(found_omit_fp);
}

}  // namespace
}  // namespace rom_nom_nom
