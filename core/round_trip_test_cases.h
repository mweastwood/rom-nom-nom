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

struct RoundTripCompilerFlagProfile {
  std::string suffix;
  std::vector<std::string> c_flags;
};

// Returns the compiler/assembler flag profiles swept across every round-trip C test case:
//   - O0_WaG:  -O0 -mips2 -mcpu=r4000 -Wa,-g
//   - O1_WaG:  -O1 -mips2 -mcpu=r4000 -Wa,-g
//   - O2_WaG:  -O2 -mips2 -mcpu=r4000 -Wa,-g
//   - O2_WaO2: -O2 -mips2 -mcpu=r4000 -Wa,-O2
//   - O3_WaG:  -O3 -mips2 -mcpu=r4000 -Wa,-g
const std::vector<RoundTripCompilerFlagProfile>& GetRoundTripCompilerFlagProfiles();

// Returns the base library of unexpanded C test cases (one entry per C function).
const std::vector<RoundTripTestCase>& GetBaseRoundTripTestCases();

// Returns the full library of round-trip C test cases swept across all compiler flag profiles in
// `GetRoundTripCompilerFlagProfiles()`, shared across the splitter, lifter, and tuner test suites.
const std::vector<RoundTripTestCase>& GetSharedRoundTripTestCases();

}  // namespace rom_nom_nom

#endif  // CORE_ROUND_TRIP_TEST_CASES_H_
