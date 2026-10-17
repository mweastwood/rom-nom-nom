#include "fuzzer/mips_emulator.h"

#include <cmath>
#include <cstring>
#include <optional>

#include "absl/strings/str_format.h"

namespace rom_nom_nom::fuzzer {

namespace {
constexpr uint32_t kSpStatusReg = 0x04040010;
constexpr uint32_t kSpDmaBusyReg = 0x04040018;
constexpr uint32_t kDpcStatusReg = 0x0410000C;
constexpr uint32_t kMiVersionReg = 0x04300004;
constexpr uint32_t kViStatusReg = 0x04400000;
constexpr uint32_t kViCurrentReg = 0x04400010;
constexpr uint32_t kAiStatusReg = 0x0450000C;
constexpr uint32_t kPiStatusReg = 0x04600010;
constexpr uint32_t kSiStatusReg = 0x04800018;

bool IsEvenFpRegister(FpRegister reg) {
  return (static_cast<int>(reg) & 1) == 0;
}

int32_t ClampDoubleToWord(double val) {
  if (std::isnan(val) || val >= 2147483648.0) {
    return 0x7FFFFFFF;
  }
  if (val < -2147483648.0) {
    return static_cast<int32_t>(0x80000000u);
  }
  return static_cast<int32_t>(val);
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
  fcr31_ = 0;
  hi_ = 0;
  lo_ = 0;
  pc_ = base_vram_;
  in_delay_slot_ = false;
  delayed_branch_target_.reset();
  delay_slot_is_return_ = false;
  delay_slot_is_call_ = false;
  call_link_reg_ = 31;
  write_log_.clear();
  call_log_.clear();
  mmio_regs_.clear();
  vi_scanline_counter_ = 0;
  written_phys_addrs_.clear();
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
  fpr_bits_[idx + 1] = static_cast<uint32_t>(bits >> 32);     // Odd register is MSW
  fpr_bits_[idx] = static_cast<uint32_t>(bits & 0xFFFFFFFF);  // Even register is LSW
}

uint64_t MipsEmulator::GetFpDoubleBits(FpRegister reg) const {
  int idx = static_cast<int>(reg);
  if ((idx & 1) != 0 || idx < 0 || idx >= 32) return 0;
  return (static_cast<uint64_t>(fpr_bits_[idx + 1]) << 32) | static_cast<uint64_t>(fpr_bits_[idx]);
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
  bool code_touched = false;
  uint32_t code_end = code_vram + static_cast<uint32_t>(code.size() * sizeof(uint32_t));
  for (auto it = write_log_.rbegin(); it != write_log_.rend(); ++it) {
    auto phys_opt = VramToPhysical(it->address);
    if (!phys_opt.has_value() || *phys_opt + it->size > memory_size_) {
      continue;
    }
    if (!code.empty() && it->address + it->size > code_vram && it->address < code_end) {
      code_touched = true;
    }
    switch (it->size) {
      case 1:
        memory_[*phys_opt] = static_cast<uint8_t>(it->previous_value);
        break;
      case 2:
        WriteBigEndian16(&memory_[*phys_opt], static_cast<uint16_t>(it->previous_value));
        break;
      case 4:
        WriteBigEndian32(&memory_[*phys_opt], static_cast<uint32_t>(it->previous_value));
        break;
      case 8:
        WriteBigEndian64(&memory_[*phys_opt], it->previous_value);
        break;
    }
  }
  if (code_touched) {
    LoadWords(code_vram, code);
  }
  write_log_.clear();
  written_phys_addrs_.clear();
  mmio_regs_.clear();
  vi_scanline_counter_ = 0;
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

uint32_t MipsEmulator::ReadMmio32(uint32_t phys_addr) const {
  switch (phys_addr) {
    case kSpStatusReg:
      return 0x0001;  // SP_STATUS_HALT
    case kSpDmaBusyReg:
      return 0x0000;
    case kDpcStatusReg:
      return 0x0080;  // DPC_STATUS_CBUF_READY
    case kMiVersionReg:
      return 0x02020102;
    case kViStatusReg:
      return 0x00000002;
    case kViCurrentReg: {
      uint32_t line = (vi_scanline_counter_ * 2) % 525;
      vi_scanline_counter_++;
      return line;
    }
    case kAiStatusReg:
      return 0x0000;
    case kPiStatusReg:
      return 0x0000;
    case kSiStatusReg:
      return 0x0000;
    default: {
      auto it = mmio_regs_.find(phys_addr);
      if (it != mmio_regs_.end()) {
        return it->second;
      }
      return 0;
    }
  }
}

void MipsEmulator::WriteMmio32(uint32_t phys_addr, uint32_t val) {
  mmio_regs_[phys_addr] = val;
}

bool MipsEmulator::IsExternalCallTarget(uint32_t target_vram) const {
  return intercept_external_calls_ && code_bounds_.has_value() &&
         (target_vram < code_bounds_->first || target_vram >= code_bounds_->second);
}

void MipsEmulator::SetupBranch(bool taken, uint32_t branch_target, uint32_t current_pc) {
  in_delay_slot_ = true;
  delayed_branch_target_ = taken ? branch_target : (current_pc + 8);
  if (taken && IsExternalCallTarget(branch_target)) {
    delay_slot_is_call_ = true;
    call_link_reg_ = 31;
  } else {
    delay_slot_is_call_ = false;
  }
}

void MipsEmulator::SetupBranchLikely(bool taken, uint32_t branch_target, uint32_t current_pc,
                                     uint32_t* next_pc) {
  if (taken) {
    in_delay_slot_ = true;
    delayed_branch_target_ = branch_target;
    if (IsExternalCallTarget(branch_target)) {
      delay_slot_is_call_ = true;
      call_link_reg_ = 31;
    } else {
      delay_slot_is_call_ = false;
    }
    *next_pc = current_pc + 4;
  } else {
    in_delay_slot_ = false;
    delay_slot_is_call_ = false;
    *next_pc = current_pc + 8;
  }
}

std::optional<ExecutionStatus> MipsEmulator::HandleCallTarget(uint32_t target_vram, int link_reg) {
  if (IsExternalCallTarget(target_vram) && VramToPhysical(target_vram).has_value() &&
      ((target_vram & 3) == 0)) {
    call_log_.push_back(ExternalCall{
        .target_address = target_vram,
        .a0 = gpr_[4],
        .a1 = gpr_[5],
        .a2 = gpr_[6],
        .a3 = gpr_[7],
    });
    gpr_[2] = 0;  // $v0 = 0
    gpr_[3] = 0;  // $v1 = 0
    SetFpRegister(FpRegister::kF0, 0.0f);
    SetFpDouble(FpRegister::kF0, 0.0);
    if (gpr_[link_reg] == kReturnAddressSentinel) {
      return ExecutionStatus::kHaltedReturn;
    }
    pc_ = gpr_[link_reg];
    return ExecutionStatus::kRunning;
  }
  return std::nullopt;
}

ExecutionStatus MipsEmulator::RecordInvalidOpcode(uint32_t raw_word, bool executing_delay_slot,
                                                  std::string_view reason) {
  uint32_t major_op = raw_word >> 26;
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  int rd = static_cast<int>((raw_word >> 11) & 0x1F);
  uint32_t funct = raw_word & 0x3F;
  last_invalid_info_ = absl::StrFormat(
      "raw_word=0x%08X (op=0x%02X, rs=%d, rt=%d, rd=%d, funct=0x%02X, delay_slot=%d) %s", raw_word,
      major_op, rs, rt, rd, funct, executing_delay_slot, reason);
  return ExecutionStatus::kInvalidOpcode;
}

ExecutionStatus MipsEmulator::StepSpecial(uint32_t raw_word, uint32_t current_pc,
                                          bool executing_delay_slot) {
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  int rd = static_cast<int>((raw_word >> 11) & 0x1F);
  uint32_t sa = (raw_word >> 6) & 0x1F;
  uint32_t funct = raw_word & 0x3F;

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
    case 0x08: {  // JR
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      in_delay_slot_ = true;
      if (gpr_[rs] == kReturnAddressSentinel) {
        delay_slot_is_return_ = true;
        delay_slot_is_call_ = false;
      } else {
        delay_slot_is_return_ = false;
        uint32_t jr_target = gpr_[rs];
        if (IsExternalCallTarget(jr_target)) {
          delay_slot_is_call_ = true;
          call_link_reg_ = 31;
        } else {
          delay_slot_is_call_ = false;
        }
        delayed_branch_target_ = jr_target;
      }
      break;
    }
    case 0x09: {  // JALR
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      int link_reg = (rd != 0) ? rd : 31;
      if (rs == link_reg) {
        return ExecutionStatus::kInvalidOpcode;  // MIPS III undefined restriction
      }
      SetGpr(link_reg, current_pc + 8);
      in_delay_slot_ = true;
      delay_slot_is_call_ = true;
      call_link_reg_ = link_reg;
      delayed_branch_target_ = gpr_[rs];
      break;
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
    case 0x14:  // DSLLV
      SetGpr(rd, static_cast<uint32_t>(static_cast<uint64_t>(gpr_[rt]) << (gpr_[rs] & 0x3F)));
      break;
    case 0x16:  // DSRLV
      SetGpr(rd, static_cast<uint32_t>(static_cast<uint64_t>(gpr_[rt]) >> (gpr_[rs] & 0x3F)));
      break;
    case 0x17:  // DSRAV
      SetGpr(rd, static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(gpr_[rt])) >>
                                       (gpr_[rs] & 0x3F)));
      break;
    case 0x18:    // MULT
    case 0x1C: {  // DMULT
      int64_t prod = static_cast<int64_t>(static_cast<int32_t>(gpr_[rs])) *
                     static_cast<int64_t>(static_cast<int32_t>(gpr_[rt]));
      hi_ = static_cast<uint32_t>(prod >> 32);
      lo_ = static_cast<uint32_t>(prod & 0xFFFFFFFF);
      break;
    }
    case 0x19:    // MULTU
    case 0x1D: {  // DMULTU
      uint64_t prod = static_cast<uint64_t>(gpr_[rs]) * static_cast<uint64_t>(gpr_[rt]);
      hi_ = static_cast<uint32_t>(prod >> 32);
      lo_ = static_cast<uint32_t>(prod & 0xFFFFFFFF);
      break;
    }
    case 0x1A:  // DIV
    case 0x1E:  // DDIV
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
    case 0x1F:  // DDIVU
      if (gpr_[rt] != 0) {
        lo_ = gpr_[rs] / gpr_[rt];
        hi_ = gpr_[rs] % gpr_[rt];
      }
      break;
    case 0x20:    // ADD
    case 0x2C: {  // DADD
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
    case 0x2D:  // DADDU
      SetGpr(rd, gpr_[rs] + gpr_[rt]);
      break;
    case 0x22:    // SUB
    case 0x2E: {  // DSUB
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
    case 0x2F:  // DSUBU
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
    case 0x38:  // DSLL
      SetGpr(rd, gpr_[rt] << sa);
      break;
    case 0x3A:  // DSRL
      SetGpr(rd, gpr_[rt] >> sa);
      break;
    case 0x3B:  // DSRA
      SetGpr(rd, static_cast<uint32_t>(static_cast<int32_t>(gpr_[rt]) >> sa));
      break;
    case 0x3C:  // DSLL32
      SetGpr(rd, static_cast<uint32_t>(static_cast<uint64_t>(gpr_[rt]) << (sa + 32)));
      break;
    case 0x3E:  // DSRL32
      SetGpr(rd, static_cast<uint32_t>(static_cast<uint64_t>(gpr_[rt]) >> (sa + 32)));
      break;
    case 0x3F:  // DSRA32
      SetGpr(rd, static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(gpr_[rt])) >>
                                       (sa + 32)));
      break;
    default:
      return ExecutionStatus::kInvalidOpcode;
  }
  return ExecutionStatus::kRunning;
}

ExecutionStatus MipsEmulator::StepRegimm(uint32_t raw_word, uint32_t current_pc,
                                         bool executing_delay_slot, uint32_t* next_pc) {
  if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  int32_t imm_s = static_cast<int32_t>(static_cast<int16_t>(raw_word & 0xFFFF));
  uint32_t branch_target = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);

