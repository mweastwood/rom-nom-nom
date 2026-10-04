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
  if ((current_pc & 3) != 0 || (current_pc >> 30) != 2) {
    return ExecutionStatus::kMemoryFault;
  }
  uint32_t phys_pc = current_pc & 0x1FFFFFFF;
  if (phys_pc + 3 >= memory_size_) {
    return ExecutionStatus::kMemoryFault;
  }
  uint32_t raw_word = ReadBigEndian32(&memory_[phys_pc]);

  bool executing_delay_slot = in_delay_slot_;
  bool delayed_is_return = delay_slot_is_return_;
  std::optional<uint32_t> next_branch_target = delayed_branch_target_;

  // Advance default next PC
  uint32_t advanced_pc = current_pc + 4;

  uint32_t major_op = raw_word >> 26;
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  int rd = static_cast<int>((raw_word >> 11) & 0x1F);
  uint32_t sa = (raw_word >> 6) & 0x1F;
  uint32_t funct = raw_word & 0x3F;
  uint32_t imm_u = raw_word & 0xFFFF;
  int32_t imm_s = static_cast<int32_t>(static_cast<int16_t>(raw_word & 0xFFFF));
  uint32_t target = raw_word & 0x03FFFFFF;

  switch (major_op) {
    case 0x00: {  // SPECIAL
      switch (funct) {
        case 0x00:  // SLL
          SetGpr(rd, gpr_[rt] << sa);
          break;
        case 0x02:  // SRL
          SetGpr(rd, gpr_[rt] >> sa);
          break;
        case 0x03:  // SRA
          SetGpr(rd, static_cast<uint32_t>(static_cast<int32_t>(gpr_[rt]) >> sa));
          break;
        case 0x04:  // SLLV
          SetGpr(rd, gpr_[rt] << (gpr_[rs] & 0x1F));
          break;
        case 0x06:  // SRLV
          SetGpr(rd, gpr_[rt] >> (gpr_[rs] & 0x1F));
          break;
        case 0x07:  // SRAV
          SetGpr(rd, static_cast<uint32_t>(static_cast<int32_t>(gpr_[rt]) >> (gpr_[rs] & 0x1F)));
          break;
        case 0x08:  // JR
          if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
          in_delay_slot_ = true;
          if (gpr_[rs] == kReturnAddressSentinel) {
            delay_slot_is_return_ = true;
          } else {
            delayed_branch_target_ = gpr_[rs];
          }
          pc_ = advanced_pc;
          return ExecutionStatus::kRunning;
        case 0x09: {  // JALR
          if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
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
        case 0x0C:  // SYSCALL
          return ExecutionStatus::kSyscallTrap;
        case 0x0D:  // BREAK
          return ExecutionStatus::kBreakTrap;
        case 0x0F:  // SYNC
          break;
        case 0x10:  // MFHI
          SetGpr(rd, hi_);
          break;
        case 0x11:  // MTHI
          hi_ = gpr_[rs];
          break;
        case 0x12:  // MFLO
          SetGpr(rd, lo_);
          break;
        case 0x13:  // MTLO
          lo_ = gpr_[rs];
          break;
        case 0x18: {  // MULT
          int64_t prod = static_cast<int64_t>(static_cast<int32_t>(gpr_[rs])) *
                         static_cast<int64_t>(static_cast<int32_t>(gpr_[rt]));
          hi_ = static_cast<uint32_t>(prod >> 32);
          lo_ = static_cast<uint32_t>(prod & 0xFFFFFFFF);
          break;
        }
        case 0x19: {  // MULTU
          uint64_t prod = static_cast<uint64_t>(gpr_[rs]) * static_cast<uint64_t>(gpr_[rt]);
          hi_ = static_cast<uint32_t>(prod >> 32);
          lo_ = static_cast<uint32_t>(prod & 0xFFFFFFFF);
          break;
        }
        case 0x1A:  // DIV
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
        case 0x1B:  // DIVU
          if (gpr_[rt] != 0) {
            lo_ = gpr_[rs] / gpr_[rt];
            hi_ = gpr_[rs] % gpr_[rt];
          }
          break;
        case 0x20: {  // ADD
          int32_t a = static_cast<int32_t>(gpr_[rs]);
          int32_t b = static_cast<int32_t>(gpr_[rt]);
          int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
          if (((a ^ res) & (b ^ res)) < 0) {
            return ExecutionStatus::kIntegerOverflow;
          }
          SetGpr(rd, static_cast<uint32_t>(res));
          break;
        }
        case 0x21:  // ADDU
          SetGpr(rd, gpr_[rs] + gpr_[rt]);
          break;
        case 0x22: {  // SUB
          int32_t a = static_cast<int32_t>(gpr_[rs]);
          int32_t b = static_cast<int32_t>(gpr_[rt]);
          int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
          if (((a ^ b) & (a ^ res)) < 0) {
            return ExecutionStatus::kIntegerOverflow;
          }
          SetGpr(rd, static_cast<uint32_t>(res));
          break;
        }
        case 0x23:  // SUBU
          SetGpr(rd, gpr_[rs] - gpr_[rt]);
          break;
        case 0x24:  // AND
          SetGpr(rd, gpr_[rs] & gpr_[rt]);
          break;
        case 0x25:  // OR
          SetGpr(rd, gpr_[rs] | gpr_[rt]);
          break;
        case 0x26:  // XOR
          SetGpr(rd, gpr_[rs] ^ gpr_[rt]);
          break;
        case 0x27:  // NOR
          SetGpr(rd, ~(gpr_[rs] | gpr_[rt]));
          break;
        case 0x2A:  // SLT
          SetGpr(rd, (static_cast<int32_t>(gpr_[rs]) < static_cast<int32_t>(gpr_[rt])) ? 1 : 0);
          break;
        case 0x2B:  // SLTU
          SetGpr(rd, (gpr_[rs] < gpr_[rt]) ? 1 : 0);
          break;
        default:
          return ExecutionStatus::kInvalidOpcode;
      }
      break;
    }
    case 0x01: {  // REGIMM
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      switch (rt) {
        case 0x00:  // BLTZ
          in_delay_slot_ = true;
          if (static_cast<int32_t>(gpr_[rs]) < 0) {
            delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
          } else {
            delayed_branch_target_ = current_pc + 8;
          }
          pc_ = advanced_pc;
          return ExecutionStatus::kRunning;
        case 0x01:  // BGEZ
          in_delay_slot_ = true;
          if (static_cast<int32_t>(gpr_[rs]) >= 0) {
            delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
          } else {
            delayed_branch_target_ = current_pc + 8;
          }
          pc_ = advanced_pc;
          return ExecutionStatus::kRunning;
        case 0x02:  // BLTZL
          if (static_cast<int32_t>(gpr_[rs]) < 0) {
            in_delay_slot_ = true;
            delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
            pc_ = advanced_pc;
          } else {
            pc_ = current_pc + 8;
          }
          return ExecutionStatus::kRunning;
        case 0x03:  // BGEZL
          if (static_cast<int32_t>(gpr_[rs]) >= 0) {
            in_delay_slot_ = true;
            delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
            pc_ = advanced_pc;
          } else {
            pc_ = current_pc + 8;
          }
          return ExecutionStatus::kRunning;
        case 0x10:  // BLTZAL
          if (rs == 31) return ExecutionStatus::kInvalidOpcode;
          SetGpr(31, current_pc + 8);
          in_delay_slot_ = true;
          if (static_cast<int32_t>(gpr_[rs]) < 0) {
            delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
          } else {
            delayed_branch_target_ = current_pc + 8;
          }
          pc_ = advanced_pc;
          return ExecutionStatus::kRunning;
        case 0x11:  // BGEZAL
          if (rs == 31) return ExecutionStatus::kInvalidOpcode;
          SetGpr(31, current_pc + 8);
          in_delay_slot_ = true;
          if (static_cast<int32_t>(gpr_[rs]) >= 0) {
            delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
          } else {
            delayed_branch_target_ = current_pc + 8;
          }
          pc_ = advanced_pc;
          return ExecutionStatus::kRunning;
        default:
          return ExecutionStatus::kInvalidOpcode;
      }
    }
    case 0x02:  // J
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      in_delay_slot_ = true;
      delayed_branch_target_ = (current_pc & 0xF0000000) | (target << 2);
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    case 0x03:  // JAL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetGpr(31, current_pc + 8);
      in_delay_slot_ = true;
      delayed_branch_target_ = (current_pc & 0xF0000000) | (target << 2);
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    case 0x04:  // BEQ
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      in_delay_slot_ = true;
      if (gpr_[rs] == gpr_[rt]) {
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      } else {
        delayed_branch_target_ = current_pc + 8;
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    case 0x05:  // BNE
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      in_delay_slot_ = true;
      if (gpr_[rs] != gpr_[rt]) {
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      } else {
        delayed_branch_target_ = current_pc + 8;
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    case 0x06:  // BLEZ
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      in_delay_slot_ = true;
      if (static_cast<int32_t>(gpr_[rs]) <= 0) {
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      } else {
        delayed_branch_target_ = current_pc + 8;
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    case 0x07:  // BGTZ
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      in_delay_slot_ = true;
      if (static_cast<int32_t>(gpr_[rs]) > 0) {
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      } else {
        delayed_branch_target_ = current_pc + 8;
      }
      pc_ = advanced_pc;
      return ExecutionStatus::kRunning;
    case 0x08: {  // ADDI
      int32_t a = static_cast<int32_t>(gpr_[rs]);
      int32_t b = imm_s;
      int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
      if (((a ^ res) & (b ^ res)) < 0) {
        return ExecutionStatus::kIntegerOverflow;
      }
      SetGpr(rt, static_cast<uint32_t>(res));
      break;
    }
    case 0x09:  // ADDIU
      SetGpr(rt, gpr_[rs] + static_cast<uint32_t>(imm_s));
      break;
    case 0x0A:  // SLTI
      SetGpr(rt, (static_cast<int32_t>(gpr_[rs]) < imm_s) ? 1 : 0);
      break;
    case 0x0B:  // SLTIU
      SetGpr(rt, (gpr_[rs] < static_cast<uint32_t>(imm_s)) ? 1 : 0);
      break;
    case 0x0C:  // ANDI
      SetGpr(rt, gpr_[rs] & imm_u);
      break;
    case 0x0D:  // ORI
      SetGpr(rt, gpr_[rs] | imm_u);
      break;
    case 0x0E:  // XORI
      SetGpr(rt, gpr_[rs] ^ imm_u);
      break;
    case 0x0F:  // LUI
      SetGpr(rt, imm_u << 16);
      break;
    case 0x11: {  // COP1
      FpRegister fs = static_cast<FpRegister>(rd);
      FpRegister ft = static_cast<FpRegister>(rt);
      FpRegister fd = static_cast<FpRegister>(sa);

      if (rs == 0x00) {  // MFC1
        SetGpr(rt, GetFpBits(fs));
        break;
      }
      if (rs == 0x04) {  // MTC1
        SetFpBits(fs, gpr_[rt]);
        break;
      }
      if (rs == 0x08) {  // BC1
        if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
        switch (rt) {
          case 0x00:    // BC1F
          case 0x01: {  // BC1T
            bool take = (rt == 0x01) ? fpu_cond_ : !fpu_cond_;
            in_delay_slot_ = true;
            if (take) {
              delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
            } else {
              delayed_branch_target_ = current_pc + 8;
            }
            pc_ = advanced_pc;
            return ExecutionStatus::kRunning;
          }
          case 0x02:    // BC1FL
          case 0x03: {  // BC1TL
            bool take = (rt == 0x03) ? fpu_cond_ : !fpu_cond_;
            if (take) {
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
      }
      if (rs == 0x10) {  // Single-precision float
        switch (funct) {
          case 0x00:
            SetFpRegister(fd, GetFpRegister(fs) + GetFpRegister(ft));
            break;
          case 0x01:
            SetFpRegister(fd, GetFpRegister(fs) - GetFpRegister(ft));
            break;
          case 0x02:
            SetFpRegister(fd, GetFpRegister(fs) * GetFpRegister(ft));
            break;
          case 0x03:
            SetFpRegister(fd, GetFpRegister(fs) / GetFpRegister(ft));
            break;
          case 0x04:
            SetFpRegister(fd, std::sqrt(GetFpRegister(fs)));
            break;
          case 0x05:
            SetFpBits(fd, GetFpBits(fs) & 0x7FFFFFFF);
            break;
          case 0x06:
            SetFpBits(fd, GetFpBits(fs));
            break;
          case 0x07:
            SetFpBits(fd, GetFpBits(fs) ^ 0x80000000);
            break;
          case 0x0D: {  // TRUNC.W.S
            float f = GetFpRegister(fs);
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
            SetFpBits(fd, static_cast<uint32_t>(val));
            break;
          }
          case 0x21:  // CVT.D.S
            if (!IsEvenFpRegister(fd)) return ExecutionStatus::kInvalidOpcode;
            SetFpDouble(fd, static_cast<double>(GetFpRegister(fs)));
            break;
          case 0x32:  // C.EQ.S
            fpu_cond_ = (GetFpRegister(fs) == GetFpRegister(ft));
            break;
          case 0x3C:  // C.LT.S
            fpu_cond_ = (GetFpRegister(fs) < GetFpRegister(ft));
            break;
          case 0x3E:  // C.LE.S
            fpu_cond_ = (GetFpRegister(fs) <= GetFpRegister(ft));
            break;
          default:
            return ExecutionStatus::kInvalidOpcode;
        }
        break;
      }
      if (rs == 0x11) {  // Double-precision float
        switch (funct) {
          case 0x00:  // ADD.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDouble(fd, GetFpDouble(fs) + GetFpDouble(ft));
            break;
          case 0x01:  // SUB.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDouble(fd, GetFpDouble(fs) - GetFpDouble(ft));
            break;
          case 0x02:  // MUL.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDouble(fd, GetFpDouble(fs) * GetFpDouble(ft));
            break;
          case 0x03:  // DIV.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDouble(fd, GetFpDouble(fs) / GetFpDouble(ft));
            break;
          case 0x04:  // SQRT.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDouble(fd, std::sqrt(GetFpDouble(fs)));
            break;
          case 0x05:  // ABS.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDoubleBits(fd, GetFpDoubleBits(fs) & ~(1ULL << 63));
            break;
          case 0x06:  // MOV.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDoubleBits(fd, GetFpDoubleBits(fs));
            break;
          case 0x07:  // NEG.D
            if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDoubleBits(fd, GetFpDoubleBits(fs) ^ (1ULL << 63));
            break;
          case 0x0D: {  // TRUNC.W.D
            if (!IsEvenFpRegister(fs)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            double d = GetFpDouble(fs);
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
            SetFpBits(fd, static_cast<uint32_t>(val));
            break;
          }
          case 0x20:  // CVT.S.D
            if (!IsEvenFpRegister(fs)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpRegister(fd, static_cast<float>(GetFpDouble(fs)));
            break;
          case 0x32:  // C.EQ.D
            if (!IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            fpu_cond_ = (GetFpDouble(fs) == GetFpDouble(ft));
            break;
          case 0x3C:  // C.LT.D
            if (!IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            fpu_cond_ = (GetFpDouble(fs) < GetFpDouble(ft));
            break;
          case 0x3E:  // C.LE.D
            if (!IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            fpu_cond_ = (GetFpDouble(fs) <= GetFpDouble(ft));
            break;
          default:
            return ExecutionStatus::kInvalidOpcode;
        }
        break;
      }
      if (rs == 0x14) {  // Word to Float
        switch (funct) {
          case 0x20:  // CVT.S.W
            SetFpRegister(fd, static_cast<float>(static_cast<int32_t>(GetFpBits(fs))));
            break;
          case 0x21:  // CVT.D.W
            if (!IsEvenFpRegister(fd)) {
              return ExecutionStatus::kInvalidOpcode;
            }
            SetFpDouble(fd, static_cast<double>(static_cast<int32_t>(GetFpBits(fs))));
            break;
          default:
            return ExecutionStatus::kInvalidOpcode;
        }
        break;
      }
      return ExecutionStatus::kInvalidOpcode;
    }
    case 0x14:  // BEQL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      if (gpr_[rs] == gpr_[rt]) {
        in_delay_slot_ = true;
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
        pc_ = advanced_pc;
      } else {
        pc_ = current_pc + 8;
      }
      return ExecutionStatus::kRunning;
    case 0x15:  // BNEL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      if (gpr_[rs] != gpr_[rt]) {
        in_delay_slot_ = true;
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
        pc_ = advanced_pc;
      } else {
        pc_ = current_pc + 8;
      }
      return ExecutionStatus::kRunning;
    case 0x16:  // BLEZL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      if (static_cast<int32_t>(gpr_[rs]) <= 0) {
        in_delay_slot_ = true;
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
        pc_ = advanced_pc;
      } else {
        pc_ = current_pc + 8;
      }
      return ExecutionStatus::kRunning;
    case 0x17:  // BGTZL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      if (static_cast<int32_t>(gpr_[rs]) > 0) {
        in_delay_slot_ = true;
        delayed_branch_target_ = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
        pc_ = advanced_pc;
      } else {
        pc_ = current_pc + 8;
      }
      return ExecutionStatus::kRunning;
    case 0x20: {  // LB
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint8_t val = 0;
      if (!Read8(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(val))));
      break;
    }
    case 0x21: {  // LH
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      uint16_t val = 0;
      if (!Read16(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(val))));
      break;
    }
    case 0x22: {  // LWL
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu << (shift_byte * 8);
      SetGpr(rt, (gpr_[rt] & ~mask) | (word << (shift_byte * 8)));
      break;
    }
    case 0x23: {  // LW
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      uint32_t val = 0;
      if (!Read32(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, val);
      break;
    }
    case 0x24: {  // LBU
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint8_t val = 0;
      if (!Read8(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, val);
      break;
    }
    case 0x25: {  // LHU
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      uint16_t val = 0;
      if (!Read16(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, val);
      break;
    }
    case 0x26: {  // LWR
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu >> ((3 - shift_byte) * 8);
      SetGpr(rt, (gpr_[rt] & ~mask) | (word >> ((3 - shift_byte) * 8)));
      break;
    }
    case 0x28: {  // SB
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if (!Write8(addr, static_cast<uint8_t>(gpr_[rt]))) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x29: {  // SH
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write16(addr, static_cast<uint16_t>(gpr_[rt]))) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x2A: {  // SWL
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
    case 0x2B: {  // SW
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write32(addr, gpr_[rt])) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x2E: {  // SWR
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
    case 0x31: {  // LWC1
      FpRegister ft = static_cast<FpRegister>(rt);
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      uint32_t word = 0;
      if (!Read32(addr, &word)) return ExecutionStatus::kMemoryFault;
      SetFpBits(ft, word);
      break;
    }
    case 0x35: {  // LDC1
      FpRegister ft = static_cast<FpRegister>(rt);
      if (!IsEvenFpRegister(ft)) return ExecutionStatus::kInvalidOpcode;
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      uint64_t val = 0;
      if (!Read64(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetFpDoubleBits(ft, val);
      break;
    }
    case 0x39: {  // SWC1
      FpRegister ft = static_cast<FpRegister>(rt);
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write32(addr, GetFpBits(ft))) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x3D: {  // SDC1
      FpRegister ft = static_cast<FpRegister>(rt);
      if (!IsEvenFpRegister(ft)) return ExecutionStatus::kInvalidOpcode;
      uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write64(addr, GetFpDoubleBits(ft))) return ExecutionStatus::kMemoryFault;
      break;
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
