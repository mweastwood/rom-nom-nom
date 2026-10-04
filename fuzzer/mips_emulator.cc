#include "fuzzer/mips_emulator.h"

#include <cmath>
#include <cstring>
#include <optional>

#include "absl/strings/str_format.h"

namespace rom_nom_nom::fuzzer {

namespace {
bool IsEvenFpRegister(FpRegister reg) {
  return (static_cast<int>(reg) & 1) == 0;
}
}  // namespace

MipsEmulator::MipsEmulator(size_t memory_size_bytes, uint32_t base_vram)
    : memory_size_(memory_size_bytes), base_vram_(base_vram), memory_(memory_size_bytes, 0) {
  Reset();
}

void MipsEmulator::Reset() {
  std::memset(gpr_, 0, sizeof(gpr_));
  std::memset(fpr_bits_, 0, sizeof(fpr_bits_));
  fpu_cond_ = false;
  hi_ = 0;
  lo_ = 0;
  pc_ = base_vram_;
  in_delay_slot_ = false;
  delayed_branch_target_.reset();
  delay_slot_is_return_ = false;
  write_log_.clear();
}

void MipsEmulator::SetFpBits(FpRegister reg, uint32_t bits) {
  int idx = static_cast<int>(reg);
  if (idx >= 0 && idx < 32) {
    fpr_bits_[idx] = bits;
  }
}

uint32_t MipsEmulator::GetFpBits(FpRegister reg) const {
  int idx = static_cast<int>(reg);
  if (idx >= 0 && idx < 32) {
    return fpr_bits_[idx];
  }
  return 0;
}

void MipsEmulator::SetFpRegister(FpRegister reg, float val) {
  uint32_t bits = 0;
  std::memcpy(&bits, &val, sizeof(float));
  SetFpBits(reg, bits);
}

float MipsEmulator::GetFpRegister(FpRegister reg) const {
  uint32_t bits = GetFpBits(reg);
  float val = 0.0f;
  std::memcpy(&val, &bits, sizeof(float));
  return val;
}

void MipsEmulator::SetFpDoubleBits(FpRegister reg, uint64_t bits) {
  int idx = static_cast<int>(reg);
  if ((idx & 1) != 0 || idx < 0 || idx >= 32) return;
  fpr_bits_[idx] = static_cast<uint32_t>(bits >> 32);             // MSW
  fpr_bits_[idx + 1] = static_cast<uint32_t>(bits & 0xFFFFFFFF);  // LSW
}

uint64_t MipsEmulator::GetFpDoubleBits(FpRegister reg) const {
  int idx = static_cast<int>(reg);
  if ((idx & 1) != 0 || idx < 0 || idx >= 32) return 0;
  return (static_cast<uint64_t>(fpr_bits_[idx]) << 32) | static_cast<uint64_t>(fpr_bits_[idx + 1]);
}

void MipsEmulator::SetFpDouble(FpRegister reg, double val) {
  uint64_t bits = 0;
  std::memcpy(&bits, &val, sizeof(double));
  SetFpDoubleBits(reg, bits);
}

double MipsEmulator::GetFpDouble(FpRegister reg) const {
  uint64_t bits = GetFpDoubleBits(reg);
  double val = 0.0;
  std::memcpy(&val, &bits, sizeof(double));
  return val;
}

std::optional<size_t> MipsEmulator::VramToPhysical(uint32_t vram) const {
  uint32_t phys = 0;
  if (vram >= 0x80000000 && vram < 0xA0000000) {
    phys = vram - 0x80000000;
  } else if (vram >= 0xA0000000 && vram < 0xC0000000) {
    phys = vram - 0xA0000000;
  } else {
    return std::nullopt;
  }

  if (phys < memory_size_) {
    return static_cast<size_t>(phys);
  }
  return std::nullopt;
}

bool MipsEmulator::LoadMemory(uint32_t vram, const void* data, size_t size) {
  auto phys_opt = VramToPhysical(vram);
  if (!phys_opt.has_value() || *phys_opt + size > memory_size_) {
    return false;
  }
  std::memcpy(&memory_[*phys_opt], data, size);
  return true;
}

bool MipsEmulator::LoadWords(uint32_t vram, absl::Span<const uint32_t> words) {
  auto phys_opt = VramToPhysical(vram);
  size_t byte_size = words.size() * sizeof(uint32_t);
  if (!phys_opt.has_value() || *phys_opt + byte_size > memory_size_) {
    return false;
  }
  for (size_t i = 0; i < words.size(); ++i) {
    uint32_t word = words[i];
    size_t offset = *phys_opt + (i * 4);
    memory_[offset] = static_cast<uint8_t>(word >> 24);
    memory_[offset + 1] = static_cast<uint8_t>(word >> 16);
    memory_[offset + 2] = static_cast<uint8_t>(word >> 8);
    memory_[offset + 3] = static_cast<uint8_t>(word);
  }
  return true;
}

void MipsEmulator::ClearMemory(uint32_t vram, size_t size) {
  auto phys_opt = VramToPhysical(vram);
  if (!phys_opt.has_value() || *phys_opt + size > memory_size_) {
    return;
  }
  std::memset(&memory_[*phys_opt], 0, size);
}

void MipsEmulator::RollbackWrites(absl::Span<const uint32_t> code, uint32_t code_vram) {
  uint32_t code_end = code_vram + static_cast<uint32_t>(code.size() * sizeof(uint32_t));
  for (const auto& w : write_log_) {
    auto phys_opt = VramToPhysical(w.address);
    if (!phys_opt.has_value() || *phys_opt + w.size > memory_size_) {
      continue;
    }
    if (!code.empty() && w.address >= code_vram && w.address < code_end) {
      for (size_t b = 0; b < w.size; ++b) {
        uint32_t addr = w.address + static_cast<uint32_t>(b);
        if (addr >= code_vram && addr < code_end) {
          size_t word_idx = (addr - code_vram) / 4;
          size_t byte_in_word = 3 - (addr % 4);
          uint8_t orig_byte = static_cast<uint8_t>((code[word_idx] >> (byte_in_word * 8)) & 0xFF);
          memory_[*phys_opt + b] = orig_byte;
        } else {
          memory_[*phys_opt + b] = 0;
        }
      }
    } else {
      std::memset(&memory_[*phys_opt], 0, w.size);
    }
  }
  write_log_.clear();
}

void MipsEmulator::SetRegister(Register reg, uint32_t value) {
  SetGpr(static_cast<int>(reg), value);
}

uint32_t MipsEmulator::GetRegister(Register reg) const {
  return GetGpr(static_cast<int>(reg));
}

void MipsEmulator::SetGpr(int index, uint32_t value) {
  if (index > 0 && index < 32) {
    gpr_[index] = value;
  }
}

uint32_t MipsEmulator::GetGpr(int index) const {
  if (index >= 0 && index < 32) {
    return gpr_[index];
  }
  return 0;
}

bool MipsEmulator::Read8(uint32_t vram, uint8_t* val) const {
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys >= memory_size_) return false;
  *val = memory_[*phys];
  return true;
}

