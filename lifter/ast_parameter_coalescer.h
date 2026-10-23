#ifndef LIFTER_AST_PARAMETER_COALESCER_H_
#define LIFTER_AST_PARAMETER_COALESCER_H_

#include <cstdint>
#include <vector>

#include "core/c_ast.h"

namespace rom_nom_nom {

// Coalesces prologue copies of parameter registers (`temp_s0 = arg0;`,
// `var_sp_N = arg0;`, `f20 = f12;`) back into the parameter variable when safe.
void CoalescePrologueParameterCopies(CompoundStatement* comp,
                                     const std::vector<CParameter>& parameters,
                                     uint32_t frame_size = 0);

// Splits `f32` parameters passed in `$a1-$a3` into `temp_a1..temp_a3` if the
// underlying GPR is later overwritten with an integer value.
void SplitReusedFloatGprParameters(CompoundStatement* comp,
                                   const std::vector<CParameter>& parameters);

// Splits parameter variables (`arg0..arg3`) into `temp_a0..temp_a3` after their
// entry value is saved to `$s0-$s7` or stack home slot and the register is
// reused as a scratch register. Returns true if single-use expression folding
// should be re-run after coalescing.
bool SplitScratchReusedParameterRegisters(CompoundStatement* body,
                                          const std::vector<CParameter>& parameters,
                                          uint32_t frame_size = 0);

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_PARAMETER_COALESCER_H_
