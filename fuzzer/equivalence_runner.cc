#include "fuzzer/equivalence_runner.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom::fuzzer {

FunctionEquivalenceResult EquivalenceRunner::EvaluateFunction(
    const FunctionEquivalenceTarget& target, DifferentialFuzzer* fuzzer) {
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

  uint32_t cand_vram = target.candidate_vram != 0 ? target.candidate_vram : target.vram;
  FunctionStructuralComparison struct_comp =
      CompareFunctionStructure(target.target_words, target.vram, target.candidate_words, cand_vram);
  res.frame_size_matches = struct_comp.frame_size_matches;
  res.saved_registers_match = struct_comp.saved_registers_match;
  res.cfg_topology_matches = struct_comp.cfg_topology_matches;
  res.normalized_lcs_instructions = struct_comp.normalized_lcs_instructions;
  res.trimmed_target_instruction_count = struct_comp.target.instruction_count;

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
        options_.pointer_vectors_per_function, options_.fuzzer_options.pointer_buffer_vram,
        /*buffer_size=*/256, options_.seed);
    test_cases.insert(test_cases.end(), pointer_cases.begin(), pointer_cases.end());
  }

  res.fuzz_vectors_evaluated = test_cases.size();

  DifferentialFuzzer::Options func_options = options_.fuzzer_options;
  if (!target.target_initial_memory.empty()) {
    func_options.target_initial_memory = target.target_initial_memory;
  }
  if (!target.candidate_initial_memory.empty()) {
    func_options.candidate_initial_memory = target.candidate_initial_memory;
  }
  auto target_insts = DecodeSequence(target.target_words, target.vram);
  if (target_insts.ok()) {
    auto target_cfg = ControlFlowGraph::Build(*target_insts);
    if (target_cfg.ok()) {
      func_options.uninitialized_callee_saved_registers =
          DetectUninitializedCalleeSavedRegisters(*target_cfg);
      bool target_returns_f0 = DetermineReturnsF0(*target_cfg);
      bool target_returns_v0 = DetermineReturnsV0(*target_cfg);

      if (target_returns_f0) {
        bool is_double = DetermineReturnsF0Double(*target_cfg);
        if (is_double) {
          func_options.check_f0_double = true;
          func_options.check_f0 = false;
        } else {
          func_options.check_f0 = true;
          func_options.check_f0_double = false;
        }
        func_options.check_v0 = false;
      } else if (target_returns_v0) {
        func_options.check_v0 = true;
        func_options.check_f0 = false;
        func_options.check_f0_double = false;
      } else {
        func_options.check_v0 = false;
        func_options.check_f0 = false;
        func_options.check_f0_double = false;
      }
    }
  }

  DifferentialFuzzer* active_fuzzer = (fuzzer != nullptr) ? fuzzer : &fuzzer_;
  active_fuzzer->SetOptions(std::move(func_options));
  auto comparisons = active_fuzzer->RunBattery(target.target_words, target.vram,
                                               target.candidate_words, cand_vram, test_cases);

  for (size_t i = 0; i < comparisons.size(); ++i) {
    const auto& comp = comparisons[i];
    if (comp.result != EquivalenceResult::kEquivalent) {
      res.status = FunctionEquivalenceStatus::kDivergent;
      res.counterexample = comp;
      res.details = absl::StrFormat(
          "Divergence on vector #%zu: %s (args: a0=0x%08X, a1=0x%08X, a2=0x%08X, a3=0x%08X)", i,
          comp.failure_reason, comp.test_case.a0, comp.test_case.a1, comp.test_case.a2,
          comp.test_case.a3);
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
  summary.function_results.resize(targets.size());

  size_t thread_count = options_.num_threads;
  if (thread_count == 0) {
    unsigned int hw = std::thread::hardware_concurrency();
    thread_count = hw > 0 ? hw : 1;
  }

  // If single-threaded or small batch, execute sequentially
  if (thread_count <= 1 || targets.size() <= 1) {
    for (size_t i = 0; i < targets.size(); ++i) {
      summary.function_results[i] = EvaluateFunction(targets[i]);
      if (options_.stop_on_first_divergence &&
          summary.function_results[i].status == FunctionEquivalenceStatus::kDivergent) {
        summary.function_results.resize(i + 1);
        summary.total_functions = i + 1;
        break;
      }
    }
  } else {
    // Multi-threaded work-stealing pool across targets
    std::atomic<size_t> next_target_idx{0};
    std::atomic<size_t> first_divergent_idx{targets.size()};
    std::vector<uint8_t> evaluated(targets.size(), 0);
    size_t actual_threads = std::min(thread_count, targets.size());
    std::vector<std::thread> workers;
    workers.reserve(actual_threads);

    EquivalenceRunnerOptions worker_options = options_;
    worker_options.num_threads = 1;  // Prevent worker recursion

    for (size_t t = 0; t < actual_threads; ++t) {
      workers.emplace_back([&, worker_options]() {
        EquivalenceRunner worker_runner(worker_options);
        while (true) {
          size_t idx = next_target_idx.fetch_add(1, std::memory_order_relaxed);
          if (idx >= targets.size()) {
            break;
          }
          if (options_.stop_on_first_divergence &&
              idx >= first_divergent_idx.load(std::memory_order_relaxed)) {
            break;
          }
          summary.function_results[idx] = worker_runner.EvaluateFunction(targets[idx]);
          evaluated[idx] = 1;
          if (options_.stop_on_first_divergence &&
              summary.function_results[idx].status == FunctionEquivalenceStatus::kDivergent) {
            size_t prev = first_divergent_idx.load(std::memory_order_relaxed);
            while (idx < prev && !first_divergent_idx.compare_exchange_weak(
                                     prev, idx, std::memory_order_relaxed)) {
            }
            break;
          }
        }
      });
    }

    for (auto& w : workers) {
      if (w.joinable()) {
        w.join();
      }
    }

    if (options_.stop_on_first_divergence &&
        first_divergent_idx.load(std::memory_order_relaxed) < targets.size()) {
      size_t limit = first_divergent_idx.load(std::memory_order_relaxed);
      std::vector<FunctionEquivalenceResult> filtered;
      filtered.reserve(limit + 1);
      for (size_t i = 0; i <= limit; ++i) {
        if (evaluated[i]) {
          filtered.push_back(std::move(summary.function_results[i]));
        }
      }
      summary.function_results = std::move(filtered);
      summary.total_functions = summary.function_results.size();
    }
  }

  // Aggregate stats deterministically in target order
  for (const auto& res : summary.function_results) {
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
    if (res.frame_size_matches) summary.frame_size_matches++;
    if (res.saved_registers_match) summary.saved_registers_matches++;
    if (res.cfg_topology_matches) summary.cfg_topology_matches++;
    if (res.frame_size_matches && res.saved_registers_match && res.cfg_topology_matches) {
      summary.structurally_isomorphic_functions++;
    }
    summary.total_target_instructions += res.trimmed_target_instruction_count;
    summary.total_normalized_lcs_instructions += res.normalized_lcs_instructions;
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
  absl::StrAppend(&out, "Structural Readiness Metrics:\n");
  absl::StrAppendFormat(&out, "  1. Stack Frame Size Match:        %zu / %zu (%.1f%%)\n",
                        summary.frame_size_matches, summary.total_functions,
                        summary.FrameSizeMatchPercentage());
  absl::StrAppendFormat(&out, "  2. Callee-Saved Register Match:   %zu / %zu (%.1f%%)\n",
                        summary.saved_registers_matches, summary.total_functions,
                        summary.SavedRegistersMatchPercentage());
  absl::StrAppendFormat(&out, "  3. CFG Block & Branch Match:      %zu / %zu (%.1f%%)\n",
                        summary.cfg_topology_matches, summary.total_functions,
                        summary.CfgTopologyMatchPercentage());
  absl::StrAppendFormat(&out, "     Structurally Isomorphic (1-3): %zu / %zu (%.1f%%)\n",
                        summary.structurally_isomorphic_functions, summary.total_functions,
                        summary.StructurallyIsomorphicPercentage());
  absl::StrAppendFormat(&out, "  4. Normalized Instruction LCS:    %zu / %zu (%.1f%%)\n",
                        summary.total_normalized_lcs_instructions,
                        summary.total_target_instructions, summary.NormalizedLcsPercentage());
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
