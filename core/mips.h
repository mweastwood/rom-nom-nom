#ifndef CORE_MIPS_H_
#define CORE_MIPS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

// MIPS General Purpose Registers (GPRs) according to O32 ABI.
enum class Register : uint8_t {
  kZero = 0,  // $0: Hardware zero
  kAt,        // $1: Assembler temporary
  kV0,
  kV1,  // $2-$3: Expression evaluation & return values
  kA0,
  kA1,
  kA2,
  kA3,  // $4-$7: Function arguments
  kT0,
  kT1,
  kT2,
  kT3,
  kT4,
  kT5,
  kT6,
  kT7,  // $8-$15: Temporaries
  kS0,
  kS1,
  kS2,
  kS3,
  kS4,
  kS5,
  kS6,
  kS7,  // $16-$23: Callee-saved registers
  kT8,
  kT9,  // $24-$25: Temporaries
  kK0,
  kK1,  // $26-$27: Reserved for OS kernel
  kGp,  // $28: Global pointer
  kSp,  // $29: Stack pointer
  kFp,  // $30: Frame pointer / $s8
  kRa,  // $31: Return address
};

// Returns standard ABI register name (e.g. "$a0", "$sp", "$zero").
std::string_view RegisterName(Register reg);

// MIPS Floating Point Registers (FPRs) for COP1.
enum class FpRegister : uint8_t {
  kF0,
  kF1,
  kF2,
  kF3,
  kF4,
  kF5,
  kF6,
  kF7,
  kF8,
  kF9,
  kF10,
  kF11,
  kF12,
  kF13,
  kF14,
  kF15,
  kF16,
  kF17,
  kF18,
  kF19,
  kF20,
  kF21,
  kF22,
  kF23,
  kF24,
  kF25,
  kF26,
  kF27,
  kF28,
  kF29,
  kF30,
  kF31,
};

// Returns standard FPU register name (e.g. "$f0", "$f12").
std::string_view FpRegisterName(FpRegister reg);

// MIPS III (VR4300) Instruction Opcode enumeration.
enum class Opcode : uint16_t {
  kUnknown = 0,

  // Arithmetic & Logic (Register)
  kAdd,
  kAddu,
  kSub,
  kSubu,
  kAnd,
  kOr,
  kXor,
  kNor,
  kSll,
  kSrl,
  kSra,
  kSllv,
  kSrlv,
  kSrav,
  kSlt,
  kSltu,

  // Arithmetic & Logic (Immediate)
  kAddi,
  kAddiu,
  kAndi,
  kOri,
  kXori,
  kLui,
  kSlti,
  kSltiu,

  // Multiply & Divide
  kMult,
  kMultu,
  kDiv,
  kDivu,
  kMfhi,
  kMthi,
  kMflo,
  kMtlo,

  // Memory Loads & Stores
  kLb,
  kLbu,
  kLh,
  kLhu,
  kLw,
  kLwl,
  kLwr,
  kSb,
  kSh,
  kSw,
  kSwl,
  kSwr,

  // 64-bit MIPS III extensions (VR4300)
  kLd,
  kSd,
  kLld,
  kScd,
  kDadd,
  kDaddu,
  kDsub,
  kDsubu,
  kDsll,
  kDsrl,
  kDsra,
  kDsll32,
  kDsrl32,
  kDsra32,

  // Control Flow: Jumps
  kJ,
  kJal,
  kJr,
  kJalr,

  // Control Flow: Conditional Branches
  kBeq,
  kBne,
  kBlez,
  kBgtz,
  kBltz,
  kBgez,
  kBltzal,
  kBgezal,

  // Control Flow: Branch Likely (MIPS II/III)
  kBeql,
  kBnel,
  kBlezl,
  kBgtzl,
  kBltzl,
  kBgezl,

  // Floating Point (COP1) Instructions
  kLwc1,
  kSwc1,
  kLdc1,
  kSdc1,
  kMfc1,
  kMtc1,
  kDmfc1,
  kDmtc1,
  kAddS,
  kSubS,
  kMulS,
  kDivS,
  kSqrtS,
  kAddD,
  kSubD,
  kMulD,
  kDivD,
  kSqrtD,
  kCEqS,
  kCLtS,
  kCLeS,
  kCEqD,
  kCLtD,
  kCLeD,
  kBc1t,
  kBc1f,
  kBc1tl,
  kBc1fl,

  // System & Coprocessor
  kSyscall,
  kBreak,
  kSync,
};

// Represents a fully decoded 32-bit MIPS instruction.
struct Instruction {
  uint32_t raw_word = 0;  // Original 32-bit instruction
  uint32_t vram = 0;      // Virtual address in RDRAM

  Opcode opcode = Opcode::kUnknown;

  // General-purpose register operands
  std::optional<Register> rd;
  std::optional<Register> rs;
  std::optional<Register> rt;

  // Floating-point register operands (COP1)
  std::optional<FpRegister> fd;
  std::optional<FpRegister> fs;
  std::optional<FpRegister> ft;

  // Immediates and targets
  int16_t immediate = 0;     // 16-bit signed immediate (I-type)
  uint8_t shift_amount = 0;  // 5-bit sa field for shifts
  uint32_t target = 0;       // 26-bit target for J/JAL

  // --- Classification Queries ---
  bool IsBranch() const;        // beq, bne, blez, etc.
  bool IsBranchLikely() const;  // beql, bnel, etc.
  bool IsJump() const;          // j, jal, jr, jalr
  bool IsCall() const;          // jal, jalr, bltzal, bgezal (calls function)
  bool IsReturn() const;        // jr $ra
  bool HasDelaySlot() const;    // All standard branches and jumps

  bool IsLoad() const;   // lw, lh, lb, lwc1, etc.
  bool IsStore() const;  // sw, sh, sb, swc1, etc.
  bool IsNop() const;    // sll $zero, $zero, 0

  // --- Target Address Calculators ---
  // Computes PC-relative target: vram + 4 + (immediate << 2)
  uint32_t BranchTarget() const;

  // Computes J-type jump target: (vram & 0xF0000000) | (target << 2)
  uint32_t JumpTarget() const;

  // Emits GNU assembly text without symbol resolution (e.g. "addu $t0, $a0, $a1")
  std::string Disassemble() const;
};

// Decodes a 32-bit big-endian instruction word located at the given VRAM address.
absl::StatusOr<Instruction> DecodeInstruction(uint32_t word, uint32_t vram = 0);

// Convenience overload: Decodes from a 4-byte buffer (reads big-endian).
absl::StatusOr<Instruction> DecodeInstruction(absl::Span<const uint8_t> bytes, uint32_t vram = 0);

}  // namespace rom_nom_nom

#endif  // CORE_MIPS_H_
