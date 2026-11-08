#ifndef LIFTER_INVERT_MAKE_RETURN_INSNS_H_
#define LIFTER_INVERT_MAKE_RETURN_INSNS_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{make\_return\_insns}}$ of GCC 2.7.2's
// `reorg.c:make_return_insns` (and `reorg.c:find_end_label`).
//
// Given a post-`dbr_schedule` control-flow graph `dbr_return_cfg_x`
// (`RtlPassKind::kDbr`), converts any delay-slot-filled `(return)` jump inside
// a `(sequence [(jump_insn ... (return) ...) <delay_insn>])` back into an
// unconditional jump `(set (pc) (label_ref L<end>))` targeting the shared
// end-of-function label basic block at the end of `rpo_blocks`, such that
// executing `RunMakeReturnInsns` on the returned CFG reproduces
// `dbr_return_cfg_x` bit-for-bit.
//
// Returns an empty `RtlControlFlowGraph` if `dbr_return_cfg_x` has no basic
// blocks or contains malformed RTL instruction patterns or out-of-range
// successor edges.
RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& dbr_return_cfg_x);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertMakeReturnInsns;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_MAKE_RETURN_INSNS_H_
