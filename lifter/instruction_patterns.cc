#include "lifter/instruction_patterns.h"

#include <cstddef>
#include <optional>
#include <string>

#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom {

std::optional<FloatingPointConversionPattern> MatchFloatingPointConversionPattern(
    absl::Span<const Instruction> instructions, size_t i) {
  if (i >= instructions.size()) {
    return std::nullopt;
  }
  const auto& inst = instructions[i];

  // 1. Integer to Float: mtc1 $rt, $fs [-> nop]* -> cvt.s.w/cvt.d.w $fd, $fs
  if (inst.opcode == Opcode::kMtc1 && inst.fs.has_value() && inst.rt.has_value()) {
    size_t j = i + 1;
    while (j < instructions.size() && instructions[j].IsNop()) {
      ++j;
    }
    if (j < instructions.size()) {
      const auto& next_inst = instructions[j];
      if ((next_inst.opcode == Opcode::kCvtSW || next_inst.opcode == Opcode::kCvtDW) &&
          next_inst.fs.has_value() && *next_inst.fs == *inst.fs && next_inst.fd.has_value()) {
        FloatingPointConversionPattern pattern;
        pattern.type = FloatingPointConversionType::kIntToFloat;
        pattern.end_index = j;
        pattern.is_int_to_float = true;
        pattern.src_gpr = *inst.rt;
        pattern.dest_floating_point_reg = *next_inst.fd;
        pattern.is_double = (next_inst.opcode == Opcode::kCvtDW);
        return pattern;
      }
    }
  }

  // 2. Float to Integer: (trunc/ceil/floor/round/cvt).w.(s/d) $fd, $fs [-> nop]* -> mfc1 $rt, $fd
  if (IsFloatToIntConvertOp(inst.opcode) && inst.fd.has_value() && inst.fs.has_value()) {
    size_t j = i + 1;
    while (j < instructions.size() && instructions[j].IsNop()) {
      ++j;
    }
    if (j < instructions.size()) {
      const auto& next_inst = instructions[j];
      if (next_inst.opcode == Opcode::kMfc1 && next_inst.fs.has_value() &&
          *next_inst.fs == *inst.fd && next_inst.rt.has_value() &&
          *next_inst.rt != Register::kZero) {
        FloatingPointConversionPattern pattern;
        pattern.type = FloatingPointConversionType::kFloatToInt;
        pattern.end_index = j;
        pattern.is_int_to_float = false;
        pattern.src_floating_point_reg = *inst.fs;
        pattern.dest_gpr = *next_inst.rt;
        pattern.is_double = IsDoublePrecisionReadOp(inst.opcode);
        if (inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD) {
          pattern.round_func = pattern.is_double ? "ceil" : "ceilf";
        } else if (inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD) {
          pattern.round_func = pattern.is_double ? "floor" : "floorf";
        } else if (inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD) {
          pattern.round_func = pattern.is_double ? "round" : "roundf";
        }
        return pattern;
      }
    }
  }

  // 3. Float rounding: (ceil/floor/round/trunc).w.(s/d) $fd, $fs [-> nop]* -> cvt.(s/d).w $dest,
  // $fd
  if ((inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD ||
       inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD ||
       inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD ||
       inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kTruncWD) &&
      inst.fd.has_value() && inst.fs.has_value()) {
    size_t j = i + 1;
    while (j < instructions.size() && instructions[j].IsNop()) {
      ++j;
    }
    if (j < instructions.size()) {
      const auto& next_inst = instructions[j];
      if ((next_inst.opcode == Opcode::kCvtSW || next_inst.opcode == Opcode::kCvtDW) &&
          next_inst.fs.has_value() && *next_inst.fs == *inst.fd && next_inst.fd.has_value()) {
        bool is_double = IsDoublePrecisionReadOp(inst.opcode) || next_inst.opcode == Opcode::kCvtDW;
        std::string func_name;
        if (inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD) {
          func_name = is_double ? "ceil" : "ceilf";
        } else if (inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD) {
          func_name = is_double ? "floor" : "floorf";
        } else if (inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD) {
          func_name = is_double ? "round" : "roundf";
        } else {
          func_name = is_double ? "trunc" : "truncf";
        }
        FloatingPointConversionPattern pattern;
        pattern.type = FloatingPointConversionType::kFloatRounding;
        pattern.end_index = j;
        pattern.is_rounding = true;
        pattern.round_func = func_name;
        pattern.dest_floating_point_reg = *next_inst.fd;
        pattern.src_floating_point_reg = *inst.fs;
        pattern.is_double = is_double;
        return pattern;
      }
    }
  }

  return std::nullopt;
}

}  // namespace rom_nom_nom
