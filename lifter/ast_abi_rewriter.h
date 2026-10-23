#ifndef LIFTER_AST_ABI_REWRITER_H_
#define LIFTER_AST_ABI_REWRITER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "core/c_ast.h"

namespace rom_nom_nom {

// Folds `v0 = expr; return v0;` and conditional return diamonds into direct
// `return expr;` statements.
void SimplifyReturnStatements(CompoundStatement* comp, bool returns_v0, bool returns_f0);

// Simplifies early returns whose delay slot was decoupled across an `if/else`
// diamond back into `if (cond) return ...; return default_val;`.
void SimplifyDecoupledDelaySlotEarlyReturns(CompoundStatement* comp);

// Coalesces prologue copies of parameter registers (`temp_s0 = arg0;`,
// `var_sp_N = arg0;`, `f20 = f12;`) back into the parameter variable when safe.
void CoalescePrologueParameterCopies(CompoundStatement* comp,
                                     const std::vector<CParameter>& parameters,
                                     uint32_t frame_size = 0);

// Sinks identical trailing argument assignments (`arg0 = ...;`, `var_sp_N = ...;`)
// out of `if/else` branches into the post-branch block.
void SinkCommonBranchTailArgAssignments(CompoundStatement* comp);

// Inlines small cross-jumped tail-call blocks reached via `goto` so each branch
// emits a structured call + return/break.
void InlineCrossJumpedCallTails(CompoundStatement* root_body);

// Splits `f32` parameters passed in `$a1-$a3` into `temp_a1..temp_a3` if the
// underlying GPR is later overwritten with an integer value.
void SplitReusedFloatGprParameters(CompoundStatement* comp,
                                   const std::vector<CParameter>& parameters);

// Rewrites calls that pass `f32` values in GPRs or stack slots via
// `*(s32*)&fX` into typed function-pointer casts with direct float arguments.
void RewriteFloatInGprAndStackCallArguments(CompoundStatement* comp);

// Collapses sequences of unaligned load/byte-store statements into typed
// `UnalignedWord` / `UnalignedBlock6` / `UnalignedBlock8` / `UnalignedBlock12`
// / `UnalignedBlock16` struct copies.
void CollapseUnalignedBlockCopies(CompoundStatement* comp);

// Trims trailing `arg0..arg3` arguments from calls to functions with unknown
// parameter counts when those argument registers were never assigned in `body`.
void TrimUnassignedTrailingCallArgRegisters(
    CompoundStatement* body, const std::vector<CParameter>& parameters,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts);

// Splits parameter variables (`arg0..arg3`) into `temp_a0..temp_a3` after their
// entry value is saved to `$s0-$s7` or stack home slot and the register is
// reused as a scratch register. Returns true if single-use expression folding
// should be re-run after coalescing.
bool SplitScratchReusedParameterRegisters(CompoundStatement* body,
                                          const std::vector<CParameter>& parameters,
                                          uint32_t frame_size = 0);

// Folds `-O0` frame-pointer scratch evaluation sequences (while-loop header conditions,
// post-increment pointer dereferences, local update chains, and dead constant loads) and
// rewrites positive constant-offset pointer dereferences `*(TYPE*)(base + offset)` into
// anonymous struct member accesses `((struct { u8 _p[offset]; TYPE _v; }*)base)->_v` so
// GCC 2.7.2 at `-O0` emits single-instruction `offset(base)` loads and stores.
void RewriteFramePointerStructOffsetAccesses(CompoundStatement* body);

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_ABI_REWRITER_H_
