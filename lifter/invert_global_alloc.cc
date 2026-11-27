#include "lifter/invert_global_alloc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/global_alloc_model.h"
#include "lifter/local_alloc_model.h"
#include "lifter/mips_reg_alloc_common.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

// Recursively validates all `(reg ...)` nodes in `expr` and updates `max_regno`.
absl::Status ValidateRtlExprRegisters(const RtlExpr& expr, int* max_regno) {
  if (expr.op == RtlOp::kReg) {
    if (expr.int_val < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid negative register number %d", expr.int_val));
    }
    if (max_regno != nullptr && !expr.IsArgPointerReg()) {
      *max_regno = std::max(*max_regno, expr.RegNo() + 1);
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (absl::Status st = ValidateRtlExprRegisters(child, max_regno); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

int MinLiveLengthForDensityCap(int refs, double max_density) {
  if (max_density <= 0.0 || refs <= 0) {
    return 1;
  }
  int l_min = std::max(1, static_cast<int>(std::floor(static_cast<double>(refs) / max_density)));
  while (static_cast<double>(refs) / l_min > max_density) {
    ++l_min;
  }
  return l_min;
}

// Computes the contiguous valid `[valid_lo, valid_hi]` range of `reg_n_refs`
// for fixed `live_length == orig_len` (`orig_len > 0`), preserving
// `CALLER_SAVE_PROFITABLE` parity (`global.c:1085`), local-eviction density cap
// (`global.c:1115-1156`), and 32-bit signed `int` priority bounds
// (`global.c:589-608`, `allocno_compare`). Used under `flag_schedule_insns`
// (`sched.c:4911-4966`), where `sched1` unconditionally overwrites positive
// `reg_live_length[r]` while preserving `reg_n_refs[r]`.
bool ComputeValidLockedLengthRefBounds(int orig_refs, int orig_len, int size, int calls_crossed,
                                       bool preserve_cs_prof, double max_density, bool lock_refs,
                                       int* valid_lo, int* valid_hi) {
  if (orig_len <= 0) {
    return false;
  }
  const int eff_size = std::max(1, size);
  const bool orig_cs_prof =
      preserve_cs_prof && calls_crossed > 0 && (4 * calls_crossed < orig_refs);
  int lo = lock_refs ? std::max(1, orig_refs) : 1;
  int hi =
      lock_refs ? std::max(1, orig_refs) : std::max({orig_refs * 64, 16384, 4 * calls_crossed + 8});
  if (preserve_cs_prof && calls_crossed > 0) {
    if (orig_cs_prof) {
      lo = std::max(lo, 4 * calls_crossed + 1);
    } else {
      hi = std::min(hi, 4 * calls_crossed);
    }
  }
  if (max_density > 0.0) {
    int dens_hi = static_cast<int>(std::floor(max_density * orig_len));
    while (dens_hi > 0 && static_cast<double>(dens_hi) / orig_len > max_density) {
      --dens_hi;
    }
    hi = std::min(hi, dens_hi);
  }
  // Cap `hi` so `(double)(FloorLog2(r) * r) / orig_len * 10000.0 * eff_size`
  // stays safely below `INT_MAX` (`global.c:594-601`, `allocno_compare`).
  auto fits_int_priority = [&](int r) {
    const double val = (static_cast<double>(FloorLog2(r)) * r / orig_len) * 10000.0 * eff_size;
    return val <= 2.0e9;
  };
  if (lo > hi || !fits_int_priority(lo)) {
    return false;
  }
  if (!fits_int_priority(hi)) {
    int b_lo = lo;
    int b_hi = hi;
    while (b_lo < b_hi) {
      const int mid = b_lo + (b_hi - b_lo + 1) / 2;
      if (fits_int_priority(mid)) {
        b_lo = mid;
      } else {
        b_hi = mid - 1;
      }
    }
    hi = b_lo;
  }
  if (lo > hi) {
    return false;
  }
  *valid_lo = lo;
  *valid_hi = hi;
  return true;
}

bool FindBestRefsAndLengthInPriorityRange(int orig_refs, int orig_len, int size, int calls_crossed,
                                          bool caller_saves, double max_density, int min_pri,
                                          int max_pri, int* out_refs, int* out_len,
                                          bool lock_refs = false, bool lock_len = false) {
  if (min_pri > max_pri) {
    return false;
  }
  const bool orig_cs_prof = caller_saves && calls_crossed > 0 && (4 * calls_crossed < orig_refs);
  const int orig_min_len = MinLiveLengthForDensityCap(orig_refs, max_density);
  const int cur_pri = ComputeAllocnoPriority(orig_refs, orig_len, size);
  if (orig_len >= orig_min_len && cur_pri >= min_pri && cur_pri <= max_pri) {
    *out_refs = orig_refs;
    *out_len = orig_len;
    return true;
  }

  if (lock_len && orig_len > 0) {
    int lo = 0;
    int hi = 0;
    if (!ComputeValidLockedLengthRefBounds(orig_refs, orig_len, size, calls_crossed, caller_saves,
                                           max_density, lock_refs, &lo, &hi)) {
      return false;
    }
    if (ComputeAllocnoPriority(hi, orig_len, size) < min_pri ||
        ComputeAllocnoPriority(lo, orig_len, size) > max_pri) {
      return false;
    }
    // Find smallest `r` in `[lo, hi]` with priority `>= min_pri`.
    int r_lo = lo;
    int r_hi = hi;
    while (r_lo < r_hi) {
      const int mid = r_lo + (r_hi - r_lo) / 2;
      if (ComputeAllocnoPriority(mid, orig_len, size) >= min_pri) {
        r_hi = mid;
      } else {
        r_lo = mid + 1;
      }
    }
    const int first_ok = r_lo;
    if (ComputeAllocnoPriority(first_ok, orig_len, size) > max_pri) {
      return false;
    }
    // Find largest `r` in `[first_ok, hi]` with priority `<= max_pri`.
    r_lo = first_ok;
    r_hi = hi;
    while (r_lo < r_hi) {
      const int mid = r_lo + (r_hi - r_lo + 1) / 2;
      if (ComputeAllocnoPriority(mid, orig_len, size) <= max_pri) {
        r_lo = mid;
      } else {
        r_hi = mid - 1;
      }
    }
    const int last_ok = r_lo;
    *out_refs = std::clamp(orig_refs, first_ok, last_ok);
    *out_len = orig_len;
    return true;
  }

  const int r_min = lock_refs ? std::max(1, orig_refs) : 1;
  const int max_refs_search =
      lock_refs ? std::max(1, orig_refs) : std::max({orig_refs * 4, 64, 4 * calls_crossed + 8});

  int best_cost = std::numeric_limits<int>::max();
  bool found = false;
  for (int r = r_min; r <= max_refs_search; ++r) {
    if (caller_saves && calls_crossed > 0 && ((4 * calls_crossed < r) != orig_cs_prof)) {
      continue;
    }
    const int r_cost = 8 * std::abs(r - orig_refs);
    if (r_cost >= best_cost) {
      continue;
    }
    const int min_len_r = MinLiveLengthForDensityCap(r, max_density);
    const int max_len_search = std::max({orig_len * 16, 8192, min_len_r + 64});
    const double num = static_cast<double>(FloorLog2(r) * r) * 10000.0 * std::max(1, size);
    if (num == 0.0) {
      // `r == 1` produces priority `0` for all `len >= 1`.
      if (0 >= min_pri && 0 <= max_pri) {
        const int l = std::max({1, orig_len, min_len_r});
        const int cost = r_cost + std::abs(l - orig_len);
        if (cost < best_cost) {
          best_cost = cost;
          *out_refs = r;
          *out_len = l;
          found = true;
        }
      }
      continue;
    }
    int l_lo = min_len_r;
    if (max_pri > 0) {
      l_lo = std::max(l_lo, static_cast<int>(std::floor(num / (max_pri + 1.0))));
    }
    int l_hi = max_len_search;
    if (min_pri > 0) {
      l_hi = std::min(max_len_search, static_cast<int>(std::ceil(num / min_pri)) + 2);
    }
    for (int l = l_lo; l <= l_hi; ++l) {
      const int pri = ComputeAllocnoPriority(r, l, size);
      if (pri < min_pri) {
        break;
      }
      if (pri <= max_pri) {
        const int cost = r_cost + std::abs(l - orig_len);
        if (cost < best_cost) {
          best_cost = cost;
          *out_refs = r;
          *out_len = l;
          found = true;
        }
        if (l >= orig_len) {
          break;
        }
      }
    }
  }
  return found;
}

bool FindLowestPriorityAtLeast(int orig_refs, int orig_len, int size, int calls_crossed,
                               bool caller_saves, double max_density, int min_pri, int* out_refs,
                               int* out_len, bool lock_refs = false, bool lock_len = false) {
  if (lock_len && orig_len > 0) {
    int lo = 0;
    int hi = 0;
    if (!ComputeValidLockedLengthRefBounds(orig_refs, orig_len, size, calls_crossed, caller_saves,
                                           max_density, lock_refs, &lo, &hi)) {
      return false;
    }
    if (ComputeAllocnoPriority(hi, orig_len, size) < min_pri) {
      return false;
    }
    int r_lo = lo;
    int r_hi = hi;
    while (r_lo < r_hi) {
      const int mid = r_lo + (r_hi - r_lo) / 2;
      if (ComputeAllocnoPriority(mid, orig_len, size) >= min_pri) {
        r_hi = mid;
      } else {
        r_lo = mid + 1;
      }
    }
    const int first_ok = r_lo;
    const int target_pri = ComputeAllocnoPriority(first_ok, orig_len, size);
    r_lo = first_ok;
    r_hi = hi;
    while (r_lo < r_hi) {
      const int mid = r_lo + (r_hi - r_lo + 1) / 2;
      if (ComputeAllocnoPriority(mid, orig_len, size) == target_pri) {
        r_lo = mid;
      } else {
        r_hi = mid - 1;
      }
    }
    *out_refs = std::clamp(orig_refs, first_ok, r_lo);
    *out_len = orig_len;
    return true;
  }

  const bool orig_cs_prof = caller_saves && calls_crossed > 0 && (4 * calls_crossed < orig_refs);
  const int r_min = lock_refs ? std::max(1, orig_refs) : 1;
  const int max_refs_search =
      lock_refs ? std::max(1, orig_refs) : std::max({orig_refs * 4, 64, 4 * calls_crossed + 8});

  int best_pri = std::numeric_limits<int>::max();
  int best_cost = std::numeric_limits<int>::max();
  bool found = false;
  for (int r = r_min; r <= max_refs_search; ++r) {
    if (caller_saves && calls_crossed > 0 && ((4 * calls_crossed < r) != orig_cs_prof)) {
      continue;
    }
    const int min_len_r = MinLiveLengthForDensityCap(r, max_density);
    const int max_len_search = std::max({orig_len * 16, 8192, min_len_r + 64});
    const int r_cost = 8 * std::abs(r - orig_refs);
    const double num = static_cast<double>(FloorLog2(r) * r) * 10000.0 * std::max(1, size);
    if (num == 0.0) {
      if (0 >= min_pri) {
        const int l = std::max({1, orig_len, min_len_r});
        const int cost = r_cost + std::abs(l - orig_len);
        if (std::make_pair(0, cost) < std::make_pair(best_pri, best_cost)) {
          best_pri = 0;
          best_cost = cost;
          *out_refs = r;
          *out_len = l;
          found = true;
        }
      }
      continue;
    }
    if (ComputeAllocnoPriority(r, min_len_r, size) < min_pri) {
      continue;
    }
    int l_start = max_len_search;
    if (min_pri > 0) {
      l_start = std::min(max_len_search, static_cast<int>(std::ceil(num / min_pri)) + 2);
    }
    l_start = std::max(l_start, min_len_r);
    while (l_start > min_len_r && ComputeAllocnoPriority(r, l_start, size) < min_pri) {
      --l_start;
    }
    const int pri = ComputeAllocnoPriority(r, l_start, size);
    if (pri < min_pri || pri > best_pri) {
      continue;
    }
    int chosen_l = l_start;
    if (orig_len >= min_len_r && orig_len <= l_start &&
        ComputeAllocnoPriority(r, orig_len, size) == pri) {
      chosen_l = orig_len;
    }
    const int cost = r_cost + std::abs(chosen_l - orig_len);
    if (std::tie(pri, cost) < std::tie(best_pri, best_cost)) {
      best_pri = pri;
      best_cost = cost;
      *out_refs = r;
      *out_len = chosen_l;
      found = true;
    }
  }
  return found;
}

// Reconciles `out->sideband.reg_n_refs` / `out->sideband.reg_live_length`
// against `greg_cfg.sideband.reg_renumber` whenever post-`combine` /
// fresh-pseudo priority ordering causes `global_alloc`'s `allocno_compare`
// sort order to diverge from the target hard-register assignment.
void ReconcileGlobalAllocPriorities(
    RtlControlFlowGraph* out, const RtlControlFlowGraph& liveness_cfg,
    const RtlControlFlowGraph& greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::btree_set<int>& live_at_any_block_start,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    const HardRegSet& scratch_hard_regs) {
  GlobalAllocModel m =
      BuildGlobalAllocModel(*out, liveness_cfg, greg_cfg, mips_cfg, pseudo_summaries, pseudo_modes,
                            fp_op_pseudos, gr_or_mem_op_pseudos, scratch_hard_regs);
  if (m.max_allocno == 0) {
    return;
  }
  std::vector<bool> lock_allocno_refs(m.max_allocno, false);
  for (int a = 0; a < m.max_allocno; ++a) {
    const int p = m.allocno_reg[a];
    if (live_at_any_block_start.contains(p)) {
      continue;
    }
    // Mirrors `local-alloc.c:472-474` (`CLASS_LIKELY_SPILLED_P`): single-register
    // preferred classes (`HI_REG`/`LO_REG`/`HILO_REG`/`ST_REGS`) with an alternate
    // class are skipped by `local_alloc` (`reg_qty[p] = -1`), so `reg_n_refs[p]`
    // has no effect on `local_alloc` quantity ordering.
    const bool single_reg_pref_with_alt =
        m.alt_class[a] != MipsRegClass::kNoRegs &&
        (m.pref_class[a] == MipsRegClass::kHiReg || m.pref_class[a] == MipsRegClass::kLoReg ||
         m.pref_class[a] == MipsRegClass::kHiloReg || m.pref_class[a] == MipsRegClass::kStRegs);
    if (single_reg_pref_with_alt) {
      continue;
    }
    const auto d_it = out->sideband.reg_n_deaths.find(p);
    const auto s_it = pseudo_summaries.find(p);
    if (d_it != out->sideband.reg_n_deaths.end() && d_it->second == 1 &&
        s_it != pseudo_summaries.end() && s_it->second.blocks_referenced.size() == 1 &&
        s_it->second.has_set_in_block && !s_it->second.used_before_set &&
        !s_it->second.alive_after_redef && s_it->second.note_deaths < 2) {
      lock_allocno_refs[a] = true;
    }
  }

  // Mirror `global.c:1085` (`CALLER_SAVE_PROFITABLE(refs, calls)`): on MIPS,
  // `CALLER_SAVE_PROFITABLE` requires `n_refs > 4 * calls_crossed` before
  // `find_reg` considers call-clobbered registers (`call_used_regs`) for a
  // call-crossing allocno when `-fcaller-saves` is enabled.
  if (mips_cfg.caller_saves) {
    for (int a = 0; a < m.max_allocno; ++a) {
      if (m.allocno_calls_crossed[a] > 0 && m.target_hr[a] >= 0 &&
          m.target_hr[a] < kFirstPseudoRegister && m.call_used_regs.TestBit(m.target_hr[a])) {
        const int min_cs_refs = 4 * m.allocno_calls_crossed[a] + 1;
        if (m.allocno_n_refs[a] < min_cs_refs) {
          m.allocno_n_refs[a] = min_cs_refs;
          const int p = m.allocno_reg[a];
          out->sideband.reg_n_refs[p] = std::max(out->sideband.reg_n_refs[p], min_cs_refs);
        }
      }
    }
  }

  // Mirror `global.c:1115-1156` (`find_reg` local-alloc eviction): any hard
  // register `hr` with `base_local_reg_n_refs[hr] > 0` that is not targeted by
  // any global allocno survived `global_alloc` without eviction, so every
  // size-1 global allocno `a` whose target is outside `pref_class` (or
  // spilled) must satisfy `n_refs / live_length <= local_density(hr)` across
  // all eligible local registers it scans before giving up on a class.
  HardRegSet global_target_hrs;
  for (int a = 0; a < m.max_allocno; ++a) {
    if (m.target_hr[a] >= 0 && m.target_hr[a] < kFirstPseudoRegister) {
      const int nregs = HardRegnoNregs(m.target_hr[a], m.pseudo_modes[m.allocno_reg[a]], mips_cfg);
      for (int hr = m.target_hr[a]; hr < m.target_hr[a] + nregs && hr < kFirstPseudoRegister;
           ++hr) {
        global_target_hrs.SetBit(hr);
      }
    }
  }
  std::vector<double> max_density(m.max_allocno, 0.0);
  for (int a = 0; a < m.max_allocno; ++a) {
    if (m.allocno_size[a] != 1) {
      continue;
    }
    if (m.target_hr[a] >= 0 && RegClassContents(m.pref_class[a]).TestBit(m.target_hr[a])) {
      continue;
    }
    HardRegSet checked_classes = RegClassContents(m.pref_class[a]);
    if (m.target_hr[a] < 0 && m.alt_class[a] != MipsRegClass::kNoRegs) {
      checked_classes.Ior(RegClassContents(m.alt_class[a]));
    }
    const bool orig_cs_prof = mips_cfg.caller_saves && m.allocno_calls_crossed[a] > 0 &&
                              (4 * m.allocno_calls_crossed[a] < m.allocno_n_refs[a]);
    HardRegSet excl =
        (orig_cs_prof || m.allocno_calls_crossed[a] == 0) ? m.fixed_regs : m.call_used_regs;
    excl.Ior(m.no_global_alloc_regs);
    const RegModeInfo& pmode = m.pseudo_modes[m.allocno_reg[a]];
    for (int hr = 0; hr < kFirstPseudoRegister; ++hr) {
      if (m.base_local_reg_n_refs[hr] != 0 && m.base_local_reg_live_length[hr] > 0 &&
          !global_target_hrs.TestBit(hr) && checked_classes.TestBit(hr) && !excl.TestBit(hr) &&
          HardRegnoModeOk(hr, pmode, mips_cfg)) {
        const double d =
            static_cast<double>(m.base_local_reg_n_refs[hr]) / m.base_local_reg_live_length[hr];
        if (max_density[a] == 0.0 || d < max_density[a]) {
          max_density[a] = d;
        }
      }
    }
    if (max_density[a] > 0.0) {
      const int min_len = MinLiveLengthForDensityCap(m.allocno_n_refs[a], max_density[a]);
      if (m.allocno_live_length[a] < min_len) {
        bool adjusted_via_refs = false;
        // Under `flag_schedule_insns` (`sched.c:4946-4947`), `sched1` overwrites
        // positive `reg_live_length[r]` with `sched_reg_live_length[r]`, so
        // satisfy the local-eviction density cap (`global.c:1115-1156`) by
        // lowering `reg_n_refs` when possible.
        if (mips_cfg.runs_sched1 && !lock_allocno_refs[a] && m.allocno_live_length[a] > 0) {
          const int cur_len = m.allocno_live_length[a];
          int cand_r = static_cast<int>(std::floor(max_density[a] * cur_len));
          while (cand_r > 0 && static_cast<double>(cand_r) / cur_len > max_density[a]) {
            --cand_r;
          }
          const int min_valid_r = orig_cs_prof ? (4 * m.allocno_calls_crossed[a] + 1) : 1;
          if (cand_r >= min_valid_r) {
            m.allocno_n_refs[a] = cand_r;
            out->sideband.reg_n_refs[m.allocno_reg[a]] = cand_r;
            adjusted_via_refs = true;
          }
        }
        if (!adjusted_via_refs) {
          m.allocno_live_length[a] = min_len;
          out->sideband.reg_live_length[m.allocno_reg[a]] = min_len;
        }
      }
    }
  }

  const std::vector<int> initial_order = SortAllocnoOrder(m);
  const SimResult init_sim = SimulateGlobalAlloc(m, initial_order);
  if (init_sim.primary_mismatches == 0 || m.max_allocno <= 1) {
    return;
  }

  // Hill-climb over allocno ordering via single-element reinsertion (and
  // pairwise swap fallback) to minimize `(primary_mismatches,
  // secondary_mismatches, displacement)`.
  std::vector<int> best_order = initial_order;
  SimResult best_sim = init_sim;
  bool single_element_move = false;
  int single_moved_pos = -1;

  // First try single-element reinsertions directly from `initial_order`,
  // picking the smallest displacement `|from - to|` that achieves minimum
  // mismatches.
  int best_single_disp = std::numeric_limits<int>::max();
  for (int from = 0; from < m.max_allocno; ++from) {
    for (int to = 0; to < m.max_allocno; ++to) {
      if (from == to) {
        continue;
      }
      std::vector<int> cand = initial_order;
      const int val = cand[from];
      cand.erase(cand.begin() + from);
      cand.insert(cand.begin() + to, val);
      const SimResult sim = SimulateGlobalAlloc(m, cand);
      const int disp = std::abs(from - to);
      if (std::tie(sim.primary_mismatches, sim.secondary_mismatches, disp) <
          std::tie(best_sim.primary_mismatches, best_sim.secondary_mismatches, best_single_disp)) {
        best_sim = sim;
        best_order = std::move(cand);
        best_single_disp = disp;
        single_element_move = true;
        single_moved_pos = to;
      }
    }
  }

  // If a single move didn't reach `0` primary mismatches, continue greedy
  // hill-climbing until convergence, and also try target-guided seed orders
  // (preferred-class allocations -> alternate-class allocations -> spilled
  // allocnos) when `initial_order` is trapped in a multi-move local minimum.
  auto climb_from_order = [&](std::vector<int> cur_order, SimResult cur_sim) {
    for (int iter = 0; iter < 32 && cur_sim.primary_mismatches > 0; ++iter) {
      bool improved = false;
      std::vector<int> step_order = cur_order;
      SimResult step_sim = cur_sim;
      int step_disp = std::numeric_limits<int>::max();
      for (int from = 0; from < m.max_allocno; ++from) {
        for (int to = 0; to < m.max_allocno; ++to) {
          if (from == to) {
            continue;
          }
          std::vector<int> cand = cur_order;
          const int val = cand[from];
          cand.erase(cand.begin() + from);
          cand.insert(cand.begin() + to, val);
          const SimResult sim = SimulateGlobalAlloc(m, cand);
          const int disp = std::abs(from - to);
          if (std::tie(sim.primary_mismatches, sim.secondary_mismatches) <
                  std::tie(step_sim.primary_mismatches, step_sim.secondary_mismatches) ||
              (std::tie(sim.primary_mismatches, sim.secondary_mismatches) ==
                   std::tie(step_sim.primary_mismatches, step_sim.secondary_mismatches) &&
               improved && disp < step_disp)) {
            step_sim = sim;
            step_order = std::move(cand);
            step_disp = disp;
            improved = true;
          }
        }
      }
      if (!improved) {
        for (int i = 0; i < m.max_allocno; ++i) {
          for (int j = i + 1; j < m.max_allocno; ++j) {
            std::vector<int> cand = cur_order;
            std::swap(cand[i], cand[j]);
            const SimResult sim = SimulateGlobalAlloc(m, cand);
            if (std::tie(sim.primary_mismatches, sim.secondary_mismatches) <
                std::tie(step_sim.primary_mismatches, step_sim.secondary_mismatches)) {
              step_sim = sim;
              step_order = std::move(cand);
              improved = true;
            }
          }
        }
      }
      if (!improved) {
        break;
      }
      cur_order = std::move(step_order);
      cur_sim = std::move(step_sim);
    }
    return std::make_pair(std::move(cur_order), std::move(cur_sim));
  };

  if (best_sim.primary_mismatches > 0) {
    auto [climbed_order, climbed_sim] = climb_from_order(best_order, best_sim);
    if (std::tie(climbed_sim.primary_mismatches, climbed_sim.secondary_mismatches) <
        std::tie(best_sim.primary_mismatches, best_sim.secondary_mismatches)) {
      best_order = std::move(climbed_order);
      best_sim = std::move(climbed_sim);
      single_element_move = false;
    }
  }

  if (best_sim.primary_mismatches > 0) {
    std::vector<int> initial_pos(m.max_allocno, 0);
    for (int idx = 0; idx < m.max_allocno; ++idx) {
      initial_pos[initial_order[idx]] = idx;
    }
    // Mirror `global.c:562-571, 962-1157` (`global_alloc` + `find_reg`):
    //   Tier 0: non-call-clobbered (callee-saved or non-call-crossing) `pref_class`
    //   Tier 1: call-crossing `call_used_regs` in `pref_class` (`global.c:1085`
    //           `CALLER_SAVE_PROFITABLE`, reached only after Tier 0 fills)
    //   Tier 2: `alt_class` fallback (`global.c:562-571`, `alt_regs_p = 1`)
    //   Tier 3: spilled (`target_hr < 0`)
    auto allocno_tier = [&](int a) {
      if (m.target_hr[a] < 0) {
        return 3;
      }
      if (!RegClassContents(m.pref_class[a]).TestBit(m.target_hr[a])) {
        return 2;
      }
      if (m.allocno_calls_crossed[a] > 0 && m.call_used_regs.TestBit(m.target_hr[a])) {
        return 1;
      }
      return 0;
    };

    std::vector<int> seed_tiered_initial = initial_order;
    std::stable_sort(seed_tiered_initial.begin(), seed_tiered_initial.end(),
                     [&](int a, int b) { return allocno_tier(a) < allocno_tier(b); });

    std::vector<int> seed_tiered_regno = initial_order;
    std::sort(seed_tiered_regno.begin(), seed_tiered_regno.end(), [&](int a, int b) {
      const int ta = allocno_tier(a);
      const int tb = allocno_tier(b);
      if (ta != tb) {
        return ta < tb;
      }
      const int hra = (m.target_hr[a] >= 0) ? m.target_hr[a] : std::numeric_limits<int>::max();
      const int hrb = (m.target_hr[b] >= 0) ? m.target_hr[b] : std::numeric_limits<int>::max();
      if (hra != hrb) {
        return hra < hrb;
      }
      return initial_pos[a] < initial_pos[b];
    });

    for (const std::vector<int>& seed : {seed_tiered_initial, seed_tiered_regno}) {
      if (best_sim.primary_mismatches == 0) {
        break;
      }
      const SimResult seed_sim = SimulateGlobalAlloc(m, seed);
      auto [climbed_order, climbed_sim] = climb_from_order(seed, seed_sim);
      if (std::tie(climbed_sim.primary_mismatches, climbed_sim.secondary_mismatches) <
          std::tie(best_sim.primary_mismatches, best_sim.secondary_mismatches)) {
        best_order = std::move(climbed_order);
        best_sim = std::move(climbed_sim);
        single_element_move = false;
      }
    }
  }

  if (best_sim.primary_mismatches >= init_sim.primary_mismatches) {
    return;
  }

  // Mirrors `global.c:1078-1095` (`find_reg`): `CALLER_SAVE_PROFITABLE` is only
  // evaluated when `best_reg < 0` on the non-call-clobbered pass. Any allocno
  // whose target is a callee-saved register in `pref_class` succeeds with
  // `best_reg >= 0` without ever reaching line 1085, so its `reg_n_refs` need
  // not stay on one side of `4 * calls_crossed`.
  auto needs_cs_prof_guard = [&](int a) {
    if (!mips_cfg.caller_saves || m.allocno_calls_crossed[a] == 0) {
      return false;
    }
    const int thr = m.target_hr[a];
    if (thr < 0 || thr >= kFirstPseudoRegister) {
      return true;
    }
    return m.call_used_regs.TestBit(thr) || !RegClassContents(m.pref_class[a]).TestBit(thr);
  };

  // Mirrors `sched.c:4915-4947`: `schedule_insns` only overwrites
  // `reg_live_length[regno]` when `sched_reg_live_length[regno] != 0`, which
  // holds only for pseudos that actually appear in the RTL CFG or are live at a
  // basic-block boundary. Zero-occurrence phantom pseudos keep their sideband
  // `reg_live_length[regno]` across `sched1`, so their length remains free to
  // satisfy local-eviction density caps (`global.c:1115-1156`) without forcing
  // real CFG allocnos to fall back to unlocked lengths.
  auto has_cfg_sched_live_length = [&](int a) {
    const int regno = m.allocno_reg[a];
    if (live_at_any_block_start.contains(regno)) {
      return true;
    }
    const auto s_it = pseudo_summaries.find(regno);
    return s_it != pseudo_summaries.end() && !s_it->second.blocks_referenced.empty();
  };

  auto lock_len_for = [&](int a, bool prefer_locked_len) {
    return prefer_locked_len && m.allocno_live_length[a] > 0 && !lock_allocno_refs[a] &&
           has_cfg_sched_live_length(a);
  };

  // Fast path: if `best_order` was obtained by moving a single allocno to
  // `single_moved_pos`, try adjusting only that one allocno's `(n_refs,
  // live_length)` within its neighbor priority interval `[min_pri, max_pri]`.
  // Under `flag_schedule_insns` (`sched.c:4946-4947`), keep `live_length`
  // locked so `sched1` write-back preserves the solved `allocno_compare` order.
  if (single_element_move && single_moved_pos >= 0 && single_moved_pos < m.max_allocno) {
    const int a = best_order[single_moved_pos];
    int max_pri = std::numeric_limits<int>::max();
    if (single_moved_pos > 0) {
      const int prev = best_order[single_moved_pos - 1];
      const int prev_pri = ComputeAllocnoPriority(
          m.allocno_n_refs[prev], m.allocno_live_length[prev], m.allocno_size[prev]);
      max_pri = prev_pri - (prev > a ? 1 : 0);
    }
    int min_pri = std::numeric_limits<int>::min();
    if (single_moved_pos + 1 < m.max_allocno) {
      const int next = best_order[single_moved_pos + 1];
      const int next_pri = ComputeAllocnoPriority(
          m.allocno_n_refs[next], m.allocno_live_length[next], m.allocno_size[next]);
      min_pri = next_pri + (a > next ? 1 : 0);
    }
    int new_refs = m.allocno_n_refs[a];
    int new_len = m.allocno_live_length[a];
    if (FindBestRefsAndLengthInPriorityRange(
            m.allocno_n_refs[a], m.allocno_live_length[a], m.allocno_size[a],
            m.allocno_calls_crossed[a], needs_cs_prof_guard(a), max_density[a], min_pri, max_pri,
            &new_refs, &new_len, lock_allocno_refs[a], lock_len_for(a, mips_cfg.runs_sched1))) {
      GlobalAllocModel test_m = m;
      test_m.allocno_n_refs[a] = new_refs;
      test_m.allocno_live_length[a] = new_len;
      const SimResult test_sim = SimulateGlobalAlloc(test_m, best_order);
      if (SortAllocnoOrder(test_m) == best_order &&
          test_sim.primary_mismatches <= best_sim.primary_mismatches) {
        const int regno = m.allocno_reg[a];
        out->sideband.reg_n_refs[regno] = new_refs;
        out->sideband.reg_live_length[regno] = new_len;
        return;
      }
    }
  }

  // General path: realize `best_order` by minimally adjusting adjacent priority
  // inversions (raising prefixes leftward when a tight integer gap requires
  // room), with a right-to-left monotone fallback if discrete priority gaps
  // prevent local pairwise steps from converging. Under `flag_schedule_insns`,
  // first solve strictly via `reg_n_refs` (preserving `sched_reg_live_length`
  // per `sched.c:4946-4947`), falling back to unlocked lengths only if no
  // `reg_n_refs`-only solution exists.
  auto solve_general_with_policy = [&](bool prefer_locked_len) -> GlobalAllocModel {
    GlobalAllocModel cand_m = m;
    const int max_guard = m.max_allocno * m.max_allocno * 2 + 64;
    for (int guard = 0; guard < max_guard; ++guard) {
      if (SortAllocnoOrder(cand_m) == best_order) {
        break;
      }
      bool changed = false;
      for (int k = 0; k + 1 < m.max_allocno; ++k) {
        const int u = best_order[k];
        const int v = best_order[k + 1];
        const int pri_u = ComputeAllocnoPriority(
            cand_m.allocno_n_refs[u], cand_m.allocno_live_length[u], cand_m.allocno_size[u]);
        const int pri_v = ComputeAllocnoPriority(
            cand_m.allocno_n_refs[v], cand_m.allocno_live_length[v], cand_m.allocno_size[v]);
        const int needed_gap = (u > v) ? 1 : 0;
        if (pri_u >= pri_v + needed_gap) {
          continue;
        }

        // First try lowering `v` below `u` without crossing `best_order[k + 2]`.
        int v_min_pri = 0;
        if (k + 2 < m.max_allocno) {
          const int w = best_order[k + 2];
          const int pri_w = ComputeAllocnoPriority(
              cand_m.allocno_n_refs[w], cand_m.allocno_live_length[w], cand_m.allocno_size[w]);
          v_min_pri = pri_w + (v > w ? 1 : 0);
        }
        const int v_max_pri = pri_u - needed_gap;
        int cand_refs = cand_m.allocno_n_refs[v];
        int cand_len = cand_m.allocno_live_length[v];
        if (FindBestRefsAndLengthInPriorityRange(
                m.allocno_n_refs[v], m.allocno_live_length[v], cand_m.allocno_size[v],
                cand_m.allocno_calls_crossed[v], needs_cs_prof_guard(v), max_density[v], v_min_pri,
                v_max_pri, &cand_refs, &cand_len, lock_allocno_refs[v],
                lock_len_for(v, prefer_locked_len))) {
          cand_m.allocno_n_refs[v] = cand_refs;
          cand_m.allocno_live_length[v] = cand_len;
          changed = true;
          break;
        }

        // Otherwise raise `u` above `v` (cascading leftward on subsequent
        // iterations if needed, and widening to `std::numeric_limits<int>::max()`
        // if discrete integer priority gaps leave `[pri_v + needed_gap, u_max_pri]` empty).
        int u_max_pri = std::numeric_limits<int>::max();
        if (k > 0) {
          const int p = best_order[k - 1];
          const int pri_p = ComputeAllocnoPriority(
              cand_m.allocno_n_refs[p], cand_m.allocno_live_length[p], cand_m.allocno_size[p]);
          u_max_pri = std::max(pri_v + needed_gap + 5000, pri_p - (p > u ? 1 : 0));
        }
        const bool lock_u_len = lock_len_for(u, prefer_locked_len);
        if (FindBestRefsAndLengthInPriorityRange(
                m.allocno_n_refs[u], m.allocno_live_length[u], cand_m.allocno_size[u],
                cand_m.allocno_calls_crossed[u], needs_cs_prof_guard(u), max_density[u],
                pri_v + needed_gap, u_max_pri, &cand_refs, &cand_len, lock_allocno_refs[u],
                lock_u_len) ||
            (u_max_pri < std::numeric_limits<int>::max() &&
             FindBestRefsAndLengthInPriorityRange(
                 m.allocno_n_refs[u], m.allocno_live_length[u], cand_m.allocno_size[u],
                 cand_m.allocno_calls_crossed[u], needs_cs_prof_guard(u), max_density[u],
                 pri_v + needed_gap, std::numeric_limits<int>::max(), &cand_refs, &cand_len,
                 lock_allocno_refs[u], lock_u_len))) {
          cand_m.allocno_n_refs[u] = cand_refs;
          cand_m.allocno_live_length[u] = cand_len;
          changed = true;
          break;
        }
      }
      if (!changed) {
        break;
      }
    }

    if (SortAllocnoOrder(cand_m) != best_order) {
      GlobalAllocModel mono_m = m;
      bool mono_ok = true;
      int min_req = 0;
      for (int k = m.max_allocno - 1; k >= 0; --k) {
        const int u = best_order[k];
        if (k + 1 < m.max_allocno) {
          const int v = best_order[k + 1];
          const int pri_v = ComputeAllocnoPriority(
              mono_m.allocno_n_refs[v], mono_m.allocno_live_length[v], mono_m.allocno_size[v]);
          min_req = pri_v + ((u > v) ? 1 : 0);
        }
        int cand_refs = m.allocno_n_refs[u];
        int cand_len = m.allocno_live_length[u];
        const bool lock_u_len = lock_len_for(u, prefer_locked_len);
        if (!FindLowestPriorityAtLeast(m.allocno_n_refs[u], m.allocno_live_length[u],
                                       mono_m.allocno_size[u], mono_m.allocno_calls_crossed[u],
                                       needs_cs_prof_guard(u), max_density[u], min_req, &cand_refs,
                                       &cand_len, lock_allocno_refs[u], lock_u_len) &&
            (!lock_allocno_refs[u] ||
             !FindLowestPriorityAtLeast(m.allocno_n_refs[u], m.allocno_live_length[u],
                                        mono_m.allocno_size[u], mono_m.allocno_calls_crossed[u],
                                        needs_cs_prof_guard(u), max_density[u], min_req, &cand_refs,
                                        &cand_len, /*lock_refs=*/false,
                                        prefer_locked_len && m.allocno_live_length[u] > 0 &&
                                            has_cfg_sched_live_length(u)))) {
          mono_ok = false;
          break;
        }
        mono_m.allocno_n_refs[u] = cand_refs;
        mono_m.allocno_live_length[u] = cand_len;
      }
      if (mono_ok && SortAllocnoOrder(mono_m) == best_order) {
        cand_m = std::move(mono_m);
      }
    }
    return cand_m;
  };

  GlobalAllocModel work_m = solve_general_with_policy(mips_cfg.runs_sched1);
  if (mips_cfg.runs_sched1 && SortAllocnoOrder(work_m) != best_order) {
    work_m = solve_general_with_policy(/*prefer_locked_len=*/false);
  }

  if (SortAllocnoOrder(work_m) == best_order) {
    const SimResult work_sim = SimulateGlobalAlloc(work_m, best_order);
    if (work_sim.primary_mismatches <= best_sim.primary_mismatches) {
      for (int a = 0; a < m.max_allocno; ++a) {
        const int regno = m.allocno_reg[a];
        if (work_m.allocno_n_refs[a] != m.allocno_n_refs[a]) {
          out->sideband.reg_n_refs[regno] = work_m.allocno_n_refs[a];
        }
        if (work_m.allocno_live_length[a] != m.allocno_live_length[a]) {
          out->sideband.reg_live_length[regno] = work_m.allocno_live_length[a];
        }
      }
    }
  }
}

absl::Status ValidateInvertGlobalAllocInput(const RtlControlFlowGraph& greg_cfg,
                                            int* max_seen_regno_out) {
  if (greg_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertGlobalAlloc: input CFG has no basic blocks");
  }

  const size_t num_blocks = greg_cfg.rpo_blocks.size();
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = greg_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: block %zu has out-of-bounds successor %zu (num_blocks=%zu)", b,
            succ, num_blocks));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: block %zu has out-of-bounds addr_vec target %zu (num_blocks=%zu)",
            b, target, num_blocks));
      }
    }
  }

  int max_seen_regno = std::max(greg_cfg.sideband.max_regno, kFirstPseudoRegister);

  for (const auto& [regno, hard_reg] : greg_cfg.sideband.reg_renumber) {
    if (regno < kFirstPseudoRegister || hard_reg < -1 || hard_reg >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: invalid sideband reg_renumber entry (%d -> %d)", regno, hard_reg));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_refs) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_refs", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_sets) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_sets", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_deaths) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_deaths", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_live_length) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative regno %d in sideband reg_live_length", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_calls_crossed) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative regno %d in sideband reg_n_calls_crossed", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }

  for (const auto& [bb_idx, live_set] : greg_cfg.sideband.basic_block_live_at_start) {
    if (bb_idx < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative block index %d in sideband basic_block_live_at_start",
          bb_idx));
    }
    for (int regno : live_set) {
      if (regno < 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: negative regno %d in sideband basic_block_live_at_start", regno));
      }
      max_seen_regno = std::max(max_seen_regno, regno + 1);
    }
  }
  for (int gpr : greg_cfg.sideband.saved_gpr_regnos) {
    if (gpr < 0 || gpr >= kFirstFprRegnum) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid GPR %d in sideband saved_gpr_regnos", gpr));
    }
  }
  for (int fpr : greg_cfg.sideband.saved_fpr_regnos) {
    if (fpr < kFirstFprRegnum || fpr > kLastFprRegnum) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid FPR %d in sideband saved_fpr_regnos", fpr));
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = greg_cfg.rpo_blocks[b];
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("InvertGlobalAlloc: failed to parse instruction %zu in block %zu: %s",
                            i, b, parsed.status().message()));
      }
      if (parsed->pattern.op == RtlOp::kNil || parsed->pattern.op == RtlOp::kUnknown) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: invalid pattern opcode at instruction %zu in block %zu", i, b));
      }
      if (absl::Status st = ValidateRtlExprRegisters(parsed->pattern, &max_seen_regno); !st.ok()) {
        return st;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (absl::Status st = ValidateRtlExprRegisters(note.datum, &max_seen_regno); !st.ok()) {
          return st;
        }
      }
      for (const RtlExpr& fu : parsed->call_fusage) {
        if (absl::Status st = ValidateRtlExprRegisters(fu, &max_seen_regno); !st.ok()) {
          return st;
        }
      }
    }
  }
  *max_seen_regno_out = max_seen_regno;
  return absl::OkStatus();
}

