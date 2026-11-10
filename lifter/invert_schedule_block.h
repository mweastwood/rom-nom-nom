#ifndef ROM_NOM_NOM_LIFTER_INVERT_SCHEDULE_BLOCK_H_
#define ROM_NOM_NOM_LIFTER_INVERT_SCHEDULE_BLOCK_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Inverts `sched.c:schedule_block` (and `sched.c:schedule_insns`) for both
// post-reload instruction scheduling (Stage 3, `RtlPassKind::kSched2` ->
// `RtlPassKind::kGreg`) and pre-allocation instruction scheduling (Stage 6,
// `RtlPassKind::kSched` -> `RtlPassKind::kCombine`).
//
// Within each basic block, identifies high-latency producers (such as memory
// loads) that `schedule_block` hoisted ahead of independent single-cycle ALU or
// frame-setup instructions to fill latency slots before a shared consumer, and
// restores them to their pre-schedule positions while preserving `REG_NOTES`
// (`REG_DEAD`, `REG_UNUSED`, `REG_EQUAL`, `REG_EQUIV`).
//
// Returns an empty `RtlControlFlowGraph` if `scheduled_cfg` is empty or
// structurally invalid.
RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg);

// Inverts `sched.c:update_flow_info` after pre-allocation `try_split` in Stage
// 6 (`RtlPassKind::kSched`): re-merges split 32-bit `large_int` immediate moves
// (`(set (reg:SI R) (const_int HI))` followed by
// `(set (reg:SI R) (ior:SI (reg:SI R) (const_int LO)))`) back into a single
// `(set (reg:SI R) (const_int V))` instruction, restores `REG_NOTES` onto the
// merged instruction, and decrements `sideband.reg_n_sets[R]`.
RtlControlFlowGraph InvertUpdateFlowInfo(const RtlControlFlowGraph& scheduled_cfg);

// Inverts `function.c:thread_prologue_and_epilogue_insns` /
// `mips.c:mips_expand_prologue` in post-reload Stage 3 (`RtlPassKind::kSched2`):
// extracts stack frame size, saved GPR/FPR masks, leaf status, and
// hard-frame-pointer usage into `sideband` (`frame_size_bytes`,
// `saved_gpr_regnos`, `saved_fpr_regnos`, `frame_pointer_needed`,
// `is_leaf_function`) and strips the prologue instructions from the entry
// basic block.
RtlControlFlowGraph InvertThreadPrologueAndEpilogueInsns(const RtlControlFlowGraph& scheduled_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_INVERT_SCHEDULE_BLOCK_H_
