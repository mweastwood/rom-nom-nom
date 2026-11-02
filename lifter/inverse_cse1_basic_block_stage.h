#ifndef LIFTER_INVERSE_CSE1_BASIC_BLOCK_STAGE_H_
#define LIFTER_INVERSE_CSE1_BASIC_BLOCK_STAGE_H_

#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Stage 11 of the inverse compiler pipeline (`InverseCse1BasicBlockStage`).
//
// Inverts GCC 2.7.2 Stage 2 (`toplev.c:rest_of_compilation` `0x08071cd6` ->
// `cse.c:cse_main` `0x0810de2e`, `cse_basic_block` `0x0810e305`, `cse_insn`
// `0x0810a5e9`, `canon_reg` `0x08106208`, `fold_rtx` `0x081075bd`,
// `delete_dead_from_cse` `0x0810da93`, and `jump.c:jump_optimize` `0x080f8772`,
// `thread_jumps` `0x080fbbf7`) to transform a `.cse` `RtlControlFlowGraph`
// into a canonical `.jump` `RtlControlFlowGraph`.
//
// Specifically, this stage:
// 1. Reverses `jump.c:thread_jumps` (`0x080fbbf7`) when `flag_thread_jumps`
//    runs after `cse_main` (`toplev.c` `0x08071dac`), unthreading conditional
//    branch targets that bypassed an intermediate identical-condition branch.
// 2. Reverses constant-folded preheader loop-entry guards (`stmt.c:expand_end_loop`
//    -> `cse.c:cse_insn` `0x0810c65c` -> `jump.c:jump_optimize` `0x080f8772`),
//    restoring the conditional branch and comparison instructions in constant-0
//    preheader blocks.
// 3. Reverses `cse_basic_block` (`0x0810e305`) + `delete_dead_from_cse`
//    (`0x0810da93`) instruction deletions across basic blocks and extended basic
//    blocks, including `expmed.c:expand_mult` leading temporary copies before
//    constant-multiply `sll` shifts, `expmed.c:emit_store_flag` folded constants
//    and dead comparisons, and intra-block / extended-basic-block CSE of
//    duplicate address calculations, loads, and narrow `zero_extend` /
//    `sign_extend` operations.
class InverseCse1BasicBlockLifter {
 public:
  static void InvertCseToJumpRtlCfg(RtlControlFlowGraph* cfg);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_CSE1_BASIC_BLOCK_STAGE_H_