RtlControlFlowGraph InvertStupidGlobalAlloc(const RtlControlFlowGraph& greg_cfg,
                                            const CompilationProfile& profile, int max_seen_regno) {
  RtlControlFlowGraph out = greg_cfg;
  out.pass = RtlPassKind::kLreg;
  out.sideband.max_regno = max_seen_regno;
  absl::erase_if(out.sideband.reg_renumber, [](const auto& kv) {
    return kv.first < kFirstPseudoRegister || kv.second < 0 || kv.second >= kFirstPseudoRegister;
  });
  ReconstructRtlLiveness(
      &out, profile,
      {.reload_completed = false, .update_reg_notes = false, .preserve_phantom_pseudos = true});
  return out;
}

void EnforceBlockAllocQuantityAtomicity(
    RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::btree_set<int>& live_at_any_block_start,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    HardRegSet* erased_global_hard_regs, absl::btree_set<int>* atomicity_erased_pseudos) {
  for (size_t b = 0; b < dst_out->rpo_blocks.size(); ++b) {
    std::optional<LocalBlockAllocModel> model = BuildSingleBlockLocalAllocModel(
        b, dst_out, liveness_cfg, mips_cfg, live_at_any_block_start, pseudo_summaries, pseudo_modes,
        fp_op_pseudos, gr_or_mem_op_pseudos, /*bump_caller_saves_refs=*/false);
    if (!model.has_value()) {
      continue;
    }
    for (const LocalBlockQty& q : model->qtys) {
      if (q.first_reg < kFirstPseudoRegister || q.member_regs.empty()) {
        continue;
      }
      int base_hr = -2;
      bool invalid_local_qty = false;
      for (size_t mi = 0; mi < q.member_regs.size(); ++mi) {
        const int p = q.member_regs[mi];
        const int off = q.member_offsets[mi];
        const int thr = model->target_hr_by_pseudo.at(p);
        const int cand_base = (thr >= 0) ? (thr - off) : -1;
        if (mi == 0) {
          base_hr = cand_base;
        } else if (cand_base != base_hr) {
          invalid_local_qty = true;
          break;
        }
      }
      if (!invalid_local_qty && base_hr >= 0) {
        if (base_hr >= kFirstPseudoRegister || !HardRegnoModeOk(base_hr, q.mode, mips_cfg)) {
          invalid_local_qty = true;
        } else {
          HardRegSet allowed_cls = RegClassContents(q.min_class);
          allowed_cls.Ior(RegClassContents(q.alt_class));
          const int nregs = HardRegnoNregs(base_hr, q.mode, mips_cfg);
          for (int k = 0; k < nregs; ++k) {
            const int r = base_hr + k;
            if (r >= kFirstPseudoRegister || !allowed_cls.TestBit(r) ||
                model->fixed_regs.TestBit(r) || r == kArgPointerRegnum ||
                r == kFramePointerRegnum ||
                (q.calls_crossed > 0 && !mips_cfg.caller_saves &&
                 model->call_used_regs.TestBit(r))) {
              invalid_local_qty = true;
              break;
            }
          }
        }
      }
      if (invalid_local_qty) {
        for (int p : q.member_regs) {
          if (auto ren_it = dst_out->sideband.reg_renumber.find(p);
              ren_it != dst_out->sideband.reg_renumber.end() && ren_it->second >= 0 &&
              ren_it->second < kFirstPseudoRegister) {
            const int hr = ren_it->second;
            const auto m_it = pseudo_modes.find(p);
            const RegModeInfo pmode =
                (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
            const int nregs = HardRegnoNregs(hr, pmode, mips_cfg);
            for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
              erased_global_hard_regs->SetBit(k);
            }
            if (atomicity_erased_pseudos != nullptr) {
              atomicity_erased_pseudos->insert(p);
            }
          }
          dst_out->sideband.reg_renumber.erase(p);
        }
      }
    }
  }
}

