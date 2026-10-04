#ifndef FUZZER_MIPS_EMULATOR_H_
#define FUZZER_MIPS_EMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom::fuzzer {

struct MemoryWrite {
  uint32_t address = 0;
  uint32_t value = 0;
  uint8_t size = 0;  // 1 (byte), 2 (half), 4 (word), 8 (doubleword)

  bool operator==(const MemoryWrite& other) const {
    return address == other.address && value == other.value && size == other.size;
  }
};

enum class ExecutionStatus {
  kRunning,
  kHaltedReturn,     // Executed jr $ra (and completed delay slot)
  kMaxStepsReached,  // Step limit reached (infinite loop guard)
  kMemoryFault,      // Attempted to read/write unmapped or unaligned address
  kIntegerOverflow,  // Signed integer overflow trap on add/addi/sub
  kBreakTrap,        // Executed break instruction
  kSyscallTrap,      // Executed syscall instruction
  kInvalidOpcode,    // Encountered unsupported or illegal instruction
};

struct ExecutionResult {
  ExecutionStatus status = ExecutionStatus::kRunning;
  uint64_t total_steps = 0;
  uint32_t v0 = 0;
  uint32_t v1 = 0;
  float f0 = 0.0f;
  uint32_t f0_bits = 0;
  std::vector<MemoryWrite> write_log;
  std::string error_message;
};

// Callee-saved registers under MIPS O32 ABI.
struct CalleeSavedRegisters {
  uint32_t s0 = 0;
  uint32_t s1 = 0;
  uint32_t s2 = 0;
  uint32_t s3 = 0;
  uint32_t s4 = 0;
  uint32_t s5 = 0;
  uint32_t s6 = 0;
  uint32_t s7 = 0;
  uint32_t gp = 0;
  uint32_t sp = 0;
  uint32_t fp = 0;

  bool operator==(const CalleeSavedRegisters& other) const {
    return s0 == other.s0 && s1 == other.s1 && s2 == other.s2 && s3 == other.s3 && s4 == other.s4 &&
           s5 == other.s5 && s6 == other.s6 && s7 == other.s7 && gp == other.gp && sp == other.sp &&
           fp == other.fp;
  }
};

class MipsEmulator {
 public:
  static constexpr uint32_t kDefaultRamBase = 0x80000000;
  static constexpr size_t kDefaultRamSize = 8 * 1024 * 1024;  // 8MB N64 RDRAM
  static constexpr uint32_t kReturnAddressSentinel = 0xFFFFFFFC;

  explicit MipsEmulator(size_t memory_size_bytes = kDefaultRamSize,
                        uint32_t base_vram = kDefaultRamBase);

  // Resets all GPRs, HI/LO, PC, and clears the memory write log.
  void Reset();

  // Loads raw bytes into the sandboxed memory.
  bool LoadMemory(uint32_t vram, const void* data, size_t size);
  bool LoadWords(uint32_t vram, absl::Span<const uint32_t> words);

  // Register access
  void SetRegister(Register reg, uint32_t value);
  uint32_t GetRegister(Register reg) const;
  void SetGpr(int index, uint32_t value);
  uint32_t GetGpr(int index) const;

  void SetPc(uint32_t pc) { pc_ = pc; }
  uint32_t GetPc() const { return pc_; }

  void SetHi(uint32_t hi) { hi_ = hi; }
  uint32_t GetHi() const { return hi_; }

  void SetLo(uint32_t lo) { lo_ = lo; }
  uint32_t GetLo() const { return lo_; }

  // Floating point register access
  void SetFpBits(FpRegister reg, uint32_t bits);
  uint32_t GetFpBits(FpRegister reg) const;
  void SetFpRegister(FpRegister reg, float val);
  float GetFpRegister(FpRegister reg) const;

  void SetFpuCondition(bool cond) { fpu_cond_ = cond; }
  bool GetFpuCondition() const { return fpu_cond_; }

  // Memory access
  bool Read8(uint32_t vram, uint8_t* val) const;
  bool Read16(uint32_t vram, uint16_t* val) const;
  bool Read32(uint32_t vram, uint32_t* val) const;

  bool Write8(uint32_t vram, uint8_t val);
  bool Write16(uint32_t vram, uint16_t val);
  bool Write32(uint32_t vram, uint32_t val);

  // Snapshots callee-saved registers.
  CalleeSavedRegisters GetCalleeSavedRegisters() const;

  // Single step execution.
  ExecutionStatus Step();

  // Runs function starting at start_vram until jr $ra or max_steps.
  ExecutionResult RunFunction(uint32_t start_vram, uint64_t max_steps = 50000);

  // Write log inspection
  const std::vector<MemoryWrite>& GetWriteLog() const { return write_log_; }
  void ClearWriteLog() { write_log_.clear(); }

  // Memory address translation to physical RDRAM index.
  std::optional<size_t> VramToPhysical(uint32_t vram) const;

 private:
  size_t memory_size_;
  uint32_t base_vram_;
  std::vector<uint8_t> memory_;

  uint32_t gpr_[32] = {0};
  uint32_t fpr_bits_[32] = {0};
  bool fpu_cond_ = false;

  uint32_t hi_ = 0;
  uint32_t lo_ = 0;
  uint32_t pc_ = 0;

  bool in_delay_slot_ = false;
  std::optional<uint32_t> delayed_branch_target_;
  bool delay_slot_is_return_ = false;

  std::vector<MemoryWrite> write_log_;
};

}  // namespace rom_nom_nom::fuzzer

#endif  // FUZZER_MIPS_EMULATOR_H_
