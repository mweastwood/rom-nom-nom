#ifndef ROM_NOM_NOM_LIFTER_RECONSTRUCT_O0_FINAL_SCAN_H_
#define ROM_NOM_NOM_LIFTER_RECONSTRUCT_O0_FINAL_SCAN_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Reconstructs RTL instructions and `SUBREG` wrappers deleted or lowered during
// `final_scan_insn` (`final.c:1671, 1800, 1933, 2102`) under `-O0`
// (`profile.optimize_level == 0`):
//   1. `emit_no_conflict_block` `(clobber (reg:DI hr))`, `(subreg:SI (reg:DI hr) w)`,
//      and trailing `(set (reg:DI hr) (reg:DI hr))` (`optabs.c:2485-2525`),
//   2. Mixed-mode `DImode` subword `(subreg:SI (reg:DI hr) w)` accesses,
//   3. Tablejump index copy `(set (reg:SI hr_idx) (reg:SI hr_idx))` (`stmt.c`),
//   4. Precomputed outgoing call-argument `(set (reg:M arg_hr) (reg:M arg_hr))`,
//   5. Post-call return-value `(set (reg:M ret_hr) (reg:M ret_hr))` and
//      `__fix*di` `emit_libcall_block` trailing self-moves (`calls.c`, `optabs.c`),
//   6. Subword truncating `(subreg:QI|HI (reg:SI hr) 0)` reads, and
//   7. `obey_regdecls` (`stmt.c:3271, 3499`) scope-boundary `(use (reg:M hr))`
//      markers plus hidden `sret` exit `(set (reg/i:SI 2) (reg:SI 2))` +
//      `(use (reg/i:SI 2))` (`function.c`).
void ReconstructO0FinalScanDeletions(RtlControlFlowGraph* cfg, const CompilationProfile& profile);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_RECONSTRUCT_O0_FINAL_SCAN_H_