bool MipsEmulator::Read16(uint32_t vram, uint16_t* val) const {
  if ((vram & 1) != 0) return false;
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 1 >= memory_size_) return false;
  *val = static_cast<uint16_t>((static_cast<uint16_t>(memory_[*phys]) << 8) |
                               static_cast<uint16_t>(memory_[*phys + 1]));
  return true;
}

bool MipsEmulator::Read32(uint32_t vram, uint32_t* val) const {
  if ((vram & 3) != 0) return false;
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 3 >= memory_size_) return false;
  *val = (static_cast<uint32_t>(memory_[*phys]) << 24) |
         (static_cast<uint32_t>(memory_[*phys + 1]) << 16) |
         (static_cast<uint32_t>(memory_[*phys + 2]) << 8) |
         static_cast<uint32_t>(memory_[*phys + 3]);
  return true;
}

bool MipsEmulator::Read64(uint32_t vram, uint64_t* val) const {
  if ((vram & 7) != 0) return false;
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 7 >= memory_size_) return false;
  uint32_t hi = (static_cast<uint32_t>(memory_[*phys]) << 24) |
                (static_cast<uint32_t>(memory_[*phys + 1]) << 16) |
                (static_cast<uint32_t>(memory_[*phys + 2]) << 8) |
                static_cast<uint32_t>(memory_[*phys + 3]);
  uint32_t lo = (static_cast<uint32_t>(memory_[*phys + 4]) << 24) |
                (static_cast<uint32_t>(memory_[*phys + 5]) << 16) |
                (static_cast<uint32_t>(memory_[*phys + 6]) << 8) |
                static_cast<uint32_t>(memory_[*phys + 7]);
  *val = (static_cast<uint64_t>(hi) << 32) | lo;
  return true;
}

bool MipsEmulator::Write8(uint32_t vram, uint8_t val) {
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys >= memory_size_) return false;
  memory_[*phys] = val;
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 1});
  return true;
}

bool MipsEmulator::Write16(uint32_t vram, uint16_t val) {
  if ((vram & 1) != 0) return false;
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 1 >= memory_size_) return false;
  memory_[*phys] = static_cast<uint8_t>(val >> 8);
  memory_[*phys + 1] = static_cast<uint8_t>(val);
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 2});
  return true;
}

