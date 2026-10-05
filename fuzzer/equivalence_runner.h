#ifndef ROM_NOM_NOM_FUZZER_EQUIVALENCE_RUNNER_H_
#define ROM_NOM_NOM_FUZZER_EQUIVALENCE_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom::fuzzer {

struct FunctionEquivalenceTarget {
  std::string name;
  uint32_t vram = 0;
  uint32_t candidate_vram = 0;
  std::vector<uint32_t> target_words;
  std::vector<uint32_t> candidate_words;
};

enum class FunctionEquivalenceStatus {
  kExactBinaryMatch,      // Binary instructions match 100% byte-for-byte (fast-pass)
  kFuzzProvenEquivalent,  // Binary instructions differ, but differential fuzzing proved equivalence
  kDivergent,             // Behavior diverges on test vectors (counterexample found)
  kCompilationOrExtractError,  // Empty or invalid instruction stream
};

struct FunctionEquivalenceResult {
  std::string name;
  uint32_t vram = 0;
  FunctionEquivalenceStatus status = FunctionEquivalenceStatus::kExactBinaryMatch;
  size_t target_instruction_count = 0;
  size_t candidate_instruction_count = 0;
  size_t fuzz_vectors_evaluated = 0;
  std::optional<DifferentialComparison> counterexample;
  std::string details;
};

struct ModuleEquivalenceSummary {
  std::string module_name;
  size_t total_functions = 0;
  size_t exact_binary_matches = 0;
  size_t fuzz_proven_equivalent = 0;
  size_t divergent_functions = 0;
  size_t error_functions = 0;

  double EquivalencePercentage() const {
    if (total_functions == 0) return 100.0;
    return (static_cast<double>(exact_binary_matches + fuzz_proven_equivalent) /
            static_cast<double>(total_functions)) *
           100.0;
  }

  std::vector<FunctionEquivalenceResult> function_results;
};

struct EquivalenceRunnerOptions {
  size_t random_vectors_per_function = 100;
  size_t pointer_vectors_per_function = 20;
  uint32_t seed = 42;
  DifferentialFuzzer::Options fuzzer_options;
  bool stop_on_first_divergence = false;
  size_t num_threads = 0;  // 0 = auto-detect hardware concurrency
};

class EquivalenceRunner {
 public:
  EquivalenceRunner() = default;
  explicit EquivalenceRunner(EquivalenceRunnerOptions options)
      : options_(options), fuzzer_(options.fuzzer_options) {}

  // Evaluates equivalence for a single function target.
  FunctionEquivalenceResult EvaluateFunction(const FunctionEquivalenceTarget& target);

  // Evaluates equivalence for a collection of function targets in a module.
  ModuleEquivalenceSummary EvaluateModule(std::string_view module_name,
                                          absl::Span<const FunctionEquivalenceTarget> targets);

  // Generates a human-readable text report.
  static std::string FormatReport(const ModuleEquivalenceSummary& summary, bool verbose = false);

 private:
  EquivalenceRunnerOptions options_;
  DifferentialFuzzer fuzzer_;
};

}  // namespace rom_nom_nom::fuzzer

#endif  // ROM_NOM_NOM_FUZZER_EQUIVALENCE_RUNNER_H_
