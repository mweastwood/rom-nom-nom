#ifndef LIFTER_CUMULATIVE_LIFT_CHAIN_H_
#define LIFTER_CUMULATIVE_LIFT_CHAIN_H_

#include <string>

#include "absl/container/flat_hash_map.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Result of lifting a `Stage0FunctionInput` in a single O(S) pass through
// inverse stages 0..S (`kDbr` -> `kJump2` -> `kSched2` -> `kGreg` -> `kLreg`
// -> `kSched` -> `kCombine`, extensible to Stages 7..13).
struct CumulativeLiftChain {
  // Effective compilation profile (including linked-ROM `$gp` symbol sizes
  // inferred from `vram_to_symbol` / `vram_to_symbol_size`, matching
  // `RightInverseVerifier::VerifySuffixRightInverse`).
  CompilationProfile effective_profile;

  // Lifted CFG snapshot after each stage up to `deepest_pass`.
  absl::flat_hash_map<RtlPassKind, RtlControlFlowGraph> stages;
  RtlPassKind deepest_pass = RtlPassKind::kDbr;
  std::string failure_stage;

  bool Ok() const { return failure_stage.empty() && stages.contains(deepest_pass); }
};

// Reconstructs the effective `CompilationProfile` for `input_x`, matching
// `RightInverseVerifier::VerifySuffixRightInverse` on both relocatable `.o`
// and fixed-VRAM linked-ROM inputs.
CompilationProfile BuildEffectiveStage0Profile(const Stage0FunctionInput& input_x);

// Lifts `input_x` in a single O(S) pass from Stage 0 (`.12.dbr`) up to
// `target_pass`, applying active-stage gating identical to GCC 2.7.2 and
// `RightInverseVerifier::VerifySuffixRightInverse`.
CumulativeLiftChain LiftCumulativeChain(const Stage0FunctionInput& input_x,
                                        RtlPassKind target_pass);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_CUMULATIVE_LIFT_CHAIN_H_
