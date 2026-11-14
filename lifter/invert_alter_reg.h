#ifndef LIFTER_INVERT_ALTER_REG_H_
#define LIFTER_INVERT_ALTER_REG_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Reverses `reload1.c:alter_reg` (`SubfunctionId::kAlterReg`), splitting
// disjoint post-reload hard-register lifetimes into distinct pseudo-register
// def-use webs (`pseudo >= first_pseudo_regno`), preserving genuine hardware
// ABI registers (entry parameters, outgoing call arguments, call return values,
// function return values, and fixed/status/accumulator registers), and lifting
// 8-byte big-endian `alter_reg(..., -1)` stack spill slots back to
// `reg_renumber[pseudo] = -1` pseudos while keeping `expand_decl` C stack
// locals on the stack.
//
// Populates `sideband.reg_renumber`, loop-depth-weighted `sideband.reg_n_refs`
// and `sideband.reg_n_sets`, `sideband.max_regno`, and `sideband.frame_size_bytes`
// (while preserving `sideband.pointer_regnos`) so that replaying
// `FunctionHooks::RunAlterReg` or `ExecuteSubfunctionBoundary(kAlterReg)`
// reproduces `allocated_cfg` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` on invalid or malformed inputs.
RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg,
                                   const CompilationProfile& profile, int first_pseudo_regno = 72);

RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg,
                                   int first_pseudo_regno = 72);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_INVERT_ALTER_REG_H_