bool MipsEmulator::Write32(uint32_t vram, uint32_t val) {
  if ((vram & 3) != 0) return false;
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 3 >= memory_size_) return false;
  memory_[*phys] = static_cast<uint8_t>(val >> 24);
  memory_[*phys + 1] = static_cast<uint8_t>(val >> 16);
  memory_[*phys + 2] = static_cast<uint8_t>(val >> 8);
  memory_[*phys + 3] = static_cast<uint8_t>(val);
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 4});
  return true;
}

bool MipsEmulator::Write64(uint32_t vram, uint64_t val) {
  if ((vram & 7) != 0) return false;
  auto phys = VramToPhysical(vram);
  if (!phys.has_value() || *phys + 7 >= memory_size_) return false;
  uint32_t hi = static_cast<uint32_t>(val >> 32);
  uint32_t lo = static_cast<uint32_t>(val & 0xFFFFFFFF);
  memory_[*phys] = static_cast<uint8_t>(hi >> 24);
  memory_[*phys + 1] = static_cast<uint8_t>(hi >> 16);
  memory_[*phys + 2] = static_cast<uint8_t>(hi >> 8);
  memory_[*phys + 3] = static_cast<uint8_t>(hi);
  memory_[*phys + 4] = static_cast<uint8_t>(lo >> 24);
  memory_[*phys + 5] = static_cast<uint8_t>(lo >> 16);
  memory_[*phys + 6] = static_cast<uint8_t>(lo >> 8);
  memory_[*phys + 7] = static_cast<uint8_t>(lo);
  write_log_.push_back(MemoryWrite{.address = vram, .value = val, .size = 8});
  return true;
}

CalleeSavedRegisters MipsEmulator::GetCalleeSavedRegisters() const {
  return CalleeSavedRegisters{
      .s0 = gpr_[16],
      .s1 = gpr_[17],
      .s2 = gpr_[18],
      .s3 = gpr_[19],
      .s4 = gpr_[20],
      .s5 = gpr_[21],
      .s6 = gpr_[22],
      .s7 = gpr_[23],
      .gp = gpr_[28],
      .sp = gpr_[29],
      .fp = gpr_[30],
  };
}

