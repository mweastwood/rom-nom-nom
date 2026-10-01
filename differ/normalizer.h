#ifndef DIFFER_NORMALIZER_H_
#define DIFFER_NORMALIZER_H_

#include <cstdint>
#include <optional>

#include "core/mips.h"

namespace rom_nom_nom {

// Context for the function being compared, specifying the VRAM start addresses
// and byte sizes in both the target retail ROM and the recompiled object.
struct FunctionDiffContext {
  uint32_t target_vram = 0;    // VRAM start address in target ROM (e.g. 0x800266C0)
  uint32_t target_size = 0;    // Function byte length in target ROM
  uint32_t compiled_vram = 0;  // VRAM start address in compiled object (typically 0x0)
  uint32_t compiled_size = 0;  // Function byte length in compiled object
};

// Classification of instruction similarity level.
enum class MatchQuality {
  // 100% byte-for-byte identical 32-bit machine code instruction word.
  kBitExact,

  // Semantically and functionally identical control flow, but differing in raw
  // machine bits solely due to known unlinked object artifacts (specifically,
  // MIPS intra-function J-type jumps whose 26-bit target field encodes an
  // absolute VRAM address in linked ROM vs. a section-relative offset in .o).
  kNormalizedExact,

  // Same instruction opcode/mnemonic (e.g. both are 'addu' or both are 'lw'),
  // but differing register assignments or immediate constants.
  kSimilarOpcode,

  // Completely different opcodes or instruction structures (e.g. 'addu' vs 'sw').
  kDifferent,
};

// Represents an instruction normalized for sequence alignment and comparison.
struct NormalizedInstruction {
  Instruction original;          // Original decoded MIPS instruction
  uint32_t normalized_word = 0;  // 32-bit word with intra-function targets normalized
  uint32_t func_offset = 0;      // Byte offset relative to function start (0x0, 0x4, ...)

  // Target byte offset relative to function start if this instruction is an
  // intra-function branch or unconditional jump.
  std::optional<uint32_t> intra_function_target_offset;
};

// Normalizer transforms raw MIPS instructions from retail ROM and compiled object
// files into normalized semantic representations, resolving cosmetic and linker-induced
// address discrepancies.
//
// ============================================================================
// JUSTIFICATION FOR INSTRUCTION NORMALIZATION RULES
// ============================================================================
//
// 1. Intra-Function PC-Relative Branches (beq, bne, blez, bgtz, bltz, bgez, bc1t/f, etc.):
//    - Difference: Disassemblers typically display branch targets as absolute VRAM
//      addresses or labels (e.g. Target ROM displays ".L800266E0" because the
//      function starts at 0x800266C0, whereas an unlinked .o displays ".L00000020"
//      because it was compiled starting at VRAM 0x0).
//    - Justification: In MIPS machine code, PC-relative branches encode a signed
//      16-bit word offset: target = (PC + 4) + (simm16 * 4). Because both the
//      target function and compiled function branch the exact same distance (e.g.
//      +8 instructions = +32 bytes), the raw 16-bit machine immediate is ALREADY
//      identical! By calculating the intra-function target offset relative to function
//      start, downstream formatters can display consistent relative labels (.L_0020)
//      without altering bit-exact machine code comparison.
//
// 2. Intra-Function J-Type Unconditional Jumps (j):
//    - Difference: MIPS 'j' instructions encode a 26-bit pseudo-absolute word address:
//      target = (PC & 0xF0000000) | (target_field << 2). In retail ROM, the linked
//      binary has the full VRAM address (e.g. 0x800266E0) baked into 'target_field'.
//      In an unlinked object file (.o), the compiler emits a section-relative jump
//      (target_field = 0x00000020 >> 2) awaiting final relocation by the linker.
//    - Justification: When an unconditional jump targets an address within the
//      function's own boundary [vram_start, vram_start + func_size), the jump is
//      strictly intra-procedural control flow (such as a loop backedge or jump to a
//      shared exit epilogue). The difference in the raw 26-bit field is purely an
//      artifact of the object file being unlinked. By computing the function-relative
//      target offset (target_vram - vram_start) and normalizing the raw 26-bit target
//      field to that relative offset, we confirm that both instructions execute the
//      identical control-flow jump without requiring a full link step.
//
// 3. Instruction Equivalence Hierarchy:
//    - Evaluates match fidelity into distinct tiers (kBitExact, kNormalizedExact,
//      kSimilarOpcode, kDifferent), allowing the sequence aligner to make optimal
//      alignment decisions and the diff formatter to highlight differences with
//      accurate semantic colors.
// ============================================================================
class Normalizer {
 public:
  explicit Normalizer(FunctionDiffContext context);

  // Normalizes an instruction extracted from the target retail ROM.
  NormalizedInstruction NormalizeTarget(const Instruction& instr) const;

  // Normalizes an instruction extracted from the recompiled object file.
  NormalizedInstruction NormalizeCompiled(const Instruction& instr) const;

  // Compares a normalized target instruction with a normalized compiled instruction
  // and determines the match quality.
  MatchQuality Compare(const NormalizedInstruction& target,
                       const NormalizedInstruction& compiled) const;

 private:
  NormalizedInstruction NormalizeInstructionInternal(const Instruction& instr, uint32_t base_vram,
                                                     uint32_t func_size) const;

  FunctionDiffContext context_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_NORMALIZER_H_
