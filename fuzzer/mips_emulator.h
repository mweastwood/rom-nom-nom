#ifndef FUZZER_MIPS_EMULATOR_H_
#define FUZZER_MIPS_EMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/endian.h"
#include "core/mips.h"

namespace rom_nom_nom::fuzzer {

struct MemoryWrite {
  uint32_t address = 0;
  uint64_t value = 0;
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

struct ExternalCall {
  uint32_t target_address = 0;
  uint32_t a0 = 0;
  uint32_t a1 = 0;
  uint32_t a2 = 0;
  uint32_t a3 = 0;

  bool operator==(const ExternalCall& other) const {
    return target_address == other.target_address && a0 == other.a0 && a1 == other.a1 &&
           a2 == other.a2 && a3 == other.a3;
  }
};

struct ExecutionResult {
  ExecutionStatus status = ExecutionStatus::kRunning;
  uint64_t total_steps = 0;
  uint32_t v0 = 0;
  uint32_t v1 = 0;
  float f0 = 0.0f;
  uint32_t f0_bits = 0;
  double f0_double = 0.0;
  uint64_t f0_double_bits = 0;
  std::vector<MemoryWrite> write_log;
  std::vector<ExternalCall> call_log;
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

  // Clears (zeros out) a range of memory in the sandboxed RAM.
  void ClearMemory(uint32_t vram, size_t size);

  // Reverts all writes recorded in write_log_.
  // If an address falls within [code_vram, code_vram + code.size() * 4), it is restored
  // to the original instruction word from code; otherwise, it is zeroed out.
  // Clears write_log_ upon completion.
  void RollbackWrites(absl::Span<const uint32_t> code = {}, uint32_t code_vram = 0);

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

  // Floating point register access (single-precision 32-bit)
  void SetFpBits(FpRegister reg, uint32_t bits);
  uint32_t GetFpBits(FpRegister reg) const;
  void SetFpRegister(FpRegister reg, float val);
  float GetFpRegister(FpRegister reg) const;

  // Floating point register access (double-precision 64-bit with even/odd pairing, Status.FR = 0)
  void SetFpDoubleBits(FpRegister reg, uint64_t bits);
  uint64_t GetFpDoubleBits(FpRegister reg) const;
  void SetFpDouble(FpRegister reg, double val);
  double GetFpDouble(FpRegister reg) const;

  void SetFpuCondition(bool cond) { fpu_cond_ = cond; }
  bool GetFpuCondition() const { return fpu_cond_; }

  uint32_t GetFcr31() const { return (fcr31_ & ~(1u << 23)) | (fpu_cond_ ? (1u << 23) : 0); }
  void SetFcr31(uint32_t val) {
    fcr31_ = val;
    fpu_cond_ = (val & (1u << 23)) != 0;
  }

  // Memory access
  bool Read8(uint32_t vram, uint8_t* val) const;
  bool Read16(uint32_t vram, uint16_t* val) const;
  bool Read32(uint32_t vram, uint32_t* val) const;
  bool Read64(uint32_t vram, uint64_t* val) const;

  bool Write8(uint32_t vram, uint8_t val);
  bool Write16(uint32_t vram, uint16_t val);
  bool Write32(uint32_t vram, uint32_t val);
  bool Write64(uint32_t vram, uint64_t val);

  // Snapshots callee-saved registers.
  CalleeSavedRegisters GetCalleeSavedRegisters() const;

  // Single step execution.
  ExecutionStatus Step();

  // Runs function starting at start_vram until jr $ra or max_steps.
  ExecutionResult RunFunction(uint32_t start_vram, uint64_t max_steps = 50000);

  // Write log inspection
  const std::vector<MemoryWrite>& GetWriteLog() const { return write_log_; }
  void ClearWriteLog() { write_log_.clear(); }

  // External call log inspection
  const std::vector<ExternalCall>& GetCallLog() const { return call_log_; }
  void ClearCallLog() { call_log_.clear(); }

  // Code execution bounds for call interception.
  void SetCodeBounds(uint32_t start_vram, uint32_t end_vram) {
    code_bounds_ = std::make_pair(start_vram, end_vram);
  }
  void ClearCodeBounds() { code_bounds_.reset(); }
  bool HasCodeBounds() const { return code_bounds_.has_value(); }
  std::optional<std::pair<uint32_t, uint32_t>> GetCodeBounds() const { return code_bounds_; }

  void SetInterceptExternalCalls(bool enable) { intercept_external_calls_ = enable; }
  bool GetInterceptExternalCalls() const { return intercept_external_calls_; }

  // MMIO hardware register mocking.
  static bool IsMmioAddress(uint32_t vram);
  void SetMockMmio(bool enable) { mock_mmio_ = enable; }
  bool GetMockMmio() const { return mock_mmio_; }

  // Memory address translation to physical RDRAM index.
  std::optional<size_t> VramToPhysical(uint32_t vram) const;

 private:
  uint32_t ReadMmio32(uint32_t phys_addr) const;
  void WriteMmio32(uint32_t phys_addr, uint32_t val);

  size_t memory_size_;
  uint32_t base_vram_;
  std::vector<uint8_t> memory_;

  uint32_t gpr_[32] = {0};
  uint32_t fpr_bits_[32] = {0};
  uint32_t fcr31_ = 0;
  bool fpu_cond_ = false;