  switch (rt) {
    case 0x00:  // BLTZ
      SetupBranch(static_cast<int32_t>(gpr_[rs]) < 0, branch_target, current_pc);
      return ExecutionStatus::kRunning;
    case 0x01:  // BGEZ
      SetupBranch(static_cast<int32_t>(gpr_[rs]) >= 0, branch_target, current_pc);
      return ExecutionStatus::kRunning;
    case 0x02:  // BLTZL
      SetupBranchLikely(static_cast<int32_t>(gpr_[rs]) < 0, branch_target, current_pc, next_pc);
      return ExecutionStatus::kRunning;
    case 0x03:  // BGEZL
      SetupBranchLikely(static_cast<int32_t>(gpr_[rs]) >= 0, branch_target, current_pc, next_pc);
      return ExecutionStatus::kRunning;
    case 0x10:    // BLTZAL
    case 0x11: {  // BGEZAL
      if (rs == 31) return ExecutionStatus::kInvalidOpcode;
      bool taken = (rt == 0x10) ? (static_cast<int32_t>(gpr_[rs]) < 0)
                                : (static_cast<int32_t>(gpr_[rs]) >= 0);
      SetGpr(31, current_pc + 8);
      in_delay_slot_ = true;
      call_link_reg_ = 31;
      delayed_branch_target_ = taken ? branch_target : (current_pc + 8);
      delay_slot_is_call_ = taken;
      return ExecutionStatus::kRunning;
    }
    case 0x12:    // BLTZALL
    case 0x13: {  // BGEZALL
      if (rs == 31) return ExecutionStatus::kInvalidOpcode;
      bool taken = (rt == 0x12) ? (static_cast<int32_t>(gpr_[rs]) < 0)
                                : (static_cast<int32_t>(gpr_[rs]) >= 0);
      SetGpr(31, current_pc + 8);
      call_link_reg_ = 31;
      if (taken) {
        in_delay_slot_ = true;
        delayed_branch_target_ = branch_target;
        delay_slot_is_call_ = true;
        *next_pc = current_pc + 4;
      } else {
        in_delay_slot_ = false;
        delay_slot_is_call_ = false;
        *next_pc = current_pc + 8;
      }
      return ExecutionStatus::kRunning;
    }
    default:
      return ExecutionStatus::kInvalidOpcode;
  }
}