void RestoreLocalReloadHardRegUseOrder(const LocalBlockAllocModel& orig_m, int target_mismatches,
                                       const std::array<bool, 2>& class_exhausted,
                                       LocalBlockAllocModel* work_m);

void ReconcileSingleBlockLocalPriorities(
    size_t b, RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::btree_set<int>& live_at_any_block_start,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    const std::array<bool, 2>& class_exhausted) {
  std::optional<LocalBlockAllocModel> model = BuildSingleBlockLocalAllocModel(
      b, dst_out, liveness_cfg, mips_cfg, live_at_any_block_start, pseudo_summaries, pseudo_modes,
      fp_op_pseudos, gr_or_mem_op_pseudos, /*bump_caller_saves_refs=*/true);
  if (!model.has_value() || model->eligible_pseudos.size() < 2) {
    return;
  }
  const LocalBlockAllocModel& m = *model;
  const auto& reg_offset = m.reg_offset;
  const HardRegSet& call_used = m.call_used_regs;

  const int init_mismatches = SimulateLocalBlockAlloc(m, nullptr);
  if (init_mismatches == 0 || m.qtys.size() <= 1) {
    return;
  }

  const int nq = static_cast<int>(m.qtys.size());
  auto qty_pri = [&](const LocalBlockAllocModel& mod, int q) {
    return ComputeLocalQtyPriority(mod.qtys[q].n_refs, mod.qtys[q].death - mod.qtys[q].birth,
                                   mod.qtys[q].size);
  };
  auto sort_pass2 = [&](const LocalBlockAllocModel& mod) {
    std::vector<int> ord(nq);
    for (int i = 0; i < nq; ++i) {
      ord[i] = i;
    }
    std::sort(ord.begin(), ord.end(), [&](int q1, int q2) {
      const int p1 = qty_pri(mod, q1);
      const int p2 = qty_pri(mod, q2);
      return (p1 != p2) ? (p1 > p2) : (q1 < q2);
    });
    return ord;
  };

  const std::vector<int> initial_order = sort_pass2(m);
  std::vector<int> initial_pos(nq, 0);
  for (int idx = 0; idx < nq; ++idx) {
    initial_pos[initial_order[idx]] = idx;
  }

  auto qty_target_hr = [&](int q) -> int {
    if (m.qtys[q].first_reg < kFirstPseudoRegister) {
      return -2;
    }
    const int p0 = m.qtys[q].first_reg;
    const int thr = m.target_hr_by_pseudo.at(p0);
    return (thr >= 0) ? (thr - reg_offset[p0]) : -1;
  };

  // Precompute per-quantity Pass-1 suggestion state (`local-alloc.c:1303-1325`)
  // and candidate hard-register classes so `realize_local_order` only enforces
  // priority inequalities between quantities that actually compete in the same
  // allocation phase and register class (avoiding spurious `reg_n_refs`
  // inflation that would perturb `reload1.c:3606` `order_regs_for_reload`).
  std::vector<HardRegSet> qty_cand_regs(nq);
  std::vector<int> qty_sugg_key(nq, 0);
  std::vector<bool> qty_pass1_settled(nq, false);
  for (int q = 0; q < nq; ++q) {
    HardRegSet cr = RegClassContents(m.qtys[q].min_class);
    if (m.qtys[q].alt_class != MipsRegClass::kNoRegs) {
      cr.Ior(RegClassContents(m.qtys[q].alt_class));
    }
    qty_cand_regs[q] = cr;
    qty_sugg_key[q] = (m.qtys[q].num_copy_sugg != 0) ? m.qtys[q].num_copy_sugg
                                                     : (m.qtys[q].num_sugg * kFirstPseudoRegister);
    const int thr = qty_target_hr(q);
    if (thr >= 0 && thr < kFirstPseudoRegister && qty_sugg_key[q] != 0) {
      const bool matches_sugg = (m.qtys[q].num_copy_sugg != 0)
                                    ? m.qtys[q].phys_copy_sugg.TestBit(thr)
                                    : m.qtys[q].phys_sugg.TestBit(thr);
      qty_pass1_settled[q] = matches_sugg;
    }
  }
  auto qtys_compete_in_priority = [&](int q, int other) {
    if (m.qtys[other].first_reg < kFirstPseudoRegister) {
      return false;
    }
    // `local-alloc.c:1491-1505` (`case 3:`): `qty_compare(0,1)`,
    // `qty_compare(1,2)`, `qty_compare(0,1)` directly couple all three
    // quantities' Pass-2 priorities regardless of Pass-1 settlement.
    if (nq == 3) {
      return true;
    }
    if ((qty_cand_regs[q].lo & qty_cand_regs[other].lo) == 0 &&
        (qty_cand_regs[q].hi & qty_cand_regs[other].hi) == 0) {
      return false;
    }
    if (qty_pass1_settled[q]) {
      return qty_sugg_key[other] == qty_sugg_key[q];
    }
    return !qty_pass1_settled[other];
  };

  // Realize an ordering `ord` via monotone right-to-left `qty_n_refs`
  // assignment on pseudo quantities (`first_reg >= kFirstPseudoRegister`).
  auto realize_local_order =
      [&](const std::vector<int>& ord) -> std::pair<LocalBlockAllocModel, int> {
    LocalBlockAllocModel work_m = m;
    for (int k = nq - 1; k >= 0; --k) {
      const int q = ord[k];
      if (work_m.qtys[q].first_reg < kFirstPseudoRegister) {
        continue;
      }
      int min_req_pri = 0;
      for (int r = k + 1; r < nq; ++r) {
        const int tail_q = ord[r];
        if (!qtys_compete_in_priority(q, tail_q)) {
          continue;
        }
        min_req_pri = std::max(min_req_pri, qty_pri(work_m, tail_q) + ((q > tail_q) ? 1 : 0));
      }
      const int span = std::max(1, work_m.qtys[q].death - work_m.qtys[q].birth);
      const int min_refs = std::max(1, static_cast<int>(work_m.qtys[q].member_regs.size()));
      const int thr = qty_target_hr(q);
      int max_refs = 4096;
      if (thr == -1 && mips_cfg.caller_saves && work_m.qtys[q].calls_crossed > 0) {
        max_refs = std::max(min_refs, 4 * work_m.qtys[q].calls_crossed);
      }
      int lo_r = min_refs;
      if (thr >= 0 && mips_cfg.caller_saves && work_m.qtys[q].calls_crossed > 0 &&
          call_used.TestBit(thr)) {
        lo_r = std::max(lo_r, 4 * work_m.qtys[q].calls_crossed + 1);
      }
      const int hi_r = std::max(lo_r, max_refs);
      const int orig_r = m.qtys[q].n_refs;
      int prev_competing = -1;
      for (int p_idx = k - 1; p_idx >= 0; --p_idx) {
        if (qtys_compete_in_priority(q, ord[p_idx])) {
          prev_competing = ord[p_idx];
          break;
        }
      }
      if (orig_r >= lo_r && orig_r <= hi_r &&
          ComputeLocalQtyPriority(orig_r, span, work_m.qtys[q].size) >= min_req_pri &&
          (prev_competing < 0 || ComputeLocalQtyPriority(orig_r, span, work_m.qtys[q].size) <=
                                     qty_pri(m, prev_competing) - ((prev_competing > q) ? 1 : 0))) {
        work_m.qtys[q].n_refs = orig_r;
        continue;
      }
      int chosen_r = (thr == -1) ? lo_r : hi_r;
      if (ComputeLocalQtyPriority(hi_r, span, work_m.qtys[q].size) >= min_req_pri) {
        int l = lo_r;
        int h = hi_r;
        while (l < h) {
          const int mid = l + (h - l) / 2;
          if (ComputeLocalQtyPriority(mid, span, work_m.qtys[q].size) >= min_req_pri) {
            h = mid;
          } else {
            l = mid + 1;
          }
        }
        chosen_r = l;
      }
      work_m.qtys[q].n_refs = chosen_r;
    }
    const int realized_mis = SimulateLocalBlockAlloc(work_m, nullptr);
    return {std::move(work_m), realized_mis};
  };

  std::vector<int> target_seed = initial_order;
  std::sort(target_seed.begin(), target_seed.end(), [&](int a, int b) {
    const int ta = qty_target_hr(a);
    const int tb = qty_target_hr(b);
    const int tier_a = (ta >= 0) ? 0 : (ta == -1 ? 1 : 2);
    const int tier_b = (tb >= 0) ? 0 : (tb == -1 ? 1 : 2);
    if (tier_a != tier_b) {
      return tier_a < tier_b;
    }
    if (ta >= 0 && tb >= 0) {
      const bool cs_a = (m.qtys[a].calls_crossed > 0 && call_used.TestBit(ta));
      const bool cs_b = (m.qtys[b].calls_crossed > 0 && call_used.TestBit(tb));
      if (cs_a != cs_b) {
        return !cs_a && cs_b;
      }
      if (ta != tb) {
        return ta < tb;
      }
    }
    return initial_pos[a] < initial_pos[b];
  });

  LocalBlockAllocModel best_work_m = m;
  int best_mismatches = init_mismatches;

  auto build_conflict_qty_mask = [&](const LocalBlockAllocModel& mod) {
    if (nq <= 3) {
      return std::vector<bool>(nq, true);
    }
    absl::btree_map<int, int> sim_hr;
    SimulateLocalBlockAlloc(mod, nullptr, &sim_hr);
    std::vector<bool> mismatched_q(nq, false);
    HardRegSet conflict_hrs;
    for (int q = 0; q < nq; ++q) {
      if (mod.qtys[q].first_reg < kFirstPseudoRegister) {
        continue;
      }
      for (int p : mod.qtys[q].member_regs) {
        const int got = sim_hr[p];
        const int exp = mod.target_hr_by_pseudo.at(p);
        if (got != exp) {
          mismatched_q[q] = true;
          const auto m_it = pseudo_modes.find(p);
          const RegModeInfo pm =
              (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
          if (got >= 0) {
            const int nregs = HardRegnoNregs(got, pm, mips_cfg);
            for (int r = got; r < got + nregs && r < kFirstPseudoRegister; ++r) {
              conflict_hrs.SetBit(r);
            }
          }
          if (exp >= 0) {
            const int nregs = HardRegnoNregs(exp, pm, mips_cfg);
            for (int r = exp; r < exp + nregs && r < kFirstPseudoRegister; ++r) {
              conflict_hrs.SetBit(r);
            }
          }
        }
      }
    }
    std::vector<bool> active_q = mismatched_q;
    for (int q = 0; q < nq; ++q) {
      if (active_q[q] || mod.qtys[q].first_reg < kFirstPseudoRegister) {
        continue;
      }
      for (int p : mod.qtys[q].member_regs) {
        const auto m_it = pseudo_modes.find(p);
        const RegModeInfo pm =
            (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
        for (int hr : {sim_hr[p], mod.target_hr_by_pseudo.at(p)}) {
          if (hr >= 0) {
            const int nregs = HardRegnoNregs(hr, pm, mips_cfg);
            for (int r = hr; r < hr + nregs && r < kFirstPseudoRegister; ++r) {
              if (conflict_hrs.TestBit(r)) {
                active_q[q] = true;
                break;
              }
            }
          }
        }
        if (active_q[q]) {
          break;
        }
      }
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (int q = 0; q < nq; ++q) {
        if (active_q[q] || mod.qtys[q].first_reg < kFirstPseudoRegister) {
          continue;
        }
        for (int a = 0; a < nq; ++a) {
          if (!active_q[a] || !ModesTieableForLocalAlloc(mod.qtys[q].mode, mod.qtys[a].mode)) {
            continue;
          }
          if (std::max(mod.qtys[q].birth, mod.qtys[a].birth) <
              std::min(mod.qtys[q].death, mod.qtys[a].death)) {
            active_q[q] = true;
            changed = true;
            break;
          }
        }
      }
    }
    return active_q;
  };

  auto climb_local = [&](std::vector<int> cur_order) {
    auto [cur_work_m, cur_mis] = realize_local_order(cur_order);
    int cur_ideal_mis = SimulateLocalBlockAlloc(m, &cur_order);
    for (int iter = 0; iter < 64 && cur_mis > 0; ++iter) {
      const std::vector<bool> active_q = build_conflict_qty_mask(cur_work_m);
      bool improved = false;
      std::vector<int> step_order = cur_order;
      LocalBlockAllocModel step_work_m = cur_work_m;
      int step_mis = cur_mis;
      int step_ideal_mis = cur_ideal_mis;
      for (int from = 0; from < nq; ++from) {
        const int q_from = cur_order[from];
        if (m.qtys[q_from].first_reg < kFirstPseudoRegister || !active_q[q_from]) {
          continue;
        }
        for (int to = 0; to < nq; ++to) {
          if (from == to) {
            continue;
          }
          std::vector<int> cand = cur_order;
          const int val = cand[from];
          cand.erase(cand.begin() + from);
          cand.insert(cand.begin() + to, val);
          const int ideal_mis = SimulateLocalBlockAlloc(m, &cand);
          if (ideal_mis > step_mis && ideal_mis >= step_ideal_mis) {
            continue;
          }
          auto [cand_work_m, real_mis] = realize_local_order(cand);
          if (real_mis < step_mis || (real_mis == step_mis && ideal_mis < step_ideal_mis)) {
            step_mis = real_mis;
            step_ideal_mis = ideal_mis;
            step_work_m = std::move(cand_work_m);
            step_order = std::move(cand);
            improved = true;
          }
        }
      }
      if (!improved) {
        break;
      }
      cur_order = std::move(step_order);
      cur_work_m = std::move(step_work_m);
      cur_mis = step_mis;
      cur_ideal_mis = step_ideal_mis;
    }
    if (cur_mis < best_mismatches) {
      best_mismatches = cur_mis;
      best_work_m = std::move(cur_work_m);
    }
  };

  if (best_mismatches > 0) {
    climb_local(initial_order);
  }
  if (best_mismatches > 0) {
    climb_local(target_seed);
  }
  if (best_mismatches < init_mismatches) {
    if (class_exhausted[0] || class_exhausted[1]) {
      RestoreLocalReloadHardRegUseOrder(m, best_mismatches, class_exhausted, &best_work_m);
    }
    for (int q = 0; q < nq; ++q) {
      const int p0 = best_work_m.qtys[q].first_reg;
      if (p0 >= kFirstPseudoRegister && best_work_m.qtys[q].n_refs != m.qtys[q].n_refs &&
          !best_work_m.qtys[q].member_regs.empty()) {
        const auto& members = best_work_m.qtys[q].member_regs;
        if (members.size() == 1) {
          dst_out->sideband.reg_n_refs[members[0]] = std::max(1, best_work_m.qtys[q].n_refs);
        } else {
          int tail_sum = 0;
          for (size_t mi = 1; mi < members.size(); ++mi) {
            dst_out->sideband.reg_n_refs[members[mi]] = 1;
            ++tail_sum;
          }
          dst_out->sideband.reg_n_refs[members[0]] =
              std::max(1, best_work_m.qtys[q].n_refs - tail_sum);
        }
      }
    }
  }
}

// Mirrors `reload1.c:3606-3712` (`order_regs_for_reload`, `hard_reg_use_compare`):
// when `ReconcileSingleBlockLocalPriorities` adjusts local `qty_n_refs` to match
// `block_alloc` order, preserve the natural `hard_reg_n_uses` relative order
// among hard registers in the same class (`FP_REGS` or `GR_REGS`) whenever an
// equivalent `block_alloc`-preserving `n_refs` assignment exists.
void RestoreLocalReloadHardRegUseOrder(const LocalBlockAllocModel& orig_m, int target_mismatches,
                                       const std::array<bool, 2>& class_exhausted,
                                       LocalBlockAllocModel* work_m) {
  const int nq = static_cast<int>(work_m->qtys.size());
  auto qty_target = [&](const LocalBlockAllocModel& mod, int q) -> int {
    if (mod.qtys[q].first_reg < kFirstPseudoRegister) {
      return -1;
    }
    const int p0 = mod.qtys[q].first_reg;
    const auto it = mod.target_hr_by_pseudo.find(p0);
    if (it == mod.target_hr_by_pseudo.end() || it->second < 0) {
      return -1;
    }
    const int hr = it->second - mod.reg_offset[p0];
    return (hr >= 0 && hr < kFirstPseudoRegister) ? hr : -1;
  };
  auto same_reload_class = [](int r1, int r2) {
    const bool fp1 = (r1 >= kFirstFprRegnum && r1 <= kLastFprRegnum);
    const bool fp2 = (r2 >= kFirstFprRegnum && r2 <= kLastFprRegnum);
    if (fp1 != fp2) {
      return false;
    }
    return fp1 || (r1 >= 2 && r1 < kFirstFprRegnum && r2 >= 2 && r2 < kFirstFprRegnum);
  };
  auto compute_uses = [&](const LocalBlockAllocModel& mod) {
    std::array<int, kFirstPseudoRegister> uses{};
    for (int q = 0; q < nq; ++q) {
      const int hr = qty_target(mod, q);
      if (hr < 0) {
        continue;
      }
      const int nregs = HardRegnoNregs(hr, mod.qtys[q].mode, mod.mips_cfg);
      for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
        uses[k] += mod.qtys[q].n_refs;
      }
    }
    return uses;
  };

  std::array<std::vector<int>, kFirstPseudoRegister> qtys_by_hr;
  for (int q = 0; q < nq; ++q) {
    const int hr = qty_target(orig_m, q);
    if (hr >= 0 && !orig_m.fixed_regs.TestBit(hr)) {
      qtys_by_hr[hr].push_back(q);
    }
  }
  const std::array<int, kFirstPseudoRegister> orig_uses = compute_uses(orig_m);
  for (int iter = 0; iter < 16; ++iter) {
    const std::array<int, kFirstPseudoRegister> cur_uses = compute_uses(*work_m);
    bool fixed_any = false;
    for (int hr_lo = 0; hr_lo < kFirstPseudoRegister && !fixed_any; ++hr_lo) {
      if (orig_uses[hr_lo] <= 0 || qtys_by_hr[hr_lo].empty()) {
        continue;
      }
      const int cls_lo = (hr_lo >= kFirstFprRegnum && hr_lo <= kLastFprRegnum) ? 0 : 1;
      if (!class_exhausted[cls_lo]) {
        continue;
      }
      for (int hr_hi = 0; hr_hi < kFirstPseudoRegister && !fixed_any; ++hr_hi) {
        if (hr_hi == hr_lo || orig_uses[hr_hi] <= 0 || qtys_by_hr[hr_hi].empty() ||
            !same_reload_class(hr_lo, hr_hi) ||
            (cur_uses[hr_lo] == orig_uses[hr_lo] && cur_uses[hr_hi] == orig_uses[hr_hi])) {
          continue;
        }
        if (!(std::tie(orig_uses[hr_lo], hr_lo) < std::tie(orig_uses[hr_hi], hr_hi))) {
          continue;
        }
        if (std::tie(cur_uses[hr_lo], hr_lo) < std::tie(cur_uses[hr_hi], hr_hi)) {
          continue;
        }
        const int need_delta = cur_uses[hr_lo] - cur_uses[hr_hi] + (hr_lo > hr_hi ? 1 : 0);
        // Step A: raise a single quantity on `hr_hi` if `block_alloc` stays valid.
        for (int q_hi : qtys_by_hr[hr_hi]) {
          LocalBlockAllocModel cand = *work_m;
          cand.qtys[q_hi].n_refs += need_delta;
          if (SimulateLocalBlockAlloc(cand, nullptr) <= target_mismatches) {
            *work_m = std::move(cand);
            fixed_any = true;
            break;
          }
        }
        if (fixed_any) {
          break;
        }
        // Step B: pair a shorter-span `hr_lo` quantity with a longer-span `hr_hi`
        // quantity so `n_refs(hr_hi)` outpaces `n_refs(hr_lo)` at equal priority.
        for (int q_lo_bump : qtys_by_hr[hr_lo]) {
          const int span_lo =
              std::max(1, work_m->qtys[q_lo_bump].death - work_m->qtys[q_lo_bump].birth);
          for (int q_hi_bump : qtys_by_hr[hr_hi]) {
            const int span_hi =
                std::max(1, work_m->qtys[q_hi_bump].death - work_m->qtys[q_hi_bump].birth);
            if (span_hi <= span_lo) {
              continue;
            }
            LocalBlockAllocModel base = *work_m;
            for (int q_other : qtys_by_hr[hr_hi]) {
              if (q_other != q_hi_bump) {
                base.qtys[q_other].n_refs =
                    std::max(1, static_cast<int>(base.qtys[q_other].member_regs.size()));
              }
            }
            for (int q_other : qtys_by_hr[hr_lo]) {
              if (q_other != q_lo_bump) {
                base.qtys[q_other].n_refs =
                    std::max(static_cast<int>(base.qtys[q_other].member_regs.size()),
                             orig_m.qtys[q_other].n_refs);
              }
            }
            const int start_hi = std::max(1, orig_m.qtys[q_hi_bump].n_refs);
            for (int r_hi = start_hi; r_hi <= start_hi + 32; ++r_hi) {
              LocalBlockAllocModel cand = base;
              cand.qtys[q_hi_bump].n_refs = r_hi;
              const int pri_hi = ComputeLocalQtyPriority(r_hi, span_hi, cand.qtys[q_hi_bump].size) +
                                 (q_lo_bump > q_hi_bump ? 1 : 0);
              int r_lo = std::max(1, orig_m.qtys[q_lo_bump].n_refs);
              while (r_lo <= r_hi + 32 &&
                     ComputeLocalQtyPriority(r_lo, span_lo, cand.qtys[q_lo_bump].size) < pri_hi) {
                ++r_lo;
              }
              cand.qtys[q_lo_bump].n_refs = r_lo;
              const int new_lo_pri =
                  ComputeLocalQtyPriority(r_lo, span_lo, cand.qtys[q_lo_bump].size);
              const int old_lo_pri = ComputeLocalQtyPriority(work_m->qtys[q_lo_bump].n_refs,
                                                             span_lo, work_m->qtys[q_lo_bump].size);
              const int scale_above = (new_lo_pri / std::max(1, old_lo_pri)) + 1;
              for (int qa = 0; qa < nq; ++qa) {
                if (qa == q_lo_bump || qa == q_hi_bump ||
                    cand.qtys[qa].first_reg < kFirstPseudoRegister) {
                  continue;
                }
                const int hqa = qty_target(cand, qa);
                if (hqa < 0 || hqa == hr_lo || hqa == hr_hi || !same_reload_class(hqa, hr_lo)) {
                  continue;
                }
                const int span_a = std::max(1, cand.qtys[qa].death - cand.qtys[qa].birth);
                const int old_a_pri =
                    ComputeLocalQtyPriority(work_m->qtys[qa].n_refs, span_a, cand.qtys[qa].size);
                if (old_a_pri >= old_lo_pri) {
                  cand.qtys[qa].n_refs = std::min(4096, cand.qtys[qa].n_refs * scale_above);
                }
              }
              const auto cand_uses = compute_uses(cand);
              if (std::tie(cand_uses[hr_lo], hr_lo) < std::tie(cand_uses[hr_hi], hr_hi) &&
                  SimulateLocalBlockAlloc(cand, nullptr) <= target_mismatches) {
                *work_m = std::move(cand);
                fixed_any = true;
                break;
              }
            }
            if (fixed_any) {
              break;
            }
          }
          if (fixed_any) {
            break;
          }
        }
      }
    }
    if (!fixed_any) {
      break;
    }
  }
}

std::array<bool, 2> ComputeReloadClassExhaustion(
    const absl::flat_hash_map<int, int>& orig_reg_n_refs, const RtlControlFlowGraph& base_greg_cfg,
    const MipsTargetConfig& mips_cfg, const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const HardRegSet& scratch_hard_regs) {
  std::array<int, kFirstPseudoRegister> orig_uses{};
  for (const auto& [p, hr] : base_greg_cfg.sideband.reg_renumber) {
    if (p < kFirstPseudoRegister || hr < 0 || hr >= kFirstPseudoRegister) {
      continue;
    }
    const auto rit = orig_reg_n_refs.find(p);
    if (rit == orig_reg_n_refs.end() || rit->second <= 0) {
      continue;
    }
    const auto mit = pseudo_modes.find(p);
    const RegModeInfo pm =
        (mit != pseudo_modes.end()) ? mit->second : RegModeInfo{RtlMachineMode::kSi, ""};
    const int nregs = HardRegnoNregs(hr, pm, mips_cfg);
    for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
      orig_uses[k] += rit->second;
    }
  }
  const HardRegSet fixed = FixedRegSet(mips_cfg);
  int free_fp = 0;
  for (int r = kFirstFprRegnum; r <= kLastFprRegnum; r += 2) {
    if (!fixed.TestBit(r) && !scratch_hard_regs.TestBit(r) && orig_uses[r] == 0) {
      ++free_fp;
    }
  }
  int free_gr = 0;
  for (int r = 2; r <= 25; ++r) {
    if (!fixed.TestBit(r) && !scratch_hard_regs.TestBit(r) && orig_uses[r] == 0) {
      ++free_gr;
    }
  }
  // `reload1.c:3681-3712` (`order_regs_for_reload`): registers with
  // `hard_reg_n_uses == 0` precede occupied registers in `potential_reload_regs`,
  // so `hard_reg_use_compare` only influences spill choice when fewer than two
  // zero-use registers remain in the class.
  return {free_fp < 2, free_gr < 2};
}

