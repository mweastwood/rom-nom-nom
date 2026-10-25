#ifndef CORE_MIPS_H_
#define CORE_MIPS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// --- MIPS 32-Bit Memory Architecture ---

// Base and boundary definitions for MIPS 32-bit virtual memory segments.
constexpr uint32_t kKusegBase = 0x00000000;
constexpr uint32_t kKusegEnd = 0x7FFFFFFF;
constexpr uint32_t kKseg0Base = 0x80000000;
constexpr uint32_t kKseg0End = 0x9FFFFFFF;
constexpr uint32_t kKseg1Base = 0xA0000000;
constexpr uint32_t kKseg1End = 0xBFFFFFFF;
constexpr uint32_t kKssegBase = 0xC0000000;
constexpr uint32_t kKssegEnd = 0xDFFFFFFF;
constexpr uint32_t kKseg3Base = 0xE0000000;
constexpr uint32_t kKseg3End = 0xFFFFFFFF;

// Returns true if the address falls within direct-mapped physical address space
// (KSEG0 / KSEG1), which covers all physical RDRAM, ROM, and memory-mapped I/O on N64.
constexpr bool IsDirectMappedAddress(uint32_t address) {
  return address >= kKseg0Base && address <= kKseg1End;
}

// Returns true if the address is in KSEG0 (cached, direct-mapped RAM/ROM).
constexpr bool IsKseg0Address(uint32_t address) {
  return address >= kKseg0Base && address <= kKseg0End;
}

// Returns true if the address is in KSEG1 (uncached, direct-mapped MMIO/ROM).
constexpr bool IsKseg1Address(uint32_t address) {
  return address >= kKseg1Base && address <= kKseg1End;
}

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
  kLdl,
  kLdr,
  kLwu,
  kSdl,
  kSdr,
  kDadd,
  kDaddu,
  kDaddi,
  kDaddiu,
  kDsub,
  kDsubu,
  kDsll,
  kDsrl,
  kDsra,
  kDsll32,
  kDsrl32,
  kDsra32,
  kDsllv,
  kDsrlv,
  kDsrav,
  kDmult,
  kDmultu,
  kDdiv,
  kDdivu,

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
  kBltzall,
  kBgezall,

  // Floating Point (COP1) Instructions
  kLwc1,
  kSwc1,
  kLdc1,
  kSdc1,
  kMfc1,
  kMtc1,
  kCfc1,
  kCtc1,
  kDmfc1,
  kDmtc1,
  kAddS,
  kSubS,
  kMulS,
  kDivS,
  kSqrtS,
  kAbsS,
  kMovS,
  kNegS,
  kAddD,
  kSubD,
  kMulD,
  kDivD,
  kSqrtD,
  kAbsD,
  kMovD,
  kNegD,
  kCvtSD,
  kCvtDS,
  kCvtSW,
  kCvtDW,
  kCvtWS,
  kCvtWD,
  kCvtLS,
  kCvtLD,
  kCvtSL,
  kCvtDL,
  kTruncWS,
  kTruncWD,
  kRoundWS,
  kRoundWD,
  kCeilWS,
  kCeilWD,
  kFloorWS,
  kFloorWD,
  kCFS,
  kCUnS,
  kCEqS,
  kCUeqS,
  kCOltS,
  kCUltS,
  kCOleS,
  kCUleS,
  kCSfS,
  kCNgleS,
  kCSeqS,
  kCNglS,
  kCLtS,
  kCNgeS,
  kCLeS,
  kCNgtS,
  kCFD,
  kCUnD,
  kCEqD,
  kCUeqD,
  kCOltD,
  kCUltD,
  kCOleD,
  kCUleD,
  kCSfD,
  kCNgleD,
  kCSeqD,
  kCNglD,
  kCLtD,
  kCNgeD,
  kCLeD,
  kCNgtD,
  kBc1t,
  kBc1f,
  kBc1tl,
  kBc1fl,

  // System & Coprocessor
  kSyscall,
  kBreak,
  kSync,
  kMfc0,
  kMtc0,
};

// --- COP1 Opcode Classification Queries ---

// Returns true if `op` is a double-precision COP1 comparison instruction (c.*.d).
bool IsDoubleComparisonOp(Opcode op);

// Returns true if `op` consumes its floating-point source operand(s) as double-precision values.
bool IsDoublePrecisionReadOp(Opcode op);

// Returns true if `op` defines its floating-point destination operand as a double-precision value.
bool IsDoublePrecisionWriteOp(Opcode op);

// Returns true if `op` converts a single- or double-precision floating-point value to a 32-bit
// integer word in an FPR (cvt.w.{s,d}, trunc.w.{s,d}, round.w.{s,d}, ceil.w.{s,d}, floor.w.{s,d}).
bool IsFloatToIntConvertOp(Opcode op);

// Represents a fully decoded 32-bit MIPS instruction.
struct Instruction {
  uint32_t raw_word = 0;  // Original 32-bit instruction
  uint32_t vram = 0;      // Virtual address in RDRAM

  Opcode opcode = Opcode::kUnknown;

  // General-purpose register operands
  std::optional<Register> rd;
  std::optional<Register> rs;
  std::optional<Register> rt;
  std::optional<Register> unscheduled_clobber_reg;

  // Floating-point register operands (COP1)
  std::optional<FpRegister> fd;
  std::optional<FpRegister> fs;
  std::optional<FpRegister> ft;

  // Immediates and targets
  int16_t immediate = 0;     // 16-bit signed immediate (I-type)
  uint8_t shift_amount = 0;  // 5-bit sa field for shifts
  uint32_t target = 0;       // 26-bit target for J/JAL

  // Returns the zero-extended 16-bit immediate for logical instructions (andi, ori, xori).
  uint16_t UnsignedImmediate() const { return static_cast<uint16_t>(immediate); }

  // --- Classification Queries ---
  bool IsBranch() const;               // beq, bne, blez, etc.
  bool IsBranchLikely() const;         // beql, bnel, etc.
  bool IsUnconditionalBranch() const;  // b, beq $r, $r, bgez $zero, etc.
  bool IsJump() const;                 // j, jal, jr, jalr
  bool IsCall() const;                 // jal, jalr, bltzal, bgezal (calls function)
  bool IsReturn() const;               // jr $ra
  bool HasDelaySlot() const;           // All standard branches and jumps

  bool IsLoad() const;             // lw, lh, lb, lwc1, etc.
  bool IsStore() const;            // sw, sh, sb, swc1, etc.
  bool IsNop() const;              // sll $zero, $zero, 0
  bool IsDoublePrecision() const;  // ldc1, sdc1, add.d, cvt.s.d, etc.

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

// Decodes a sequence of 32-bit big-endian instruction words starting at base_vram.
absl::StatusOr<std::vector<Instruction>> DecodeSequence(absl::Span<const uint32_t> words,
                                                        uint32_t base_vram = 0x80000000);

// Formats a branch instruction targeting the specified label using standard
// MIPS pseudo-instructions (e.g. "b", "beqz", "bnez", "blez", etc.).
// If inst is not a branch, falls back to inst.Disassemble().
std::string FormatBranch(const Instruction& inst, std::string_view target_label);

}  // namespace rom_nom_nom

#endif  // CORE_MIPS_H_