ExecutionStatus MipsEmulator::StepCop0(uint32_t raw_word, bool executing_delay_slot) {
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  uint32_t funct = raw_word & 0x3F;

  if ((raw_word & 0x02000000) != 0) {
    // COP0 CO functions (TLB instructions, ERET, etc.)
    if (funct == 0x18) {  // ERET
      if (delayed_branch_target_.has_value()) {
        pc_ = *delayed_branch_target_;
      }
      return ExecutionStatus::kHaltedReturn;
    }
    return ExecutionStatus::kRunning;
  }
  if (rs == 0x00) {  // MFC0
    SetGpr(rt, 0);
    return ExecutionStatus::kRunning;
  }
  if (rs == 0x04) {  // MTC0
    return ExecutionStatus::kRunning;
  }
  return RecordInvalidOpcode(raw_word, executing_delay_slot, "unhandled COP0");
}

ExecutionStatus MipsEmulator::StepCop1Single(uint32_t raw_word, FpRegister fs, FpRegister ft,
                                             FpRegister fd) {
  uint32_t funct = raw_word & 0x3F;
  if (funct >= 0x30 && funct <= 0x3F) {
    float a = GetFpRegister(fs);
    float b = GetFpRegister(ft);
    bool unordered = std::isnan(a) || std::isnan(b);
    bool equal = !unordered && (a == b);
    bool less = !unordered && (a < b);
    fpu_cond_ = (((funct & 1) != 0) && unordered) || (((funct & 2) != 0) && equal) ||
                (((funct & 4) != 0) && less);
    return ExecutionStatus::kRunning;
  }
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
    case 0x0C: {  // ROUND.W.S
      double d = std::nearbyint(static_cast<double>(GetFpRegister(fs)));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x0D: {  // TRUNC.W.S
      double d = std::trunc(static_cast<double>(GetFpRegister(fs)));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x0E: {  // CEIL.W.S
      double d = std::ceil(static_cast<double>(GetFpRegister(fs)));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x0F: {  // FLOOR.W.S
      double d = std::floor(static_cast<double>(GetFpRegister(fs)));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x21:  // CVT.D.S
      if (!IsEvenFpRegister(fd)) return ExecutionStatus::kInvalidOpcode;
      SetFpDouble(fd, static_cast<double>(GetFpRegister(fs)));
      break;
    case 0x24: {  // CVT.W.S
      double d = static_cast<double>(GetFpRegister(fs));
      switch (fcr31_ & 3) {
        case 0:
          d = std::nearbyint(d);
          break;
        case 1:
          d = std::trunc(d);
          break;
        case 2:
          d = std::ceil(d);
          break;
        case 3:
          d = std::floor(d);
          break;
      }
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    default:
      return ExecutionStatus::kInvalidOpcode;
  }
  return ExecutionStatus::kRunning;
}

ExecutionStatus MipsEmulator::StepCop1Double(uint32_t raw_word, FpRegister fs, FpRegister ft,
                                             FpRegister fd) {
  uint32_t funct = raw_word & 0x3F;
  if (funct >= 0x30 && funct <= 0x3F) {
    if (!IsEvenFpRegister(fs) || !IsEvenFpRegister(ft)) {
      return ExecutionStatus::kInvalidOpcode;
    }
    double a = GetFpDouble(fs);
    double b = GetFpDouble(ft);
    bool unordered = std::isnan(a) || std::isnan(b);
    bool equal = !unordered && (a == b);
    bool less = !unordered && (a < b);
    fpu_cond_ = (((funct & 1) != 0) && unordered) || (((funct & 2) != 0) && equal) ||
                (((funct & 4) != 0) && less);
    return ExecutionStatus::kRunning;
  }
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
    case 0x0C: {  // ROUND.W.D
      if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
        return ExecutionStatus::kInvalidOpcode;
      }
      double d = std::nearbyint(GetFpDouble(fs));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x0D: {  // TRUNC.W.D
      if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
        return ExecutionStatus::kInvalidOpcode;
      }
      double d = std::trunc(GetFpDouble(fs));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x0E: {  // CEIL.W.D
      if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
        return ExecutionStatus::kInvalidOpcode;
      }
      double d = std::ceil(GetFpDouble(fs));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x0F: {  // FLOOR.W.D
      if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
        return ExecutionStatus::kInvalidOpcode;
      }
      double d = std::floor(GetFpDouble(fs));
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    case 0x20:  // CVT.S.D
      if (!IsEvenFpRegister(fs)) {
        return ExecutionStatus::kInvalidOpcode;
      }
      SetFpRegister(fd, static_cast<float>(GetFpDouble(fs)));
      break;
    case 0x24: {  // CVT.W.D
      if (!IsEvenFpRegister(fd) || !IsEvenFpRegister(fs)) {
        return ExecutionStatus::kInvalidOpcode;
      }
      double d = GetFpDouble(fs);
      switch (fcr31_ & 3) {
        case 0:
          d = std::nearbyint(d);
          break;
        case 1:
          d = std::trunc(d);
          break;
        case 2:
          d = std::ceil(d);
          break;
        case 3:
          d = std::floor(d);
          break;
      }
      SetFpBits(fd, static_cast<uint32_t>(ClampDoubleToWord(d)));
      break;
    }
    default:
      return ExecutionStatus::kInvalidOpcode;
  }
  return ExecutionStatus::kRunning;
}

ExecutionStatus MipsEmulator::StepCop1(uint32_t raw_word, uint32_t current_pc,
                                       bool executing_delay_slot, uint32_t* next_pc) {
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  int rd = static_cast<int>((raw_word >> 11) & 0x1F);
  uint32_t sa = (raw_word >> 6) & 0x1F;
  uint32_t funct = raw_word & 0x3F;
  int32_t imm_s = static_cast<int32_t>(static_cast<int16_t>(raw_word & 0xFFFF));

  FpRegister fs = static_cast<FpRegister>(rd);
  FpRegister ft = static_cast<FpRegister>(rt);
  FpRegister fd = static_cast<FpRegister>(sa);

  switch (rs) {
    case 0x00:  // MFC1
      SetGpr(rt, GetFpBits(fs));
      return ExecutionStatus::kRunning;
    case 0x01:  // DMFC1
      if (!IsEvenFpRegister(fs)) return ExecutionStatus::kInvalidOpcode;
      SetGpr(rt, GetFpBits(fs));
      return ExecutionStatus::kRunning;
    case 0x02: {  // CFC1
      uint32_t ctrl_reg = static_cast<uint32_t>(fs);
      uint32_t val = 0;
      if (ctrl_reg == 31) {
        val = (fcr31_ & ~(1u << 23)) | (fpu_cond_ ? (1u << 23) : 0);
      } else if (ctrl_reg == 0) {
        val = 0x00000B00;  // VR4300 revision
      }
      SetGpr(rt, val);
      return ExecutionStatus::kRunning;
    }
    case 0x04:  // MTC1
      SetFpBits(fs, gpr_[rt]);
      return ExecutionStatus::kRunning;
    case 0x05:  // DMTC1
      if (!IsEvenFpRegister(fs)) return ExecutionStatus::kInvalidOpcode;
      SetFpDoubleBits(fs, static_cast<uint64_t>(gpr_[rt]));
      return ExecutionStatus::kRunning;
    case 0x06: {  // CTC1
      uint32_t ctrl_reg = static_cast<uint32_t>(fs);
      if (ctrl_reg == 31) {
        fcr31_ = gpr_[rt];
        fpu_cond_ = (gpr_[rt] & (1u << 23)) != 0;
      }
      return ExecutionStatus::kRunning;
    }
    case 0x08: {  // BC1
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      uint32_t branch_target = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);
      switch (rt) {
        case 0x00:    // BC1F
        case 0x01: {  // BC1T
          bool take = (rt == 0x01) ? fpu_cond_ : !fpu_cond_;
          SetupBranch(take, branch_target, current_pc);
          return ExecutionStatus::kRunning;
        }
        case 0x02:    // BC1FL
        case 0x03: {  // BC1TL
          bool take = (rt == 0x03) ? fpu_cond_ : !fpu_cond_;
          SetupBranchLikely(take, branch_target, current_pc, next_pc);
          return ExecutionStatus::kRunning;
        }
        default:
          return ExecutionStatus::kInvalidOpcode;
      }
    }
    case 0x10:  // Single-precision float
      return StepCop1Single(raw_word, fs, ft, fd);
    case 0x11:  // Double-precision float
      return StepCop1Double(raw_word, fs, ft, fd);
    case 0x14:  // Word to Float
      switch (funct) {
        case 0x20:  // CVT.S.W
          SetFpRegister(fd, static_cast<float>(static_cast<int32_t>(GetFpBits(fs))));
          return ExecutionStatus::kRunning;
        case 0x21:  // CVT.D.W
          if (!IsEvenFpRegister(fd)) {
            return ExecutionStatus::kInvalidOpcode;
          }
          SetFpDouble(fd, static_cast<double>(static_cast<int32_t>(GetFpBits(fs))));
          return ExecutionStatus::kRunning;
        default:
          return ExecutionStatus::kInvalidOpcode;
      }
    default:
      return ExecutionStatus::kInvalidOpcode;
  }
}

ExecutionStatus MipsEmulator::StepLoadStore(uint32_t raw_word) {
  uint32_t major_op = raw_word >> 26;
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  int32_t imm_s = static_cast<int32_t>(static_cast<int16_t>(raw_word & 0xFFFF));
  uint32_t addr = gpr_[rs] + static_cast<uint32_t>(imm_s);

  switch (major_op) {
    case 0x20: {  // LB
      uint8_t val = 0;
      if (!Read8(addr, &val)) {
        last_invalid_info_ = absl::StrFormat("LB faulting addr=0x%08X", addr);
        return ExecutionStatus::kMemoryFault;
      }
      SetGpr(rt, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(val))));
      break;
    }
    case 0x21: {  // LH
      uint16_t val = 0;
      if ((addr & 1) != 0 || !Read16(addr, &val)) {
        last_invalid_info_ = absl::StrFormat("LH faulting addr=0x%08X", addr);
        return ExecutionStatus::kMemoryFault;
      }
      SetGpr(rt, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(val))));
      break;
    }
    case 0x22: {  // LWL
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) {
        last_invalid_info_ = absl::StrFormat("LWL faulting addr=0x%08X", word_addr);
        return ExecutionStatus::kMemoryFault;
      }
      uint32_t mask = 0xFFFFFFFFu << (shift_byte * 8);
      SetGpr(rt, (gpr_[rt] & ~mask) | (word << (shift_byte * 8)));
      break;
    }
    case 0x23:    // LW
    case 0x27: {  // LWU
      if ((addr & 3) != 0) {
        last_invalid_info_ = absl::StrFormat("LW unaligned addr=0x%08X", addr);
        return ExecutionStatus::kMemoryFault;
      }
      uint32_t val = 0;
      if (!Read32(addr, &val)) {
        last_invalid_info_ = absl::StrFormat("LW faulting addr=0x%08X", addr);
        return ExecutionStatus::kMemoryFault;
      }
      SetGpr(rt, val);
      break;
    }
    case 0x24: {  // LBU
      uint8_t val = 0;
      if (!Read8(addr, &val)) {
        last_invalid_info_ = absl::StrFormat("LBU faulting addr=0x%08X", addr);
        return ExecutionStatus::kMemoryFault;
      }
      SetGpr(rt, val);
      break;
    }
    case 0x25: {  // LHU
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      uint16_t val = 0;
      if (!Read16(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, val);
      break;
    }
    case 0x26: {  // LWR
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu >> ((3 - shift_byte) * 8);
      SetGpr(rt, (gpr_[rt] & ~mask) | (word >> ((3 - shift_byte) * 8)));
      break;
    }
    case 0x28: {  // SB
      if (!Write8(addr, static_cast<uint8_t>(gpr_[rt]), rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x29: {  // SH
      if ((addr & 1) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write16(addr, static_cast<uint16_t>(gpr_[rt]), rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x2A: {  // SWL
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu >> (shift_byte * 8);
      uint32_t new_word = (word & ~mask) | (gpr_[rt] >> (shift_byte * 8));
      if (!Write32(word_addr, new_word, rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x2B: {  // SW
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write32(addr, gpr_[rt], rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x2E: {  // SWR
      uint32_t shift_byte = addr & 3;
      uint32_t word_addr = addr & ~3;
      uint32_t word = 0;
      if (!Read32(word_addr, &word)) return ExecutionStatus::kMemoryFault;
      uint32_t mask = 0xFFFFFFFFu << ((3 - shift_byte) * 8);
      uint32_t new_word = (word & ~mask) | (gpr_[rt] << ((3 - shift_byte) * 8));
      if (!Write32(word_addr, new_word, rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x2F:  // CACHE
      break;
    case 0x31: {  // LWC1
      FpRegister ft = static_cast<FpRegister>(rt);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      uint32_t word = 0;
      if (!Read32(addr, &word, /*is_fp=*/true)) return ExecutionStatus::kMemoryFault;
      SetFpBits(ft, word);
      break;
    }
    case 0x35: {  // LDC1
      FpRegister ft = static_cast<FpRegister>(rt);
      if (!IsEvenFpRegister(ft)) return ExecutionStatus::kInvalidOpcode;
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      uint64_t val = 0;
      if (!Read64(addr, &val, /*is_fp=*/true)) return ExecutionStatus::kMemoryFault;
      SetFpDoubleBits(ft, val);
      break;
    }
    case 0x37: {  // LD
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      uint64_t val = 0;
      if (!Read64(addr, &val)) return ExecutionStatus::kMemoryFault;
      SetGpr(rt, static_cast<uint32_t>(val));
      break;
    }
    case 0x39: {  // SWC1
      FpRegister ft = static_cast<FpRegister>(rt);
      if ((addr & 3) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write32(addr, GetFpBits(ft), rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x3D: {  // SDC1
      FpRegister ft = static_cast<FpRegister>(rt);
      if (!IsEvenFpRegister(ft)) return ExecutionStatus::kInvalidOpcode;
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      if (!Write64(addr, GetFpDoubleBits(ft), rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    case 0x3F: {  // SD
      if ((addr & 7) != 0) return ExecutionStatus::kMemoryFault;
      uint64_t val = static_cast<uint64_t>(gpr_[rt]);
      if (!Write64(addr, val, rs)) return ExecutionStatus::kMemoryFault;
      break;
    }
    default:
      return ExecutionStatus::kInvalidOpcode;
  }
  return ExecutionStatus::kRunning;
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
  bool delayed_is_call = delay_slot_is_call_;
  int delayed_call_link = call_link_reg_;
  std::optional<uint32_t> next_branch_target = delayed_branch_target_;

  uint32_t advanced_pc = current_pc + 4;
  uint32_t major_op = raw_word >> 26;
  int rs = static_cast<int>((raw_word >> 21) & 0x1F);
  int rt = static_cast<int>((raw_word >> 16) & 0x1F);
  uint32_t imm_u = raw_word & 0xFFFF;
  int32_t imm_s = static_cast<int32_t>(static_cast<int16_t>(raw_word & 0xFFFF));
  uint32_t target = raw_word & 0x03FFFFFF;
  uint32_t branch_target = current_pc + 4 + (static_cast<uint32_t>(imm_s) << 2);

  ExecutionStatus status = ExecutionStatus::kRunning;
  switch (major_op) {
    case 0x00:  // SPECIAL
      status = StepSpecial(raw_word, current_pc, executing_delay_slot);
      break;
    case 0x01:  // REGIMM
      status = StepRegimm(raw_word, current_pc, executing_delay_slot, &advanced_pc);
      break;
    case 0x02: {  // J
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      in_delay_slot_ = true;
      uint32_t j_target = (current_pc & 0xF0000000) | (target << 2);
      if (IsExternalCallTarget(j_target)) {
        delay_slot_is_call_ = true;
        call_link_reg_ = 31;
      } else {
        delay_slot_is_call_ = false;
      }
      delayed_branch_target_ = j_target;
      break;
    }
    case 0x03:  // JAL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetGpr(31, current_pc + 8);
      in_delay_slot_ = true;
      delay_slot_is_call_ = true;
      call_link_reg_ = 31;
      delayed_branch_target_ = (current_pc & 0xF0000000) | (target << 2);
      break;
    case 0x04:  // BEQ
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranch(gpr_[rs] == gpr_[rt], branch_target, current_pc);
      break;
    case 0x05:  // BNE
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranch(gpr_[rs] != gpr_[rt], branch_target, current_pc);
      break;
    case 0x06:  // BLEZ
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranch(static_cast<int32_t>(gpr_[rs]) <= 0, branch_target, current_pc);
      break;
    case 0x07:  // BGTZ
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranch(static_cast<int32_t>(gpr_[rs]) > 0, branch_target, current_pc);
      break;
    case 0x08:    // ADDI
    case 0x18: {  // DADDI
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
    case 0x19:  // DADDIU
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
    case 0x10:  // COP0
      status = StepCop0(raw_word, executing_delay_slot);
      break;
    case 0x11:  // COP1
      status = StepCop1(raw_word, current_pc, executing_delay_slot, &advanced_pc);
      break;
    case 0x14:  // BEQL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranchLikely(gpr_[rs] == gpr_[rt], branch_target, current_pc, &advanced_pc);
      break;
    case 0x15:  // BNEL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranchLikely(gpr_[rs] != gpr_[rt], branch_target, current_pc, &advanced_pc);
      break;
    case 0x16:  // BLEZL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranchLikely(static_cast<int32_t>(gpr_[rs]) <= 0, branch_target, current_pc,
                        &advanced_pc);
      break;
    case 0x17:  // BGTZL
      if (executing_delay_slot) return ExecutionStatus::kInvalidOpcode;
      SetupBranchLikely(static_cast<int32_t>(gpr_[rs]) > 0, branch_target, current_pc,
                        &advanced_pc);
      break;
    case 0x20:  // LB
    case 0x21:  // LH
    case 0x22:  // LWL
    case 0x23:  // LW
    case 0x24:  // LBU
    case 0x25:  // LHU
    case 0x26:  // LWR
    case 0x27:  // LWU
    case 0x28:  // SB
    case 0x29:  // SH
    case 0x2A:  // SWL
    case 0x2B:  // SW
    case 0x2E:  // SWR
    case 0x2F:  // CACHE
    case 0x31:  // LWC1
    case 0x35:  // LDC1
    case 0x37:  // LD
    case 0x39:  // SWC1
    case 0x3D:  // SDC1
    case 0x3F:  // SD
      status = StepLoadStore(raw_word);
      break;
    default:
      return RecordInvalidOpcode(raw_word, executing_delay_slot, "unhandled major_op");
  }

  if (status != ExecutionStatus::kRunning) {
    return status;
  }

  // Handle PC advancement and delay slot completion
  if (executing_delay_slot) {
    in_delay_slot_ = false;
    delayed_branch_target_.reset();
    delay_slot_is_call_ = false;
    if (delayed_is_return) {
      delay_slot_is_return_ = false;
      return ExecutionStatus::kHaltedReturn;
    }
    if (delayed_is_call && next_branch_target.has_value()) {
      auto call_status = HandleCallTarget(*next_branch_target, delayed_call_link);
      if (call_status.has_value()) {
        return *call_status;
      }
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
  delay_slot_is_call_ = false;
  call_link_reg_ = 31;
  call_log_.clear();
  last_invalid_info_.clear();
  written_phys_addrs_.clear();

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
      result.call_log = call_log_;
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
      result.call_log = call_log_;
      result.error_message = absl::StrFormat("Execution halted with status %d at PC 0x%08X [%s]",
                                             static_cast<int>(status), pc_, last_invalid_info_);
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
  result.call_log = call_log_;
  result.error_message = absl::StrFormat("Exceeded maximum step limit of %llu", max_steps);
  return result;
}

}  // namespace rom_nom_nom::fuzzer
