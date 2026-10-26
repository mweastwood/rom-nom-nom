#ifndef LIFTER_AST_ARITHMETIC_SIMPLIFIER_H_
#define LIFTER_AST_ARITHMETIC_SIMPLIFIER_H_

#include <string_view>

#include "core/c_ast.h"
#include "lifter/ast_pass_pipeline.h"

namespace rom_nom_nom {

// Rewrites shift-add/subtract multiplication chains (e.g., `(x << 2) + x` ->
// `x * 5`, `((x << 2) + x) << 2` -> `x * 20`, `(x << 3) - x` -> `x * 7`) and
// sign-extension shift pairs (`((x << 16) >> 16)` -> `(s16)x`) inside `comp`.
void SimplifyShiftAddMultiplication(CompoundStatement* comp);

// Rewrites integer modulo idioms (`x - (x / D) * D` -> `x % D`) and GCC 2.7.2
// unsigned integer-to-float conversion diamonds inside `comp` (non-recursive).
void SimplifyModuloAndUnsignedFloatDiamondsInBlock(CompoundStatement* comp);

// Rewrites signed and unsigned reciprocal multiplication high-word idioms back
// into integer division (`/ D`), folds modulo and unsigned integer-to-float
// conversion diamonds across `comp` and its nested blocks, and removes dead
// reciprocal magic constant assignments.
void SimplifyReciprocalDivisionAndUnsignedFloatConversions(CompoundStatement* comp);

// Concrete `AstPass` wrappers for arithmetic strength-reduction inversions.
class ShiftAddMultiplicationSimplifierPass : public AstPass {
 public:
  std::string_view Name() const override { return "SimplifyShiftAddMultiplication"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

class ReciprocalAndUnsignedFloatSimplifierPass : public AstPass {
 public:
  std::string_view Name() const override {
    return "SimplifyReciprocalDivisionAndUnsignedFloatConversions";
  }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_ARITHMETIC_SIMPLIFIER_H_
