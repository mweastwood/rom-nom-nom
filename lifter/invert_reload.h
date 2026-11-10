#ifndef ROM_NOM_NOM_LIFTER_INVERT_RELOAD_H_
#define ROM_NOM_NOM_LIFTER_INVERT_RELOAD_H_

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Step 20: Reverses `reload1.c:reload` (`SubfunctionId::kReload`) on a
// `.07.greg` (`SubfunctionId::kReload` post-state) `RtlControlFlowGraph` to
// recover the pre-reload `.06.lreg` / `SubfunctionId::kGlobalAlloc` post-state
// `RtlControlFlowGraph`:
//   1. Reverses frame-pointer and argument-pointer register elimination
//      (`eliminate_regs` / `eliminate_regs_in_insn`: restoring `$fp` (`$30`)
//      from `$sp` (`$29`) when `!frame_pointer_needed`, and updating
//      `REG_NOTES` and `basic_block_live_at_start`).
//   2. Reverses spill/fill reload sequences (`reload_as_needed` /
//      `emit_reload_insns` / `subst_reloads`) and unallocated pseudo stack-slot
//      replacements (`alter_reg`) back into unallocated pseudo registers.
//   3. Preserves `RtlSidebandData` (`reg_renumber`, `reg_n_refs`, `reg_n_sets`,
//      `reg_n_deaths`, `reg_live_length`, `reg_n_calls_crossed`,
//      `basic_block_live_at_start`, `frame_size_bytes`) for upstream register
//      allocator inversion (`InvertGlobalAlloc`, `InvertBlockAlloc`).
absl::StatusOr<RtlControlFlowGraph> InvertReload(const RtlControlFlowGraph& reload_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_INVERT_RELOAD_H_