void PreserveReloadHardRegUseOrderAcrossPhases(
    RtlControlFlowGraph* dst_out, const absl::flat_hash_map<int, int>& orig_reg_n_refs,
    const RtlControlFlowGraph& base_greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const std::array<bool, 2>& class_exhausted) {
  if (!class_exhausted[0] && !class_exhausted[1]) {
    return;
  }
  auto compute_all_uses = [&](const absl::flat_hash_map<int, int>& refs_map) {
    std::array<int, kFirstPseudoRegister> uses{};
    for (const auto& [p, hr] : base_greg_cfg.sideband.reg_renumber) {
      if (p < kFirstPseudoRegister || hr < 0 || hr >= kFirstPseudoRegister) {
        continue;
      }
      const auto rit = refs_map.find(p);
      if (rit == refs_map.end() || rit->second <= 0) {
        continue;
      }
      const auto mit = pseudo_modes.find(p);
      const RegModeInfo pm =
          (mit != pseudo_modes.end()) ? mit->second : RegModeInfo{RtlMachineMode::kSi, ""};
      const int nregs = HardRegnoNregs(hr, pm, mips_cfg);
      for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
        uses[k] += rit->second;
      }
    }
    return uses;
  };
  const std::array<int, kFirstPseudoRegister> orig_uses = compute_all_uses(orig_reg_n_refs);
  std::array<int, kFirstPseudoRegister> cur_uses = compute_all_uses(dst_out->sideband.reg_n_refs);

  // Compute the smallest uniform integer scale factor `scale >= 2` for global
  // allocnos in each exhausted register class (`FP_REGS` / `GR_REGS`) whenever a
  // local `n_refs` bump inverted `reload1.c:3606` (`order_regs_for_reload`)
  // against a global hard register. Uniform scaling preserves `global.c:587-608`
  // (`allocno_compare`) priority order and `CALLER_SAVE_PROFITABLE` profitability.
  const HardRegSet call_used = CallUsedRegSet(mips_cfg);
  for (int cls = 0; cls < 2; ++cls) {
    if (!class_exhausted[cls]) {
      continue;
    }
    auto in_cls = [cls](int r) {
      return cls == 0 ? (r >= kFirstFprRegnum && r <= kLastFprRegnum)
                      : (r >= 2 && r < kFirstFprRegnum);
    };
    int scale = 1;
    for (int hr_lo = 0; hr_lo < kFirstPseudoRegister; ++hr_lo) {
      if (!in_cls(hr_lo) || orig_uses[hr_lo] <= 0 || cur_uses[hr_lo] <= orig_uses[hr_lo]) {
        continue;
      }
      for (int hr_hi = 0; hr_hi < kFirstPseudoRegister; ++hr_hi) {
        if (hr_hi == hr_lo || !in_cls(hr_hi) || orig_uses[hr_hi] <= 0 || cur_uses[hr_hi] <= 0) {
          continue;
        }
        if (!(std::tie(orig_uses[hr_lo], hr_lo) < std::tie(orig_uses[hr_hi], hr_hi))) {
          continue;
        }
        if (std::tie(cur_uses[hr_lo], hr_lo) < std::tie(cur_uses[hr_hi], hr_hi)) {
          continue;
        }
        // Only scale if `hr_hi` is owned by global allocno(s) (not local).
        bool has_global = false;
        for (const auto& [p, hr] : base_greg_cfg.sideband.reg_renumber) {
          if (p >= kFirstPseudoRegister && hr == hr_hi &&
              !dst_out->sideband.reg_renumber.contains(p)) {
            has_global = true;
            break;
          }
        }
        if (!has_global) {
          continue;
        }
        const int target_hi = cur_uses[hr_lo] + (hr_lo > hr_hi ? 1 : 0);
        const int need_scale = (target_hi + cur_uses[hr_hi] - 1) / cur_uses[hr_hi];
        scale = std::max(scale, need_scale);
      }
    }
    if (scale > 1) {
      for (const auto& [p, hr] : base_greg_cfg.sideband.reg_renumber) {
        if (p < kFirstPseudoRegister || hr < 0 || !in_cls(hr) ||
            dst_out->sideband.reg_renumber.contains(p)) {
          continue;
        }
        auto rit = dst_out->sideband.reg_n_refs.find(p);
        if (rit == dst_out->sideband.reg_n_refs.end() || rit->second <= 0) {
          continue;
        }
        const auto cc_it = dst_out->sideband.reg_n_calls_crossed.find(p);
        const int cc = (cc_it != dst_out->sideband.reg_n_calls_crossed.end()) ? cc_it->second : 0;
        if (mips_cfg.caller_saves && cc > 0 && call_used.TestBit(hr) && rit->second <= 4 * cc) {
          continue;
        }
        rit->second = std::min(4096, rit->second * scale);
      }
    }
  }
}

