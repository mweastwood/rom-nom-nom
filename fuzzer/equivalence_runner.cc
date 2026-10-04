#include "fuzzer/equivalence_runner.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom::fuzzer {

FunctionEquivalenceResult EquivalenceRunner::EvaluateFunction(
    const FunctionEquivalenceTarget& target) {
  FunctionEquivalenceResult res;
  res.name = target.name;
  res.vram = target.vram;
  res.target_instruction_count = target.target_words.size();
  res.candidate_instruction_count = target.candidate_words.size();

  if (target.target_words.empty() || target.candidate_words.empty()) {
    res.status = FunctionEquivalenceStatus::kCompilationOrExtractError;
    res.details = "Target or candidate instructions are empty";
    return res;
  }

  // Fast-Pass: If instructions match 100% byte-for-byte, trivially equivalent
  if (target.target_words == target.candidate_words) {
    res.status = FunctionEquivalenceStatus::kExactBinaryMatch;
    res.fuzz_vectors_evaluated = 0;
    res.details = "100% bit-exact instruction match (fast-pass)";
    return res;
  }

  // Differential Fuzzing Battery

  std::vector<FuzzTestCase> test_cases = DifferentialFuzzer::GenerateBoundaryCases();
  if (options_.random_vectors_per_function > 0) {
    auto random_cases = DifferentialFuzzer::GenerateRandomCases(
        options_.random_vectors_per_function, options_.seed);
    test_cases.insert(test_cases.end(), random_cases.begin(), random_cases.end());
  }
  if (options_.pointer_vectors_per_function > 0) {
    auto pointer_cases = DifferentialFuzzer::GeneratePointerCases(
        options_.pointer_vectors_per_function, /*buffer_vram=*/0x80100000,
        /*buffer_size=*/256, options_.seed);
    test_cases.insert(test_cases.end(), pointer_cases.begin(), pointer_cases.end());
  }

  res.fuzz_vectors_evaluated = test_cases.size();

  auto comparisons = fuzzer_.RunBattery(target.target_words, target.vram, target.candidate_words,
                                        target.vram, test_cases);

  for (size_t i = 0; i < comparisons.size(); ++i) {
    const auto& comp = comparisons[i];
    if (comp.result != EquivalenceResult::kEquivalent) {
      res.status = FunctionEquivalenceStatus::kDivergent;
      res.counterexample = comp;
      res.details = absl::StrFormat("Divergence on vector #%zu: %s", i, comp.failure_reason);
      return res;
    }
  }

  res.status = FunctionEquivalenceStatus::kFuzzProvenEquivalent;
  res.details =
      absl::StrFormat("Proved functionally equivalent across %zu vectors", test_cases.size());
  return res;
}

ModuleEquivalenceSummary EquivalenceRunner::EvaluateModule(
    std::string_view module_name, absl::Span<const FunctionEquivalenceTarget> targets) {
  ModuleEquivalenceSummary summary;
  summary.module_name = std::string(module_name);
  summary.total_functions = targets.size();
  summary.function_results.reserve(targets.size());

  for (const auto& target : targets) {
    FunctionEquivalenceResult res = EvaluateFunction(target);
    switch (res.status) {
      case FunctionEquivalenceStatus::kExactBinaryMatch:
        summary.exact_binary_matches++;
        break;
      case FunctionEquivalenceStatus::kFuzzProvenEquivalent:
        summary.fuzz_proven_equivalent++;
        break;
      case FunctionEquivalenceStatus::kDivergent:
        summary.divergent_functions++;
        break;
      case FunctionEquivalenceStatus::kCompilationOrExtractError:
        summary.error_functions++;
        break;
    }

    summary.function_results.push_back(res);

    if (options_.stop_on_first_divergence && res.status == FunctionEquivalenceStatus::kDivergent) {
      break;
    }
  }

  return summary;
}

std::string EquivalenceRunner::FormatReport(const ModuleEquivalenceSummary& summary, bool verbose) {
  std::string out;
  absl::StrAppend(
      &out, "================================================================================\n");
  absl::StrAppendFormat(&out, "EQUIVALENCE FUZZING REPORT: %s\n", summary.module_name);
  absl::StrAppend(
      &out, "================================================================================\n");
  absl::StrAppendFormat(&out, "Total Functions Evaluated: %zu\n", summary.total_functions);

  double pct_exact = summary.total_functions > 0
                         ? (100.0 * summary.exact_binary_matches / summary.total_functions)
                         : 0.0;
  double pct_fuzz = summary.total_functions > 0
                        ? (100.0 * summary.fuzz_proven_equivalent / summary.total_functions)
                        : 0.0;
  double pct_div = summary.total_functions > 0
                       ? (100.0 * summary.divergent_functions / summary.total_functions)
                       : 0.0;
  double pct_err = summary.total_functions > 0
                       ? (100.0 * summary.error_functions / summary.total_functions)
                       : 0.0;

  absl::StrAppendFormat(&out, "  Exact Binary Matches (Fast-Pass): %zu (%.1f%%)\n",
                        summary.exact_binary_matches, pct_exact);
  absl::StrAppendFormat(&out, "  Fuzz-Proven Equivalent:           %zu (%.1f%%)\n",
                        summary.fuzz_proven_equivalent, pct_fuzz);
  absl::StrAppendFormat(&out, "  Divergent (Behavioral Bugs):      %zu (%.1f%%)\n",
                        summary.divergent_functions, pct_div);
  absl::StrAppendFormat(&out, "  Compilation/Extract Errors:       %zu (%.1f%%)\n",
                        summary.error_functions, pct_err);
  absl::StrAppendFormat(&out, "Overall Functional Equivalence:    %.1f%%\n",
                        summary.EquivalencePercentage());
  absl::StrAppend(
      &out, "--------------------------------------------------------------------------------\n");

  // Report divergences
  if (summary.divergent_functions > 0) {
    absl::StrAppend(&out, "Divergent Functions:\n");
    for (const auto& res : summary.function_results) {
      if (res.status == FunctionEquivalenceStatus::kDivergent) {
        absl::StrAppendFormat(&out, "  - %s (0x%08X):\n", res.name, res.vram);
        absl::StrAppendFormat(&out, "      %s\n", res.details);
        if (res.counterexample.has_value()) {
          const auto& tc = res.counterexample->test_case;
          absl::StrAppendFormat(
              &out, "      Counterexample args: a0=0x%08X, a1=0x%08X, a2=0x%08X, a3=0x%08X\n",
              tc.a0, tc.a1, tc.a2, tc.a3);
        }
      }
    }
    absl::StrAppend(
        &out, "--------------------------------------------------------------------------------\n");
  }

  // Report Fuzz-Proven functions
  if (verbose || summary.fuzz_proven_equivalent > 0) {
    absl::StrAppend(
        &out,
        "Fuzz-Proven Equivalent Functions (Instruction mismatch, Semantically Equivalent):\n");
    for (const auto& res : summary.function_results) {
      if (res.status == FunctionEquivalenceStatus::kFuzzProvenEquivalent) {
        absl::StrAppendFormat(&out, "  - %s: %zu instructions, %zu vectors evaluated\n", res.name,
                              res.target_instruction_count, res.fuzz_vectors_evaluated);
      }
    }
    absl::StrAppend(
        &out, "--------------------------------------------------------------------------------\n");
  }

  absl::StrAppend(
      &out, "================================================================================\n");
  return out;
}

}  // namespace rom_nom_nom::fuzzer
