#include "fuzzer/differential_fuzzer.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace rom_nom_nom::fuzzer {
namespace {

bool CheckCalleeSavedRegisters(const CalleeSavedRegisters& target_init,
                               const CalleeSavedRegisters& target_final,
                               const CalleeSavedRegisters& cand_init,
                               const CalleeSavedRegisters& cand_final,
                               std::string* failure_reason) {
  struct RegRef {
    const char* name;
    uint32_t CalleeSavedRegisters::*member;
  };
  static constexpr RegRef kRegs[] = {
      {"s0", &CalleeSavedRegisters::s0}, {"s1", &CalleeSavedRegisters::s1},
      {"s2", &CalleeSavedRegisters::s2}, {"s3", &CalleeSavedRegisters::s3},
      {"s4", &CalleeSavedRegisters::s4}, {"s5", &CalleeSavedRegisters::s5},
      {"s6", &CalleeSavedRegisters::s6}, {"s7", &CalleeSavedRegisters::s7},
      {"gp", &CalleeSavedRegisters::gp}, {"sp", &CalleeSavedRegisters::sp},
      {"fp", &CalleeSavedRegisters::fp},
  };

  std::string diffs;
  for (const auto& reg : kRegs) {
    uint32_t t_init = target_init.*(reg.member);
    uint32_t t_final = target_final.*(reg.member);
    uint32_t c_init = cand_init.*(reg.member);
    uint32_t c_final = cand_final.*(reg.member);

    // If target preserved this register, candidate must also preserve it.
    if (t_init == t_final) {
      if (c_init != c_final) {
        absl::StrAppend(&diffs,
                        absl::StrFormat(" %s: init=0x%x curr=0x%x", reg.name, c_init, c_final));
      }
    } else {
      // Target did not preserve this register (e.g. hand-written asm trashing a callee-saved reg).
      // Candidate is valid if it preserved it (standard ABI compliance) or matched target's final
      // value.
      if (c_init != c_final && c_final != t_final) {
        absl::StrAppend(
            &diffs, absl::StrFormat(" %s: target=0x%x cand=0x%x (neither preserved nor matched)",
                                    reg.name, t_final, c_final));
      }
    }
  }

  if (!diffs.empty()) {
    *failure_reason = absl::StrCat("Candidate code corrupted callee-saved registers:", diffs);
    return false;
  }
  return true;
}

}  // namespace

