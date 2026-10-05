#ifndef ROM_NOM_NOM_FUZZER_DIFFERENTIAL_FUZZER_H_
#define ROM_NOM_NOM_FUZZER_DIFFERENTIAL_FUZZER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "fuzzer/mips_emulator.h"

namespace rom_nom_nom::fuzzer {

struct MemoryPayload {
  uint32_t address = 0;
  std::vector<uint8_t> data;

  bool operator==(const MemoryPayload& other) const {
    return address == other.address && data == other.data;
  }
};

struct FuzzTestCase {
  uint32_t a0 = 0;
  uint32_t a1 = 0;
  uint32_t a2 = 0;
  uint32_t a3 = 0;

  float f12 = 0.0f;
  float f14 = 0.0f;
  double f12_double = 0.0;
  double f14_double = 0.0;

  std::vector<MemoryPayload> initial_memory;
};

enum class EquivalenceResult {
  kEquivalent,
  kStatusMismatch,
  kReturnValueMismatch,
  kMemoryWriteMismatch,
  kCalleeSavedMismatch,
  kCallMismatch,
  kExecutionError,
};

struct DifferentialComparison {
  EquivalenceResult result = EquivalenceResult::kEquivalent;
  ExecutionResult target_result;
  ExecutionResult candidate_result;
  FuzzTestCase test_case;
  std::string failure_reason;
};

class DifferentialFuzzer {
 public:
  struct Options {
    uint64_t max_steps_per_run = 50000;
    uint32_t initial_sp = 0x801F0000;
    bool check_callee_saved = true;
    bool check_memory_writes = true;
    bool check_v0 = true;
    bool check_v1 = false;
    bool check_f0 = false;
    bool check_f0_double = false;
    bool intercept_external_calls = true;
    bool check_external_calls = false;
    bool filter_local_stack_writes = true;
    bool mock_mmio = true;
  };

  DifferentialFuzzer() = default;
  explicit DifferentialFuzzer(Options options) : options_(options) {}

  // Executes and differentially compares target and candidate code on a single test case.
  DifferentialComparison Compare(const std::vector<uint32_t>& target_code, uint32_t target_vram,
                                 const std::vector<uint32_t>& candidate_code,
                                 uint32_t candidate_vram, const FuzzTestCase& test_case);

  // Executes a battery of test cases. Returns comparison for each test case.
  std::vector<DifferentialComparison> RunBattery(const std::vector<uint32_t>& target_code,
                                                 uint32_t target_vram,
                                                 const std::vector<uint32_t>& candidate_code,
                                                 uint32_t candidate_vram,
                                                 const std::vector<FuzzTestCase>& test_cases);

  // Test Case Generators
  static std::vector<FuzzTestCase> GenerateBoundaryCases();
  static std::vector<FuzzTestCase> GenerateRandomCases(size_t count, uint32_t seed = 42);
  static std::vector<FuzzTestCase> GeneratePointerCases(size_t count,
                                                        uint32_t buffer_vram = 0x80100000,
                                                        size_t buffer_size = 256,
                                                        uint32_t seed = 42);

 private:
  void SetupEmulator(MipsEmulator* emu, const FuzzTestCase& test_case) const;
  void EvaluateComparison(DifferentialComparison* comp, absl::Span<const uint32_t> target_code,
                          uint32_t target_vram, absl::Span<const uint32_t> candidate_code,
                          uint32_t candidate_vram, const FuzzTestCase& test_case);

  Options options_;
  MipsEmulator target_emu_;
  MipsEmulator candidate_emu_;
};

}  // namespace rom_nom_nom::fuzzer

#endif  // ROM_NOM_NOM_FUZZER_DIFFERENTIAL_FUZZER_H_
