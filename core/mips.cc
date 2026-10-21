#include "core/mips.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/endian.h"

namespace rom_nom_nom {

namespace {

constexpr std::array<std::string_view, 32> kGprNames = {
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3", "$t0", "$t1", "$t2",
    "$t3",   "$t4", "$t5", "$t6", "$t7", "$s0", "$s1", "$s2", "$s3", "$s4", "$s5",
    "$s6",   "$s7", "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
};

constexpr std::array<std::string_view, 32> kFprNames = {
    "$f0",  "$f1",  "$f2",  "$f3",  "$f4",  "$f5",  "$f6",  "$f7",  "$f8",  "$f9",  "$f10",
    "$f11", "$f12", "$f13", "$f14", "$f15", "$f16", "$f17", "$f18", "$f19", "$f20", "$f21",
    "$f22", "$f23", "$f24", "$f25", "$f26", "$f27", "$f28", "$f29", "$f30", "$f31",
};

Register ToRegister(uint32_t val) {
  return static_cast<Register>(val & 0x1F);
}

FpRegister ToFpRegister(uint32_t val) {
  return static_cast<FpRegister>(val & 0x1F);
}

Opcode DecodeCop1FloatOp(uint32_t funct, bool is_double) {
  switch (funct) {
    case 0x00:
      return is_double ? Opcode::kAddD : Opcode::kAddS;
    case 0x01:
      return is_double ? Opcode::kSubD : Opcode::kSubS;
    case 0x02:
      return is_double ? Opcode::kMulD : Opcode::kMulS;
    case 0x03:
      return is_double ? Opcode::kDivD : Opcode::kDivS;
    case 0x04:
      return is_double ? Opcode::kSqrtD : Opcode::kSqrtS;
    case 0x05:
      return is_double ? Opcode::kAbsD : Opcode::kAbsS;
    case 0x06:
      return is_double ? Opcode::kMovD : Opcode::kMovS;
    case 0x07:
      return is_double ? Opcode::kNegD : Opcode::kNegS;
    case 0x0C:
      return is_double ? Opcode::kRoundWD : Opcode::kRoundWS;
    case 0x0D:
      return is_double ? Opcode::kTruncWD : Opcode::kTruncWS;
    case 0x0E:
      return is_double ? Opcode::kCeilWD : Opcode::kCeilWS;
    case 0x0F:
      return is_double ? Opcode::kFloorWD : Opcode::kFloorWS;
    case 0x20:
      return is_double ? Opcode::kCvtSD : Opcode::kUnknown;
    case 0x21:
      return is_double ? Opcode::kUnknown : Opcode::kCvtDS;
    case 0x24:
      return is_double ? Opcode::kCvtWD : Opcode::kCvtWS;
    case 0x25:
      return is_double ? Opcode::kCvtLD : Opcode::kCvtLS;
    case 0x30:
      return is_double ? Opcode::kCFD : Opcode::kCFS;
    case 0x31:
      return is_double ? Opcode::kCUnD : Opcode::kCUnS;
    case 0x32:
      return is_double ? Opcode::kCEqD : Opcode::kCEqS;
    case 0x33:
      return is_double ? Opcode::kCUeqD : Opcode::kCUeqS;
    case 0x34:
      return is_double ? Opcode::kCOltD : Opcode::kCOltS;
    case 0x35:
      return is_double ? Opcode::kCUltD : Opcode::kCUltS;
    case 0x36:
      return is_double ? Opcode::kCOleD : Opcode::kCOleS;
    case 0x37:
      return is_double ? Opcode::kCUleD : Opcode::kCUleS;
    case 0x38:
      return is_double ? Opcode::kCSfD : Opcode::kCSfS;
    case 0x39:
      return is_double ? Opcode::kCNgleD : Opcode::kCNgleS;
    case 0x3A:
      return is_double ? Opcode::kCSeqD : Opcode::kCSeqS;
    case 0x3B:
      return is_double ? Opcode::kCNglD : Opcode::kCNglS;
    case 0x3C:
      return is_double ? Opcode::kCLtD : Opcode::kCLtS;
    case 0x3D:
      return is_double ? Opcode::kCNgeD : Opcode::kCNgeS;
    case 0x3E:
      return is_double ? Opcode::kCLeD : Opcode::kCLeS;
    case 0x3F:
      return is_double ? Opcode::kCNgtD : Opcode::kCNgtS;
    default:
      return Opcode::kUnknown;
  }
}

}  // namespace

std::string_view RegisterName(Register reg) {
  const auto idx = static_cast<size_t>(reg);
  if (idx < kGprNames.size()) {
    return kGprNames[idx];
  }
  return "$unknown";
}

std::string_view FpRegisterName(FpRegister reg) {
  const auto idx = static_cast<size_t>(reg);
  if (idx < kFprNames.size()) {
    return kFprNames[idx];
  }
  return "$f_unknown";
}

bool Instruction::IsBranch() const {
  switch (opcode) {
    case Opcode::kBeq:
    case Opcode::kBne:
    case Opcode::kBlez:
    case Opcode::kBgtz:
    case Opcode::kBltz:
    case Opcode::kBgez:
    case Opcode::kBltzal:
    case Opcode::kBgezal:
    case Opcode::kBeql:
    case Opcode::kBnel:
    case Opcode::kBlezl:
    case Opcode::kBgtzl:
    case Opcode::kBltzl:
    case Opcode::kBgezl:
    case Opcode::kBltzall:
    case Opcode::kBgezall:
    case Opcode::kBc1t:
    case Opcode::kBc1f:
    case Opcode::kBc1tl:
    case Opcode::kBc1fl:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsBranchLikely() const {
  switch (opcode) {
    case Opcode::kBeql:
    case Opcode::kBnel:
    case Opcode::kBlezl:
    case Opcode::kBgtzl:
    case Opcode::kBltzl:
    case Opcode::kBgezl:
    case Opcode::kBltzall:
    case Opcode::kBgezall:
    case Opcode::kBc1tl:
    case Opcode::kBc1fl:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsUnconditionalBranch() const {
  if (opcode == Opcode::kBeq || opcode == Opcode::kBeql) {
    return rs.has_value() && rt.has_value() && *rs == *rt;
  }
  if (opcode == Opcode::kBgez || opcode == Opcode::kBgezl || opcode == Opcode::kBgezal ||
      opcode == Opcode::kBgezall) {
    return rs.has_value() && *rs == Register::kZero;
  }
  if (opcode == Opcode::kBlez || opcode == Opcode::kBlezl) {
    return rs.has_value() && *rs == Register::kZero;
  }
  return false;
}

bool Instruction::IsJump() const {
  switch (opcode) {
    case Opcode::kJ:
    case Opcode::kJal:
    case Opcode::kJr:
    case Opcode::kJalr:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsCall() const {
  switch (opcode) {
    case Opcode::kJal:
    case Opcode::kJalr:
    case Opcode::kBltzal:
    case Opcode::kBgezal:
    case Opcode::kBltzall:
    case Opcode::kBgezall:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsReturn() const {
  return opcode == Opcode::kJr && rs.has_value() && *rs == Register::kRa;
}

bool Instruction::HasDelaySlot() const {
  return IsBranch() || IsJump();
}

bool Instruction::IsLoad() const {
  switch (opcode) {
    case Opcode::kLb:
    case Opcode::kLbu:
    case Opcode::kLh:
    case Opcode::kLhu:
    case Opcode::kLw:
    case Opcode::kLwl:
    case Opcode::kLwr:
    case Opcode::kLwu:
    case Opcode::kLld:
    case Opcode::kLd:
    case Opcode::kLdl:
    case Opcode::kLdr:
    case Opcode::kLwc1:
    case Opcode::kLdc1:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsStore() const {
  switch (opcode) {
    case Opcode::kSb:
    case Opcode::kSh:
    case Opcode::kSw:
    case Opcode::kSwl:
    case Opcode::kSwr:
    case Opcode::kScd:
    case Opcode::kSd:
    case Opcode::kSdl:
    case Opcode::kSdr:
    case Opcode::kSwc1:
    case Opcode::kSdc1:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsNop() const {
  return raw_word == 0 || (opcode == Opcode::kSll && rd.has_value() && *rd == Register::kZero &&
                           rt.has_value() && *rt == Register::kZero && shift_amount == 0);
}

bool Instruction::IsDoublePrecision() const {
  switch (opcode) {
    case Opcode::kLdc1:
    case Opcode::kSdc1:
    case Opcode::kDmfc1:
    case Opcode::kDmtc1:
    case Opcode::kAddD:
    case Opcode::kSubD:
    case Opcode::kMulD:
    case Opcode::kDivD:
    case Opcode::kSqrtD:
    case Opcode::kAbsD:
    case Opcode::kMovD:
    case Opcode::kNegD:
    case Opcode::kCvtSD:
    case Opcode::kCvtDS:
    case Opcode::kCvtDW:
    case Opcode::kCvtWD:
    case Opcode::kCvtLD:
    case Opcode::kCvtDL:
    case Opcode::kTruncWD:
    case Opcode::kRoundWD:
    case Opcode::kCeilWD:
    case Opcode::kFloorWD:
    case Opcode::kCFD:
    case Opcode::kCUnD:
    case Opcode::kCEqD:
    case Opcode::kCUeqD:
    case Opcode::kCOltD:
    case Opcode::kCUltD:
    case Opcode::kCOleD:
    case Opcode::kCUleD:
    case Opcode::kCSfD:
    case Opcode::kCNgleD:
    case Opcode::kCSeqD:
    case Opcode::kCNglD:
    case Opcode::kCLtD:
    case Opcode::kCNgeD:
    case Opcode::kCLeD:
    case Opcode::kCNgtD:
      return true;
    default:
      return false;
  }
}

uint32_t Instruction::BranchTarget() const {
  return vram + 4 + (static_cast<int32_t>(immediate) << 2);
}

uint32_t Instruction::JumpTarget() const {
  return (vram & 0xF0000000) | (target << 2);
}

std::string Instruction::Disassemble() const {
  if (IsNop()) {
    return "nop";
  }

  const auto reg_str = [](std::optional<Register> r) -> std::string_view {
    return r.has_value() ? RegisterName(*r) : "$unknown";
  };

  const auto fp_reg_str = [](std::optional<FpRegister> r) -> std::string_view {
    return r.has_value() ? FpRegisterName(*r) : "$f_unknown";
  };

  switch (opcode) {
    // R-Type 3-register: op rd, rs, rt
    case Opcode::kAdd:
      return absl::StrFormat("add   %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kAddu:
      return absl::StrFormat("addu  %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kSub:
      return absl::StrFormat("sub   %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kSubu:
      return absl::StrFormat("subu  %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kAnd:
      return absl::StrFormat("and   %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kOr:
      return absl::StrFormat("or    %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kXor:
      return absl::StrFormat("xor   %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kNor:
      return absl::StrFormat("nor   %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kSlt:
      return absl::StrFormat("slt   %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kSltu:
      return absl::StrFormat("sltu  %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kDadd:
      return absl::StrFormat("dadd  %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kDaddu:
      return absl::StrFormat("daddu %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kDsub:
      return absl::StrFormat("dsub  %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));
    case Opcode::kDsubu:
      return absl::StrFormat("dsubu %s, %s, %s", reg_str(rd), reg_str(rs), reg_str(rt));

    // R-Type Shifts (Immediate): op rd, rt, sa
    case Opcode::kSll:
      return absl::StrFormat("sll   %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kSrl:
      return absl::StrFormat("srl   %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kSra:
      return absl::StrFormat("sra   %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kDsll:
      return absl::StrFormat("dsll  %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kDsrl:
      return absl::StrFormat("dsrl  %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kDsra:
      return absl::StrFormat("dsra  %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kDsll32:
      return absl::StrFormat("dsll32 %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kDsrl32:
      return absl::StrFormat("dsrl32 %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);
    case Opcode::kDsra32:
      return absl::StrFormat("dsra32 %s, %s, %d", reg_str(rd), reg_str(rt), shift_amount);

    // R-Type Shifts (Variable): op rd, rt, rs
    case Opcode::kSllv:
      return absl::StrFormat("sllv  %s, %s, %s", reg_str(rd), reg_str(rt), reg_str(rs));
    case Opcode::kSrlv:
      return absl::StrFormat("srlv  %s, %s, %s", reg_str(rd), reg_str(rt), reg_str(rs));
    case Opcode::kSrav:
      return absl::StrFormat("srav  %s, %s, %s", reg_str(rd), reg_str(rt), reg_str(rs));
    case Opcode::kDsllv:
      return absl::StrFormat("dsllv %s, %s, %s", reg_str(rd), reg_str(rt), reg_str(rs));
    case Opcode::kDsrlv:
      return absl::StrFormat("dsrlv %s, %s, %s", reg_str(rd), reg_str(rt), reg_str(rs));
    case Opcode::kDsrav:
      return absl::StrFormat("dsrav %s, %s, %s", reg_str(rd), reg_str(rt), reg_str(rs));

    // Multiplies / Divides: op rs, rt
    case Opcode::kMult:
      return absl::StrFormat("mult  %s, %s", reg_str(rs), reg_str(rt));
    case Opcode::kMultu:
      return absl::StrFormat("multu %s, %s", reg_str(rs), reg_str(rt));
    case Opcode::kDiv:
      return absl::StrFormat("div   $zero, %s, %s", reg_str(rs), reg_str(rt));
    case Opcode::kDivu:
      return absl::StrFormat("divu  $zero, %s, %s", reg_str(rs), reg_str(rt));
    case Opcode::kDmult:
      return absl::StrFormat("dmult %s, %s", reg_str(rs), reg_str(rt));
    case Opcode::kDmultu:
      return absl::StrFormat("dmultu %s, %s", reg_str(rs), reg_str(rt));
    case Opcode::kDdiv:
      return absl::StrFormat("ddiv  $zero, %s, %s", reg_str(rs), reg_str(rt));
    case Opcode::kDdivu:
      return absl::StrFormat("ddivu $zero, %s, %s", reg_str(rs), reg_str(rt));

    // HI/LO moves
    case Opcode::kMfhi:
      return absl::StrFormat("mfhi  %s", reg_str(rd));
    case Opcode::kMthi:
      return absl::StrFormat("mthi  %s", reg_str(rs));
    case Opcode::kMflo:
      return absl::StrFormat("mflo  %s", reg_str(rd));
    case Opcode::kMtlo:
      return absl::StrFormat("mtlo  %s", reg_str(rs));

    // Jumps (Register)
    case Opcode::kJr:
      return absl::StrFormat("jr    %s", reg_str(rs));
    case Opcode::kJalr:
      if (rd.has_value() && *rd == Register::kRa) {
        return absl::StrFormat("jalr  %s", reg_str(rs));
      }
      return absl::StrFormat("jalr  %s, %s", reg_str(rd), reg_str(rs));

    // Jumps (Immediate / Target)
    case Opcode::kJ:
      return absl::StrFormat("j     0x%08X", JumpTarget());
    case Opcode::kJal:
      return absl::StrFormat("jal   0x%08X", JumpTarget());

    // Branches: op rs, rt, label
    case Opcode::kBeq:
      return absl::StrFormat("beq   %s, %s, .L%08X", reg_str(rs), reg_str(rt), BranchTarget());
    case Opcode::kBne:
      return absl::StrFormat("bne   %s, %s, .L%08X", reg_str(rs), reg_str(rt), BranchTarget());
    case Opcode::kBeql:
      return absl::StrFormat("beql  %s, %s, .L%08X", reg_str(rs), reg_str(rt), BranchTarget());
    case Opcode::kBnel:
      return absl::StrFormat("bnel  %s, %s, .L%08X", reg_str(rs), reg_str(rt), BranchTarget());

    // Single-register branches: op rs, label
    case Opcode::kBlez:
      return absl::StrFormat("blez  %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBgtz:
      return absl::StrFormat("bgtz  %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBltz:
      return absl::StrFormat("bltz  %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBgez:
      return absl::StrFormat("bgez  %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBlezl:
      return absl::StrFormat("blezl %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBgtzl:
      return absl::StrFormat("bgtzl %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBltzl:
      return absl::StrFormat("bltzl %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBgezl:
      return absl::StrFormat("bgezl %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBltzal:
      return absl::StrFormat("bltzal %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBgezal:
      return absl::StrFormat("bgezal %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBltzall:
      return absl::StrFormat("bltzall %s, .L%08X", reg_str(rs), BranchTarget());
    case Opcode::kBgezall:
      return absl::StrFormat("bgezall %s, .L%08X", reg_str(rs), BranchTarget());

    // I-Type Arithmetic: op rt, rs, imm
    case Opcode::kAddi:
      return absl::StrFormat("addi  %s, %s, %d", reg_str(rt), reg_str(rs), immediate);
    case Opcode::kAddiu:
      return absl::StrFormat("addiu %s, %s, %d", reg_str(rt), reg_str(rs), immediate);
    case Opcode::kDaddi:
      return absl::StrFormat("daddi %s, %s, %d", reg_str(rt), reg_str(rs), immediate);
    case Opcode::kDaddiu:
      return absl::StrFormat("daddiu %s, %s, %d", reg_str(rt), reg_str(rs), immediate);
    case Opcode::kSlti:
      return absl::StrFormat("slti  %s, %s, %d", reg_str(rt), reg_str(rs), immediate);
    case Opcode::kSltiu:
      return absl::StrFormat("sltiu %s, %s, %d", reg_str(rt), reg_str(rs), immediate);
    case Opcode::kAndi:
      return absl::StrFormat("andi  %s, %s, 0x%X", reg_str(rt), reg_str(rs),
                             static_cast<uint16_t>(immediate));
    case Opcode::kOri:
      return absl::StrFormat("ori   %s, %s, 0x%X", reg_str(rt), reg_str(rs),
                             static_cast<uint16_t>(immediate));
    case Opcode::kXori:
      return absl::StrFormat("xori  %s, %s, 0x%X", reg_str(rt), reg_str(rs),
                             static_cast<uint16_t>(immediate));
    case Opcode::kLui:
      return absl::StrFormat("lui   %s, 0x%X", reg_str(rt), static_cast<uint16_t>(immediate));

    // Memory Loads: op rt, offset(rs)
    case Opcode::kLb:
      return absl::StrFormat("lb    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLbu:
      return absl::StrFormat("lbu   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLh:
      return absl::StrFormat("lh    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLhu:
      return absl::StrFormat("lhu   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLw:
      return absl::StrFormat("lw    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLwl:
      return absl::StrFormat("lwl   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLwr:
      return absl::StrFormat("lwr   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLwu:
      return absl::StrFormat("lwu   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLd:
      return absl::StrFormat("ld    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLld:
      return absl::StrFormat("lld   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLdl:
      return absl::StrFormat("ldl   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kLdr:
      return absl::StrFormat("ldr   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));

    // Memory Stores: op rt, offset(rs)
    case Opcode::kSb:
      return absl::StrFormat("sb    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kSh:
      return absl::StrFormat("sh    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kSw:
      return absl::StrFormat("sw    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kSwl:
      return absl::StrFormat("swl   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kSwr:
      return absl::StrFormat("swr   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kSd:
      return absl::StrFormat("sd    %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kSdl:
      return absl::StrFormat("sdl   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kSdr:
      return absl::StrFormat("sdr   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));
    case Opcode::kScd:
      return absl::StrFormat("scd   %s, %d(%s)", reg_str(rt), immediate, reg_str(rs));

    // FPU Memory
    case Opcode::kLwc1:
      return absl::StrFormat("lwc1  %s, %d(%s)", fp_reg_str(ft), immediate, reg_str(rs));
    case Opcode::kSwc1:
      return absl::StrFormat("swc1  %s, %d(%s)", fp_reg_str(ft), immediate, reg_str(rs));
    case Opcode::kLdc1:
      return absl::StrFormat("ldc1  %s, %d(%s)", fp_reg_str(ft), immediate, reg_str(rs));
    case Opcode::kSdc1:
      return absl::StrFormat("sdc1  %s, %d(%s)", fp_reg_str(ft), immediate, reg_str(rs));

    // FPU Moves
    case Opcode::kMfc1:
      return absl::StrFormat("mfc1  %s, %s", reg_str(rt), fp_reg_str(fs));
    case Opcode::kMtc1:
      return absl::StrFormat("mtc1  %s, %s", reg_str(rt), fp_reg_str(fs));
    case Opcode::kCfc1:
      return absl::StrFormat("cfc1  %s, $%d", reg_str(rt),
                             static_cast<int>(fs.value_or(FpRegister::kF0)));
    case Opcode::kCtc1:
      return absl::StrFormat("ctc1  %s, $%d", reg_str(rt),
                             static_cast<int>(fs.value_or(FpRegister::kF0)));
    case Opcode::kDmfc1:
      return absl::StrFormat("dmfc1 %s, %s", reg_str(rt), fp_reg_str(fs));
    case Opcode::kDmtc1:
      return absl::StrFormat("dmtc1 %s, %s", reg_str(rt), fp_reg_str(fs));

    // FPU Arithmetic
    case Opcode::kAddS:
      return absl::StrFormat("add.s %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kSubS:
      return absl::StrFormat("sub.s %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kMulS:
      return absl::StrFormat("mul.s %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kDivS:
      return absl::StrFormat("div.s %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kSqrtS:
      return absl::StrFormat("sqrt.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kAbsS:
      return absl::StrFormat("abs.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kMovS:
      return absl::StrFormat("mov.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kNegS:
      return absl::StrFormat("neg.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kAddD:
      return absl::StrFormat("add.d %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kSubD:
      return absl::StrFormat("sub.d %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kMulD:
      return absl::StrFormat("mul.d %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kDivD:
      return absl::StrFormat("div.d %s, %s, %s", fp_reg_str(fd), fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kSqrtD:
      return absl::StrFormat("sqrt.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kAbsD:
      return absl::StrFormat("abs.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kMovD:
      return absl::StrFormat("mov.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kNegD:
      return absl::StrFormat("neg.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtSD:
      return absl::StrFormat("cvt.s.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtDS:
      return absl::StrFormat("cvt.d.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtSW:
      return absl::StrFormat("cvt.s.w %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtDW:
      return absl::StrFormat("cvt.d.w %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtWS:
      return absl::StrFormat("cvt.w.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtWD:
      return absl::StrFormat("cvt.w.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtLS:
      return absl::StrFormat("cvt.l.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtLD:
      return absl::StrFormat("cvt.l.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtSL:
      return absl::StrFormat("cvt.s.l %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCvtDL:
      return absl::StrFormat("cvt.d.l %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kTruncWS:
      return absl::StrFormat("trunc.w.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kTruncWD:
      return absl::StrFormat("trunc.w.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kRoundWS:
      return absl::StrFormat("round.w.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kRoundWD:
      return absl::StrFormat("round.w.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCeilWS:
      return absl::StrFormat("ceil.w.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kCeilWD:
      return absl::StrFormat("ceil.w.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kFloorWS:
      return absl::StrFormat("floor.w.s %s, %s", fp_reg_str(fd), fp_reg_str(fs));
    case Opcode::kFloorWD:
      return absl::StrFormat("floor.w.d %s, %s", fp_reg_str(fd), fp_reg_str(fs));

    // FPU Comparisons
    case Opcode::kCFS:
      return absl::StrFormat("c.f.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUnS:
      return absl::StrFormat("c.un.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCEqS:
      return absl::StrFormat("c.eq.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUeqS:
      return absl::StrFormat("c.ueq.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCOltS:
      return absl::StrFormat("c.olt.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUltS:
      return absl::StrFormat("c.ult.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCOleS:
      return absl::StrFormat("c.ole.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUleS:
      return absl::StrFormat("c.ule.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCSfS:
      return absl::StrFormat("c.sf.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNgleS:
      return absl::StrFormat("c.ngle.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCSeqS:
      return absl::StrFormat("c.seq.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNglS:
      return absl::StrFormat("c.ngl.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCLtS:
      return absl::StrFormat("c.lt.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNgeS:
      return absl::StrFormat("c.nge.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCLeS:
      return absl::StrFormat("c.le.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNgtS:
      return absl::StrFormat("c.ngt.s %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCFD:
      return absl::StrFormat("c.f.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUnD:
      return absl::StrFormat("c.un.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCEqD:
      return absl::StrFormat("c.eq.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUeqD:
      return absl::StrFormat("c.ueq.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCOltD:
      return absl::StrFormat("c.olt.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUltD:
      return absl::StrFormat("c.ult.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCOleD:
      return absl::StrFormat("c.ole.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCUleD:
      return absl::StrFormat("c.ule.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCSfD:
      return absl::StrFormat("c.sf.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNgleD:
      return absl::StrFormat("c.ngle.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCSeqD:
      return absl::StrFormat("c.seq.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNglD:
      return absl::StrFormat("c.ngl.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCLtD:
      return absl::StrFormat("c.lt.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNgeD:
      return absl::StrFormat("c.nge.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCLeD:
      return absl::StrFormat("c.le.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));
    case Opcode::kCNgtD:
      return absl::StrFormat("c.ngt.d %s, %s", fp_reg_str(fs), fp_reg_str(ft));

    // FPU Branches
    case Opcode::kBc1t:
      return absl::StrFormat("bc1t  .L%08X", BranchTarget());
    case Opcode::kBc1f:
      return absl::StrFormat("bc1f  .L%08X", BranchTarget());
    case Opcode::kBc1tl:
      return absl::StrFormat("bc1tl .L%08X", BranchTarget());
    case Opcode::kBc1fl:
      return absl::StrFormat("bc1fl .L%08X", BranchTarget());

    // System
    case Opcode::kSyscall:
      return "syscall";
    case Opcode::kBreak: {
      uint32_t code = (raw_word >> 16) & 0x3FF;
      if (code != 0 && (raw_word & 0x0000FFC0) == 0) {
        return absl::StrFormat("break %d", code);
      }
      if ((raw_word & 0x03FFFFC0) == 0) {
        return "break";
      }
      return absl::StrFormat(".word 0x%08X", raw_word);
    }
    case Opcode::kSync:
      return "sync";
    case Opcode::kMfc0:
      return absl::StrFormat("mfc0  %s, $%d", reg_str(rt),
                             rd.has_value() ? static_cast<int>(*rd) : 0);
    case Opcode::kMtc0:
      return absl::StrFormat("mtc0  %s, $%d", reg_str(rt),
                             rd.has_value() ? static_cast<int>(*rd) : 0);

    default:
      return absl::StrFormat(".word 0x%08X", raw_word);
  }
}

absl::StatusOr<Instruction> DecodeInstruction(uint32_t word, uint32_t vram) {
  Instruction inst;
  inst.raw_word = word;
  inst.vram = vram;

  const uint32_t major_op = (word >> 26) & 0x3F;
  const uint32_t rs_val = (word >> 21) & 0x1F;
  const uint32_t rt_val = (word >> 16) & 0x1F;
  const uint32_t rd_val = (word >> 11) & 0x1F;
  const uint32_t sa_val = (word >> 6) & 0x1F;
  const uint32_t funct = word & 0x3F;

  inst.immediate = static_cast<int16_t>(word & 0xFFFF);
  inst.shift_amount = static_cast<uint8_t>(sa_val);
  inst.target = word & 0x03FFFFFF;

  // Major Opcode 0x00: SPECIAL (R-type)
  if (major_op == 0x00) {
    inst.rd = ToRegister(rd_val);
    inst.rs = ToRegister(rs_val);
    inst.rt = ToRegister(rt_val);

    switch (funct) {
      case 0x00:
        inst.opcode = Opcode::kSll;
        break;
      case 0x02:
        inst.opcode = Opcode::kSrl;
        break;
      case 0x03:
        inst.opcode = Opcode::kSra;
        break;
      case 0x04:
        inst.opcode = Opcode::kSllv;
        break;
      case 0x06:
        inst.opcode = Opcode::kSrlv;
        break;
      case 0x07:
        inst.opcode = Opcode::kSrav;
        break;
      case 0x08:
        inst.opcode = Opcode::kJr;
        break;
      case 0x09:
        inst.opcode = Opcode::kJalr;
        break;
      case 0x0C:
        inst.opcode = Opcode::kSyscall;
        break;
      case 0x0D:
        inst.opcode = Opcode::kBreak;
        break;
      case 0x0F:
        inst.opcode = Opcode::kSync;
        break;
      case 0x10:
        inst.opcode = Opcode::kMfhi;
        break;
      case 0x11:
        inst.opcode = Opcode::kMthi;
        break;
      case 0x12:
        inst.opcode = Opcode::kMflo;
        break;
      case 0x13:
        inst.opcode = Opcode::kMtlo;
        break;
      case 0x14:
        inst.opcode = Opcode::kDsllv;
        break;
      case 0x16:
        inst.opcode = Opcode::kDsrlv;
        break;
      case 0x17:
        inst.opcode = Opcode::kDsrav;
        break;
      case 0x18:
        inst.opcode = Opcode::kMult;
        break;
      case 0x19:
        inst.opcode = Opcode::kMultu;
        break;
      case 0x1A:
        inst.opcode = Opcode::kDiv;
        break;
      case 0x1B:
        inst.opcode = Opcode::kDivu;
        break;
      case 0x1C:
        inst.opcode = Opcode::kDmult;
        break;
      case 0x1D:
        inst.opcode = Opcode::kDmultu;
        break;
      case 0x1E:
        inst.opcode = Opcode::kDdiv;
        break;
      case 0x1F:
        inst.opcode = Opcode::kDdivu;
        break;
      case 0x20:
        inst.opcode = Opcode::kAdd;
        break;
      case 0x21:
        inst.opcode = Opcode::kAddu;
        break;
      case 0x22:
        inst.opcode = Opcode::kSub;
        break;
      case 0x23:
        inst.opcode = Opcode::kSubu;
        break;
      case 0x24:
        inst.opcode = Opcode::kAnd;
        break;
      case 0x25:
        inst.opcode = Opcode::kOr;
        break;
      case 0x26:
        inst.opcode = Opcode::kXor;
        break;
      case 0x27:
        inst.opcode = Opcode::kNor;
        break;
      case 0x2A:
        inst.opcode = Opcode::kSlt;
        break;
      case 0x2B:
        inst.opcode = Opcode::kSltu;
        break;
      case 0x2C:
        inst.opcode = Opcode::kDadd;
        break;
      case 0x2D:
        inst.opcode = Opcode::kDaddu;
        break;
      case 0x2E:
        inst.opcode = Opcode::kDsub;
        break;
      case 0x2F:
        inst.opcode = Opcode::kDsubu;
        break;
      case 0x38:
        inst.opcode = Opcode::kDsll;
        break;
      case 0x3A:
        inst.opcode = Opcode::kDsrl;
        break;
      case 0x3B:
        inst.opcode = Opcode::kDsra;
        break;
      case 0x3C:
        inst.opcode = Opcode::kDsll32;
        break;
      case 0x3E:
        inst.opcode = Opcode::kDsrl32;
        break;
      case 0x3F:
        inst.opcode = Opcode::kDsra32;
        break;
      default:
        inst.opcode = Opcode::kUnknown;
        break;
    }
    return inst;
  }

  // Major Opcode 0x01: REGIMM (Branches against zero)
  if (major_op == 0x01) {
    inst.rs = ToRegister(rs_val);
    switch (rt_val) {
      case 0x00:
        inst.opcode = Opcode::kBltz;
        break;
      case 0x01:
        inst.opcode = Opcode::kBgez;
        break;
      case 0x02:
        inst.opcode = Opcode::kBltzl;
        break;
      case 0x03:
        inst.opcode = Opcode::kBgezl;
        break;
      case 0x10:
        inst.opcode = Opcode::kBltzal;
        break;
      case 0x11:
        inst.opcode = Opcode::kBgezal;
        break;
      case 0x12:
        inst.opcode = Opcode::kBltzall;
        break;
      case 0x13:
        inst.opcode = Opcode::kBgezall;
        break;
      default:
        inst.opcode = Opcode::kUnknown;
        break;
    }
    return inst;
  }

  // Major Opcode 0x10: COP0
  if (major_op == 0x10) {
    if (rs_val == 0x00) {
      inst.opcode = Opcode::kMfc0;
      inst.rt = ToRegister(rt_val);
      inst.rd = ToRegister(rd_val);
      return inst;
    }
    if (rs_val == 0x04) {
      inst.opcode = Opcode::kMtc0;
      inst.rt = ToRegister(rt_val);
      inst.rd = ToRegister(rd_val);
      return inst;
    }
  }

  // Major Opcode 0x11: COP1 (Floating Point)
  if (major_op == 0x11) {
    inst.ft = ToFpRegister(rt_val);
    inst.fs = ToFpRegister(rd_val);
    inst.fd = ToFpRegister(sa_val);

    // Moves between CPU and FPU
    if (rs_val == 0x00) {
      inst.opcode = Opcode::kMfc1;
      inst.rt = ToRegister(rt_val);
      inst.fs = ToFpRegister(rd_val);
      return inst;
    }
    if (rs_val == 0x01) {
      inst.opcode = Opcode::kDmfc1;
      inst.rt = ToRegister(rt_val);
      inst.fs = ToFpRegister(rd_val);
      return inst;
    }
    if (rs_val == 0x02) {
      inst.opcode = Opcode::kCfc1;
      inst.rt = ToRegister(rt_val);
      inst.fs = ToFpRegister(rd_val);
      return inst;
    }
    if (rs_val == 0x04) {
      inst.opcode = Opcode::kMtc1;
      inst.rt = ToRegister(rt_val);
      inst.fs = ToFpRegister(rd_val);
      return inst;
    }
    if (rs_val == 0x05) {
      inst.opcode = Opcode::kDmtc1;
      inst.rt = ToRegister(rt_val);
      inst.fs = ToFpRegister(rd_val);
      return inst;
    }
    if (rs_val == 0x06) {
      inst.opcode = Opcode::kCtc1;
      inst.rt = ToRegister(rt_val);
      inst.fs = ToFpRegister(rd_val);
      return inst;
    }

    // Branch on FPU Condition: bc1f, bc1t, bc1fl, bc1tl
    if (rs_val == 0x08) {
      switch (rt_val) {
        case 0x00:
          inst.opcode = Opcode::kBc1f;
          break;
        case 0x01:
          inst.opcode = Opcode::kBc1t;
          break;
        case 0x02:
          inst.opcode = Opcode::kBc1fl;
          break;
        case 0x03:
          inst.opcode = Opcode::kBc1tl;
          break;
        default:
          inst.opcode = Opcode::kUnknown;
          break;
      }
      return inst;
    }

    // Single/Double Precision Float Ops: rs == 0x10 (S) or rs == 0x11 (D)
    if (rs_val == 0x10 || rs_val == 0x11) {
      inst.opcode = DecodeCop1FloatOp(funct, /*is_double=*/rs_val == 0x11);
      return inst;
    }

    // Word to Float Ops: rs == 0x14
    if (rs_val == 0x14) {
      switch (funct) {
        case 0x20:
          inst.opcode = Opcode::kCvtSW;
          break;
        case 0x21:
          inst.opcode = Opcode::kCvtDW;
          break;
        default:
          inst.opcode = Opcode::kUnknown;
          break;
      }
      return inst;
    }

    // Longword to Float Ops: rs == 0x15
    if (rs_val == 0x15) {
      switch (funct) {
        case 0x20:
          inst.opcode = Opcode::kCvtSL;
          break;
        case 0x21:
          inst.opcode = Opcode::kCvtDL;
          break;
        default:
          inst.opcode = Opcode::kUnknown;
          break;
      }
      return inst;
    }

    inst.opcode = Opcode::kUnknown;
    return inst;
  }

  // J-Type / Immediate Opcodes
  inst.rs = ToRegister(rs_val);
  inst.rt = ToRegister(rt_val);

  switch (major_op) {
    case 0x02:
      inst.opcode = Opcode::kJ;
      break;
    case 0x03:
      inst.opcode = Opcode::kJal;
      break;
    case 0x04:
      inst.opcode = Opcode::kBeq;
      break;
    case 0x05:
      inst.opcode = Opcode::kBne;
      break;
    case 0x06:
      inst.opcode = Opcode::kBlez;
      break;
    case 0x07:
      inst.opcode = Opcode::kBgtz;
      break;
    case 0x08:
      inst.opcode = Opcode::kAddi;
      break;
    case 0x09:
      inst.opcode = Opcode::kAddiu;
      break;
    case 0x0A:
      inst.opcode = Opcode::kSlti;
      break;
    case 0x0B:
      inst.opcode = Opcode::kSltiu;
      break;
    case 0x0C:
      inst.opcode = Opcode::kAndi;
      break;
    case 0x0D:
      inst.opcode = Opcode::kOri;
      break;
    case 0x0E:
      inst.opcode = Opcode::kXori;
      break;
    case 0x0F:
      inst.opcode = Opcode::kLui;
      break;

    case 0x14:
      inst.opcode = Opcode::kBeql;
      break;
    case 0x15:
      inst.opcode = Opcode::kBnel;
      break;
    case 0x16:
      inst.opcode = Opcode::kBlezl;
      break;
    case 0x17:
      inst.opcode = Opcode::kBgtzl;
      break;
    case 0x18:
      inst.opcode = Opcode::kDaddi;
      break;
    case 0x19:
      inst.opcode = Opcode::kDaddiu;
      break;
    case 0x1A:
      inst.opcode = Opcode::kLdl;
      break;
    case 0x1B:
      inst.opcode = Opcode::kLdr;
      break;

    case 0x20:
      inst.opcode = Opcode::kLb;
      break;
    case 0x21:
      inst.opcode = Opcode::kLh;
      break;
    case 0x22:
      inst.opcode = Opcode::kLwl;
      break;
    case 0x23:
      inst.opcode = Opcode::kLw;
      break;
    case 0x24:
      inst.opcode = Opcode::kLbu;
      break;
    case 0x25:
      inst.opcode = Opcode::kLhu;
      break;
    case 0x26:
      inst.opcode = Opcode::kLwr;
      break;
    case 0x27:
      inst.opcode = Opcode::kLwu;
      break;

    case 0x28:
      inst.opcode = Opcode::kSb;
      break;
    case 0x29:
      inst.opcode = Opcode::kSh;
      break;
    case 0x2A:
      inst.opcode = Opcode::kSwl;
      break;
    case 0x2B:
      inst.opcode = Opcode::kSw;
      break;
    case 0x2C:
      inst.opcode = Opcode::kSdl;
      break;
    case 0x2D:
      inst.opcode = Opcode::kSdr;
      break;
    case 0x2E:
      inst.opcode = Opcode::kSwr;
      break;

    case 0x31:
      inst.opcode = Opcode::kLwc1;
      inst.ft = ToFpRegister(rt_val);
      inst.rt.reset();
      break;
    case 0x34:
      inst.opcode = Opcode::kLld;
      break;
    case 0x35:
      inst.opcode = Opcode::kLdc1;
      inst.ft = ToFpRegister(rt_val);
      inst.rt.reset();
      break;
    case 0x37:
      inst.opcode = Opcode::kLd;
      break;

    case 0x39:
      inst.opcode = Opcode::kSwc1;
      inst.ft = ToFpRegister(rt_val);
      inst.rt.reset();
      break;
    case 0x3C:
      inst.opcode = Opcode::kScd;
      break;
    case 0x3D:
      inst.opcode = Opcode::kSdc1;
      inst.ft = ToFpRegister(rt_val);
      inst.rt.reset();
      break;
    case 0x3F:
      inst.opcode = Opcode::kSd;
      break;

    default:
      inst.opcode = Opcode::kUnknown;
      break;
  }

  return inst;
}

absl::StatusOr<Instruction> DecodeInstruction(absl::Span<const uint8_t> bytes, uint32_t vram) {
  if (bytes.size() < 4) {
    return absl::InvalidArgumentError(
        "Buffer size must be at least 4 bytes to decode a MIPS instruction");
  }

  const uint32_t word = ReadBigEndian32(bytes);

  return DecodeInstruction(word, vram);
}

absl::StatusOr<std::vector<Instruction>> DecodeSequence(absl::Span<const uint32_t> words,
                                                        uint32_t base_vram) {
  std::vector<Instruction> instructions;
  instructions.reserve(words.size());
  for (size_t i = 0; i < words.size(); ++i) {
    auto inst_or = DecodeInstruction(words[i], base_vram + static_cast<uint32_t>(i * 4));
    if (!inst_or.ok()) {
      return inst_or.status();
    }
    instructions.push_back(std::move(*inst_or));
  }
  return instructions;
}

std::string FormatBranch(const Instruction& inst, std::string_view target_label) {
  Register rs_reg = inst.rs.value_or(Register::kZero);
  Register rt_reg = inst.rt.value_or(Register::kZero);
  std::string_view rs = RegisterName(rs_reg);
  std::string_view rt = RegisterName(rt_reg);

  switch (inst.opcode) {
    case Opcode::kBeq:
      if (rs_reg == Register::kZero && rt_reg == Register::kZero) {
        return absl::StrFormat("b      %s", target_label);
      }
      if (rt_reg == Register::kZero) {
        return absl::StrFormat("beqz   %s, %s", rs, target_label);
      }
      return absl::StrFormat("beq    %s, %s, %s", rs, rt, target_label);

    case Opcode::kBne:
      if (rt_reg == Register::kZero) {
        return absl::StrFormat("bnez   %s, %s", rs, target_label);
      }
      return absl::StrFormat("bne    %s, %s, %s", rs, rt, target_label);

    case Opcode::kBlez:
      return absl::StrFormat("blez   %s, %s", rs, target_label);
    case Opcode::kBgtz:
      return absl::StrFormat("bgtz   %s, %s", rs, target_label);
    case Opcode::kBltz:
      return absl::StrFormat("bltz   %s, %s", rs, target_label);
    case Opcode::kBgez:
      return absl::StrFormat("bgez   %s, %s", rs, target_label);
    case Opcode::kBltzal:
      return absl::StrFormat("bltzal %s, %s", rs, target_label);
    case Opcode::kBgezal:
      return absl::StrFormat("bgezal %s, %s", rs, target_label);

    case Opcode::kBeql:
      return absl::StrFormat("beql   %s, %s, %s", rs, rt, target_label);
    case Opcode::kBnel:
      return absl::StrFormat("bnel   %s, %s, %s", rs, rt, target_label);
    case Opcode::kBlezl:
      return absl::StrFormat("blezl  %s, %s", rs, target_label);
    case Opcode::kBgtzl:
      return absl::StrFormat("bgtzl  %s, %s", rs, target_label);
    case Opcode::kBltzl:
      return absl::StrFormat("bltzl  %s, %s", rs, target_label);
    case Opcode::kBgezl:
      return absl::StrFormat("bgezl  %s, %s", rs, target_label);
    case Opcode::kBltzall:
      return absl::StrFormat("bltzall %s, %s", rs, target_label);
    case Opcode::kBgezall:
      return absl::StrFormat("bgezall %s, %s", rs, target_label);

    case Opcode::kBc1f:
      return absl::StrFormat("bc1f   %s", target_label);
    case Opcode::kBc1t:
      return absl::StrFormat("bc1t   %s", target_label);
    case Opcode::kBc1fl:
      return absl::StrFormat("bc1fl  %s", target_label);
    case Opcode::kBc1tl:
      return absl::StrFormat("bc1tl  %s", target_label);

    default:
      return inst.Disassemble();
  }
}

}  // namespace rom_nom_nom
