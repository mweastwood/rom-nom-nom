#ifndef LIFTER_RTL_LIVENESS_H_
#define LIFTER_RTL_LIVENESS_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

struct RtlLivenessOptions {
  // Mirrors GCC 2.7.2 `reload_completed` (`reload1.c` / `sched.c`):
  // - When `false` (pre-reload RTL, e.g., `.05.flow`..`.08.lreg`), `$30`
  //   (`FRAME_POINTER_REGNUM`) is always exit-live and fixed, and `(reg:SI 0)`
  //   acts as `ARG_POINTER_REGNUM`.
  // - When `true` (post-reload RTL, e.g., `.09.greg`..`.12.dbr`), `$0` is
  //   hardwired zero, `$30` is fixed/exit-live only when
  //   `cfg->sideband.frame_pointer_needed == true` (otherwise allocated as
  //   ordinary callee-saved `s8`), and `epilogue_delay_pattern` / `(return)`
  //   exit blocks are honored.
  bool reload_completed = false;

  // When `true`, synthesizes per-instruction `REG_DEAD` and `REG_UNUSED` notes
  // (at `profile.optimize_level > 0`, mirroring `flow.c:propagate_block` with
  // `final = 1`) or strips liveness notes (at `profile.optimize_level == 0`,
  // mirroring `stupid.c:stupid_life_analysis`), while preserving non-liveness
  // notes (`REG_EQUAL`, `REG_EQUIV`, `REG_LIBCALL`, `REG_RETVAL`, `REG_WAS_0`,
  // `REG_LABEL`, `REG_NONNEG`, `REG_INC`, `REG_NO_CONFLICT`) and never writing
  // `REG_NOTES` onto `CALL_INSN` pattern strings.
  // When `false`, leaves instruction `REG_NOTES` untouched while still
  // reconstructing missing `call_fusage`, `basic_block_live_at_start`, and
  // `RtlSidebandState` flow statistics.
  bool update_reg_notes = true;
};

// Reconstructs GCC 2.7.2 dataflow liveness and `RtlSidebandState` flow
// statistics (`max_regno`, `basic_block_live_at_start`, `reg_n_refs`,
// `reg_n_sets`, `reg_n_deaths`, `reg_live_length`, `reg_n_calls_crossed`, and
// missing `CALL_INSN` `call_fusage`) on `cfg` using structured `RtlExpr` ASTs.
//
// Selects between `flow.c:life_analysis` (`profile.optimize_level > 0`) and
// `stupid.c:stupid_life_analysis` (`profile.optimize_level == 0`) strictly via
// `profile`.
void ReconstructRtlLiveness(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                            const RtlLivenessOptions& options = {});

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_RTL_LIVENESS_H_
