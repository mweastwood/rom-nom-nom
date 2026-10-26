#ifndef CORE_ROUND_TRIP_TEST_CASES_H_
#define CORE_ROUND_TRIP_TEST_CASES_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rom_nom_nom {

struct RoundTripOptions {
  std::vector<std::string> c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  uint32_t base_vram = 0x80020000;
  std::string extra_types_header;
};

struct RoundTripTestCase {
  std::string test_name;
  std::string function_name;
  std::string original_c;
  RoundTripOptions options;
};

// Returns the canonical library of round-trip C test cases shared across the splitter,
// lifter, and tuner round-trip test suites.
const std::vector<RoundTripTestCase>& GetSharedRoundTripTestCases();

}  // namespace rom_nom_nom

#endif  // CORE_ROUND_TRIP_TEST_CASES_H_