void DifferentialFuzzer::SetupEmulator(MipsEmulator* emu, const FuzzTestCase& test_case) const {
  emu->SetRegister(Register::kSp, options_.initial_sp);

  // Callee-saved register canary values (O32 ABI: s0-s7, gp, fp).
  // If a register is used before definition in target, it represents an uninitialized
  // variable, so initialize with 0 to match C semantics.
  auto set_callee_saved = [&](Register reg, uint32_t canary) {
    if (options_.uninitialized_callee_saved_registers.contains(reg)) {
      emu->SetRegister(reg, 0);
    } else {
      emu->SetRegister(reg, canary);
    }
  };

  set_callee_saved(Register::kS0, 0xCAFE0010);
  set_callee_saved(Register::kS1, 0xCAFE0011);
  set_callee_saved(Register::kS2, 0xCAFE0012);
  set_callee_saved(Register::kS3, 0xCAFE0013);
  set_callee_saved(Register::kS4, 0xCAFE0014);
  set_callee_saved(Register::kS5, 0xCAFE0015);
  set_callee_saved(Register::kS6, 0xCAFE0016);
  set_callee_saved(Register::kS7, 0xCAFE0017);
  set_callee_saved(Register::kGp, 0xCAFE001C);
  set_callee_saved(Register::kFp, 0xCAFE001E);

  // Reset caller-saved temporary registers to 0 for cross-vector determinism
  emu->SetRegister(Register::kAt, 0);
  emu->SetRegister(Register::kV0, 0);
  emu->SetRegister(Register::kV1, 0);
  emu->SetRegister(Register::kT0, 0);
  emu->SetRegister(Register::kT1, 0);
  emu->SetRegister(Register::kT2, 0);
  emu->SetRegister(Register::kT3, 0);
  emu->SetRegister(Register::kT4, 0);
  emu->SetRegister(Register::kT5, 0);
  emu->SetRegister(Register::kT6, 0);
  emu->SetRegister(Register::kT7, 0);
  emu->SetRegister(Register::kT8, 0);
  emu->SetRegister(Register::kT9, 0);

  // Argument registers (a0 - a3)
  emu->SetRegister(Register::kA0, test_case.a0);
  emu->SetRegister(Register::kA1, test_case.a1);
  emu->SetRegister(Register::kA2, test_case.a2);
  emu->SetRegister(Register::kA3, test_case.a3);

  // Floating point argument registers
  if (options_.check_f0_double) {
    if (test_case.f12_double != 0.0 || test_case.f14_double != 0.0) {
      emu->SetFpDouble(FpRegister::kF12, test_case.f12_double);
      emu->SetFpDouble(FpRegister::kF14, test_case.f14_double);
    } else {
      emu->SetFpDouble(FpRegister::kF12, static_cast<double>(test_case.f12));
      emu->SetFpDouble(FpRegister::kF14, static_cast<double>(test_case.f14));
    }
  } else {
    if (test_case.f12_double != 0.0 || test_case.f14_double != 0.0) {
      emu->SetFpDouble(FpRegister::kF12, test_case.f12_double);
      emu->SetFpDouble(FpRegister::kF14, test_case.f14_double);
    } else {
      emu->SetFpRegister(FpRegister::kF12, test_case.f12);
      emu->SetFpRegister(FpRegister::kF14, test_case.f14);
    }
  }

  // Stack arguments (sp + 16, sp + 20, ...)
  uint8_t stack_buf[16 * 4] = {0};
  for (size_t i = 0; i < 16; ++i) {
    uint32_t val = (i < test_case.stack_args.size()) ? test_case.stack_args[i] : 0;
    stack_buf[i * 4] = static_cast<uint8_t>(val >> 24);
    stack_buf[i * 4 + 1] = static_cast<uint8_t>(val >> 16);
    stack_buf[i * 4 + 2] = static_cast<uint8_t>(val >> 8);
    stack_buf[i * 4 + 3] = static_cast<uint8_t>(val);
  }
  emu->LoadMemory(options_.initial_sp + 16, stack_buf, sizeof(stack_buf));

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

  if (comp->target_result.status == ExecutionStatus::kMaxStepsReached &&
      comp->candidate_result.status != ExecutionStatus::kMaxStepsReached) {
    SetupEmulator(&target_emu_, test_case);
    comp->target_result = target_emu_.RunFunction(target_vram, options_.max_steps_per_run * 5);
  } else if (comp->candidate_result.status == ExecutionStatus::kMaxStepsReached &&
             comp->target_result.status != ExecutionStatus::kMaxStepsReached) {
    SetupEmulator(&candidate_emu_, test_case);
    comp->candidate_result =
        candidate_emu_.RunFunction(candidate_vram, options_.max_steps_per_run * 5);
  }

  uint32_t cand_v0 = comp->candidate_result.v0;
  if (cand_v0 != comp->target_result.v0 && options_.candidate_to_target_address) {
    uint32_t mapped_v0 = options_.candidate_to_target_address(cand_v0);
    if (mapped_v0 == comp->target_result.v0) {
      cand_v0 = mapped_v0;
    }
  }

  // 1. Check ExecutionStatus
  bool both_faulted = (comp->target_result.status == ExecutionStatus::kMemoryFault ||
                       comp->target_result.status == ExecutionStatus::kIntegerOverflow) &&
                      (comp->candidate_result.status == ExecutionStatus::kMemoryFault ||
                       comp->candidate_result.status == ExecutionStatus::kIntegerOverflow);

  if (comp->target_result.status != comp->candidate_result.status && !both_faulted) {
    comp->result = EquivalenceResult::kStatusMismatch;
    comp->failure_reason = absl::StrFormat(
        "ExecutionStatus mismatch: target=%d (%s, steps=%llu, v0=0x%x) vs candidate=%d (%s, "
        "steps=%llu)",
        static_cast<int>(comp->target_result.status), comp->target_result.error_message,
        comp->target_result.total_steps, comp->target_result.v0,
        static_cast<int>(comp->candidate_result.status), comp->candidate_result.error_message,
        comp->candidate_result.total_steps);
  } else if (comp->target_result.status != ExecutionStatus::kHaltedReturn || both_faulted) {
    // If both trapped or halted with a non-return status, consider equivalent in error.
    comp->result = EquivalenceResult::kEquivalent;
  } else if (options_.check_callee_saved &&
             !CheckCalleeSavedRegisters(target_initial_saved, target_emu_.GetCalleeSavedRegisters(),
                                        candidate_initial_saved,
                                        candidate_emu_.GetCalleeSavedRegisters(),
                                        &comp->failure_reason)) {
    comp->result = EquivalenceResult::kCalleeSavedMismatch;
  } else if (options_.check_v0 && comp->target_result.v0 != cand_v0) {
    comp->result = EquivalenceResult::kReturnValueMismatch;
    comp->failure_reason =
        absl::StrFormat("Return value v0 mismatch: target=0x%08X (%d) vs candidate=0x%08X (%d)",
                        comp->target_result.v0, static_cast<int32_t>(comp->target_result.v0),
                        cand_v0, static_cast<int32_t>(cand_v0));
  } else if (options_.check_v1 && comp->target_result.v1 != comp->candidate_result.v1) {
    comp->result = EquivalenceResult::kReturnValueMismatch;
    comp->failure_reason =
        absl::StrFormat("Return value v1 mismatch: target=0x%08X (%d) vs candidate=0x%08X (%d)",
                        comp->target_result.v1, static_cast<int32_t>(comp->target_result.v1),
                        comp->candidate_result.v1, static_cast<int32_t>(comp->candidate_result.v1));
  } else if (options_.check_f0) {
    auto fp_equal = [](float a, float b, uint32_t a_bits, uint32_t b_bits) {
      if (a_bits == b_bits) return true;
      if (a == b) return true;
      if (std::isnan(a) && std::isnan(b)) return true;
      return false;
    };
    if (!fp_equal(comp->target_result.f0, comp->candidate_result.f0, comp->target_result.f0_bits,
                  comp->candidate_result.f0_bits)) {
      comp->result = EquivalenceResult::kReturnValueMismatch;
      comp->failure_reason =
          absl::StrFormat("Return value f0 mismatch: target=%f (0x%08X) vs candidate=%f (0x%08X)",
                          comp->target_result.f0, comp->target_result.f0_bits,
                          comp->candidate_result.f0, comp->candidate_result.f0_bits);
    }
  } else if (options_.check_f0_double) {
    auto fp_double_equal = [](double a, double b, uint64_t a_bits, uint64_t b_bits) {
      if (a_bits == b_bits) return true;
      if (a == b) return true;
      if (std::isnan(a) && std::isnan(b)) return true;
      return false;
    };
    if (!fp_double_equal(comp->target_result.f0_double, comp->candidate_result.f0_double,
                         comp->target_result.f0_double_bits,
                         comp->candidate_result.f0_double_bits)) {
      comp->result = EquivalenceResult::kReturnValueMismatch;
      comp->failure_reason = absl::StrFormat(
          "Return value f0_double mismatch: target=%f (0x%016llX) vs candidate=%f (0x%016llX)",
          comp->target_result.f0_double, comp->target_result.f0_double_bits,
          comp->candidate_result.f0_double, comp->candidate_result.f0_double_bits);
    }
  } else if (options_.check_memory_writes) {
    std::vector<MemoryWrite> target_writes = comp->target_result.write_log;
    std::vector<MemoryWrite> candidate_writes = comp->candidate_result.write_log;

    if (options_.filter_local_stack_writes) {
      auto is_local_stack = [&](const MemoryWrite& w) {
        if (w.is_stack_relative) return true;
        if (w.address < options_.initial_sp + 0x100 && w.address >= options_.initial_sp - 0x10000) {
          return true;
        }
        return false;
      };
      target_writes.erase(
          std::remove_if(target_writes.begin(), target_writes.end(), is_local_stack),
          target_writes.end());
      candidate_writes.erase(
          std::remove_if(candidate_writes.begin(), candidate_writes.end(), is_local_stack),
          candidate_writes.end());
    }

    if (options_.candidate_to_target_address) {
      absl::flat_hash_set<std::tuple<uint32_t, uint64_t, uint8_t>> exact_target_stores;
      absl::flat_hash_set<uint32_t> target_addresses;
      for (const auto& tw : target_writes) {
        exact_target_stores.insert({tw.address, tw.value, tw.size});
        target_addresses.insert(tw.address);
      }
      uint32_t buf_start = options_.pointer_buffer_vram;
      uint32_t buf_end = options_.pointer_buffer_vram + options_.pointer_buffer_size;
      bool is_pointer_case = (test_case.a0 >= buf_start && test_case.a0 < buf_end) ||
                             (test_case.a1 >= buf_start && test_case.a1 < buf_end);
      for (auto& cw : candidate_writes) {
        if (!is_pointer_case || !(cw.address >= buf_start && cw.address < buf_end)) {
          uint32_t mapped_addr = options_.candidate_to_target_address(cw.address);
          if (target_addresses.contains(mapped_addr) || !target_addresses.contains(cw.address)) {
            cw.address = mapped_addr;
          }
        }
        if (cw.size == 4 && !exact_target_stores.contains({cw.address, cw.value, cw.size})) {
          uint32_t mapped_val =
              options_.candidate_to_target_address(static_cast<uint32_t>(cw.value));
          if (exact_target_stores.contains({cw.address, mapped_val, cw.size})) {
            cw.value = mapped_val;
          }
        }
      }
    }

    absl::flat_hash_map<uint32_t, uint8_t> target_bytes;
    for (const auto& tw : target_writes) {
      for (uint8_t b = 0; b < tw.size; ++b) {
        uint8_t byte_val = static_cast<uint8_t>((tw.value >> ((tw.size - 1 - b) * 8)) & 0xFF);
        target_bytes[tw.address + b] = byte_val;
      }
    }
    absl::flat_hash_map<uint32_t, uint8_t> candidate_bytes;
    for (const auto& cw : candidate_writes) {
      for (uint8_t b = 0; b < cw.size; ++b) {
        uint8_t byte_val = static_cast<uint8_t>((cw.value >> ((cw.size - 1 - b) * 8)) & 0xFF);
        candidate_bytes[cw.address + b] = byte_val;
      }
    }

    bool writes_match = (target_bytes == candidate_bytes);
    if (!writes_match && target_writes.size() == candidate_writes.size()) {
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
      writes_match = true;
      for (size_t i = 0; i < target_writes.size(); ++i) {
        if (!(target_writes[i] == candidate_writes[i])) {
          writes_match = false;
          break;
        }
      }
    }

    if (!writes_match) {
      comp->result = EquivalenceResult::kMemoryWriteMismatch;
      if (target_writes.size() != candidate_writes.size()) {
        std::string tw_str;
        for (const auto& w : target_writes) {
          absl::StrAppend(&tw_str, absl::StrFormat(" [0x%X:0x%X]", w.address, w.value));
        }
        std::string cw_str;
        for (const auto& w : candidate_writes) {
          absl::StrAppend(&cw_str, absl::StrFormat(" [0x%X:0x%X]", w.address, w.value));
        }
        comp->failure_reason = absl::StrFormat(
            "Write log size mismatch: target logged %zu writes (%s) vs candidate logged %zu writes "
            "(%s)",
            target_writes.size(), tw_str, candidate_writes.size(), cw_str);
      } else {
        for (size_t i = 0; i < target_writes.size(); ++i) {
          const auto& tw = target_writes[i];
          const auto& cw = candidate_writes[i];
          if (!(tw == cw)) {
            std::string sample;
            for (size_t k = (i > 2 ? i - 2 : 0); k < std::min(target_writes.size(), i + 5); ++k) {
              absl::StrAppend(
                  &sample,
                  absl::StrFormat(
                      "\n  #%zu: target(0x%X, val=0x%X, sz=%d) vs cand(0x%X, val=0x%X, sz=%d)", k,
                      target_writes[k].address, target_writes[k].value, target_writes[k].size,
                      candidate_writes[k].address, candidate_writes[k].value,
                      candidate_writes[k].size));
            }
            comp->failure_reason = absl::StrFormat(
                "Write log mismatch at store #%zu: target(addr=0x%08X, val=0x%llX, size=%d) vs "
                "candidate(addr=0x%08X, val=0x%llX, size=%d)%s",
                i, tw.address, tw.value, tw.size, cw.address, cw.value, cw.size, sample);
            break;
          }
        }
      }
    } else {
      comp->result = EquivalenceResult::kEquivalent;
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

  for (const auto& payload : options_.target_initial_memory) {
    if (!payload.data.empty()) {
      target_emu_.LoadMemory(payload.address, payload.data.data(), payload.data.size());
    }
  }
  for (const auto& payload : options_.candidate_initial_memory) {
    if (!payload.data.empty()) {
      candidate_emu_.LoadMemory(payload.address, payload.data.data(), payload.data.size());
    }
  }

  if (options_.target_read_hook) {
    target_emu_.SetReadHook(options_.target_read_hook);
  }
  if (options_.candidate_read_hook) {
    candidate_emu_.SetReadHook(options_.candidate_read_hook);
  }

  EvaluateComparison(&comp, target_code, target_vram, candidate_code, candidate_vram, test_case);

  target_emu_.ClearReadHook();
  candidate_emu_.ClearReadHook();

  for (const auto& payload : options_.target_initial_memory) {
    target_emu_.ClearMemory(payload.address, payload.data.size());
  }
  for (const auto& payload : options_.candidate_initial_memory) {
    candidate_emu_.ClearMemory(payload.address, payload.data.size());
  }

  target_emu_.ClearCodeBounds();
  candidate_emu_.ClearCodeBounds();
  target_emu_.ClearMemory(target_vram, target_code.size() * sizeof(uint32_t));
  candidate_emu_.ClearMemory(candidate_vram, candidate_code.size() * sizeof(uint32_t));
  target_emu_.ClearMemory(options_.initial_sp + 16, 16 * sizeof(uint32_t));
  candidate_emu_.ClearMemory(options_.initial_sp + 16, 16 * sizeof(uint32_t));

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

  for (const auto& payload : options_.target_initial_memory) {
    if (!payload.data.empty()) {
      target_emu_.LoadMemory(payload.address, payload.data.data(), payload.data.size());
    }
  }
  for (const auto& payload : options_.candidate_initial_memory) {
    if (!payload.data.empty()) {
      candidate_emu_.LoadMemory(payload.address, payload.data.data(), payload.data.size());
    }
  }

  if (options_.target_read_hook) {
    target_emu_.SetReadHook(options_.target_read_hook);
  }
  if (options_.candidate_read_hook) {
    candidate_emu_.SetReadHook(options_.candidate_read_hook);
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

  target_emu_.ClearReadHook();
  candidate_emu_.ClearReadHook();

  for (const auto& payload : options_.target_initial_memory) {
    target_emu_.ClearMemory(payload.address, payload.data.size());
  }
  for (const auto& payload : options_.candidate_initial_memory) {
    candidate_emu_.ClearMemory(payload.address, payload.data.size());
  }

  target_emu_.ClearCodeBounds();
  candidate_emu_.ClearCodeBounds();
  target_emu_.ClearMemory(target_vram, target_code.size() * sizeof(uint32_t));
  candidate_emu_.ClearMemory(candidate_vram, candidate_code.size() * sizeof(uint32_t));
  target_emu_.ClearMemory(options_.initial_sp + 16, 16 * sizeof(uint32_t));
  candidate_emu_.ClearMemory(options_.initial_sp + 16, 16 * sizeof(uint32_t));

  return results;
}

std::vector<FuzzTestCase> DifferentialFuzzer::GenerateBoundaryCases() {
  std::vector<uint32_t> int_boundaries = {
      0,
      1,
      static_cast<uint32_t>(-1),
      2,
      static_cast<uint32_t>(-2),
      0x7FFF,
      0x8000,
      0xFFFF,
      0x10000,
      0x55555555u,
      0xAAAAAAAAu,
      0x000000FFu,
      0x0000FF00u,
      0x00FF0000u,
      0xFF000000u,
      0x7FFFFFFFu,  // INT32_MAX
      0x80000000u,  // INT32_MIN
      0xFFFFFFFFu,
  };

  std::vector<float> float_boundaries = {
      0.0f,
      -0.0f,
      1.0f,
      -1.0f,
      0.5f,
      -0.5f,
      2.0f,
      -2.0f,
      100.0f,
      -100.0f,
      std::numeric_limits<float>::min(),
      std::numeric_limits<float>::max(),
      std::numeric_limits<float>::denorm_min(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
  };

  std::vector<double> double_boundaries = {
      0.0,
      -0.0,
      1.0,
      -1.0,
      0.5,
      -0.5,
      2.0,
      -2.0,
      100.0,
      -100.0,
      3.141592653589793,
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
  };

  std::vector<FuzzTestCase> cases;
  cases.reserve(int_boundaries.size() * 5 + float_boundaries.size() + double_boundaries.size());

  // Integer boundary combinations across a0, a1, a2, a3, and stack args
  for (uint32_t v : int_boundaries) {
    FuzzTestCase tc;
    tc.a0 = v;
    tc.a1 = 0;
    tc.a2 = 0;
    tc.a3 = 0;
    cases.push_back(tc);

    tc.a0 = 0;
    tc.a1 = v;
    tc.a2 = 0;
    tc.a3 = 0;
    cases.push_back(tc);

    tc.a0 = 0;
    tc.a1 = 0;
    tc.a2 = v;
    tc.a3 = 0;
    cases.push_back(tc);

    tc.a0 = 0;
    tc.a1 = 0;
    tc.a2 = 0;
    tc.a3 = v;
    cases.push_back(tc);

    tc.a0 = v;
    tc.a1 = v;
    tc.a2 = v;
    tc.a3 = v;
    tc.stack_args = {v, v, v, v};
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
    if (i % 2 == 1) {
      tc.f12_double = dist_double(rng);
      tc.f14_double = dist_double(rng);
    }
    for (size_t s = 0; s < 12; ++s) {
      tc.stack_args.push_back(dist_u32(rng));
    }
    cases.push_back(tc);
  }

  return cases;
}

std::vector<FuzzTestCase> DifferentialFuzzer::GeneratePointerCases(size_t count,
                                                                   uint32_t buffer_vram,
                                                                   size_t buffer_size,
                                                                   uint32_t seed) {
  std::vector<FuzzTestCase> cases;
  if (count == 0) {
    return cases;
  }
  cases.reserve(count);

  std::mt19937 rng(seed);
  std::uniform_int_distribution<uint32_t> dist_byte(0, 255);
  std::uniform_int_distribution<uint32_t> dist_len(1, static_cast<uint32_t>(buffer_size));

  // Deterministic structured pointer cases
  // 1. All-zero memory (e.g. empty string or clear buffer)
  if (cases.size() < count) {
    FuzzTestCase tc;
    tc.a0 = buffer_vram;
    tc.a1 = static_cast<uint32_t>(buffer_size);
    MemoryPayload payload;
    payload.address = buffer_vram;
    payload.data.assign(buffer_size, 0);
    tc.initial_memory.push_back(std::move(payload));
    cases.push_back(tc);
  }

  // 2. All-0xFF memory
  if (cases.size() < count) {
    FuzzTestCase tc;
    tc.a0 = buffer_vram;
    tc.a1 = static_cast<uint32_t>(buffer_size);
    MemoryPayload payload;
    payload.address = buffer_vram;
    payload.data.assign(buffer_size, 0xFF);
    tc.initial_memory.push_back(std::move(payload));
    cases.push_back(tc);
  }

  // 3. Null-terminated ASCII string pattern
  if (cases.size() < count) {
    FuzzTestCase tc;
    tc.a0 = buffer_vram;
    tc.a1 = 32;
    MemoryPayload payload;
    payload.address = buffer_vram;
    payload.data.assign(buffer_size, 0);
    const char sample_string[] = "The quick brown fox jumps over the lazy dog";
    for (size_t i = 0; i < sizeof(sample_string) && i < buffer_size; ++i) {
      payload.data[i] = static_cast<uint8_t>(sample_string[i]);
    }
    tc.initial_memory.push_back(std::move(payload));
    cases.push_back(tc);
  }

  // 4. Two pointers: a0 pointing to buffer, a1 pointing to halfway offset
  if (cases.size() < count && buffer_size >= 64) {
    FuzzTestCase tc;
    tc.a0 = buffer_vram;
    tc.a1 = buffer_vram + static_cast<uint32_t>(buffer_size / 2);
    tc.a2 = static_cast<uint32_t>(buffer_size / 4);
    MemoryPayload payload;
    payload.address = buffer_vram;
    payload.data.resize(buffer_size);
    for (size_t b = 0; b < buffer_size; ++b) {
      payload.data[b] = static_cast<uint8_t>(dist_byte(rng));
    }
    tc.initial_memory.push_back(std::move(payload));
    cases.push_back(tc);
  }

  while (cases.size() < count) {
    FuzzTestCase tc;
    tc.a0 = buffer_vram;
    tc.a1 = dist_len(rng);
    tc.a2 = dist_byte(rng);

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
