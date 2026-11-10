#ifndef LIFTER_INVERT_SUBST_H_
#define LIFTER_INVERT_SUBST_H_

#include <string_view>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

using SubstInverseSpec = ::rom_nom_nom::SubstInverseSpec;
using SubstPreImage = ::rom_nom_nom::SubstPreImage;

// Structured AST pre-image `(target_expr, from_reg, to_expr)` recovered by
// `InvertSubstExpr` for `combine.c:subst`.
struct SubstExprPreImage {
  RtlExpr target_expr;
  RtlExpr from_reg;
  RtlExpr to_expr;

  bool operator==(const SubstExprPreImage& other) const {
    return target_expr == other.target_expr && from_reg == other.from_reg &&
           to_expr == other.to_expr;
  }
  bool operator!=(const SubstExprPreImage& other) const { return !(*this == other); }
};

// Reconstructs a pre-substitution RTL expression triple `(target_expr, from_reg,
// to_expr)` from a post-`combine.c:subst` RTL expression AST `combined_expr`
// such that `combine.c:subst(target_expr, from_reg, to_expr, 0, 0)` reproduces
// `combined_expr`.
//
// Returns `absl::InvalidArgumentError` if `combined_expr` contains unknown
// opcodes, unknown machine modes, negative register numbers, or malformed
// operand structures.
absl::StatusOr<SubstExprPreImage> InvertSubstExpr(const RtlExpr& combined_expr);

// Reconstructs a pre-substitution string specification `(target_expr, from_reg,
// to_expr)` from a serialized RTL S-expression `combined_rtx_x` for
// `SubfunctionInverseProvider::InvertSubst`. Returns an empty `SubstPreImage` if
// `combined_rtx_x` cannot be parsed as a valid RTL expression.
SubstPreImage InvertSubst(std::string_view combined_rtx_x);

// Reconstructs a pre-`combine.c:subst` RTL CFG from a post-`subst` RTL CFG
// (`SubfunctionId::kSubst`). Returns `absl::InvalidArgumentError` if
// `combine_cfg` is empty, has invalid sideband state, or contains malformed RTL
// instruction patterns.
absl::StatusOr<RtlControlFlowGraph> InvertSubstCfg(const RtlControlFlowGraph& combine_cfg);

// Convenience overload returning an empty `RtlControlFlowGraph` if
// `InvertSubstCfg` fails validation.
RtlControlFlowGraph InvertSubst(const RtlControlFlowGraph& combine_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_INVERT_SUBST_H_