  uint32_t hi_ = 0;
  uint32_t lo_ = 0;
  uint32_t pc_ = 0;

  bool in_delay_slot_ = false;
  std::optional<uint32_t> delayed_branch_target_;
  bool delay_slot_is_return_ = false;
  bool delay_slot_is_call_ = false;
  int call_link_reg_ = 31;

  bool intercept_external_calls_ = false;
  std::optional<std::pair<uint32_t, uint32_t>> code_bounds_;

  bool mock_mmio_ = true;
  mutable absl::flat_hash_map<uint32_t, uint32_t> mmio_regs_;
  mutable uint32_t vi_scanline_counter_ = 0;

  std::vector<MemoryWrite> write_log_;
  std::vector<ExternalCall> call_log_;
};

inline std::optional<size_t> MipsEmulator::VramToPhysical(uint32_t vram) const {
  if ((vram >> 30) == 2) {
    uint32_t phys = vram & 0x1FFFFFFF;
    if (phys < memory_size_) {
      return static_cast<size_t>(phys);
    }
  }
  return std::nullopt;
}

inline bool MipsEmulator::IsMmioAddress(uint32_t vram) {
  uint32_t phys = vram & 0x1FFFFFFF;
  return (phys >= 0x04000000 && phys < 0x04900000) || (phys >= 0x1FC00000 && phys < 0x1FC00800);
}

inline void MipsEmulator::SetGpr(int index, uint32_t value) {
  if (index > 0 && index < 32) {
    gpr_[index] = value;
  }
}

inline uint32_t MipsEmulator::GetGpr(int index) const {
  if (index >= 0 && index < 32) {
    return gpr_[index];
  }
  return 0;
}

inline bool MipsEmulator::Read8(uint32_t vram, uint8_t* val) const {
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    uint32_t word = ReadMmio32(phys & ~3);
    size_t byte_in_word = 3 - (phys % 4);
    *val = static_cast<uint8_t>((word >> (byte_in_word * 8)) & 0xFF);
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value()) return false;
  *val = memory_[*phys];
  return true;
}

inline bool MipsEmulator::Read16(uint32_t vram, uint16_t* val) const {
  if ((vram & 1) != 0) return false;
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    uint32_t word = ReadMmio32(phys & ~3);
    size_t half_in_word = (phys % 4 == 0) ? 1 : 0;
    *val = static_cast<uint16_t>((word >> (half_in_word * 16)) & 0xFFFF);
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 1 >= memory_size_) return false;
  *val = ReadBigEndian16(&memory_[*phys]);
  return true;
}

inline bool MipsEmulator::Read32(uint32_t vram, uint32_t* val) const {
  if ((vram & 3) != 0) return false;
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    *val = ReadMmio32(phys);
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 3 >= memory_size_) return false;
  *val = ReadBigEndian32(&memory_[*phys]);
  return true;
}

inline bool MipsEmulator::Read64(uint32_t vram, uint64_t* val) const {
  if ((vram & 7) != 0) return false;
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    uint32_t hi = ReadMmio32(phys);
    uint32_t lo = ReadMmio32(phys + 4);
    *val = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 7 >= memory_size_) return false;
  *val = ReadBigEndian64(&memory_[*phys]);
  return true;
}

inline bool MipsEmulator::Write8(uint32_t vram, uint8_t val) {
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    uint32_t word = ReadMmio32(phys & ~3);
    size_t byte_in_word = 3 - (phys % 4);
    uint32_t mask = 0xFFu << (byte_in_word * 8);
    word = (word & ~mask) | (static_cast<uint32_t>(val) << (byte_in_word * 8));
    WriteMmio32(phys & ~3, word);
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value()) return false;
  memory_[*phys] = val;
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 1});
  return true;
}

inline bool MipsEmulator::Write16(uint32_t vram, uint16_t val) {
  if ((vram & 1) != 0) return false;
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    uint32_t word = ReadMmio32(phys & ~3);
    size_t half_in_word = (phys % 4 == 0) ? 1 : 0;
    uint32_t mask = 0xFFFFu << (half_in_word * 16);
    word = (word & ~mask) | (static_cast<uint32_t>(val) << (half_in_word * 16));
    WriteMmio32(phys & ~3, word);
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 1 >= memory_size_) return false;
  WriteBigEndian16(&memory_[*phys], val);
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 2});
  return true;
}

inline bool MipsEmulator::Write32(uint32_t vram, uint32_t val) {
  if ((vram & 3) != 0) return false;
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    WriteMmio32(phys, val);
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 3 >= memory_size_) return false;
  WriteBigEndian32(&memory_[*phys], val);
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 4});
  return true;
}

inline bool MipsEmulator::Write64(uint32_t vram, uint64_t val) {
  if ((vram & 7) != 0) return false;
  if (mock_mmio_ && IsMmioAddress(vram)) {
    uint32_t phys = vram & 0x1FFFFFFF;
    WriteMmio32(phys, static_cast<uint32_t>(val >> 32));
    WriteMmio32(phys + 4, static_cast<uint32_t>(val & 0xFFFFFFFF));
    return true;
  }
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 7 >= memory_size_) return false;
  WriteBigEndian64(&memory_[*phys], val);
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 8});
  return true;
}

}  // namespace rom_nom_nom::fuzzer

#endif  // FUZZER_MIPS_EMULATOR_H_
