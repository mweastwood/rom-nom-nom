#ifndef ROM_NOM_NOM_LIFTER_ROUND_TRIP_TEST_UTIL_H_
#define ROM_NOM_NOM_LIFTER_ROUND_TRIP_TEST_UTIL_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/compiler.h"
#include "core/elf_reader.h"
#include "core/toolchain.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {

struct RoundTripOptions {
  std::vector<std::string> c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  uint32_t base_vram = 0x80020000;
  std::string extra_types_header;
};

class RoundTripTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite();
  static void TearDownTestSuite();

  void SetUp() override;
  void TearDown() override;

  void AssertRoundTripBitExact(std::string_view func_name, std::string_view original_c,
                               const RoundTripOptions& options = {});

  static bool CompareFunctions(const ElfFunction& orig, const ElfFunction& cand, uint32_t base_vram,
                               std::string_view orig_c, std::string_view lifted_c,
                               std::string* diagnostic_out);

 private:
  std::filesystem::path work_dir_;
  std::unique_ptr<Compiler> compiler_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_ROUND_TRIP_TEST_UTIL_H_
