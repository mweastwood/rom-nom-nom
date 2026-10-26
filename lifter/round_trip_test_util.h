#ifndef ROM_NOM_NOM_LIFTER_ROUND_TRIP_TEST_UTIL_H_
#define ROM_NOM_NOM_LIFTER_ROUND_TRIP_TEST_UTIL_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "core/elf_reader.h"
#include "core/round_trip_runner.h"
#include "core/round_trip_test_cases.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {

// Concrete `RoundTripStage` for `LifterPipeline`: lifts the compiled target binary to C,
// recompiles it with the original toolchain, and evaluates structural isomorphism (`passed`)
// while recording bit-exactness for suite-level reporting.
class LifterRoundTripStage : public RoundTripStage {
 public:
  std::string_view Name() const override { return "lifter"; }

  absl::StatusOr<RoundTripCaseOutcome> Execute(const PreparedRoundTripCase& prepared) override;
};

class RoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite();
  static void TearDownTestSuite();

  void AssertRoundTripIsomorphic(const RoundTripTestCase& test_case);
  void AssertRoundTripBitExact(std::string_view func_name, std::string_view original_c,
                               const RoundTripOptions& options = {});

  static bool CompareFunctions(const ElfFunction& orig, const ElfFunction& cand, uint32_t base_vram,
                               std::string_view orig_c, std::string_view lifted_c,
                               std::string* diagnostic_out);

  static RoundTripRunner* runner;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_ROUND_TRIP_TEST_UTIL_H_
