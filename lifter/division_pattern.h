#ifndef LIFTER_DIVISION_PATTERN_H_
#define LIFTER_DIVISION_PATTERN_H_

#include <cstddef>
#include <optional>

#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom {

// Represents a recognized MIPS division / modulo pattern.
struct DivisionPattern {
  bool is_unsigned = false;
  Register num_reg = Register::kZero;  // numerator ($rs)
  Register den_reg = Register::kZero;  // denominator ($rt)

  std::optional<Register> div_dest_reg;  // destination of mflo (quotient /)
  std::optional<Register> mod_dest_reg;  // destination of mfhi (remainder %)

  size_t start_index = 0;  // index of div/divu instruction
  size_t end_index = 0;    // index of last instruction in pattern (e.g. mfhi/mflo)
};

// Matches MIPS GCC integer division and modulo trap expansions starting at start_idx.
// If matched, returns the pattern details; otherwise std::nullopt.
std::optional<DivisionPattern> MatchDivisionPattern(absl::Span<const Instruction> instructions,
                                                    size_t start_idx);

}  // namespace rom_nom_nom

#endif  // LIFTER_DIVISION_PATTERN_H_
