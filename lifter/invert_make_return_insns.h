#ifndef LIFTER_INVERT_MAKE_RETURN_INSNS_H_
#define LIFTER_INVERT_MAKE_RETURN_INSNS_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{make\_return\_insns}}$ of KMC GCC 2.7.2's
// `reorg.c:make_return_insns` (and trailing `end_of_function_label` cleanup at
// `reorg.c:4349-4355`).
//
// On stock MIPS (`mips.md` `return` / `return_internal`), return jumps have the
// `(parallel [(return) (use (reg:SI 31 ra))])` pattern rather than bare
// `(return)`, so `make_return_insns` (`reorg.c:4058-4172`) only deletes an
// unused `end_of_function_label` (`--LABEL_NUSES == 0`) while preserving all
// valid `return_internal` jumps and delay-slot `SEQUENCE` bundles. Given a
// post-`dbr_schedule` control-flow graph `dbr_return_cfg_x` (`RtlPassKind::kDbr`),
// validates that no synthetic bare `(return)` exists and restores a trailing
// unused `end_of_function_label` block when needed so executing
// `RunMakeReturnInsns` reproduces `dbr_return_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` if `dbr_return_cfg_x` has no basic
// blocks or contains malformed RTL instruction patterns (including bare
// `(return)` outside `parallel`) or out-of-range successor edges.
RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& dbr_return_cfg_x,
                                          const CompilationProfile& profile);
RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& dbr_return_cfg_x);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertMakeReturnInsns;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_MAKE_RETURN_INSNS_H_
