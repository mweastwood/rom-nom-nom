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

int Normalizer::SimilarityScore(const NormalizedInstruction& target,
                                const NormalizedInstruction& compiled) const {
  MatchQuality quality = Compare(target, compiled);
  if (quality == MatchQuality::kBitExact) {
    return 42;
  }
  if (quality == MatchQuality::kNormalizedExact) {
    return 40;
  }
  if (quality == MatchQuality::kDifferent) {
    return -20;
  }

  // MatchQuality::kSimilarOpcode: base score +12, up to +38 with operand matches.
  // Keep strictly < 40 (kNormalizedExact) and > -20 (kGapPenalty).
  int score = 12;
  const Instruction& t = target.original;
  const Instruction& c = compiled.original;

  if (t.IsBranch() && c.IsBranch()) {
    if (t.rs.has_value() && t.rs == c.rs) score += 8;
    if (t.rt.has_value() && t.rt == c.rt) score += 6;
    const bool t_backward = t.immediate < 0;
    const bool c_backward = c.immediate < 0;
    if (t_backward == c_backward) {
      score += 6;
    } else {
      score -= 8;
    }
    if (target.intra_function_target_offset.has_value() &&
        compiled.intra_function_target_offset.has_value()) {
      int32_t t_rel = static_cast<int32_t>(*target.intra_function_target_offset) -
                      static_cast<int32_t>(target.func_offset);
      int32_t c_rel = static_cast<int32_t>(*compiled.intra_function_target_offset) -
                      static_cast<int32_t>(compiled.func_offset);
      int32_t diff_words = ((t_rel > c_rel) ? (t_rel - c_rel) : (c_rel - t_rel)) >> 2;
      if (diff_words <= 2) {
        score += 6;
      } else if (diff_words <= 6) {
        score += 3;
      }
    }
    return score;
  }

  if (t.opcode == Opcode::kJ || t.opcode == Opcode::kJal) {
    if (target.intra_function_target_offset.has_value() &&
        compiled.intra_function_target_offset.has_value()) {
      int32_t t_dest = static_cast<int32_t>(*target.intra_function_target_offset);
      int32_t c_dest = static_cast<int32_t>(*compiled.intra_function_target_offset);
      int32_t diff_words = ((t_dest > c_dest) ? (t_dest - c_dest) : (c_dest - t_dest)) >> 2;
      if (diff_words <= 2) {
        score += 14;
      } else if (diff_words <= 6) {
        score += 8;
      }
    }
    return score;
  }

  if (t.rd.has_value() && t.rd == c.rd) score += 8;
  if (t.rs.has_value() && t.rs == c.rs) score += 8;
  if (t.rt.has_value() && t.rt == c.rt) score += 6;
  if (t.fd.has_value() && t.fd == c.fd) score += 8;
  if (t.fs.has_value() && t.fs == c.fs) score += 8;
  if (t.ft.has_value() && t.ft == c.ft) score += 6;
  if (!t.rd.has_value() && !t.fd.has_value() && t.immediate == c.immediate) {
    score += 8;
  }
  if (t.shift_amount != 0 && t.shift_amount == c.shift_amount) {
    score += 4;
  }
  return score;
}

}  // namespace rom_nom_nom
