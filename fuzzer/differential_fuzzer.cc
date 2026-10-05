#include "fuzzer/differential_fuzzer.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"

namespace rom_nom_nom::fuzzer {

void DifferentialFuzzer::SetupEmulator(MipsEmulator* emu, const FuzzTestCase& test_case) const {
  emu->SetRegister(Register::kSp, options_.initial_sp);

  // Callee-saved register canary values (O32 ABI: s0-s7, gp, fp)
  emu->SetRegister(Register::kS0, 0xCAFE0010);
  emu->SetRegister(Register::kS1, 0xCAFE0011);
  emu->SetRegister(Register::kS2, 0xCAFE0012);
  emu->SetRegister(Register::kS3, 0xCAFE0013);
  emu->SetRegister(Register::kS4, 0xCAFE0014);
  emu->SetRegister(Register::kS5, 0xCAFE0015);
  emu->SetRegister(Register::kS6, 0xCAFE0016);
  emu->SetRegister(Register::kS7, 0xCAFE0017);
  emu->SetRegister(Register::kGp, 0xCAFE001C);
  emu->SetRegister(Register::kFp, 0xCAFE001E);

  // Argument registers (a0 - a3)
  emu->SetRegister(Register::kA0, test_case.a0);
  emu->SetRegister(Register::kA1, test_case.a1);
  emu->SetRegister(Register::kA2, test_case.a2);
  emu->SetRegister(Register::kA3, test_case.a3);

  // Floating point argument registers
  if (test_case.f12_double != 0.0 || test_case.f14_double != 0.0) {
    emu->SetFpDouble(FpRegister::kF12, test_case.f12_double);
    emu->SetFpDouble(FpRegister::kF14, test_case.f14_double);
  } else {
    emu->SetFpRegister(FpRegister::kF12, test_case.f12);
    emu->SetFpRegister(FpRegister::kF14, test_case.f14);
  }

  // Pre-load memory payloads
  for (const auto& payload : test_case.initial_memory) {
    if (!payload.data.empty()) {
      emu->LoadMemory(payload.address, payload.data.data(), payload.data.size());
    }
  }

  emu->SetMockMmio(options_.mock_mmio);
}

void DifferentialFuzzer::EvaluateComparison(DifferentialComparison* comp,
                                            absl::Span<const uint32_t> target_code,
                                            uint32_t target_vram,
                                            absl::Span<const uint32_t> candidate_code,
                                            uint32_t candidate_vram,
                                            const FuzzTestCase& test_case) {
  SetupEmulator(&target_emu_, test_case);
  SetupEmulator(&candidate_emu_, test_case);

  CalleeSavedRegisters target_initial_saved = target_emu_.GetCalleeSavedRegisters();
  CalleeSavedRegisters candidate_initial_saved = candidate_emu_.GetCalleeSavedRegisters();

  comp->target_result = target_emu_.RunFunction(target_vram, options_.max_steps_per_run);
  comp->candidate_result = candidate_emu_.RunFunction(candidate_vram, options_.max_steps_per_run);

  // 1. Check ExecutionStatus
  if (comp->target_result.status != comp->candidate_result.status) {
    comp->result = EquivalenceResult::kStatusMismatch;
    comp->failure_reason = absl::StrFormat(
        "ExecutionStatus mismatch: target=%d (%s) vs candidate=%d (%s)",
        static_cast<int>(comp->target_result.status), comp->target_result.error_message,
        static_cast<int>(comp->candidate_result.status), comp->candidate_result.error_message);
  } else if (comp->target_result.status != ExecutionStatus::kHaltedReturn) {
    // If both trapped or halted identically with a non-return status, consider equivalent in error.
    comp->result = EquivalenceResult::kEquivalent;
  } else if (options_.check_callee_saved &&
             !(target_emu_.GetCalleeSavedRegisters() == target_initial_saved)) {
    comp->result = EquivalenceResult::kCalleeSavedMismatch;
    comp->failure_reason = "Target code corrupted callee-saved registers";
  } else if (options_.check_callee_saved &&
             !(candidate_emu_.GetCalleeSavedRegisters() == candidate_initial_saved)) {
    comp->result = EquivalenceResult::kCalleeSavedMismatch;
    comp->failure_reason = "Candidate code corrupted callee-saved registers";
  } else if (options_.check_v0 && comp->target_result.v0 != comp->candidate_result.v0) {
    comp->result = EquivalenceResult::kReturnValueMismatch;
    comp->failure_reason =
        absl::StrFormat("Return value v0 mismatch: target=0x%08X (%d) vs candidate=0x%08X (%d)",
                        comp->target_result.v0, static_cast<int32_t>(comp->target_result.v0),
                        comp->candidate_result.v0, static_cast<int32_t>(comp->candidate_result.v0));
  } else if (options_.check_v1 && comp->target_result.v1 != comp->candidate_result.v1) {
    comp->result = EquivalenceResult::kReturnValueMismatch;
    comp->failure_reason =
        absl::StrFormat("Return value v1 mismatch: target=0x%08X (%d) vs candidate=0x%08X (%d)",
                        comp->target_result.v1, static_cast<int32_t>(comp->target_result.v1),
                        comp->candidate_result.v1, static_cast<int32_t>(comp->candidate_result.v1));
  } else if (options_.check_f0 && comp->target_result.f0_bits != comp->candidate_result.f0_bits) {
    comp->result = EquivalenceResult::kReturnValueMismatch;
    comp->failure_reason =
        absl::StrFormat("Return value f0 mismatch: target=%f (0x%08X) vs candidate=%f (0x%08X)",
                        comp->target_result.f0, comp->target_result.f0_bits,
                        comp->candidate_result.f0, comp->candidate_result.f0_bits);
  } else if (options_.check_f0_double &&
             comp->target_result.f0_double_bits != comp->candidate_result.f0_double_bits) {
    comp->result = EquivalenceResult::kReturnValueMismatch;
    comp->failure_reason = absl::StrFormat(
        "Return value f0_double mismatch: target=%f (0x%016llX) vs candidate=%f (0x%016llX)",
        comp->target_result.f0_double, comp->target_result.f0_double_bits,
        comp->candidate_result.f0_double, comp->candidate_result.f0_double_bits);
  } else if (options_.check_memory_writes) {
    std::vector<MemoryWrite> target_writes;
    std::vector<MemoryWrite> candidate_writes;
    if (options_.filter_local_stack_writes) {
      for (const auto& w : comp->target_result.write_log) {
        if (!(w.address < options_.initial_sp && w.address >= options_.initial_sp - 0x4000)) {
          target_writes.push_back(w);
        }
      }
      for (const auto& w : comp->candidate_result.write_log) {
        if (!(w.address < options_.initial_sp && w.address >= options_.initial_sp - 0x4000)) {
          candidate_writes.push_back(w);
        }
      }
    } else {
      target_writes = comp->target_result.write_log;
      candidate_writes = comp->candidate_result.write_log;
    }

    if (target_writes.size() != candidate_writes.size()) {
      comp->result = EquivalenceResult::kMemoryWriteMismatch;
      comp->failure_reason = absl::StrFormat(
          "Write log size mismatch: target logged %zu writes vs candidate logged %zu writes",
          target_writes.size(), candidate_writes.size());
    } else {
      std::sort(target_writes.begin(), target_writes.end(),
                [](const MemoryWrite& a, const MemoryWrite& b) {
                  if (a.address != b.address) return a.address < b.address;
                  if (a.size != b.size) return a.size < b.size;
                  return a.value < b.value;
                });
      std::sort(candidate_writes.begin(), candidate_writes.end(),
                [](const MemoryWrite& a, const MemoryWrite& b) {
                  if (a.address != b.address) return a.address < b.address;
                  if (a.size != b.size) return a.size < b.size;
                  return a.value < b.value;
                });
      comp->result = EquivalenceResult::kEquivalent;
      for (size_t i = 0; i < target_writes.size(); ++i) {
        const auto& tw = target_writes[i];
        const auto& cw = candidate_writes[i];
        if (!(tw == cw)) {
          comp->result = EquivalenceResult::kMemoryWriteMismatch;
          comp->failure_reason = absl::StrFormat(
              "Write log mismatch at store #%zu: target(addr=0x%08X, val=0x%llX, size=%d) vs "
              "candidate(addr=0x%08X, val=0x%llX, size=%d)",
              i, tw.address, tw.value, tw.size, cw.address, cw.value, cw.size);
          break;
        }
      }
    }
  } else {
    comp->result = EquivalenceResult::kEquivalent;
  }

  if (comp->result == EquivalenceResult::kEquivalent && options_.check_external_calls) {
    if (comp->target_result.call_log.size() != comp->candidate_result.call_log.size()) {
      comp->result = EquivalenceResult::kCallMismatch;
      comp->failure_reason = absl::StrFormat(
          "Call log size mismatch: target logged %zu calls vs candidate logged %zu calls",
          comp->target_result.call_log.size(), comp->candidate_result.call_log.size());
    } else {
      for (size_t i = 0; i < comp->target_result.call_log.size(); ++i) {
        const auto& tc = comp->target_result.call_log[i];
        const auto& cc = comp->candidate_result.call_log[i];
        if (!(tc == cc)) {
          comp->result = EquivalenceResult::kCallMismatch;
          comp->failure_reason = absl::StrFormat(
              "Call log mismatch at call #%zu: target(addr=0x%08X, a0=0x%X, a1=0x%X, a2=0x%X, "
              "a3=0x%X) vs "
              "candidate(addr=0x%08X, a0=0x%X, a1=0x%X, a2=0x%X, a3=0x%X)",
              i, tc.target_address, tc.a0, tc.a1, tc.a2, tc.a3, cc.target_address, cc.a0, cc.a1,
              cc.a2, cc.a3);
          break;
        }
      }
    }
  }

  // Roll back memory writes recorded during execution
  target_emu_.RollbackWrites(target_code, target_vram);
  candidate_emu_.RollbackWrites(candidate_code, candidate_vram);

  // Clear memory payloads that were pre-loaded for this test case
  for (const auto& payload : test_case.initial_memory) {
    target_emu_.ClearMemory(payload.address, payload.data.size());
    candidate_emu_.ClearMemory(payload.address, payload.data.size());
  }

  target_emu_.Reset();
  candidate_emu_.Reset();
}

DifferentialComparison DifferentialFuzzer::Compare(const std::vector<uint32_t>& target_code,
                                                   uint32_t target_vram,
                                                   const std::vector<uint32_t>& candidate_code,
                                                   uint32_t candidate_vram,
                                                   const FuzzTestCase& test_case) {
  DifferentialComparison comp;
  comp.test_case = test_case;

  target_emu_.Reset();
  candidate_emu_.Reset();

  if (!target_emu_.LoadWords(target_vram, target_code)) {
    comp.result = EquivalenceResult::kExecutionError;
    comp.failure_reason = absl::StrFormat("Failed to load target code at 0x%08X", target_vram);
    return comp;
  }
  if (!candidate_emu_.LoadWords(candidate_vram, candidate_code)) {
    comp.result = EquivalenceResult::kExecutionError;
    comp.failure_reason =
        absl::StrFormat("Failed to load candidate code at 0x%08X", candidate_vram);
    target_emu_.ClearMemory(target_vram, target_code.size() * sizeof(uint32_t));
    return comp;
  }

  if (options_.intercept_external_calls) {
    target_emu_.SetInterceptExternalCalls(true);
    target_emu_.SetCodeBounds(
        target_vram, target_vram + static_cast<uint32_t>(target_code.size() * sizeof(uint32_t)));
    candidate_emu_.SetInterceptExternalCalls(true);
    candidate_emu_.SetCodeBounds(
        candidate_vram,
        candidate_vram + static_cast<uint32_t>(candidate_code.size() * sizeof(uint32_t)));
  }

  EvaluateComparison(&comp, target_code, target_vram, candidate_code, candidate_vram, test_case);

  target_emu_.ClearCodeBounds();
  candidate_emu_.ClearCodeBounds();
  target_emu_.ClearMemory(target_vram, target_code.size() * sizeof(uint32_t));
  candidate_emu_.ClearMemory(candidate_vram, candidate_code.size() * sizeof(uint32_t));

  return comp;
}

std::vector<DifferentialComparison> DifferentialFuzzer::RunBattery(
    const std::vector<uint32_t>& target_code, uint32_t target_vram,
    const std::vector<uint32_t>& candidate_code, uint32_t candidate_vram,
    const std::vector<FuzzTestCase>& test_cases) {
  std::vector<DifferentialComparison> results;
  results.reserve(test_cases.size());

  target_emu_.Reset();
  candidate_emu_.Reset();

  if (!target_emu_.LoadWords(target_vram, target_code)) {
    DifferentialComparison comp;
    comp.result = EquivalenceResult::kExecutionError;
    comp.failure_reason = absl::StrFormat("Failed to load target code at 0x%08X", target_vram);
    results.push_back(comp);
    return results;
  }
  if (!candidate_emu_.LoadWords(candidate_vram, candidate_code)) {
    DifferentialComparison comp;
    comp.result = EquivalenceResult::kExecutionError;
    comp.failure_reason =
        absl::StrFormat("Failed to load candidate code at 0x%08X", candidate_vram);
    target_emu_.ClearMemory(target_vram, target_code.size() * sizeof(uint32_t));
    results.push_back(comp);
    return results;
  }

  if (options_.intercept_external_calls) {
    target_emu_.SetInterceptExternalCalls(true);
    target_emu_.SetCodeBounds(
        target_vram, target_vram + static_cast<uint32_t>(target_code.size() * sizeof(uint32_t)));
    candidate_emu_.SetInterceptExternalCalls(true);
    candidate_emu_.SetCodeBounds(
        candidate_vram,
        candidate_vram + static_cast<uint32_t>(candidate_code.size() * sizeof(uint32_t)));
  }

  for (const auto& tc : test_cases) {
    DifferentialComparison comp;
    comp.test_case = tc;
    EvaluateComparison(&comp, target_code, target_vram, candidate_code, candidate_vram, tc);
    results.push_back(comp);
    if (comp.result != EquivalenceResult::kEquivalent) {
      break;
    }
  }

  target_emu_.ClearCodeBounds();
  candidate_emu_.ClearCodeBounds();
  target_emu_.ClearMemory(target_vram, target_code.size() * sizeof(uint32_t));
  candidate_emu_.ClearMemory(candidate_vram, candidate_code.size() * sizeof(uint32_t));

  return results;
}

std::vector<FuzzTestCase> DifferentialFuzzer::GenerateBoundaryCases() {
  std::vector<uint32_t> int_boundaries = {
      0,           1,       static_cast<uint32_t>(-1), 2, static_cast<uint32_t>(-2), 0x7FFF, 0x8000,
      0xFFFF,      0x10000,
      0x7FFFFFFFu,  // INT32_MAX
      0x80000000u,  // INT32_MIN
      0xFFFFFFFFu,
  };

  std::vector<float> float_boundaries = {
      0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f, 100.0f, -100.0f,
  };

  std::vector<double> double_boundaries = {
      0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 100.0, -100.0, 3.141592653589793,
  };

  std::vector<FuzzTestCase> cases;
  cases.reserve(int_boundaries.size() * 2 + float_boundaries.size());

  // Pure integer boundary combinations on a0/a1
  for (uint32_t v : int_boundaries) {
    FuzzTestCase tc;
    tc.a0 = v;
    tc.a1 = 0;
    cases.push_back(tc);

    tc.a0 = 0;
    tc.a1 = v;
    cases.push_back(tc);

    tc.a0 = v;
    tc.a1 = v;
    cases.push_back(tc);
  }

  // Float boundaries
  for (float f : float_boundaries) {
    FuzzTestCase tc;
    tc.f12 = f;
    tc.f14 = 1.0f;
    cases.push_back(tc);
  }

  // Double boundaries
  for (double d : double_boundaries) {
    FuzzTestCase tc;
    tc.f12_double = d;
    tc.f14_double = 1.0;
    cases.push_back(tc);
  }

  return cases;
}

std::vector<FuzzTestCase> DifferentialFuzzer::GenerateRandomCases(size_t count, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<uint32_t> dist_u32(0, 0xFFFFFFFFu);
  std::uniform_real_distribution<float> dist_float(-1000.0f, 1000.0f);
  std::uniform_real_distribution<double> dist_double(-100000.0, 100000.0);

  std::vector<FuzzTestCase> cases;
  cases.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    FuzzTestCase tc;
    tc.a0 = dist_u32(rng);
    tc.a1 = dist_u32(rng);
    tc.a2 = dist_u32(rng);
    tc.a3 = dist_u32(rng);
    tc.f12 = dist_float(rng);
    tc.f14 = dist_float(rng);
    tc.f12_double = dist_double(rng);
    tc.f14_double = dist_double(rng);
    cases.push_back(tc);
  }

  return cases;
}

std::vector<FuzzTestCase> DifferentialFuzzer::GeneratePointerCases(size_t count,
                                                                   uint32_t buffer_vram,
                                                                   size_t buffer_size,
                                                                   uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<uint32_t> dist_byte(0, 255);
  std::uniform_int_distribution<uint32_t> dist_len(1, static_cast<uint32_t>(buffer_size));

  std::vector<FuzzTestCase> cases;
  cases.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    FuzzTestCase tc;
    tc.a0 = buffer_vram;
    tc.a1 = dist_len(rng);

    MemoryPayload payload;
    payload.address = buffer_vram;
    payload.data.resize(buffer_size);
    for (size_t b = 0; b < buffer_size; ++b) {
      payload.data[b] = static_cast<uint8_t>(dist_byte(rng));
    }
    tc.initial_memory.push_back(std::move(payload));
    cases.push_back(tc);
  }

  return cases;
}

}  // namespace rom_nom_nom::fuzzer
