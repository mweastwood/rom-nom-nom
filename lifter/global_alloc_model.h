#ifndef ROM_NOM_NOM_LIFTER_GLOBAL_ALLOC_MODEL_H_
#define ROM_NOM_NOM_LIFTER_GLOBAL_ALLOC_MODEL_H_

#include <cstdint>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "core/rtl_cfg.h"
#include "lifter/mips_reg_alloc_common.h"

namespace rom_nom_nom::lifter {

struct BlockMdPseudoNeed {
  int regno = -1;
  HardRegSet required_md;
};

// Complete deterministic forward model of GCC 2.7.2 `global.c:global_alloc`
// (plus optional post-global `reload1.c` GR/MD spill + `retry_global_alloc`).
struct GlobalAllocModel {
  MipsTargetConfig mips_cfg;
  int max_regno = kFirstPseudoRegister;
  int max_allocno = 0;
  int num_blocks = 0;
  std::vector<int> reg_allocno;
  std::vector<int> local_reg_renumber;
  std::vector<RegModeInfo> pseudo_modes;
  std::vector<int> allocno_reg;
  std::vector<int> allocno_size;
  std::vector<int> allocno_calls_crossed;
  std::vector<int> allocno_n_refs;
  std::vector<int> allocno_live_length;
  std::vector<int> allocno_single_block;
  std::vector<MipsRegClass> pref_class;
  std::vector<MipsRegClass> alt_class;
  // Optional Stage 4 target post-pass `reg_renumber` per allocno (`-1` when
  // `greg_cfg` is empty; zero `pre_state` peeking). Extracted verbatim from
  // `invert_global_alloc.cc` as a temporary `regclass.c` float-vs-GR
  // tie-breaker and reload-MD-spill stand-in; slated for replacement by a pure
  // forward `regclass.c` cost model in Sub-PR 2.
  std::vector<int> target_hr;

  HardRegSet fixed_regs;
  HardRegSet call_used_regs;
  HardRegSet no_global_alloc_regs;
  HardRegSet bad_spill_regs;
  HardRegSet base_regs_used_so_far;
  std::vector<int> reg_n_refs;
  std::vector<int> base_local_reg_n_refs;
  std::vector<int> base_local_reg_live_length;

  std::vector<HardRegSet> base_hard_reg_conflicts;
  std::vector<HardRegSet> base_hard_reg_preferences;
  std::vector<HardRegSet> base_hard_reg_copy_preferences;
  std::vector<HardRegSet> base_hard_reg_full_preferences;
  std::vector<absl::btree_set<int>> allocno_md_clobber_blocks;
  std::vector<std::vector<BlockMdPseudoNeed>> block_md_pseudos;
  std::vector<HardRegSet> block_hard_md_needs;
  std::vector<std::vector<int>> non_move_read_pseudo_groups;
  // Asymmetric conflict table `conflicts[i * max_allocno + j]` matching
  // `CONFLICTP(i, j)` (`global.c:123`).
  std::vector<uint8_t> conflicts;

  bool ConflictP(int i, int j) const { return conflicts[i * max_allocno + j] != 0; }
  void SetConflict(int i, int j) { conflicts[i * max_allocno + j] = 1; }
};

struct SimResult {
  std::vector<int> allocno_hr;
  std::vector<int> reg_renumber;
  int primary_mismatches = 0;
  int secondary_mismatches = 0;
};

// Simulates GCC 2.7.2 `global.c:834-886` (`prune_preferences`) +
// `global.c:904-1195` (`find_reg`, scanning hard registers in natural MIPS
// order `0 .. 67` because `REG_ALLOC_ORDER` is undefined on MIPS at
// `global.c:964-990, 1107-1118`) and, when `simulate_reload_spills` is true,
// the subsequent `reload()` GR/MD `spill_hard_reg` + `retry_global_alloc` pass
// (`global.c:580, 1203`, `reload1.c:709, 1611, 3445`). Pure forward callers
// build `m` with an empty `greg_cfg` (`target_hr[a] == -1` for all allocnos).
SimResult SimulateGlobalAlloc(const GlobalAllocModel& m, const std::vector<int>& order,
                              bool simulate_reload_spills = true);

// Returns allocnos sorted in GCC `global.c:589-610` (`allocno_compare`) order
// (decreasing priority, breaking ties by ascending allocno index).
std::vector<int> SortAllocnoOrder(const GlobalAllocModel& m);

// Builds the deterministic `GlobalAllocModel` mirroring `global.c:277-524`
// (`global_alloc`), `global.c:614-775` (`global_conflicts`),
// `global.c:1535-1605` (`set_preference`), and `global.c:781-824`
// (`expand_preferences`). Pure forward callers pass an empty `greg_cfg` so
// `target_hr` stays `-1` (zero target peeking).
GlobalAllocModel BuildGlobalAllocModel(
    const RtlControlFlowGraph& out, const RtlControlFlowGraph& liveness_cfg,
    const RtlControlFlowGraph& greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    const HardRegSet& scratch_hard_regs);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_GLOBAL_ALLOC_MODEL_H_
