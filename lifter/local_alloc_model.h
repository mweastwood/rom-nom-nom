#ifndef ROM_NOM_NOM_LIFTER_LOCAL_ALLOC_MODEL_H_
#define ROM_NOM_NOM_LIFTER_LOCAL_ALLOC_MODEL_H_

#include <cstddef>
#include <optional>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "core/rtl_cfg.h"
#include "lifter/mips_reg_alloc_common.h"

namespace rom_nom_nom::lifter {

// Mirrors `local-alloc.c:1579-1595` (`qty_compare`) priority formula:
//   `((double)(floor_log2(qty_n_refs) * qty_n_refs * qty_size) / span_len) * 10000`.
int ComputeLocalQtyPriority(int n_refs, int span_len, int qty_size);

// Mirrors `MODES_TIEABLE_P` (`config/mips/mips.h:1280-1284`): float/complex-float
// modes may only tie with float/complex-float modes.
bool ModesTieableForLocalAlloc(const RegModeInfo& m1, const RegModeInfo& m2);

// One local-alloc quantity (`local-alloc.c:149-209`), representing either a
// coalesced chain of block-local pseudo registers (`first_reg >= kFirstPseudoRegister`)
// or a `match_scratch` temporary (`first_reg == -1`, `local-alloc.c:306-389`).
struct LocalBlockQty {
  int qty_id = 0;
  int first_reg = -1;
  std::vector<int> member_regs;
  std::vector<int> member_offsets;
  RegModeInfo mode;
  int size = 1;
  int birth = -1;
  int death = -1;
  int calls_crossed = 0;
  MipsRegClass min_class = MipsRegClass::kGrRegs;
  MipsRegClass alt_class = MipsRegClass::kNoRegs;
  int n_refs = 1;
  HardRegSet phys_copy_sugg;
  int num_copy_sugg = 0;
  HardRegSet phys_sugg;
  int num_sugg = 0;
};

// Complete per-block deterministic forward model of GCC 2.7.2 `local-alloc.c:block_alloc`.
struct LocalBlockAllocModel {
  MipsTargetConfig mips_cfg;
  HardRegSet fixed_regs;
  HardRegSet call_used_regs;
  std::vector<HardRegSet> base_regs_live_at;
  std::vector<LocalBlockQty> qtys;
  std::vector<int> eligible_pseudos;
  std::vector<int> reg_offset;
  // Optional Stage 4 target post-pass `reg_renumber` per pseudo (`-1` when
  // absent; zero `pre_state` peeking). Pure forward callers leave
  // `dst_out->sideband.reg_renumber` empty so `target_hr == -1`. Extracted
  // verbatim from `invert_global_alloc.cc` as a temporary `regclass.c`
  // float-vs-GR tie-breaker stand-in; slated for replacement by a pure
  // `regclass.c` cost model in Sub-PR 2.
  absl::btree_map<int, int> target_hr_by_pseudo;
};

// Simulates GCC 2.7.2 `local-alloc.c:1435-1565` (`block_alloc` Pass 1
// suggested-reg pass via `qty_sugg_compare` at `local-alloc.c:1633-1659` +
// Pass 2 priority pass via `qty_compare` at `local-alloc.c:1579-1595` +
// `find_free_reg` at `local-alloc.c:2073-2217`, scanning hard registers in
// natural MIPS order `0 .. 67` because `REG_ALLOC_ORDER` is undefined on MIPS).
// Returns the number of `eligible_pseudos` whose simulated hard register
// differs from `target_hr_by_pseudo` (0 when `target_hr_by_pseudo` entries are
// `-1`, i.e. pure forward mode). If `sim_hr_out != nullptr`, writes the
// deterministic `pseudo -> hard_reg` map (`-1` if spilled).
int SimulateLocalBlockAlloc(const LocalBlockAllocModel& m,
                            const std::vector<int>* custom_pass2_order,
                            absl::btree_map<int, int>* sim_hr_out = nullptr);

// Builds the deterministic `LocalBlockAllocModel` for basic block `b`,
// modeling `local-alloc.c:472-474` eligibility, `local-alloc.c:1211-1297`
// operand tying & `combine_regs` (`local-alloc.c:1722-1875`),
// `local-alloc.c:1317-1352, 2282-2312` `REG_NO_CONFLICT` libcall combining,
// `local-alloc.c:306-389` `alloc_qty_for_scratch`, and
// `local-alloc.c:1369-1415, 1976-2065` liveness intervals.
// Returns `std::nullopt` if block `b` has no eligible local pseudos, fails RTL
// parsing, or contains an unclosed quantity interval (`birth < 0 || death < birth`).
std::optional<LocalBlockAllocModel> BuildSingleBlockLocalAllocModel(
    size_t b, RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::btree_set<int>& live_at_any_block_start,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    bool bump_caller_saves_refs);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_LOCAL_ALLOC_MODEL_H_
