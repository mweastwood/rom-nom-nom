#include "fuzzer/equivalence_runner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/endian.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom::fuzzer {

namespace {

std::optional<uint64_t> ReadPayloadValue(absl::Span<const MemoryPayload> payloads, uint32_t addr,
                                         size_t size) {
  // Iterate in reverse so later overlays (e.g. PlacedFunction jump-table overlays) take precedence.
  for (auto it = payloads.rbegin(); it != payloads.rend(); ++it) {
    const MemoryPayload& p = *it;
    if (addr >= p.address && addr + size <= p.address + p.data.size()) {
      size_t offset = addr - p.address;
      if (size == 1) return p.data[offset];
      if (size == 2) return ReadBigEndian16(p.data.data() + offset);
      if (size == 4) return ReadBigEndian32(p.data.data() + offset);
      if (size == 8) return ReadBigEndian64(p.data.data() + offset);
    }
  }
  return std::nullopt;
}

bool IsMatchingFastPassStream(absl::Span<const uint32_t> target_words, uint32_t target_vram,
                              absl::Span<const uint32_t> cand_words, uint32_t cand_vram,
                              absl::Span<const MemoryPayload> target_memory,
                              absl::Span<const MemoryPayload> cand_memory,
                              const DifferentialFuzzer::Options& fuzzer_opts) {
  absl::Span<const uint32_t> trimmed_target =
      TrimTrailingSectionAlignmentNops(target_words, target_vram);
  absl::Span<const uint32_t> trimmed_cand = TrimTrailingSectionAlignmentNops(cand_words, cand_vram);
  if (trimmed_target.size() != trimmed_cand.size()) {
    return false;
  }
  if (trimmed_target == trimmed_cand) {
    return true;
  }

  struct HiRegState {
    bool valid = false;
    bool indexed = false;
    bool mismatched_hi = false;
    uint32_t t_hi = 0;
    uint32_t c_hi = 0;
    size_t consumed_count = 0;
  };
  std::array<HiRegState, 32> hi_regs{};
  bool has_mismatched_hi = false;
  size_t matched_mismatched_hi_consumers = 0;
  size_t total_mismatched_hi = 0;

  const uint32_t t_func_end = target_vram + static_cast<uint32_t>(trimmed_target.size() * 4);
  const uint32_t c_func_end = cand_vram + static_cast<uint32_t>(trimmed_cand.size() * 4);

  auto check_scalar_or_mapped_rodata = [&](uint32_t t_addr, uint32_t c_addr, size_t load_size,
                                           bool indexed) -> bool {
    if (t_addr == c_addr) return true;
    if (fuzzer_opts.candidate_to_target_address &&
        fuzzer_opts.candidate_to_target_address(c_addr) == t_addr) {
      return true;
    }
    if (!indexed && load_size > 0) {
      auto t_val = ReadPayloadValue(target_memory, t_addr, load_size);
      auto c_val = ReadPayloadValue(cand_memory, c_addr, load_size);
      if (t_val.has_value() && c_val.has_value() && *t_val == *c_val) {
        return true;
      }
    }
    if (indexed && load_size == 4) {
      // Relocated switch jump-table load (`addu $at, $at, $idx; lw $reg, lo($at)`):
      // Verify that every consecutive in-function jump-table target offset matches between
      // target_memory and cand_memory (requiring at least 2 valid entries).
      size_t matched_entries = 0;
      for (size_t k = 0; k < 256; ++k) {
        uint32_t offset_bytes = static_cast<uint32_t>(k * 4);
        auto t_entry = ReadPayloadValue(target_memory, t_addr + offset_bytes, 4);
        auto c_entry = ReadPayloadValue(cand_memory, c_addr + offset_bytes, 4);
        if (!t_entry.has_value() || !c_entry.has_value()) break;
        uint32_t t_target = static_cast<uint32_t>(*t_entry);
        uint32_t c_target = static_cast<uint32_t>(*c_entry);
        bool t_in_func = (t_target >= target_vram && t_target < t_func_end && (t_target & 3u) == 0);
        bool c_in_func = (c_target >= cand_vram && c_target < c_func_end && (c_target & 3u) == 0);
        if (!t_in_func && !c_in_func) {
          break;
        }
        if (t_in_func != c_in_func || (t_target - target_vram) != (c_target - cand_vram)) {
          return false;
        }
        ++matched_entries;
      }
      if (matched_entries >= 2) {
        return true;
      }
    }
    return false;
  };

  for (size_t i = 0; i < trimmed_target.size(); ++i) {
    uint32_t tw = trimmed_target[i];
    uint32_t cw = trimmed_cand[i];
    uint32_t top = tw >> 26;
    uint32_t cop = cw >> 26;

    if (tw == cw) {
      // Update tracked LUI high-16 register state for identical instructions
      if (top == 0x0F) {
        uint32_t rt = (tw >> 16) & 0x1F;
        if (rt != 0) {
          hi_regs[rt] = HiRegState{.valid = true,
                                   .indexed = false,
                                   .mismatched_hi = false,
                                   .t_hi = tw & 0xFFFFu,
                                   .c_hi = cw & 0xFFFFu,
                                   .consumed_count = 0};
        }
      } else if (top == 0x00 && (tw & 0x3Fu) == 0x21) {
        // addu rd, rs, rt
        uint32_t rs = (tw >> 21) & 0x1F;
        uint32_t rt = (tw >> 16) & 0x1F;
        uint32_t rd = (tw >> 11) & 0x1F;
        if (rd != 0) {
          if (hi_regs[rs].valid && !hi_regs[rt].valid) {
            bool was_mismatched = hi_regs[rs].mismatched_hi;
            HiRegState next = hi_regs[rs];
            next.indexed = true;
            if (rd != rs && was_mismatched) {
              ++total_mismatched_hi;
            }
            hi_regs[rd] = next;
          } else if (hi_regs[rt].valid && !hi_regs[rs].valid) {
            bool was_mismatched = hi_regs[rt].mismatched_hi;
            HiRegState next = hi_regs[rt];
            next.indexed = true;
            if (rd != rt && was_mismatched) {
              ++total_mismatched_hi;
            }
            hi_regs[rd] = next;
          } else {
            hi_regs[rd] = HiRegState{};
          }
        }
      } else {
        auto inst_or = DecodeInstruction(tw, target_vram + static_cast<uint32_t>(i * 4));
        if (inst_or.ok()) {
          RegisterUseDef ud = GetInstructionUseDef(*inst_or);
          for (Register d : ud.gpr_defs) {
            uint32_t d_idx = static_cast<uint32_t>(d);
            if (d_idx < 32 && d_idx != 0) {
              if (hi_regs[d_idx].valid && hi_regs[d_idx].mismatched_hi &&
                  hi_regs[d_idx].consumed_count == 0) {
                return false;
              }
              hi_regs[d_idx] = HiRegState{};
            }
          }
        }
      }
      continue;
    }

    // 1. Relocated R_MIPS_26 (j / jal)
    if ((top == 2 || top == 3) && top == cop) {
      uint32_t t_pc = target_vram + static_cast<uint32_t>(i * 4);
      uint32_t c_pc = cand_vram + static_cast<uint32_t>(i * 4);
      uint32_t t_dest = ((t_pc + 4) & 0xF0000000u) | ((tw & 0x03FFFFFFu) << 2);
      uint32_t c_dest = ((c_pc + 4) & 0xF0000000u) | ((cw & 0x03FFFFFFu) << 2);
      bool internal_match = (c_dest - cand_vram) == (t_dest - target_vram);
      bool mapped_match = fuzzer_opts.candidate_to_target_address &&
                          fuzzer_opts.candidate_to_target_address(c_dest) == t_dest;
      if (internal_match || mapped_match) {
        continue;
      }
      return false;
    }

    // 2. Relocated R_MIPS_HI16 / R_MIPS_LO16 with identical opcode and register fields
    if ((tw & 0xFFFF0000u) == (cw & 0xFFFF0000u)) {
      if (top == 0x0F) {
        // LUI rt, imm
        uint32_t rt = (tw >> 16) & 0x1F;
        if (rt == 0) return false;
        if (hi_regs[rt].valid && hi_regs[rt].mismatched_hi && hi_regs[rt].consumed_count == 0) {
          return false;
        }
        hi_regs[rt] = HiRegState{.valid = true,
                                 .indexed = false,
                                 .mismatched_hi = true,
                                 .t_hi = tw & 0xFFFFu,
                                 .c_hi = cw & 0xFFFFu,
                                 .consumed_count = 0};
        has_mismatched_hi = true;
        ++total_mismatched_hi;
        continue;
      }

      uint32_t rs = (tw >> 21) & 0x1F;
      uint32_t rt = (tw >> 16) & 0x1F;
      size_t load_size = 0;
      bool is_lo16_op = false;
      bool clobbers_rt_gpr = false;
      switch (top) {
        case 0x09:  // addiu
          is_lo16_op = true;
          load_size = 0;
          clobbers_rt_gpr = true;
          break;
        case 0x20:  // lb
        case 0x24:  // lbu
          is_lo16_op = true;
          load_size = 1;
          clobbers_rt_gpr = true;
          break;
        case 0x21:  // lh
        case 0x25:  // lhu
          is_lo16_op = true;
          load_size = 2;
          clobbers_rt_gpr = true;
          break;
        case 0x23:  // lw
        case 0x27:  // lwu
          is_lo16_op = true;
          load_size = 4;
          clobbers_rt_gpr = true;
          break;
        case 0x31:  // lwc1
          is_lo16_op = true;
          load_size = 4;
          clobbers_rt_gpr = false;
          break;
        case 0x35:  // ldc1
        case 0x37:  // ld
          is_lo16_op = true;
          load_size = 8;
          clobbers_rt_gpr = (top == 0x37);
          break;
        default:
          break;
      }

      if (is_lo16_op && hi_regs[rs].valid) {
        int32_t t_lo = static_cast<int16_t>(tw & 0xFFFFu);
        int32_t c_lo = static_cast<int16_t>(cw & 0xFFFFu);
        uint32_t t_addr = (hi_regs[rs].t_hi << 16) + static_cast<uint32_t>(t_lo);
        uint32_t c_addr = (hi_regs[rs].c_hi << 16) + static_cast<uint32_t>(c_lo);
        if (check_scalar_or_mapped_rodata(t_addr, c_addr, load_size, hi_regs[rs].indexed)) {
          if (hi_regs[rs].mismatched_hi && hi_regs[rs].consumed_count == 0) {
            ++matched_mismatched_hi_consumers;
          }
          ++hi_regs[rs].consumed_count;
          if (clobbers_rt_gpr && rt != 0) {
            if (rt != rs && hi_regs[rt].valid && hi_regs[rt].mismatched_hi &&
                hi_regs[rt].consumed_count == 0) {
              return false;
            }
            hi_regs[rt] = HiRegState{};
          }
          continue;
        }
      }
    }

    return false;
  }

  return !has_mismatched_hi || matched_mismatched_hi_consumers >= total_mismatched_hi;
}

}  // namespace

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

  const auto& target_memory = !target.target_initial_memory.empty()
                                  ? target.target_initial_memory
                                  : options_.fuzzer_options.target_initial_memory;
  const auto& cand_memory = !target.candidate_initial_memory.empty()
                                ? target.candidate_initial_memory
                                : options_.fuzzer_options.candidate_initial_memory;

  // Fast-Pass: If trimmed instructions match 100% byte-for-byte (or up to relocated
  // R_MIPS_26 j/jal targets and verified .rodata literal/jump-table HI16/LO16 pairs),
  // trivially equivalent.
  if (IsMatchingFastPassStream(target.target_words, target.vram, target.candidate_words, cand_vram,
                               target_memory, cand_memory, options_.fuzzer_options)) {
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
