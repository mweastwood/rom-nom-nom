#ifndef LIFTER_INVERT_SIMPLIFY_RTX_H_
#define LIFTER_INVERT_SIMPLIFY_RTX_H_

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// Structured AST right-inverse of KMC GCC 2.7.2 `combine.c:simplify_rtx`.
//
// Given a canonical simplified `RtlExpr` `simplified_expr`, constructs an
// unsimplified `RtlExpr` $y$ using the algebraic, logical, shift, relational,
// and conditional reduction rules of `combine.c:simplify_rtx` (and its helpers
// `simplify_logical`, `simplify_if_then_else`, `simplify_shift_const`,
// `simplify_binary_operation`, and `simplify_ternary_operation`) such that
// replaying `simplify_rtx` in-process reproduces `simplified_expr`.
absl::StatusOr<RtlExpr> InvertSimplifyRtxExpr(const RtlExpr& simplified_expr);

// String-level right-inverse of `combine.c:simplify_rtx` matching the
// `SubfunctionInverseProvider::InvertSimplifyRtx` interface.
//
// Returns an empty string `""` if `simplified_rtx_x` is empty, malformed,
// contains an unrecognized opcode or mode, or fails structural validation.
std::string InvertSimplifyRtx(std::string_view simplified_rtx_x);

// Whole-CFG right-inverse of `SubfunctionId::kSimplifyRtx`.
//
// Validates `combine_cfg` and replaces the compound source expression of each
// non-branch `(set dst src)` instruction with its `InvertSimplifyRtxExpr`
// preimage so that replaying `ExecuteSubfunctionBoundary(kSimplifyRtx)`
// reproduces `combine_cfg` identically.
absl::StatusOr<RtlControlFlowGraph> InvertSimplifyRtxCfg(const RtlControlFlowGraph& combine_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_INVERT_SIMPLIFY_RTX_H_
