#include "differ/normalizer.h"

#include <cstdint>
#include <optional>

#include "core/mips.h"

namespace rom_nom_nom {

Normalizer::Normalizer(FunctionDiffContext context) : context_(context) {}

NormalizedInstruction Normalizer::NormalizeTarget(const Instruction& instr) const {
  return NormalizeInstructionInternal(instr, context_.target_vram, context_.target_size);
}

NormalizedInstruction Normalizer::NormalizeCompiled(const Instruction& instr) const {
  return NormalizeInstructionInternal(instr, context_.compiled_vram, context_.compiled_size);
}

NormalizedInstruction Normalizer::NormalizeInstructionInternal(const Instruction& instr,
                                                               uint32_t base_vram,
                                                               uint32_t func_size) const {
  NormalizedInstruction norm;
  norm.original = instr;

  // Compute offset relative to function start.
  if (instr.vram >= base_vram) {
    norm.func_offset = instr.vram - base_vram;
  } else {
    norm.func_offset = instr.vram;
  }

  // 1. Intra-Function Branches:
  // PC-relative branches encode a signed 16-bit word displacement from (PC + 4).
  // Because both the target function and compiled function branch the exact same
  // distance in bytes/instructions, the raw 16-bit machine immediate is already
  // bit-exact. We record the intra-function target offset so formatters can render
  // consistent relative labels (.L_%04X).
  if (instr.IsBranch()) {
    uint32_t branch_dest = instr.BranchTarget();
    if (func_size > 0 && branch_dest >= base_vram && branch_dest < base_vram + func_size) {
      norm.intra_function_target_offset = branch_dest - base_vram;
    }
    norm.normalized_word = instr.raw_word;
  }
  // 2. Intra-Function Jumps:
  // J-type jumps encode a 26-bit pseudo-absolute word target: (vram & 0xF0000000) | (target << 2).
  // In retail ROM, the linked binary has the full VRAM address baked in. In unlinked
  // object files, the 26-bit target encodes an offset relative to the section start.
  // When the target falls within the function's own boundary, we normalize the raw 26-bit
  // target field to the function-relative offset: (relative_offset >> 2).
  else if (instr.opcode == Opcode::kJ) {
    uint32_t jump_dest = instr.JumpTarget();
    if (func_size > 0 && jump_dest >= base_vram && jump_dest < base_vram + func_size) {
      uint32_t rel_offset = jump_dest - base_vram;
      norm.intra_function_target_offset = rel_offset;
      // Mask out bits 25..0 and insert normalized relative target index.
      norm.normalized_word = (instr.raw_word & 0xFC000000) | ((rel_offset >> 2) & 0x03FFFFFF);
    } else {
      norm.normalized_word = instr.raw_word;
    }
  }
  // All other instructions: normalized word is the original raw machine word.
  else {
    norm.normalized_word = instr.raw_word;
  }

  return norm;
}

MatchQuality Normalizer::Compare(const NormalizedInstruction& target,
                                 const NormalizedInstruction& compiled) const {
  // Tier 1: 100% byte-for-byte exact match.
  if (target.original.raw_word == compiled.original.raw_word) {
    return MatchQuality::kBitExact;
  }

  // Tier 2: Semantically identical control flow modulo unlinked J-type VRAM target.
  if (target.normalized_word == compiled.normalized_word) {
    return MatchQuality::kNormalizedExact;
  }

  // Tier 3: Same instruction mnemonic/opcode, but differing operands or registers.
  if (target.original.opcode == compiled.original.opcode) {
    return MatchQuality::kSimilarOpcode;
  }

  // Tier 4: Completely different instruction opcode.
  return MatchQuality::kDifferent;
}

}  // namespace rom_nom_nom