ExecutionStatus MipsEmulator::Step() {
  uint32_t current_pc = pc_;
  if ((current_pc & 3) != 0) {
    return ExecutionStatus::kMemoryFault;
  }
  uint32_t raw_word = 0;
  if (!Read32(current_pc, &raw_word)) {
    return ExecutionStatus::kMemoryFault;
  }

  auto inst_or = DecodeInstruction(raw_word, current_pc);
  if (!inst_or.ok()) {
    return ExecutionStatus::kInvalidOpcode;
  }
  const Instruction& inst = *inst_or;

  bool executing_delay_slot = in_delay_slot_;
  bool delayed_is_return = delay_slot_is_return_;
  std::optional<uint32_t> next_branch_target = delayed_branch_target_;

  if (executing_delay_slot && (inst.IsBranch() || inst.IsJump())) {
    return ExecutionStatus::kInvalidOpcode;
  }

  // Advance default next PC
  uint32_t advanced_pc = current_pc + 4;

  int rs = inst.rs.has_value() ? static_cast<int>(*inst.rs) : 0;
  int rt = inst.rt.has_value() ? static_cast<int>(*inst.rt) : 0;
  int rd = inst.rd.has_value() ? static_cast<int>(*inst.rd) : 0;
  uint32_t imm_u = static_cast<uint32_t>(inst.immediate) & 0xFFFF;
  int32_t imm_s = static_cast<int32_t>(inst.immediate);

  switch (inst.opcode) {
    case Opcode::kAdd: {
      int32_t a = static_cast<int32_t>(gpr_[rs]);
      int32_t b = static_cast<int32_t>(gpr_[rt]);
      int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
      if (((a ^ res) & (b ^ res)) < 0) {
        return ExecutionStatus::kIntegerOverflow;
      }
      SetGpr(rd, static_cast<uint32_t>(res));
      break;
    }
    case Opcode::kAddu:
      SetGpr(rd, gpr_[rs] + gpr_[rt]);
      break;
    case Opcode::kSub: {
      int32_t a = static_cast<int32_t>(gpr_[rs]);
      int32_t b = static_cast<int32_t>(gpr_[rt]);
      int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
      if (((a ^ b) & (a ^ res)) < 0) {
        return ExecutionStatus::kIntegerOverflow;
      }
      SetGpr(rd, static_cast<uint32_t>(res));
      break;
    }
    case Opcode::kSubu:
      SetGpr(rd, gpr_[rs] - gpr_[rt]);
      break;
    case Opcode::kAddi: {
      int32_t a = static_cast<int32_t>(gpr_[rs]);
      int32_t b = imm_s;
      int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
      if (((a ^ res) & (b ^ res)) < 0) {
        return ExecutionStatus::kIntegerOverflow;
      }
      SetGpr(rt, static_cast<uint32_t>(res));
      break;
    }
    case Opcode::kAddiu:
      SetGpr(rt, gpr_[rs] + static_cast<uint32_t>(imm_s));
      break;
    case Opcode::kAnd:
      SetGpr(rd, gpr_[rs] & gpr_[rt]);
      break;
    case Opcode::kAndi:
      SetGpr(rt, gpr_[rs] & imm_u);
      break;
    case Opcode::kOr:
      SetGpr(rd, gpr_[rs] | gpr_[rt]);
      break;
    case Opcode::kOri:
      SetGpr(rt, gpr_[rs] | imm_u);
      break;
    case Opcode::kXor:
      SetGpr(rd, gpr_[rs] ^ gpr_[rt]);
      break;
    case Opcode::kXori:
      SetGpr(rt, gpr_[rs] ^ imm_u);
      break;
    case Opcode::kNor:
      SetGpr(rd, ~(gpr_[rs] | gpr_[rt]));
      break;
    case Opcode::kLui:
      SetGpr(rt, imm_u << 16);
      break;
    case Opcode::kSll:
      SetGpr(rd, gpr_[rt] << inst.shift_amount);
      break;
    case Opcode::kSrl:
      SetGpr(rd, gpr_[rt] >> inst.shift_amount);
      break;
    case Opcode::kSra:
      SetGpr(rd, static_cast<uint32_t>(static_cast<int32_t>(gpr_[rt]) >> inst.shift_amount));
      break;
    case Opcode::kSllv:
      SetGpr(rd, gpr_[rt] << (gpr_[rs] & 0x1F));
      break;
    case Opcode::kSrlv:
      SetGpr(rd, gpr_[rt] >> (gpr_[rs] & 0x1F));
      break;
    case Opcode::kSrav:
      SetGpr(rd, static_cast<uint32_t>(static_cast<int32_t>(gpr_[rt]) >> (gpr_[rs] & 0x1F)));
      break;
    case Opcode::kSlt:
      SetGpr(rd, (static_cast<int32_t>(gpr_[rs]) < static_cast<int32_t>(gpr_[rt])) ? 1 : 0);
      break;
    case Opcode::kSltu:
      SetGpr(rd, (gpr_[rs] < gpr_[rt]) ? 1 : 0);
      break;
    case Opcode::kSlti:
      SetGpr(rt, (static_cast<int32_t>(gpr_[rs]) < imm_s) ? 1 : 0);
      break;
    case Opcode::kSltiu:
      SetGpr(rt, (gpr_[rs] < static_cast<uint32_t>(imm_s)) ? 1 : 0);
      break;
    case Opcode::kMult: {
      int64_t prod = static_cast<int64_t>(static_cast<int32_t>(gpr_[rs])) *
                     static_cast<int64_t>(static_cast<int32_t>(gpr_[rt]));
      hi_ = static_cast<uint32_t>(prod >> 32);
      lo_ = static_cast<uint32_t>(prod & 0xFFFFFFFF);
      break;
    }
    case Opcode::kMultu: {
      uint64_t prod = static_cast<uint64_t>(gpr_[rs]) * static_cast<uint64_t>(gpr_[rt]);
      hi_ = static_cast<uint32_t>(prod >> 32);
      lo_ = static_cast<uint32_t>(prod & 0xFFFFFFFF);
      break;
    }
    case Opcode::kDiv:
      if (gpr_[rt] != 0) {
        int32_t num = static_cast<int32_t>(gpr_[rs]);
        int32_t den = static_cast<int32_t>(gpr_[rt]);
        if (num == static_cast<int32_t>(0x80000000u) && den == -1) {
          lo_ = 0x80000000u;
          hi_ = 0;
        } else {
          lo_ = static_cast<uint32_t>(num / den);
          hi_ = static_cast<uint32_t>(num % den);
        }
      }
      break;
    case Opcode::kDivu:
      if (gpr_[rt] != 0) {
        lo_ = gpr_[rs] / gpr_[rt];
        hi_ = gpr_[rs] % gpr_[rt];
      }
      break;
    case Opcode::kMfhi:
      SetGpr(rd, hi_);
      break;
    case Opcode::kMflo:
      SetGpr(rd, lo_);
      break;
    case Opcode::kMthi:
      hi_ = gpr_[rs];
      break;
    case Opcode::kMtlo:
      lo_ = gpr_[rs];
      break;
    case Opcode::kLw: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      uint32_t val = 0;
      if (!Read32(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, val);
      break;
    }
    case Opcode::kLh: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      uint16_t val = 0;
      if (!Read16(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(val))));
      break;
    }
    case Opcode::kLhu: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      uint16_t val = 0;
      if (!Read16(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, val);
      break;
    }
    case Opcode::kLb: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint8_t val = 0;
      if (!Read8(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(val))));
      break;
    }
    case Opcode::kLbu: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint8_t val = 0;
      if (!Read8(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, val);
      break;
    }
    case Opcode::kSw: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write32(addr, gpr_[rt])) return ExecutionStatus::kMemoryFault;
      break;
    }
    case Opcode::kSh: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write16(addr, static_cast<uint16_t>(gpr_[rt]))) return ExecutionStatus::kMemoryFault;
      break;
    }
    case Opcode::kSb: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if (!Write8(addr, static_cast<uint8_t>(gpr_[rt]))) return ExecutionStatus::kMemoryFault;
      break;
    }
    case Opcode::kLwl: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu << (shift_byte * 8);
      SetGpr(rt, (gpr_[rt] & ~mask) | (word << (shift_byte * 8)));
      break;
    }
    case Opcode::kLwr: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu >> ((3 - shift_byte) * 8);
      SetGpr(rt, (gpr_[rt] & ~mask) | (word >> ((3 - shift_byte) * 8)));
      break;
    }
    case Opcode::kSwl: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu >> (shift_byte * 8);
      uint32_t new_word = (word & ~mask) | (gpr_[rt] >> (shift_byte * 8));
      if (!Write32(word_addr, new_word)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case Opcode::kSwr: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu << ((3 - shift_byte) * 8);
      uint32_t new_word = (word & ~mask) | (gpr_[rt] << ((3 - shift_byte) * 8));
      if (!Write32(word_addr, new_word)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case Opcode::kBeq:
    case Opcode::kBne:
    case Opcode::kBlez:
    case Opcode::kBgtz:
    case Opcode::kBltz:
    case Opcode::kBgez: {
      bool take_branch = false;
      if (inst.opcode == Opcode::kBeq) take_branch = (gpr_[rs] == gpr_[rt]);
      if (inst.opcode == Opcode::kBne) take_branch = (gpr_[rs] != gpr_[rt]);
      if (inst.opcode == Opcode::kBlez) take_branch = (static_cast<int32_t>(gpr_[rs]) <= 0);
      if (inst.opcode == Opcode::kBgtz) take_branch = (static_cast<int32_t>(gpr_[rs]) > 0);
      if (inst.opcode == Opcode::kBltz) take_branch = (static_cast<int32_t>(gpr_[rs]) < 0);
      if (inst.opcode == Opcode::kBgez) take_branch = (static_cast<int32_t>(gpr_[rs]) >= 0);

      in_delay_slot_ = true;
      if (take_branch) {
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      } else {
        delayed_branch_target_ = current_pc + 8;
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    }
    case Opcode::kBltzal:
    case Opcode::kBgezal: {
      if (rs == 31) {
        return ExecutionStatus::kInvalidOpcode;  // MIPS III undefined restriction
      }
      bool take_branch = (inst.opcode == Opcode::kBltzal) ? (static_cast<int32_t>(gpr_[rs]) < 0)
                                                          : (static_cast<int32_t>(gpr_[rs]) >= 0);
      SetGpr(31, current_pc + 8);
      in_delay_slot_ = true;
      if (take_branch) {
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      } else {
        delayed_branch_target_ = current_pc + 8;
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    }
    case Opcode::kBeql:
    case Opcode::kBnel:
    case Opcode::kBlezl:
    case Opcode::kBgtzl:
    case Opcode::kBltzl:
    case Opcode::kBgezl: {
      bool take_branch = false;
      if (inst.opcode == Opcode::kBeql) take_branch = (gpr_[rs] == gpr_[rt]);
      if (inst.opcode == Opcode::kBnel) take_branch = (gpr_[rs] != gpr_[rt]);
      if (inst.opcode == Opcode::kBlezl) take_branch = (static_cast<int32_t>(gpr_[rs]) <= 0);
      if (inst.opcode == Opcode::kBgtzl) take_branch = (static_cast<int32_t>(gpr_[rs]) > 0);
      if (inst.opcode == Opcode::kBltzl) take_branch = (static_cast<int32_t>(gpr_[rs]) < 0);
      if (inst.opcode == Opcode::kBgezl) take_branch = (static_cast<int32_t>(gpr_[rs]) >= 0);

      if (take_branch) {
        in_delay_slot_ = true;
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
        pc_ = advanced_pc;
      } else {
        // Delay slot is annulled (skipped) on branch likely when branch not taken
        pc_ = current_pc + 8;
      }
      return ExecutionStatus::kRunning;
    }
    case Opcode::kJ: {
      uint32_t target = (current_pc & 0xF0000000) | (inst.target << 2);
      in_delay_slot_ = true;
      delayed_branch_target_ = target;
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    }
    case Opcode::kJal: {
      uint32_t target = (current_pc & 0xF0000000) | (inst.target << 2);
      SetGpr(31, current_pc + 8);
      in_delay_slot_ = true;
      delayed_branch_target_ = target;
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    }
    case Opcode::kJalr: {
      int link_reg = (rd != 0) ? rd : 31;
      if (rs == link_reg) {
        return ExecutionStatus::kInvalidOpcode;  // MIPS III undefined restriction
      }
      SetGpr(link_reg, current_pc + 8);
      in_delay_slot_ = true;
      delayed_branch_target_ = gpr_[rs];
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    }
    case Opcode::kJr: {
      in_delay_slot_ = true;
      if (gpr_[rs] == kReturnAddressSentinel) {
        delay_slot_is_return_ = true;
      } else {
        delayed_branch_target_ = gpr_[rs];
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    }
    case Opcode::kSync:
      break;
    case Opcode::kBreak:
      return ExecutionStatus::kBreakTrap;
    case Opcode::kSyscall:
      return ExecutionStatus::kSyscallTrap;

    // Floating-Point (COP1) Loads & Stores
    case Opcode::kLwc1: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      uint32_t word = 0;
      if (!Read32(addr, &word)) return ExecutionStatus::kMemoryFault;
      if (inst.ft.has_value()) {
        SetFpBits(*inst.ft, word);
      }
      break;
    }
    case Opcode::kSwc1: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      uint32_t word = inst.ft.has_value() ? GetFpBits(*inst.ft) : 0;
      if (!Write32(addr, word)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case Opcode::kLdc1: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      uint64_t val = 0;
      if (!Read64(addr, &val)) return ExecutionStatus::kMemoryFault;
      if (inst.ft.has_value()) {
        if (!IsEvenFpRegister(*inst.ft)) return ExecutionStatus::kInvalidOpcode;
        SetFpDoubleBits(*inst.ft, val);
      }
      break;
    }
    case Opcode::kSdc1: {
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      if (inst.ft.has_value()) {
        if (!IsEvenFpRegister(*inst.ft)) return ExecutionStatus::kInvalidOpcode;
        uint64_t val = GetFpDoubleBits(*inst.ft);
        if (!Write64(addr, val)) return ExecutionStatus::kMemoryFault;
      }
      break;
    }

    // Floating-Point (COP1) Moves
    case Opcode::kMfc1: {
      uint32_t word = inst.fs.has_value() ? GetFpBits(*inst.fs) : 0;
      SetGpr(rt, word);
      break;
    }
    case Opcode::kMtc1: {
      if (inst.fs.has_value()) {
        SetFpBits(*inst.fs, gpr_[rt]);
      }
      break;
    }

    // Single-Precision Floating-Point Arithmetic
    case Opcode::kAddS: {
      if (inst.fd && inst.fs && inst.ft) {
        SetFpRegister(*inst.fd, GetFpRegister(*inst.fs) + GetFpRegister(*inst.ft));
      }
      break;
    }
    case Opcode::kSubS: {
      if (inst.fd && inst.fs && inst.ft) {
        SetFpRegister(*inst.fd, GetFpRegister(*inst.fs) - GetFpRegister(*inst.ft));
      }
      break;
    }
    case Opcode::kMulS: {
      if (inst.fd && inst.fs && inst.ft) {
        SetFpRegister(*inst.fd, GetFpRegister(*inst.fs) * GetFpRegister(*inst.ft));
      }
      break;
    }
    case Opcode::kDivS: {
      if (inst.fd && inst.fs && inst.ft) {
        SetFpRegister(*inst.fd, GetFpRegister(*inst.fs) / GetFpRegister(*inst.ft));
      }
      break;
    }
    case Opcode::kSqrtS: {
      if (inst.fd && inst.fs) {
        SetFpRegister(*inst.fd, std::sqrt(GetFpRegister(*inst.fs)));
      }
      break;
    }
    case Opcode::kAbsS: {
      if (inst.fd && inst.fs) {
        SetFpRegister(*inst.fd, std::fabs(GetFpRegister(*inst.fs)));
      }
      break;
    }
    case Opcode::kMovS: {
      if (inst.fd && inst.fs) {
        SetFpBits(*inst.fd, GetFpBits(*inst.fs));
      }
      break;
    }
    case Opcode::kNegS: {
      if (inst.fd && inst.fs) {
        SetFpBits(*inst.fd, GetFpBits(*inst.fs) ^ 0x80000000u);
      }
      break;
    }
    case Opcode::kCvtSW: {
      if (inst.fd && inst.fs) {
        int32_t val = static_cast<int32_t>(GetFpBits(*inst.fs));
        SetFpRegister(*inst.fd, static_cast<float>(val));
      }
      break;
    }
    case Opcode::kTruncWS: {
      if (inst.fd && inst.fs) {
        float f = GetFpRegister(*inst.fs);
        int32_t val = 0;
        if (std::isnan(f)) {
          val = 0x7FFFFFFF;
        } else if (f >= 2147483647.0f) {
          val = 0x7FFFFFFF;
        } else if (f <= -2147483648.0f) {
          val = static_cast<int32_t>(0x80000000u);
        } else {
          val = static_cast<int32_t>(std::trunc(f));
        }
        SetFpBits(*inst.fd, static_cast<uint32_t>(val));
      }
      break;
    }

    // Double-Precision Floating-Point Arithmetic
    case Opcode::kAddD: {
      if (inst.fd && inst.fs && inst.ft) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs) ||
            !IsEvenFpRegister(*inst.ft)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDouble(*inst.fd, GetFpDouble(*inst.fs) + GetFpDouble(*inst.ft));
      }
      break;
    }
    case Opcode::kSubD: {
      if (inst.fd && inst.fs && inst.ft) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs) ||
            !IsEvenFpRegister(*inst.ft)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDouble(*inst.fd, GetFpDouble(*inst.fs) - GetFpDouble(*inst.ft));
      }
      break;
    }
    case Opcode::kMulD: {
      if (inst.fd && inst.fs && inst.ft) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs) ||
            !IsEvenFpRegister(*inst.ft)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDouble(*inst.fd, GetFpDouble(*inst.fs) * GetFpDouble(*inst.ft));
      }
      break;
    }
    case Opcode::kDivD: {
      if (inst.fd && inst.fs && inst.ft) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs) ||
            !IsEvenFpRegister(*inst.ft)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDouble(*inst.fd, GetFpDouble(*inst.fs) / GetFpDouble(*inst.ft));
      }
      break;
    }
    case Opcode::kSqrtD: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDouble(*inst.fd, std::sqrt(GetFpDouble(*inst.fs)));
      }
      break;
    }
    case Opcode::kAbsD: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDoubleBits(*inst.fd, GetFpDoubleBits(*inst.fs) & ~(1ULL << 63));
      }
      break;
    }
    case Opcode::kMovD: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDoubleBits(*inst.fd, GetFpDoubleBits(*inst.fs));
      }
      break;
    }
    case Opcode::kNegD: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fd) || !IsEvenFpRegister(*inst.fs)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDoubleBits(*inst.fd, GetFpDoubleBits(*inst.fs) ^ (1ULL << 63));
      }
      break;
    }
    case Opcode::kCvtSD: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fs)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpRegister(*inst.fd, static_cast<float>(GetFpDouble(*inst.fs)));
      }
      break;
    }
    case Opcode::kCvtDS: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fd)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        SetFpDouble(*inst.fd, static_cast<double>(GetFpRegister(*inst.fs)));
      }
      break;
    }
    case Opcode::kCvtDW: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fd)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        int32_t val = static_cast<int32_t>(GetFpBits(*inst.fs));
        SetFpDouble(*inst.fd, static_cast<double>(val));
      }
      break;
    }
    case Opcode::kTruncWD: {
      if (inst.fd && inst.fs) {
        if (!IsEvenFpRegister(*inst.fs)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        double d = GetFpDouble(*inst.fs);
        int32_t val = 0;
        if (std::isnan(d)) {
          val = 0x7FFFFFFF;
        } else if (d >= 2147483647.0) {
          val = 0x7FFFFFFF;
        } else if (d <= -2147483648.0) {
          val = static_cast<int32_t>(0x80000000u);
        } else {
          val = static_cast<int32_t>(std::trunc(d));
        }
        SetFpBits(*inst.fd, static_cast<uint32_t>(val));
      }
      break;
    }

    // Floating-Point Comparisons
    case Opcode::kCEqS: {
      if (inst.fs && inst.ft) {
        fpu_cond_ = (GetFpRegister(*inst.fs) == GetFpRegister(*inst.ft));
      }
      break;
    }
    case Opcode::kCLtS: {
      if (inst.fs && inst.ft) {
        fpu_cond_ = (GetFpRegister(*inst.fs) < GetFpRegister(*inst.ft));
      }
      break;
    }
    case Opcode::kCLeS: {
      if (inst.fs && inst.ft) {
        fpu_cond_ = (GetFpRegister(*inst.fs) <= GetFpRegister(*inst.ft));
      }
      break;
    }
    case Opcode::kCEqD: {
      if (inst.fs && inst.ft) {
        if (!IsEvenFpRegister(*inst.fs) || !IsEvenFpRegister(*inst.ft)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        fpu_cond_ = (GetFpDouble(*inst.fs) == GetFpDouble(*inst.ft));
      }
      break;
    }
    case Opcode::kCLtD: {
      if (inst.fs && inst.ft) {
        if (!IsEvenFpRegister(*inst.fs) || !IsEvenFpRegister(*inst.ft)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        fpu_cond_ = (GetFpDouble(*inst.fs) < GetFpDouble(*inst.ft));
      }
      break;
    }
    case Opcode::kCLeD: {
      if (inst.fs && inst.ft) {
        if (!IsEvenFpRegister(*inst.fs) || !IsEvenFpRegister(*inst.ft)) {
          return ExecutionStatus::kInvalidOpcode;
        }
        fpu_cond_ = (GetFpDouble(*inst.fs) <= GetFpDouble(*inst.ft));
      }
      break;
    }

    // Floating-Point Conditional Branches
    case Opcode::kBc1t:
    case Opcode::kBc1f: {
      bool take_branch = (inst.opcode == Opcode::kBc1t) ? fpu_cond_ : !fpu_cond_;
      in_delay_slot_ = true;
      if (take_branch) {
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      } else {
        delayed_branch_target_ = current_pc + 8;
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    }
    case Opcode::kBc1tl:
    case Opcode::kBc1fl: {
      bool take_branch = (inst.opcode == Opcode::kBc1tl) ? fpu_cond_ : !fpu_cond_;
      if (take_branch) {
        in_delay_slot_ = true;
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
        pc_ = advanced_pc;
      } else {
        pc_ = current_pc + 8;
      }
      return ExecutionStatus::kRunning;
    }
    default:
      return ExecutionStatus::kInvalidOpcode;
  }

  // Handle PC advancement and delay slot completion
  if (executing_delay_slot) {
    in_delay_slot_ = false;
    delayed_branch_target_.reset();
    if (delayed_is_return) {
      delay_slot_is_return_ = false;
      return ExecutionStatus::kHaltedReturn;
    }
    if (next_branch_target.has_value()) {
      pc_ = *next_branch_target;
    } else {
      pc_ = advanced_pc;
    }
  } else {
    pc_ = advanced_pc;
  }

  return ExecutionStatus::kRunning;
}

ExecutionResult MipsEmulator::RunFunction(uint32_t start_vram, uint64_t max_steps) {
  ExecutionResult result;
  pc_ = start_vram;
  in_delay_slot_ = false;
  delayed_branch_target_.reset();
  delay_slot_is_return_ = false;

  // Set return address sentinel if unset
  if (gpr_[31] == 0) {
    gpr_[31] = kReturnAddressSentinel;
  }

  while (result.total_steps < max_steps) {
    result.total_steps++;
    ExecutionStatus status = Step();
    if (status == ExecutionStatus::kHaltedReturn) {
      result.status = ExecutionStatus::kHaltedReturn;
      result.v0 = gpr_[2];
      result.v1 = gpr_[3];
      result.f0 = GetFpRegister(FpRegister::kF0);
      result.f0_bits = GetFpBits(FpRegister::kF0);
      result.f0_double = GetFpDouble(FpRegister::kF0);
      result.f0_double_bits = GetFpDoubleBits(FpRegister::kF0);
      result.write_log = write_log_;
      return result;
    }
    if (status != ExecutionStatus::kRunning) {
      result.status = status;
      result.v0 = gpr_[2];
      result.v1 = gpr_[3];
      result.f0 = GetFpRegister(FpRegister::kF0);
      result.f0_bits = GetFpBits(FpRegister::kF0);
      result.f0_double = GetFpDouble(FpRegister::kF0);
      result.f0_double_bits = GetFpDoubleBits(FpRegister::kF0);
      result.write_log = write_log_;
      result.error_message = absl::StrFormat("Execution halted with status %d at PC 0x%08X",
                                             static_cast<int>(status), pc_);
      return result;
    }
  }

  result.status = ExecutionStatus::kMaxStepsReached;
  result.v0 = gpr_[2];
  result.v1 = gpr_[3];
  result.f0 = GetFpRegister(FpRegister::kF0);
  result.f0_bits = GetFpBits(FpRegister::kF0);
  result.f0_double = GetFpDouble(FpRegister::kF0);
  result.f0_double_bits = GetFpDoubleBits(FpRegister::kF0);
  result.write_log = write_log_;
  result.error_message = absl::StrFormat("Exceeded maximum step limit of %llu", max_steps);
  return result;
}

}  // namespace rom_nom_nom::fuzzer
