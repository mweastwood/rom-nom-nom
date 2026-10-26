#include "core/round_trip_test_cases.h"
#include "gtest/gtest.h"
#include "lifter/round_trip_test_util.h"

namespace rom_nom_nom {
namespace {

TEST_P(RoundTripTest, LiftsToStructurallyIsomorphicC) {
  AssertRoundTripIsomorphic(GetParam());
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, RoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