void ReconcileLocalBlockAllocPriorities(
    RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::btree_set<int>& live_at_any_block_start,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    const std::array<bool, 2>& class_exhausted) {
  for (size_t b = 0; b < dst_out->rpo_blocks.size(); ++b) {
    ReconcileSingleBlockLocalPriorities(b, dst_out, liveness_cfg, mips_cfg, live_at_any_block_start,
                                        pseudo_summaries, pseudo_modes, fp_op_pseudos,
                                        gr_or_mem_op_pseudos, class_exhausted);
  }
}

// Mirrors `global.c:1535-1605` (`set_preference`): when a local-allocated
// pseudo (`reg_renumber[p_loc] >= 0`) appears in `(set dst src)` paired with a
// global allocno `p_glob` (`dst` vs `src` or `XEXP(src, 0)`), `global_alloc`
// converts `p_loc` to its pre-assigned hard reg `hr_loc` and sets
// `hard_reg_preferences[allocno(p_glob)] |= hr_loc`. If that local-to-global
// preference forces `p_glob` onto `hr_loc` instead of its target hard reg,
// hand `p_loc` to `global_alloc` (erasing it from pre-`global_alloc`
// `reg_renumber`) iff `SimulateGlobalAlloc` strictly reduces primary mismatches.
bool ErasePreferencePoisoningLocalPseudos(
    RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const RtlControlFlowGraph& base_greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    const HardRegSet& scratch_hard_regs, HardRegSet* erased_global_hard_regs) {
  if (dst_out->sideband.reg_renumber.empty()) {
    return false;
  }
  GlobalAllocModel cur_m =
      BuildGlobalAllocModel(*dst_out, liveness_cfg, base_greg_cfg, mips_cfg, pseudo_summaries,
                            pseudo_modes, fp_op_pseudos, gr_or_mem_op_pseudos, scratch_hard_regs);
  if (cur_m.max_allocno == 0) {
    return false;
  }
  SimResult cur_sim = SimulateGlobalAlloc(cur_m, SortAllocnoOrder(cur_m));
  if (cur_sim.primary_mismatches == 0) {
    return false;
  }

  auto extract_pseudo = [](const RtlExpr& e) -> int {
    if (e.op == RtlOp::kReg && !e.IsArgPointerReg() && e.RegNo() >= kFirstPseudoRegister) {
      return e.RegNo();
    }
    if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg &&
        !e.children[0].IsArgPointerReg() && e.children[0].RegNo() >= kFirstPseudoRegister) {
      return e.children[0].RegNo();
    }
    return -1;
  };

  bool any_erased = false;
  for (int iter = 0; iter < 8 && cur_sim.primary_mismatches > 0; ++iter) {
    absl::btree_set<int> candidates;
    for (const RtlBasicBlock& blk : liveness_cfg.rpo_blocks) {
      for (const std::string& insn_s : blk.instructions) {
        auto pn = ParsePatternAndNotes(insn_s);
        if (!pn.ok()) {
          continue;
        }
        auto check_set = [&](const RtlExpr& s_expr) {
          if (s_expr.op != RtlOp::kSet || s_expr.children.size() != 2) {
            return;
          }
          const int d_p = extract_pseudo(s_expr.children[0]);
          const RtlExpr* src = &s_expr.children[1];
          int s_p = extract_pseudo(*src);
          if (s_p < 0 && !src->children.empty() && src->op != RtlOp::kMem &&
              src->op != RtlOp::kCall) {
            s_p = extract_pseudo(src->children[0]);
          }
          if (d_p < 0 || s_p < 0 || d_p == s_p) {
            return;
          }
          auto maybe_add = [&](int p_loc, int p_glob) {
            const auto lit = dst_out->sideband.reg_renumber.find(p_loc);
            if (lit == dst_out->sideband.reg_renumber.end() || lit->second < 0) {
              return;
            }
            if (p_glob >= static_cast<int>(cur_m.reg_allocno.size())) {
              return;
            }
            const int a = cur_m.reg_allocno[p_glob];
            if (a < 0 || cur_m.target_hr[a] < 0 || cur_m.target_hr[a] == lit->second) {
              return;
            }
            if (cur_sim.allocno_hr[a] != cur_m.target_hr[a]) {
              candidates.insert(p_loc);
            }
          };
          maybe_add(s_p, d_p);
          maybe_add(d_p, s_p);
        };
        if (pn->pattern.op == RtlOp::kSet) {
          check_set(pn->pattern);
        } else if (pn->pattern.op == RtlOp::kParallel) {
          for (const RtlExpr& ch : pn->pattern.children) {
            check_set(ch);
          }
        }
      }
    }
    bool improved = false;
    for (int p_loc : candidates) {
      const auto lit = dst_out->sideband.reg_renumber.find(p_loc);
      if (lit == dst_out->sideband.reg_renumber.end()) {
        continue;
      }
      const int saved_hr = lit->second;
      dst_out->sideband.reg_renumber.erase(p_loc);
      GlobalAllocModel cand_m = BuildGlobalAllocModel(
          *dst_out, liveness_cfg, base_greg_cfg, mips_cfg, pseudo_summaries, pseudo_modes,
          fp_op_pseudos, gr_or_mem_op_pseudos, scratch_hard_regs);
      const SimResult cand_sim = SimulateGlobalAlloc(cand_m, SortAllocnoOrder(cand_m));
      if (cand_sim.primary_mismatches < cur_sim.primary_mismatches) {
        if (erased_global_hard_regs != nullptr && saved_hr >= 0 &&
            saved_hr < kFirstPseudoRegister) {
          const auto m_it = pseudo_modes.find(p_loc);
          const RegModeInfo pmode =
              (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
          const int nregs = HardRegnoNregs(saved_hr, pmode, mips_cfg);
          for (int k = saved_hr; k < saved_hr + nregs && k < kFirstPseudoRegister; ++k) {
            erased_global_hard_regs->SetBit(k);
          }
        }
        cur_m = std::move(cand_m);
        cur_sim = cand_sim;
        improved = true;
        any_erased = true;
        break;
      }
      dst_out->sideband.reg_renumber[p_loc] = saved_hr;
    }
    if (!improved) {
      break;
    }
  }
  return any_erased;
}

