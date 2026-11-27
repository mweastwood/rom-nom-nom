#ifndef ROM_NOM_NOM_LIFTER_FUZZ_TEST_UTILS_H_
#define ROM_NOM_NOM_LIFTER_FUZZ_TEST_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/functional/function_ref.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// Selects `count` stratified corpus case indices from
// `GetSharedRoundTripTestCases()`, evenly covering all 7 compilation profiles
// in `GetRoundTripCompilerFlagProfiles()`. Strictly deterministic.
std::vector<size_t> GetStratifiedCorpusCaseIndices(int count);

// Returns the smallest unused pseudo-register number (`>= kMipsFirstUserPseudoRegister`)
// not appearing in `cfg` or `cfg.sideband.max_regno`.
int NextFreshPseudo(const RtlControlFlowGraph& cfg);

// Formats a parsed `RtlPatternAndNotes` back to GCC 2.7.2 `print-rtl.c` syntax.
std::string FormatPn(const RtlPatternAndNotes& pn);

// Formats a single `(reg:M regno)` atom matching GCC 2.7.2 MIPS `DEBUG_REGISTER_NAMES`
// (`config/mips/mips.h` reg 30 prints as `$fp`).
std::string FormatRegAtom(RtlMachineMode mode, int regno);

// Collects every hard register (including multi-register `DI`/`DF` spans)
// referenced in `cfg` instructions, notes, `call_fusage`, `basic_block_live_at_start`,
// saved register lists, or `epilogue_delay_pattern`.
absl::btree_set<int> CollectAllUsedHardRegsInCfg(const RtlControlFlowGraph& cfg,
                                                 const CompilationProfile& profile);

// Allocates up to `count` free caller-saved GPRs (`$t0..$t9`) not in `used`.
std::vector<int> PickFreeCallerSavedGprs(const absl::btree_set<int>& used, int count);

// Allocates up to `count` free caller-saved even FPR pairs (`$f4..$f10, $f16, $f18`)
// not overlapping `used`.
std::vector<int> PickFreeCallerSavedFprs(const absl::btree_set<int>& used, int count);

// Allocates up to `count` free callee-saved GPRs (`$s0..$s7`) not in `used`.
std::vector<int> PickFreeCalleeSavedGprs(const absl::btree_set<int>& used, int count);

// Allocates up to `count` free callee-saved even FPR pairs (`$f20..$f30`)
// not overlapping `used`.
std::vector<int> PickFreeCalleeSavedFprs(const absl::btree_set<int>& used, int count);

// Inserts `to_insert` into `blk` at index `pos` (clamped), keeping
// `blk->instructions` and `blk->rtl_insns` synchronized.
void InsertInsnsAt(RtlBasicBlock* blk, size_t pos, absl::Span<const RtlInsn> to_insert);

// Constructs a synthetic `RtlInsn` with loop-note classification and optional
// explicit `CALL_INSN_FUNCTION_USAGE`.
RtlInsn MakeRtlInsn(std::string pattern, std::vector<RtlExpr> call_fusage = {},
                    bool has_explicit_fusage = false);

// Recomputes consistent `flow.c` liveness / `REG_DEAD` / `sideband` after
// structural mutations while preserving any post-reload `REG_DEAD (mem ...)` notes.
void RefreshLivenessPreservingMemDeadNotes(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile, bool reload_completed,
                                           bool update_reg_notes);

// Alpha-renames user pseudo-registers (`>= kMipsFirstUserPseudoRegister`) in a
// pre-reload `cfg` (instructions, notes, and sideband maps). Returns true if
// any pseudo was renamed.
bool ApplyPseudoAlphaRename(RtlControlFlowGraph* cfg, std::mt19937_64* rng);

// Alpha-renames eligible caller/callee hard registers in a post-reload `cfg`
// strictly respecting even `DI`/`DF`/FPR pairs and pinned ABI/prologue/`/i`/
// `call_fusage`/`epilogue_delay_pattern` registers. Returns true if renamed.
bool ApplyPostReloadHardRegAlphaRename(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                                       std::mt19937_64* rng);

// Oracle F4-C greedy CFG shrinker: tries collapsing multi-block CFGs to block 0,
// then greedily deletes coarse (4-insn) and single-insn chunks while `still_fails`
// continues returning true.
RtlControlFlowGraph GreedyShrinkRtlCfg(
    const RtlControlFlowGraph& failing_cfg, const CompilationProfile& profile,
    bool reload_completed, absl::FunctionRef<bool(const RtlControlFlowGraph&)> still_fails,
    int max_passes = 6, int max_checks = 96);

// Oracle F4-C greedy Stage-0 machine-word shrinker: greedily deletes contiguous
// non-branch-target word chunks while preserving delay-slot pairing and updating
// relative branch offsets, relocation `word_index`es, and jump-table
// `target_word_indices`.
Stage0FunctionInput GreedyShrinkStage0Input(
    const Stage0FunctionInput& failing_input,
    absl::FunctionRef<bool(const Stage0FunctionInput&)> still_fails, int max_passes = 6,
    int max_checks = 96);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_FUZZ_TEST_UTILS_H_
