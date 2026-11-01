#ifndef LIFTER_INVERSE_COMBINE_STAGE_H_
#define LIFTER_INVERSE_COMBINE_STAGE_H_

#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Stage 7 of the inverse compiler pipeline (`InverseCombineStage`).
//
// Inverts GCC 2.7.2 Stage 6 (`combine.c:combine_instructions`) to transform a `.combine`
// `RtlControlFlowGraph` into a canonical `.flow` `RtlControlFlowGraph` by un-combining:
//   1. `function.c:assign_parms` entry parameter hard-register to pseudo-register moves that
//      `combine.c` coalesced into downstream uses;
//   2. Subword memory loads (`lb`, `lh`, `lbu`, `lhu`) combined with `sign_extend`/`zero_extend`
//      or bitfield extraction (`ashift` + `ashiftrt`/`lshiftrt`);
//   3. Symbol-indexed memory accesses (`kGasSymbolIndexedAccess`), small-offset symbol address
//      calculations (`kGasAddressHiLo`), and division-with-trap (`kDivisionWithTrap`) expansions;
//   4. Narrow (`QI`/`HI`) subreg arithmetic and bitmask operations (`addiu` + `andi 0xff/0xffff`);
//   5. Pre-call argument hard-register copies (`$a0..$a3`, `$f12..$f14`) and post-call return
//      register copies (`$v0`, `$f0`) coalesced by `combine.c`;
//   6. Function return-register copies `(set (reg/i:M $v0/$f0) (reg:M pseudo))` coalesced into the
//      final defining instruction of a return-computing block.
class InverseCombineLifter {
 public:
  static void InvertCombineToFlowRtlCfg(RtlControlFlowGraph* cfg);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_COMBINE_STAGE_H_
