#ifndef LIFTER_INVERT_ALTER_REG_H_
#define LIFTER_INVERT_ALTER_REG_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Reverses `reload1.c:alter_reg` (`SubfunctionId::kAlterReg`), lifting
// allocated hard-register occurrences `(reg[/flags]:M <hard>)` and
// `alter_reg`-assigned `$fp`-relative stack-slot `(mem[/u]:M ...)` operands
// back to pseudo-registers `(reg[/flags]:M <pseudo>)` (`pseudo >=
// first_pseudo_regno`) and populating `sideband.reg_renumber`,
// `sideband.reg_n_refs`, `sideband.reg_n_sets`, and `sideband.max_regno` so
// that re-running `FunctionHooks::RunAlterReg` or
// `ExecuteSubfunctionBoundary(SubfunctionId::kAlterReg)` reproduces
// `allocated_cfg` bit-for-bit.
// Returns an empty `RtlControlFlowGraph` on invalid or malformed inputs.
RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg,
                                   int first_pseudo_regno = 72);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_INVERT_ALTER_REG_H_
