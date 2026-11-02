#ifndef LIFTER_INVERSE_JUMP1_CFG_CLEANUP_STAGE_H_
#define LIFTER_INVERSE_JUMP1_CFG_CLEANUP_STAGE_H_

#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Stage 12 of the inverse compiler pipeline (`InverseJump1CfgCleanupStage`).
//
// Inverts GCC 2.7.2 Stage 1 (`toplev.c:rest_of_compilation` `0x08071bce` ->
// `jump.c:jump_optimize` `0x080f7c54`, `duplicate_loop_exit_test` `0x080f8bcd`,
// `follow_jumps` `0x080fb2cc`, `delete_jump` `0x080fba54`, `invert_jump`
// `0x080fbe04`, and `delete_computation` `0x080fb72c`) to transform a `.jump`
// `RtlControlFlowGraph` into a canonical `.rtl` `RtlControlFlowGraph` matching
// the RTL emitted by `expand_function_start` / `expand_stmt` /
// `expand_function_end` + `instantiate_virtual_regs`.
class InverseJump1CfgCleanupLifter {
 public:
  static void InvertJumpToRtlCfg(RtlControlFlowGraph* cfg);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_JUMP1_CFG_CLEANUP_STAGE_H_
