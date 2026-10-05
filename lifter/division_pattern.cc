#include "lifter/division_pattern.h"

#include <algorithm>

namespace rom_nom_nom {

std::optional<DivisionPattern> MatchDivisionPattern(absl::Span<const Instruction> instructions,
                                                    size_t start_idx) {
  if (start_idx >= instructions.size()) {
    return std::nullopt;
  }

  const auto& div_inst = instructions[start_idx];
  if (div_inst.opcode != Opcode::kDiv && div_inst.opcode != Opcode::kDivu) {
    return std::nullopt;
  }

  DivisionPattern pattern;
  pattern.is_unsigned = (div_inst.opcode == Opcode::kDivu);
  pattern.num_reg = div_inst.rs.value_or(Register::kZero);
  pattern.den_reg = div_inst.rt.value_or(Register::kZero);
  pattern.start_index = start_idx;

  size_t curr = start_idx + 1;

  // Check for zero-division trap check:
  //   bnez den_reg, <target>
  //   nop (delay slot)
  //   break 7
  if (curr + 2 < instructions.size() && instructions[curr].opcode == Opcode::kBne &&
      ((instructions[curr].rs == pattern.den_reg && instructions[curr].rt == Register::kZero) ||
       (instructions[curr].rt == pattern.den_reg && instructions[curr].rs == Register::kZero)) &&
      instructions[curr + 2].opcode == Opcode::kBreak) {
    curr += 3;

    // Check for optional overflow trap check (signed division only):
    //   addiu $at, $zero, -1
    //   bne den_reg, $at, <target>
    //   lui $at, 0x8000
    //   bne num_reg, $at, <target>
    //   nop (delay slot)
    //   break 6
    if (curr + 5 < instructions.size() && instructions[curr].opcode == Opcode::kAddiu &&
        instructions[curr].rt == Register::kAt && instructions[curr].immediate == -1 &&
        instructions[curr + 1].opcode == Opcode::kBne &&
        ((instructions[curr + 1].rs == pattern.den_reg &&
          instructions[curr + 1].rt == Register::kAt) ||
         (instructions[curr + 1].rt == pattern.den_reg &&
          instructions[curr + 1].rs == Register::kAt)) &&
        instructions[curr + 2].opcode == Opcode::kLui &&
        instructions[curr + 2].rt == Register::kAt &&
        instructions[curr + 3].opcode == Opcode::kBne &&
        ((instructions[curr + 3].rs == pattern.num_reg &&
          instructions[curr + 3].rt == Register::kAt) ||
         (instructions[curr + 3].rt == pattern.num_reg &&
          instructions[curr + 3].rs == Register::kAt)) &&
        instructions[curr + 5].opcode == Opcode::kBreak) {
      curr += 6;
    }
  }

  // Next, look for mfhi and/or mflo (skipping up to 3 hazard NOPs)
  size_t scan_limit = std::min(instructions.size(), curr + 6);
  size_t mf_found_idx = 0;
  bool found_any = false;

  for (; curr < scan_limit; ++curr) {
    const auto& inst = instructions[curr];
    if (inst.IsNop()) {
      continue;
    }
    if (inst.opcode == Opcode::kMfhi && !pattern.mod_dest_reg.has_value() && inst.rd.has_value()) {
      pattern.mod_dest_reg = *inst.rd;
      found_any = true;
      mf_found_idx = curr;
      continue;
    }
    if (inst.opcode == Opcode::kMflo && !pattern.div_dest_reg.has_value() && inst.rd.has_value()) {
      pattern.div_dest_reg = *inst.rd;
      found_any = true;
      mf_found_idx = curr;
      continue;
    }
    break;
  }

  if (!found_any) {
    return std::nullopt;
  }

  pattern.end_index = mf_found_idx;
  return pattern;
}

}  // namespace rom_nom_nom
