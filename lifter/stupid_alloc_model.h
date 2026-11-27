#ifndef ROM_NOM_NOM_LIFTER_STUPID_ALLOC_MODEL_H_
#define ROM_NOM_NOM_LIFTER_STUPID_ALLOC_MODEL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// Prologue and top-of-frame spill metadata extracted from block 0 at `-O0`
// (`config/mips/mips.c` `mips_expand_prologue`, `reload1.c:2320-2395`
// `alter_reg(..., -1)`).
struct O0PrologueInfo {
  size_t prologue_end = 0;
  bool frame_pointer_needed = false;
  int local_frame_bytes = 0;
  int outgoing_args_bytes = 0;
  absl::btree_set<int> valid_top_spill_slot_bases;
};

// Per-pseudo SUID lifetime summary mirroring `stupid.c:157-168, 464-540`
// (`reg_where_born`, `reg_where_dead`, `reg_n_refs`, `reg_n_calls_crossed`,
// `regs_change_size`).
struct O0PseudoSuidSpan {
  int born = std::numeric_limits<int>::max();
  int dead = -1;
  int n_refs = 0;
  int n_calls = 0;
  size_t birth_b = 0;
  size_t birth_i = 0;
  RtlMachineMode mode = RtlMachineMode::kSi;
  bool changes_size = false;
  bool live_flag = false;
  bool has_explicit_use_insn = false;
  bool is_entry_param = false;
};

// SUID timeline and `after_insn_hard_regs` matrix built by backward scan
// mirroring `stupid.c:142-239` (`stupid_life_analysis`).
struct O0StupidTimeline {
  int exit_suid = 1;
  int b0_prefix_suid = 1;
  std::vector<int> call_suids;
  std::vector<std::array<bool, 68>> after_hr;
  absl::btree_map<int, O0PseudoSuidSpan> spans;
};

// Classification of `obey_regdecls` scope `(use (reg:M hr))` markers emitted
// by `stmt.c:3266-3272, 3498-3499` (`expand_end_bindings` / `expand_decl`)
// and `function.c:5152-5158, 5320-5336`.
enum class O0ScopeKind { kEntryParam, kB0Local, kBirthLocal };

// Deterministic ordering of `obey_regdecls` scope markers at function entry,
// block-0 declaration prefix, and per-variable birth points.
struct O0OrderedScopes {
  std::vector<int> entry_params;
  std::vector<int> b0_locals;
  std::vector<int> birth_locals;
  absl::btree_map<int, int> index_in_list;
};

// Trial pre-`stupid.c` pseudo-register model reconstructed from post-`final`
// `-O0` hard-register RTL blocks.
struct TrialO0Model {
  std::vector<std::vector<RtlPatternAndNotes>> blocks;
  std::vector<std::vector<size_t>> orig_idx;
  absl::btree_map<int, int> reg_renumber;
  absl::btree_map<int, RtlMachineMode> pseudo_modes;
};

// Result of solving `obey_regdecls` scope `(use ...)` placement against
// `SimulateO0StupidAlloc`.
struct ScopeSolveResult {
  bool matched = false;
  int hidden_sret_pseudo = -1;
  size_t b0_param_prefix_end = 0;
  O0StupidTimeline tl;
  absl::btree_map<int, O0ScopeKind> scope_kinds;
  O0OrderedScopes ord;
};

// Shared `-O0` MIPS ABI & pattern predicates (`config/mips/mips.h:1186-1210,
// 1915-1917`).
bool IsO0OutgoingCallArgHardReg(int hr);
bool IsMipsO0CallUsedHardReg(int hr);
bool IsMipsO0CalleeSavedRange(int hr, int nw);
bool IsO0FixDiLibcall(const RtlExpr& call_set);
bool IsO0FpInGprFallback(const O0PseudoSuidSpan& sp, int exp_hr);
std::optional<int64_t> MatchO0RegPlusConstOffset(const RtlExpr& addr, int base_reg);

// Reconstructs `final.c:1933, 2095-2107` (`alter_subreg`) collapsed
// `(subreg:SI (reg:DI hr) 0/1)` subword accesses inside single blocks and
// `optabs.c:2485-2525` (`emit_no_conflict_block`) sequences.
void WrapO0SingleBlockDiSubregs(size_t prologue_end,
                                std::vector<std::vector<RtlPatternAndNotes>>* blocks);

