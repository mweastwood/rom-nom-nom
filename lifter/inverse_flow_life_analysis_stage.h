#ifndef LIFTER_INVERSE_FLOW_LIFE_ANALYSIS_STAGE_H_
#define LIFTER_INVERSE_FLOW_LIFE_ANALYSIS_STAGE_H_

#include "core/compiler_flags.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Stage 8 of the inverse compiler pipeline (`InverseFlowLifeAnalysisStage`).
//
// Inverts GCC 2.7.2 Stage 5 (`flow.c:flow_analysis` / `flow.c:life_analysis` /
// `flow.c:propagate_block` / `flow.c:insn_dead_p`) to transform a `.flow` `RtlControlFlowGraph`
// into a canonical `.cse2` `RtlControlFlowGraph` by restoring instructions that `flow.c` deleted
// as dead (`NOTE_INSN_DELETED`):
//   1. Store-flag / if-conversion speculative return-register default initializations
//      (`(set (reg/i:SI 2 v0) (const_int 1/-1))`) left dead by `jump.c` and `cse.c`;
//   2. Loop strength-reduction (`loop.c:strength_reduce`) unreduced initial BIV shift instructions
//      (`(set (reg:SI r_shift) (ashift:SI r_biv (const_int k)))`), dead compound-stride
//      intermediate GIV initializations and increments, `check_dbra_loop` reversed-loop dead
//      BIV initializations/post-loop final-value assignments, and hoisted `symbol_ref` movable
//      copies;
//   3. Dead pre-branch dividend copies (`expmed.c:expand_divmod`), duplicate quotient copies
//      before remainder subtraction (`total % 240`), and post-branch CSE-forwarded zero-extension
//      copies;
//   4. Dead entry variable initializations (`s32 res = 0;`) overwritten across all arms of a
//      `switch` dispatch before reaching the common join block.
class InverseFlowLifeAnalysisLifter {
 public:
  static void InvertFlowToCse2RtlCfg(RtlControlFlowGraph* cfg,
                                     const ParsedCompilerFlags* flags = nullptr);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_FLOW_LIFE_ANALYSIS_STAGE_H_
