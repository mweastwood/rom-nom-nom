#ifndef LIFTER_INSTRUCTION_PATTERNS_H_
#define LIFTER_INSTRUCTION_PATTERNS_H_

#include <cstddef>
#include <optional>
#include <string>

#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom {

// Classification of multi-instruction floating-point conversion sequences.
enum class FloatingPointConversionType {
  kIntToFloat,
  kFloatToInt,
  kFloatRounding,
};

// Describes a matched multi-instruction floating-point conversion or rounding sequence.
struct FloatingPointConversionPattern {
  FloatingPointConversionType type = FloatingPointConversionType::kIntToFloat;
  size_t end_index = 0;
  bool is_int_to_float = false;
  Register src_gpr = Register::kZero;
  FpRegister dest_floating_point_reg = FpRegister::kF0;
  bool is_double = false;
  FpRegister src_floating_point_reg = FpRegister::kF0;
  Register dest_gpr = Register::kZero;
  bool is_rounding = false;
  std::string round_func;
};

// Matches multi-instruction MIPS GCC floating-point conversion and rounding idioms
// starting at `instructions[i]`.
std::optional<FloatingPointConversionPattern> MatchFloatingPointConversionPattern(
    absl::Span<const Instruction> instructions, size_t i);

}  // namespace rom_nom_nom

#endif  // LIFTER_INSTRUCTION_PATTERNS_H_