// Orders `obey_regdecls` scope markers deterministically for `ComputeO0PostScopeSpan`
// and `SimulateO0StupidAlloc`.
O0OrderedScopes OrderO0Scopes(const O0StupidTimeline& tl,
                              const absl::btree_map<int, int>& exp_renumber,
                              const absl::btree_map<int, O0ScopeKind>& scope_kinds);

// Computes effective `(born, dead, alloc_len)` for pseudo `p` after inserting
// `scope_kinds` `obey_regdecls` `(use ...)` markers (`stmt.c:3266-3272,
// 3498-3499`, `stupid.c:287-306`).
void ComputeO0PostScopeSpan(const O0StupidTimeline& tl,
                            const absl::btree_map<int, O0ScopeKind>& scope_kinds,
                            const O0OrderedScopes& ord, int p, int* eff_born, int* eff_dead,
                            int* alloc_len);

// Simulates GCC 2.7.2 `-O0` `stupid_life_analysis` (`stupid.c:243-277`,
// `stupid_reg_compare` `287-306`, `stupid_find_reg` `323-405`) on `tl` with
// `scope_kinds` applied, returning the deterministic `pseudo -> hard_reg` map.
absl::btree_map<int, int> SimulateO0StupidAlloc(
    bool gp64, bool fp64, const O0StupidTimeline& tl, const absl::btree_map<int, int>& exp_renumber,
    const absl::btree_map<int, O0ScopeKind>& scope_kinds, const O0OrderedScopes& ord);

// Folds `-O0` `mulsidi3` top-of-frame 8-byte `$fp` spills and `reload1.c`
// (`emit_reload_insns`, `SECONDARY_INPUT_RELOAD_CLASS`, earlyclobber `=&d`,
// `"f"` constraint) scratch moves in `model` so `SimulateO0StupidAlloc` sees
// pre-reload live spans.
void FoldTrialO0ReloadScratchesAndMulsidi3Spills(const O0PrologueInfo& pinfo, bool gp64,
                                                 int* next_p, TrialO0Model* model);

// Reconstructs a trial pre-`stupid.c` pseudo-register model from post-`final`
// `-O0` hard-register blocks by partitioning non-ABI hard-register webs via
// forward reaching definitions, coalescing intra-block RMW webs, and folding
// reload scratches.
TrialO0Model BuildTrialO0PseudoModel(
    const RtlControlFlowGraph& cfg, const O0PrologueInfo& pinfo, bool gp64, bool fp64,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_blocks);

// Runs the backward SUID scan mirroring `stupid.c:142-239, 412-558`
// (`stupid_life_analysis` + `stupid_mark_refs`) on `model`.
O0StupidTimeline BuildO0StupidTimeline(bool gp64, bool fp64, const TrialO0Model& model,
                                       size_t b0_param_prefix_end,
                                       const absl::btree_set<int>& b0_entry_param_cands);

// When FP registers were exhausted and spilled into callee-saved GPRs, merges
// duplicate trial pseudos sharing the same callee-saved FPR (`$f20..$f30`)
// and recompacts pseudo numbers in ascending hard-register order.
void CoalesceTrialExhaustedCalleeSavedFprs(TrialO0Model* model);

// Solves `obey_regdecls` scope `(use ...)` placement on `model` so
// `SimulateO0StupidAlloc` reproduces `model.reg_renumber` (`stmt.c:3266-3272,
// 3498-3499`, `stupid.c:243-405`).
ScopeSolveResult SolveO0ScopesOnModel(bool gp64, bool fp64, const TrialO0Model& model);

// Reconstructs `obey_regdecls` scope-boundary `(use (reg:M hr))` markers,
// hidden `sret` exit `(set (reg/i:SI 2) (reg:SI 2))` + `(use (reg/i:SI 2))`,
// and `reload1.c:3606-3700` (`order_regs_for_reload`) unused `movstrsi`
// scratch clobbers on `all_blocks`.
void ReconstructO0ObeyRegdeclsScopeUses(const RtlControlFlowGraph& cfg, const O0PrologueInfo& pinfo,
                                        bool gp64, bool fp64,
                                        std::vector<std::vector<RtlPatternAndNotes>>* all_blocks);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_STUPID_ALLOC_MODEL_H_