void RunOptimizingGlobalAllocInverseOnce(const RtlControlFlowGraph& base_greg_cfg,
                                         const CompilationProfile& profile,
                                         const MipsTargetConfig& mips_cfg, size_t num_blocks,
                                         int base_max_regno, RtlControlFlowGraph* dst_out) {
  *dst_out = base_greg_cfg;
  dst_out->pass = RtlPassKind::kLreg;
  dst_out->sideband.max_regno = base_max_regno;

  // Deterministically recompute `flow.c` liveness sidebands and death notes on
  // a temporary copy (preserving `dst_out->rpo_blocks` verbatim and preserving
  // zero-occurrence phantom pseudos in `sideband`).
  RtlControlFlowGraph liveness_cfg = *dst_out;
  ReconstructRtlLiveness(&liveness_cfg, profile,
                         {.reload_completed = false,
                          .update_reg_notes = true,
                          .preserve_phantom_pseudos = true,
                          .apply_post_sched_and_equiv_live_length = true,
                          .include_call_reg_notes = true});
  dst_out->sideband.basic_block_live_at_start =
      std::move(liveness_cfg.sideband.basic_block_live_at_start);
  const absl::flat_hash_map<int, int> orig_reg_n_refs = liveness_cfg.sideband.reg_n_refs;
  dst_out->sideband.reg_n_refs = std::move(liveness_cfg.sideband.reg_n_refs);
  dst_out->sideband.reg_n_sets = std::move(liveness_cfg.sideband.reg_n_sets);
  dst_out->sideband.reg_n_deaths = std::move(liveness_cfg.sideband.reg_n_deaths);
  dst_out->sideband.reg_live_length = std::move(liveness_cfg.sideband.reg_live_length);
  dst_out->sideband.reg_n_calls_crossed = std::move(liveness_cfg.sideband.reg_n_calls_crossed);
  dst_out->sideband.max_regno =
      std::max(dst_out->sideband.max_regno, liveness_cfg.sideband.max_regno);
  {
    bool any_pseudo_live_len = false;
    for (const auto& [r, len] : dst_out->sideband.reg_live_length) {
      if (r >= kFirstPseudoRegister && len != 0) {
        any_pseudo_live_len = true;
        break;
      }
    }
    if (!any_pseudo_live_len) {
      dst_out->sideband.reg_live_length.try_emplace(kFirstPseudoRegister, -1);
      dst_out->sideband.basic_block_live_at_start.try_emplace(0);
    }
  }

  // `combine.c:10839` (`move_deaths`) can emit a standalone `(use (reg:M p))`
  // anchor before `combine.c:2336` eliminates the last setter of `p` and
  // clears `reg_n_refs[p] = 0` (`reg_n_sets[p] == 0`, `p` not live at entry).
  // Clear `reg_n_refs` and block-start liveness for such orphaned `USE`-only
  // pseudos so neither `GlobalAllocModel` nor `global_alloc` allocates them.
  {
    absl::btree_set<int> use_insn_pseudos;
    absl::btree_set<int> non_use_insn_pseudos;
    for (const RtlBasicBlock& blk : dst_out->rpo_blocks) {
      for (const std::string& insn_str : blk.instructions) {
        auto pn = ParsePatternAndNotes(insn_str);
        if (!pn.ok()) {
          continue;
        }
        absl::btree_set<int>* bucket =
            (pn->pattern.op == RtlOp::kUse) ? &use_insn_pseudos : &non_use_insn_pseudos;
        pn->pattern.Visit([&](const RtlExpr& expr) {
          if (expr.IsPseudoReg()) {
            bucket->insert(expr.RegNo());
          }
        });
        for (const RtlExpr& fu : pn->call_fusage) {
          fu.Visit([&](const RtlExpr& expr) {
            if (expr.IsPseudoReg()) {
              non_use_insn_pseudos.insert(expr.RegNo());
            }
          });
        }
      }
    }
    for (int p : use_insn_pseudos) {
      if (non_use_insn_pseudos.contains(p) || base_greg_cfg.sideband.reg_renumber.contains(p)) {
        continue;
      }
      if (auto s_it = dst_out->sideband.reg_n_sets.find(p);
          s_it != dst_out->sideband.reg_n_sets.end() && s_it->second != 0) {
        continue;
      }
      dst_out->sideband.reg_n_refs.erase(p);
      dst_out->sideband.reg_live_length.erase(p);
      dst_out->sideband.reg_n_calls_crossed.erase(p);
      std::vector<int> empty_bbs;
      for (auto& [bb_id, live_set] : dst_out->sideband.basic_block_live_at_start) {
        live_set.erase(p);
        if (live_set.empty()) {
          empty_bbs.push_back(bb_id);
        }
      }
      for (int bb_id : empty_bbs) {
        dst_out->sideband.basic_block_live_at_start.erase(bb_id);
      }
    }
  }

  const MipsCfgRegAllocSummary alloc_summary =
      AnalyzeCfgForMipsRegAlloc(*dst_out, liveness_cfg, mips_cfg);
  const absl::btree_set<int>& live_at_any_block_start = alloc_summary.live_at_any_block_start;
  const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries =
      alloc_summary.pseudo_summaries;
  const absl::btree_map<int, RegModeInfo>& pseudo_modes = alloc_summary.pseudo_modes;
  const absl::btree_set<int>& fp_op_pseudos = alloc_summary.fp_op_pseudos;
  const absl::btree_set<int>& gr_or_mem_op_pseudos = alloc_summary.gr_or_mem_op_pseudos;
  const HardRegSet& scratch_hard_regs = alloc_summary.scratch_hard_regs;
  const absl::btree_set<int>& all_di_mult_pseudos = alloc_summary.all_di_mult_pseudos;
  const int num_hard_non_hi_di_mults = alloc_summary.num_hard_non_hi_di_mults;
  const std::vector<std::vector<bool>>& block_insn_clobbers_md =
      alloc_summary.block_insn_clobbers_md;

  // Determine which pseudos in `sideband.reg_renumber` were allocated by
  // `global.c:global_alloc` rather than `local-alloc.c:block_alloc`.
  std::vector<int> global_pseudos_to_erase;
  for (const auto& [regno, hard_reg] : dst_out->sideband.reg_renumber) {
    bool is_global_alloc_pseudo = false;
    const auto mode_it = pseudo_modes.find(regno);
    const bool is_local_eligible_32bit_di_md = !mips_cfg.gp_64bit && hard_reg == kHiRegnum &&
                                               mode_it != pseudo_modes.end() &&
                                               mode_it->second.mode == RtlMachineMode::kDi;
    if (regno < kFirstPseudoRegister || hard_reg < 0 || hard_reg == kFramePointerRegnum ||
        (hard_reg >= kHiRegnum && !is_local_eligible_32bit_di_md)) {
      is_global_alloc_pseudo = true;
    } else if (live_at_any_block_start.contains(regno)) {
      is_global_alloc_pseudo = true;
    } else if (auto death_it = dst_out->sideband.reg_n_deaths.find(regno);
               death_it == dst_out->sideband.reg_n_deaths.end() || death_it->second != 1) {
      is_global_alloc_pseudo = true;
    } else {
      auto sum_it = pseudo_summaries.find(regno);
      if (sum_it == pseudo_summaries.end() || sum_it->second.blocks_referenced.size() != 1 ||
          !sum_it->second.has_set_in_block || sum_it->second.used_before_set ||
          sum_it->second.alive_after_redef || sum_it->second.note_deaths >= 2) {
        is_global_alloc_pseudo = true;
      } else if (sum_it->second.def_count_in_block == 1 &&
                 sum_it->second.single_reg_md_class != MipsRegClass::kNoRegs &&
                 !sum_it->second.used_in_mem_address && sum_it->second.total_read_count <= 1) {
        is_global_alloc_pseudo = true;
      }
    }

    if (is_global_alloc_pseudo) {
      global_pseudos_to_erase.push_back(regno);
    }
  }

  HardRegSet erased_global_hard_regs;
  for (int regno : global_pseudos_to_erase) {
    if (auto ren_it = dst_out->sideband.reg_renumber.find(regno);
        ren_it != dst_out->sideband.reg_renumber.end() && ren_it->second >= 0 &&
        ren_it->second < kFirstPseudoRegister) {
      const int hr = ren_it->second;
      const auto m_it = pseudo_modes.find(regno);
      const RegModeInfo pmode =
          (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
      const int nregs = HardRegnoNregs(hr, pmode, mips_cfg);
      for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
        erased_global_hard_regs.SetBit(k);
      }
    }
    dst_out->sideband.reg_renumber.erase(regno);
  }

  EnforceBlockAllocQuantityAtomicity(dst_out, liveness_cfg, mips_cfg, live_at_any_block_start,
                                     pseudo_summaries, pseudo_modes, fp_op_pseudos,
                                     gr_or_mem_op_pseudos, &erased_global_hard_regs,
                                     /*atomicity_erased_pseudos=*/nullptr);
  if (ErasePreferencePoisoningLocalPseudos(
          dst_out, liveness_cfg, base_greg_cfg, mips_cfg, pseudo_summaries, pseudo_modes,
          fp_op_pseudos, gr_or_mem_op_pseudos, scratch_hard_regs, &erased_global_hard_regs)) {
    EnforceBlockAllocQuantityAtomicity(dst_out, liveness_cfg, mips_cfg, live_at_any_block_start,
                                       pseudo_summaries, pseudo_modes, fp_op_pseudos,
                                       gr_or_mem_op_pseudos, &erased_global_hard_regs,
                                       /*atomicity_erased_pseudos=*/nullptr);
  }

  // Prune post-reload `mark_home_live` callee-saved registers (`16..23, 30`
  // and `52..63`) that belonged solely to lifted global-alloc pseudos so
  // pre-`global_alloc` `regs_ever_live` matches `flow` (`saved_gpr_regnos` /
  // `saved_fpr_regnos`).
  if (!erased_global_hard_regs.IsEmpty()) {
    HardRegSet explicit_or_local_hard_regs;
    if (dst_out->sideband.frame_pointer_needed) {
      explicit_or_local_hard_regs.SetBit(kFramePointerRegnum);
    }
    for (const auto& [regno, hr] : dst_out->sideband.reg_renumber) {
      if (regno >= kFirstPseudoRegister && hr >= 0 && hr < kFirstPseudoRegister) {
        const auto m_it = pseudo_modes.find(regno);
        const RegModeInfo pmode =
            (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
        const int nregs = HardRegnoNregs(hr, pmode, mips_cfg);
        for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
          explicit_or_local_hard_regs.SetBit(k);
        }
      }
    }
    auto scan_explicit_expr = [&](const RtlExpr& root, bool is_store_dst) {
      root.Visit([&](const RtlExpr& e) {
        if (e.op != RtlOp::kReg || e.IsArgPointerReg() || e.RegNo() >= kFirstPseudoRegister) {
          return;
        }
        const int r0 = e.RegNo();
        if (r0 == kArgPointerRegnum || (r0 == kFramePointerRegnum && !is_store_dst)) {
          return;
        }
        if (scratch_hard_regs.TestBit(r0) && !is_store_dst) {
          return;
        }
        const int nregs = HardRegnoNregs(r0, {e.mode, e.raw_mode}, mips_cfg);
        for (int k = r0; k < r0 + nregs && k < kFirstPseudoRegister; ++k) {
          if (!scratch_hard_regs.TestBit(k)) {
            explicit_or_local_hard_regs.SetBit(k);
          }
        }
      });
    };
    for (const RtlBasicBlock& blk : dst_out->rpo_blocks) {
      for (const std::string& insn_s : blk.instructions) {
        auto pn = ParsePatternAndNotes(insn_s);
        if (!pn.ok() || pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kCodeLabel ||
            pn->pattern.op == RtlOp::kBarrier) {
          continue;
        }
        NoteStores(pn->pattern, [&](const RtlExpr& dst, const RtlExpr&) {
          const RtlExpr* base_dst = &dst;
          while (base_dst->op == RtlOp::kSubreg || base_dst->op == RtlOp::kStrictLowPart ||
                 base_dst->op == RtlOp::kZeroExtract || base_dst->op == RtlOp::kSignExtract) {
            if (base_dst->children.empty()) {
              break;
            }
            base_dst = &base_dst->children[0];
          }
          if (base_dst->op == RtlOp::kReg) {
            scan_explicit_expr(*base_dst, /*is_store_dst=*/true);
          }
        });
        scan_explicit_expr(pn->pattern, /*is_store_dst=*/false);
        for (const RtlExpr& fu : pn->call_fusage) {
          scan_explicit_expr(fu, /*is_store_dst=*/false);
        }
      }
    }
    if (dst_out->sideband.epilogue_delay_pattern.has_value() &&
        !dst_out->sideband.epilogue_delay_pattern->empty()) {
      if (auto ep = ParsePatternAndNotes(*dst_out->sideband.epilogue_delay_pattern); ep.ok()) {
        NoteStores(ep->pattern, [&](const RtlExpr& dst, const RtlExpr&) {
          if (dst.op == RtlOp::kReg) {
            scan_explicit_expr(dst, /*is_store_dst=*/true);
          }
        });
        scan_explicit_expr(ep->pattern, /*is_store_dst=*/false);
      }
    }

    auto& saved_gprs = dst_out->sideband.saved_gpr_regnos;
    saved_gprs.erase(std::remove_if(saved_gprs.begin(), saved_gprs.end(),
                                    [&](int r) {
                                      const bool callee_saved =
                                          (r >= 16 && r <= 23) || r == kFramePointerRegnum;
                                      return callee_saved && erased_global_hard_regs.TestBit(r) &&
                                             !explicit_or_local_hard_regs.TestBit(r);
                                    }),
                     saved_gprs.end());
    auto& saved_fprs = dst_out->sideband.saved_fpr_regnos;
    saved_fprs.erase(std::remove_if(saved_fprs.begin(), saved_fprs.end(),
                                    [&](int r) {
                                      return r >= 52 && r <= 63 &&
                                             erased_global_hard_regs.TestBit(r) &&
                                             !explicit_or_local_hard_regs.TestBit(r);
                                    }),
                     saved_fprs.end());
  }

  if (!mips_cfg.gp_64bit &&
      static_cast<int>(all_di_mult_pseudos.size()) + num_hard_non_hi_di_mults >= 2) {
    for (int p : all_di_mult_pseudos) {
      if (base_greg_cfg.sideband.reg_renumber.contains(p) || live_at_any_block_start.contains(p)) {
        continue;
      }
      if (auto d_it = dst_out->sideband.reg_n_deaths.find(p);
          d_it == dst_out->sideband.reg_n_deaths.end() || d_it->second != 1) {
        continue;
      }
      if (auto s_it = dst_out->sideband.reg_n_sets.find(p);
          s_it == dst_out->sideband.reg_n_sets.end() || s_it->second != 1) {
        continue;
      }
      if (auto c_it = dst_out->sideband.reg_n_calls_crossed.find(p);
          c_it != dst_out->sideband.reg_n_calls_crossed.end() && c_it->second != 0) {
        continue;
      }
      auto sum_it = pseudo_summaries.find(p);
      if (sum_it == pseudo_summaries.end()) {
        continue;
      }
      const PseudoLivenessSummary& sum = sum_it->second;
      if (sum.blocks_referenced.size() != 1 || !sum.has_set_in_block ||
          sum.def_count_in_block != 1 || !sum.defined_by_di_mult || sum.used_before_set ||
          sum.alive_after_redef || sum.note_deaths != 1 || sum.death_insn_idx <= sum.def_insn_idx) {
        continue;
      }
      const size_t bb_idx = *sum.blocks_referenced.begin();
      bool md_conflict_in_range = false;
      for (size_t k = sum.def_insn_idx + 1; k < sum.death_insn_idx; ++k) {
        if (block_insn_clobbers_md[bb_idx][k]) {
          md_conflict_in_range = true;
          break;
        }
      }
      if (md_conflict_in_range) {
        continue;
      }
      bool other_mult_needs_md_reload = (num_hard_non_hi_di_mults > 0);
      for (int q : all_di_mult_pseudos) {
        if (q == p) {
          continue;
        }
        auto q_it = base_greg_cfg.sideband.reg_renumber.find(q);
        if (q_it == base_greg_cfg.sideband.reg_renumber.end() || q_it->second != kHiRegnum) {
          other_mult_needs_md_reload = true;
          break;
        }
      }
      if (other_mult_needs_md_reload) {
        dst_out->sideband.reg_renumber[p] = kHiRegnum;
      }
    }
  }

  const std::array<bool, 2> class_exhausted = ComputeReloadClassExhaustion(
      orig_reg_n_refs, base_greg_cfg, mips_cfg, pseudo_modes, scratch_hard_regs);
  ReconcileLocalBlockAllocPriorities(dst_out, liveness_cfg, mips_cfg, live_at_any_block_start,
                                     pseudo_summaries, pseudo_modes, fp_op_pseudos,
                                     gr_or_mem_op_pseudos, class_exhausted);
  ReconcileGlobalAllocPriorities(dst_out, liveness_cfg, base_greg_cfg, mips_cfg,
                                 live_at_any_block_start, pseudo_summaries, pseudo_modes,
                                 fp_op_pseudos, gr_or_mem_op_pseudos, scratch_hard_regs);
  PreserveReloadHardRegUseOrderAcrossPhases(dst_out, orig_reg_n_refs, base_greg_cfg, mips_cfg,
                                            pseudo_modes, class_exhausted);
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg,
                                                      const CompilationProfile& profile) {
  int max_seen_regno = kFirstPseudoRegister;
  if (absl::Status st = ValidateInvertGlobalAllocInput(greg_cfg, &max_seen_regno); !st.ok()) {
    return st;
  }

  const size_t num_blocks = greg_cfg.rpo_blocks.size();
  const ParsedCompilerFlags flags = ParseProfileFlags(profile);
  const MipsTargetConfig mips_cfg = ExtractMipsTargetConfig(flags);

  if (flags.opt_level == 0) {
    return InvertStupidGlobalAlloc(greg_cfg, profile, max_seen_regno);
  }

  RtlControlFlowGraph out;
  RunOptimizingGlobalAllocInverseOnce(greg_cfg, profile, mips_cfg, num_blocks, max_seen_regno,
                                      &out);
  return out;
}

absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg) {
  return InvertGlobalAlloc(greg_cfg, {.optimize_level = 2, .c_flags = {"-O2"}});
}

}  // namespace rom_nom_nom::lifter
