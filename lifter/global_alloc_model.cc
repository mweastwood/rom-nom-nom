#include "lifter/global_alloc_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_reg_alloc_common.h"

namespace rom_nom_nom::lifter {
namespace {

// Mirrors `global.c:365-430`: assigns allocno indices to unallocated live
// pseudos and populates sizes, single-block ownership, call-crossing counts,
// ref/live-length weights, target hard regs, and MIPS register classes
// (`config/mips/mips.h:2914-2929`, `regclass.c:960`).
void InitGlobalAllocnosAndClasses(
    const RtlControlFlowGraph& out, const RtlControlFlowGraph& liveness_cfg,
    const RtlControlFlowGraph& greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    GlobalAllocModel* m) {
  m->mips_cfg = mips_cfg;
  m->max_regno = std::max(out.sideband.max_regno, kFirstPseudoRegister);
  m->reg_allocno.assign(m->max_regno, -1);
  m->local_reg_renumber.assign(m->max_regno, -1);
  m->pseudo_modes.assign(m->max_regno, {RtlMachineMode::kSi, ""});

  for (const auto& [regno, info] : pseudo_modes) {
    if (regno >= 0 && regno < m->max_regno) {
      m->pseudo_modes[regno] = info;
    }
  }
  for (const auto& [regno, hr] : out.sideband.reg_renumber) {
    if (regno >= kFirstPseudoRegister && regno < m->max_regno && hr >= 0) {
      m->local_reg_renumber[regno] = hr;
    }
  }

  for (int i = kFirstPseudoRegister; i < m->max_regno; ++i) {
    const auto refs_it = out.sideband.reg_n_refs.find(i);
    const int n_refs = refs_it != out.sideband.reg_n_refs.end() ? refs_it->second : 0;
    const auto len_it = out.sideband.reg_live_length.find(i);
    const int live_len = len_it != out.sideband.reg_live_length.end() ? len_it->second : 0;
    if (n_refs != 0 && m->local_reg_renumber[i] < 0 && live_len != -1 && live_len != 0) {
      m->reg_allocno[i] = m->max_allocno++;
    }
  }
  if (m->max_allocno == 0) {
    return;
  }

  m->num_blocks = static_cast<int>(liveness_cfg.rpo_blocks.size());
  m->allocno_reg.assign(m->max_allocno, 0);
  m->allocno_size.assign(m->max_allocno, 1);
  m->allocno_calls_crossed.assign(m->max_allocno, 0);
  m->allocno_n_refs.assign(m->max_allocno, 0);
  m->allocno_live_length.assign(m->max_allocno, 0);
  m->allocno_single_block.assign(m->max_allocno, -2);
  m->pref_class.assign(m->max_allocno, MipsRegClass::kGrRegs);
  m->alt_class.assign(m->max_allocno, MipsRegClass::kNoRegs);
  m->target_hr.assign(m->max_allocno, -1);
  m->allocno_md_clobber_blocks.resize(m->max_allocno);
  m->block_md_pseudos.resize(m->num_blocks);
  m->block_hard_md_needs.resize(m->num_blocks);

  absl::btree_set<int> model_live_at_block_start;
  for (const auto& [bb_idx, live_set] : out.sideband.basic_block_live_at_start) {
    if (bb_idx >= 0 && bb_idx < m->num_blocks) {
      for (int r : live_set) {
        if (r >= kFirstPseudoRegister) {
          model_live_at_block_start.insert(r);
        }
      }
    }
  }

  for (int i = kFirstPseudoRegister; i < m->max_regno; ++i) {
    const int a = m->reg_allocno[i];
    if (a < 0) {
      continue;
    }
    m->allocno_reg[a] = i;
    m->allocno_size[a] = std::max(1, PseudoRegnoSize(m->pseudo_modes[i], mips_cfg));
    if (!model_live_at_block_start.contains(i)) {
      if (auto sum_sb = pseudo_summaries.find(i);
          sum_sb != pseudo_summaries.end() && sum_sb->second.blocks_referenced.size() == 1) {
        m->allocno_single_block[a] = static_cast<int>(*sum_sb->second.blocks_referenced.begin());
      }
    }
    if (auto c_it = out.sideband.reg_n_calls_crossed.find(i);
        c_it != out.sideband.reg_n_calls_crossed.end()) {
      m->allocno_calls_crossed[a] += c_it->second;
    }
    if (auto r_it = out.sideband.reg_n_refs.find(i); r_it != out.sideband.reg_n_refs.end()) {
      m->allocno_n_refs[a] += r_it->second;
    }
    if (auto l_it = out.sideband.reg_live_length.find(i);
        l_it != out.sideband.reg_live_length.end() && m->allocno_live_length[a] < l_it->second) {
      m->allocno_live_length[a] = l_it->second;
    }
    if (m->allocno_live_length[a] == 0) {
      m->allocno_live_length[a] = -1;
    }
    if (auto t_it = greg_cfg.sideband.reg_renumber.find(i);
        t_it != greg_cfg.sideband.reg_renumber.end()) {
      m->target_hr[a] = t_it->second;
    }

    const RegModeInfo& pmode = m->pseudo_modes[i];
    if (IsFpCcMode(pmode)) {
      m->pref_class[a] = MipsRegClass::kStRegs;
      m->alt_class[a] = MipsRegClass::kNoRegs;
    } else if (IsFloatOrComplexFloatMode(pmode) && !mips_cfg.soft_float) {
      // Pre-existing `regclass.c` stand-in extracted verbatim from
      // `invert_global_alloc.cc`: uses only the Stage 4 target post-pass
      // `greg_cfg.sideband.reg_renumber` (`target_hr`, zero `pre_state`
      // peeking) to disambiguate `SF`/`DF` allocnos without FP-ALU uses between
      // `GR_REGS` and `FP_REGS`. Pure forward callers pass an empty `greg_cfg`
      // (`target_hr[a] == -1`). Slated for replacement by a pure forward
      // `regclass.c:record_reg_classes` cost model in Sub-PR 2.
      if (!fp_op_pseudos.contains(i) && gr_or_mem_op_pseudos.contains(i) && m->target_hr[a] >= 0 &&
          m->target_hr[a] < kFirstFprRegnum) {
        m->pref_class[a] = MipsRegClass::kGrRegs;
        m->alt_class[a] = MipsRegClass::kAllRegs;
      } else {
        // On MIPS, `REGISTER_MOVE_COST(GR_REGS, FP_REGS) == 4`
        // (`config/mips/mips.h:2914-2925`), whereas `MEMORY_MOVE_COST` is `6`
        // on R4000/R6000 and `4` on R3000 (`config/mips/mips.h:2928-2929`).
        // Thus `regclass.c:960` (`p->cost[GR_REGS] < p->mem_cost`) includes
        // `GR_REGS` (`ALL_REGS`) in `reg_alternate_class` iff `mem_move_cost_6`.
        m->pref_class[a] = MipsRegClass::kFpRegs;
        m->alt_class[a] = mips_cfg.mem_move_cost_6 ? MipsRegClass::kAllRegs : MipsRegClass::kNoRegs;
      }
    } else {
      auto sum_it = pseudo_summaries.find(i);
      if (sum_it == pseudo_summaries.end()) {
        // Zero-occurrence `combine`-eliminated phantom pseudo (`costs[i] == 0`
        // for all classes and `mem_cost == 0`): in `regclass.c:931-975`, the
        // downward scan starts at `ST_REGS` (`ALL_REGS - 1`) and every pairwise
        // `reg_class_subunion[ST_REGS][c]` (`regclass.c:230-261`) keeps the
        // highest-numbered non-subset class `ST_REGS`, while `alt = NO_REGS`.
        // Because `HARD_REGNO_MODE_OK(67, mode)` is false for non-FP-CC modes
        // (`config/mips/mips.c:3442-3443`), `global_alloc` leaves it unallocated.
        m->pref_class[a] = MipsRegClass::kStRegs;
        m->alt_class[a] = MipsRegClass::kNoRegs;
      } else if (sum_it->second.total_def_count == 1 &&
                 sum_it->second.single_reg_md_class != MipsRegClass::kNoRegs &&
                 !sum_it->second.used_in_mem_address && sum_it->second.total_read_count <= 1) {
        m->pref_class[a] = sum_it->second.single_reg_md_class;
        m->alt_class[a] = MipsRegClass::kGrRegs;
      } else if (!mips_cfg.gp_64bit && sum_it->second.total_def_count == 1 &&
                 sum_it->second.defined_by_di_mult && !sum_it->second.used_in_mem_address &&
                 sum_it->second.total_read_count <= 1) {
        m->pref_class[a] = MipsRegClass::kMdRegs;
        m->alt_class[a] = MipsRegClass::kGrRegs;
      } else {
        m->pref_class[a] = MipsRegClass::kGrRegs;
        m->alt_class[a] = MipsRegClass::kNoRegs;
      }
    }
  }
}

// Reconstructs `regs_ever_live` (`flow.c:2046, 2443, 2485`), `bad_spill_regs`,
// `base_regs_used_so_far`, `base_local_reg_n_refs`/`live_length`, and
// `non_move_read_pseudo_groups`, excluding `local_alloc` `scratch_list`
// allocations (`global.c:451`, `reload1.c:486, 506`).
void ComputeRegsEverLiveAndBaseUseStats(const RtlControlFlowGraph& out,
                                        const RtlControlFlowGraph& liveness_cfg,
                                        const MipsTargetConfig& mips_cfg,
                                        const HardRegSet& scratch_hard_regs, GlobalAllocModel* m) {
  m->fixed_regs = FixedRegSet(mips_cfg);
  m->call_used_regs = CallUsedRegSet(mips_cfg);
  m->no_global_alloc_regs.Clear();
  m->no_global_alloc_regs.SetBit(kArgPointerRegnum);
  if (!mips_cfg.omit_frame_pointer || out.sideband.frame_pointer_needed) {
    m->no_global_alloc_regs.SetBit(kFramePointerRegnum);
  }

  HardRegSet regs_ever_live;
  for (const RtlBasicBlock& blk : liveness_cfg.rpo_blocks) {
    for (const std::string& insn_str : blk.instructions) {
      auto pn = ParsePatternAndNotes(insn_str);
      if (!pn.ok() || pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kCodeLabel ||
          pn->pattern.op == RtlOp::kBarrier) {
        continue;
      }
      auto mark_explicit_hard_regs = [&](const RtlExpr& root, bool is_store_dst) {
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
              regs_ever_live.SetBit(k);
            }
          }
        });
      };
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
          mark_explicit_hard_regs(*base_dst, /*is_store_dst=*/true);
        }
      });
      mark_explicit_hard_regs(pn->pattern, /*is_store_dst=*/false);
      for (const RtlExpr& fu : pn->call_fusage) {
        mark_explicit_hard_regs(fu, /*is_store_dst=*/false);
      }

      if (pn->pattern.op != RtlOp::kUse) {
        const RtlExpr* sset = SingleSet(*pn);
        const bool is_plain_reg_move = sset != nullptr && sset->children.size() == 2 &&
                                       sset->children[0].op == RtlOp::kReg &&
                                       sset->children[1].op == RtlOp::kReg;
        if (!is_plain_reg_move) {
          absl::btree_set<int> read_pseudos;
          auto collect_reads = [&](const auto& self, const RtlExpr& e, bool is_dst) -> void {
            if (e.op == RtlOp::kSet && e.children.size() == 2) {
              self(self, e.children[0], true);
              self(self, e.children[1], false);
              return;
            }
            if (e.op == RtlOp::kClobber) {
              if (!e.children.empty() && e.children[0].op == RtlOp::kMem) {
                self(self, e.children[0], false);
              }
              return;
            }
            if (is_dst && (e.op == RtlOp::kReg || (e.op == RtlOp::kSubreg && !e.children.empty() &&
                                                   e.children[0].op == RtlOp::kReg))) {
              return;
            }
            if (e.IsPseudoReg()) {
              read_pseudos.insert(e.RegNo());
              return;
            }
            for (const RtlExpr& ch : e.children) {
              self(self, ch, false);
            }
          };
          collect_reads(collect_reads, pn->pattern, false);
          if (!read_pseudos.empty()) {
            m->non_move_read_pseudo_groups.emplace_back(read_pseudos.begin(), read_pseudos.end());
          }
        }
      }
    }
  }

  m->bad_spill_regs = m->fixed_regs;
  m->bad_spill_regs.SetBit(kArgPointerRegnum);
  m->bad_spill_regs.SetBit(kFramePointerRegnum);
  m->bad_spill_regs.Ior(regs_ever_live);

  m->reg_n_refs.assign(m->max_regno, 0);
  for (int i = kFirstPseudoRegister; i < m->max_regno; ++i) {
    if (auto it = out.sideband.reg_n_refs.find(i); it != out.sideband.reg_n_refs.end()) {
      m->reg_n_refs[i] = it->second;
    }
  }

  m->base_regs_used_so_far = m->call_used_regs;
  m->base_regs_used_so_far.Ior(regs_ever_live);
  for (int i = kFirstPseudoRegister; i < m->max_regno; ++i) {
    if (m->local_reg_renumber[i] >= 0) {
      const int hr = m->local_reg_renumber[i];
      const int lim = hr + HardRegnoNregs(hr, m->pseudo_modes[i], mips_cfg);
      for (int r = hr; r < lim && r < kFirstPseudoRegister; ++r) {
        m->base_regs_used_so_far.SetBit(r);
      }
    }
  }

  m->base_local_reg_n_refs.assign(kFirstPseudoRegister, 0);
  m->base_local_reg_live_length.assign(kFirstPseudoRegister, 0);
  for (int i = kFirstPseudoRegister; i < m->max_regno; ++i) {
    if (m->reg_allocno[i] < 0 && m->local_reg_renumber[i] >= 0) {
      const int regno = m->local_reg_renumber[i];
      const int endregno = regno + HardRegnoNregs(regno, m->pseudo_modes[i], mips_cfg);
      const int refs = m->reg_n_refs[i];
      const int len =
          out.sideband.reg_live_length.contains(i) ? out.sideband.reg_live_length.at(i) : 0;
      for (int j = regno; j < endregno && j < kFirstPseudoRegister; ++j) {
        m->base_local_reg_n_refs[j] += refs;
        m->base_local_reg_live_length[j] += len;
      }
    }
  }
  for (int r = 0; r < kFirstPseudoRegister; ++r) {
    if (regs_ever_live.TestBit(r) || scratch_hard_regs.TestBit(r)) {
      m->base_local_reg_n_refs[r] = 0;
    }
  }
}

// Mirrors `global.c:1535-1605` (`set_preference`).
void SetGlobalPreference(const RtlExpr& dest, const RtlExpr& orig_src,
                         const MipsTargetConfig& mips_cfg, GlobalAllocModel* m) {
  const RtlExpr* src = &orig_src;
  int offset = 0;
  bool copy = true;
  if (FirstFormatCharIsExpr(src->op) && !src->children.empty()) {
    src = &src->children[0];
    copy = false;
  }
  int src_regno = -1;
  if (src->op == RtlOp::kReg) {
    src_regno = src->IsArgPointerReg() ? kArgPointerRegnum : src->RegNo();
  } else if (src->op == RtlOp::kSubreg && !src->children.empty() &&
             src->children[0].op == RtlOp::kReg) {
    src_regno = src->children[0].IsArgPointerReg() ? kArgPointerRegnum : src->children[0].RegNo();
    offset += static_cast<int>(src->int_val);
  } else {
    return;
  }

  int dest_regno = -1;
  if (dest.op == RtlOp::kReg) {
    dest_regno = dest.IsArgPointerReg() ? kArgPointerRegnum : dest.RegNo();
  } else if (dest.op == RtlOp::kSubreg && !dest.children.empty() &&
             dest.children[0].op == RtlOp::kReg) {
    dest_regno = dest.children[0].IsArgPointerReg() ? kArgPointerRegnum : dest.children[0].RegNo();
    offset -= static_cast<int>(dest.int_val);
  } else {
    return;
  }

  if (src_regno >= kFirstPseudoRegister && src_regno < m->max_regno &&
      m->local_reg_renumber[src_regno] >= 0) {
    src_regno = m->local_reg_renumber[src_regno];
  }
  if (dest_regno >= kFirstPseudoRegister && dest_regno < m->max_regno &&
      m->local_reg_renumber[dest_regno] >= 0) {
    dest_regno = m->local_reg_renumber[dest_regno];
  }

  if (dest_regno >= 0 && dest_regno < kFirstPseudoRegister && src_regno >= kFirstPseudoRegister &&
      src_regno < m->max_regno && m->reg_allocno[src_regno] >= 0) {
    const int adj_dest = dest_regno - offset;
    const int a = m->reg_allocno[src_regno];
    if (adj_dest >= 0 && adj_dest < kFirstPseudoRegister) {
      if (copy) {
        m->base_hard_reg_copy_preferences[a].SetBit(adj_dest);
      }
      m->base_hard_reg_preferences[a].SetBit(adj_dest);
      const int nregs = HardRegnoNregs(adj_dest, {dest.mode, dest.raw_mode}, mips_cfg);
      for (int k = adj_dest; k < adj_dest + nregs && k < kFirstPseudoRegister; ++k) {
        m->base_hard_reg_full_preferences[a].SetBit(k);
      }
    }
  }

  if (src_regno >= 0 && src_regno < kFirstPseudoRegister && dest_regno >= kFirstPseudoRegister &&
      dest_regno < m->max_regno && m->reg_allocno[dest_regno] >= 0) {
    const int adj_src = src_regno + offset;
    const int a = m->reg_allocno[dest_regno];
    if (adj_src >= 0 && adj_src < kFirstPseudoRegister) {
      if (copy) {
        m->base_hard_reg_copy_preferences[a].SetBit(adj_src);
      }
      m->base_hard_reg_preferences[a].SetBit(adj_src);
      const int nregs = HardRegnoNregs(adj_src, {src->mode, src->raw_mode}, mips_cfg);
      for (int k = adj_src; k < adj_src + nregs && k < kFirstPseudoRegister; ++k) {
        m->base_hard_reg_full_preferences[a].SetBit(k);
      }
    }
  }
}

void RecordGlobalInsnMdNeeds(size_t b, const RtlExpr& pat, const MipsTargetConfig& mips_cfg,
                             const std::vector<uint8_t>& allocnos_live, GlobalAllocModel* m) {
  if (ExprContainsOp(pat, RtlOp::kCall)) {
    return;
  }
  if (InsnTouchesMdRegsOrCall(pat)) {
    for (int j = 0; j < m->max_allocno; ++j) {
      if (allocnos_live[j] != 0) {
        m->allocno_md_clobber_blocks[j].insert(static_cast<int>(b));
      }
    }
  }
  CollectHardAndScratchMdNeeds(pat, mips_cfg, &m->block_hard_md_needs[b]);
  absl::btree_map<int, MipsRegClass> insn_single_md;
  CollectSingleRegMdOutputClasses(pat, mips_cfg, &insn_single_md);
  for (const auto& [p, cls] : insn_single_md) {
    HardRegSet req;
    if (cls == MipsRegClass::kHiReg) {
      req.SetBit(kHiRegnum);
    } else if (cls == MipsRegClass::kLoReg) {
      req.SetBit(kLoRegnum);
    } else if (cls == MipsRegClass::kHiloReg) {
      req.SetBit(kHiloRegnum);
    }
    if (!req.IsEmpty()) {
      m->block_md_pseudos[b].push_back({p, req});
    }
  }
  if (!mips_cfg.gp_64bit) {
    absl::btree_set<int> insn_di_mult;
    CollectDiMultDestRegs(pat, &insn_di_mult);
    for (int p : insn_di_mult) {
      HardRegSet req;
      req.SetBit(kHiRegnum);
      req.SetBit(kLoRegnum);
      m->block_md_pseudos[b].push_back({p, req});
    }
  }
}

// Mirrors `global.c:614-775` (`global_conflicts`), `global.c:1236`
// (`record_one_conflict`), `global.c:1317` (`mark_reg_store`), `global.c:1381`
// (`mark_reg_clobber`), and `global.c:1477` (`mark_reg_death`).
void RecordGlobalConflictsAndPreferences(const RtlControlFlowGraph& out,
                                         const RtlControlFlowGraph& liveness_cfg,
                                         const MipsTargetConfig& mips_cfg, GlobalAllocModel* m) {
  m->base_hard_reg_conflicts.assign(m->max_allocno, {});
  m->base_hard_reg_preferences.assign(m->max_allocno, {});
  m->base_hard_reg_copy_preferences.assign(m->max_allocno, {});
  m->base_hard_reg_full_preferences.assign(m->max_allocno, {});
  m->conflicts.assign(static_cast<size_t>(m->max_allocno) * m->max_allocno, 0);

  std::vector<uint8_t> allocnos_live(m->max_allocno, 0);
  HardRegSet hard_regs_live;

  auto record_one_conflict = [&](int regno) {
    if (regno < kFirstPseudoRegister) {
      for (int j = 0; j < m->max_allocno; ++j) {
        if (allocnos_live[j] != 0) {
          m->base_hard_reg_conflicts[j].SetBit(regno);
        }
      }
    } else {
      const int ialloc = m->reg_allocno[regno];
      if (ialloc >= 0) {
        m->base_hard_reg_conflicts[ialloc].Ior(hard_regs_live);
        for (int j = 0; j < m->max_allocno; ++j) {
          if (allocnos_live[j] != 0) {
            m->SetConflict(ialloc, j);
          }
        }
      }
    }
  };

  auto mark_reg_death = [&](const RtlExpr& reg) {
    if (reg.op != RtlOp::kReg) {
      return;
    }
    int regno = reg.IsArgPointerReg() ? kArgPointerRegnum : reg.RegNo();
    if (regno >= kFirstPseudoRegister && regno < m->max_regno &&
        m->local_reg_renumber[regno] >= 0) {
      regno = m->local_reg_renumber[regno];
    }
    if (regno >= kFirstPseudoRegister) {
      if (regno < m->max_regno && m->reg_allocno[regno] >= 0) {
        allocnos_live[m->reg_allocno[regno]] = 0;
      }
    } else if (regno >= 0 && !m->fixed_regs.TestBit(regno)) {
      const int last = regno + HardRegnoNregs(regno, {reg.mode, reg.raw_mode}, mips_cfg);
      for (int r = regno; r < last && r < kFirstPseudoRegister; ++r) {
        hard_regs_live.ClearBit(r);
      }
    }
  };

  for (size_t b = 0; b < liveness_cfg.rpo_blocks.size(); ++b) {
    std::fill(allocnos_live.begin(), allocnos_live.end(), 0);
    hard_regs_live.Clear();
    std::vector<int> block_start_allocnos;

    if (auto live_it = out.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        live_it != out.sideband.basic_block_live_at_start.end()) {
      for (int r : live_it->second) {
        if (r >= 0 && r < kFirstPseudoRegister) {
          hard_regs_live.SetBit(r);
        } else if (r >= kFirstPseudoRegister && r < m->max_regno) {
          if (m->reg_allocno[r] >= 0) {
            allocnos_live[m->reg_allocno[r]] = 1;
            block_start_allocnos.push_back(m->reg_allocno[r]);
          } else if (m->local_reg_renumber[r] >= 0) {
            const int hr = m->local_reg_renumber[r];
            const int last = hr + HardRegnoNregs(hr, m->pseudo_modes[r], mips_cfg);
            for (int k = hr; k < last && k < kFirstPseudoRegister; ++k) {
              hard_regs_live.SetBit(k);
            }
          }
        }
      }
    }
    for (int a : block_start_allocnos) {
      m->base_hard_reg_conflicts[a].Ior(hard_regs_live);
      for (int j = 0; j < m->max_allocno; ++j) {
        if (allocnos_live[j] != 0) {
          m->SetConflict(a, j);
        }
      }
    }

    const RtlBasicBlock& out_blk = out.rpo_blocks[b];
    const RtlBasicBlock& live_blk = liveness_cfg.rpo_blocks[b];
    std::vector<RtlExpr> regs_set;
    for (size_t i = 0; i < out_blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(out_blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      if (ExprContainsOp(parsed->pattern, RtlOp::kCall) && i < live_blk.instructions.size()) {
        if (auto live_pn = ParsePatternAndNotes(live_blk.instructions[i]); live_pn.ok()) {
          parsed->reg_notes = std::move(live_pn->reg_notes);
        }
      }
      regs_set.clear();

      auto mark_clobber = [&](const RtlExpr& orig_reg, const RtlExpr& setter) {
        if (setter.op != RtlOp::kClobber) {
          return;
        }
        const RtlExpr* reg = &orig_reg;
        if (reg->op == RtlOp::kSubreg && !reg->children.empty()) {
          reg = &reg->children[0];
        }
        if (reg->op != RtlOp::kReg) {
          return;
        }
        regs_set.push_back(*reg);
        int regno = reg->IsArgPointerReg() ? kArgPointerRegnum : reg->RegNo();
        if (regno >= kFirstPseudoRegister && regno < m->max_regno &&
            m->local_reg_renumber[regno] >= 0) {
          regno = m->local_reg_renumber[regno];
        }
        if (regno >= kFirstPseudoRegister) {
          if (regno < m->max_regno && m->reg_allocno[regno] >= 0) {
            allocnos_live[m->reg_allocno[regno]] = 1;
            record_one_conflict(regno);
          }
        } else if (regno >= 0 && !m->fixed_regs.TestBit(regno)) {
          const int last = regno + HardRegnoNregs(regno, {reg->mode, reg->raw_mode}, mips_cfg);
          for (int r = regno; r < last && r < kFirstPseudoRegister; ++r) {
            record_one_conflict(r);
            hard_regs_live.SetBit(r);
          }
        }
      };

      auto mark_store = [&](const RtlExpr& orig_reg, const RtlExpr& setter) {
        if (setter.op == RtlOp::kClobber) {
          mark_clobber(orig_reg, setter);
          return;
        }
        const RtlExpr* reg = &orig_reg;
        if (reg->op == RtlOp::kSubreg && !reg->children.empty()) {
          reg = &reg->children[0];
        }
        if (reg->op != RtlOp::kReg) {
          return;
        }
        regs_set.push_back(*reg);
        if (setter.op == RtlOp::kSet && setter.children.size() >= 2) {
          SetGlobalPreference(*reg, setter.children[1], mips_cfg, m);
        }
        int regno = reg->IsArgPointerReg() ? kArgPointerRegnum : reg->RegNo();
        if (regno >= kFirstPseudoRegister && regno < m->max_regno &&
            m->local_reg_renumber[regno] >= 0) {
          regno = m->local_reg_renumber[regno];
        }
        if (regno >= kFirstPseudoRegister) {
          if (regno < m->max_regno && m->reg_allocno[regno] >= 0) {
            allocnos_live[m->reg_allocno[regno]] = 1;
            record_one_conflict(regno);
          }
        } else if (regno >= 0 && !m->fixed_regs.TestBit(regno)) {
          const int last = regno + HardRegnoNregs(regno, {reg->mode, reg->raw_mode}, mips_cfg);
          for (int r = regno; r < last && r < kFirstPseudoRegister; ++r) {
            record_one_conflict(r);
            hard_regs_live.SetBit(r);
          }
        }
      };

      NoteStores(parsed->pattern, mark_clobber);
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead) {
          mark_reg_death(note.datum);
        }
      }
      RecordGlobalInsnMdNeeds(b, parsed->pattern, mips_cfg, allocnos_live, m);
      NoteStores(parsed->pattern, mark_store);

      if (parsed->pattern.op == RtlOp::kParallel && SingleSet(*parsed) == nullptr) {
        for (const RtlRegNote& note : parsed->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead) {
            bool used_in_output = false;
            for (int idx = static_cast<int>(parsed->pattern.children.size()) - 1; idx >= 0; --idx) {
              const RtlExpr& sub = parsed->pattern.children[idx];
              if (sub.op == RtlOp::kSet && !sub.children.empty() &&
                  sub.children[0].op != RtlOp::kReg && note.datum != sub.children[0] &&
                  RegOverlapMentionedP(note.datum, sub.children[0], mips_cfg)) {
                used_in_output = true;
              }
            }
            if (used_in_output) {
              const RtlExpr* r = &note.datum;
              if (r->op == RtlOp::kSubreg && !r->children.empty()) {
                r = &r->children[0];
              }
              if (r->op == RtlOp::kReg) {
                int regno = r->IsArgPointerReg() ? kArgPointerRegnum : r->RegNo();
                if (regno >= kFirstPseudoRegister && regno < m->max_regno &&
                    m->local_reg_renumber[regno] >= 0) {
                  regno = m->local_reg_renumber[regno];
                }
                if (regno >= kFirstPseudoRegister) {
                  if (regno < m->max_regno && m->reg_allocno[regno] >= 0) {
                    record_one_conflict(regno);
                  }
                } else if (regno >= 0 && !m->fixed_regs.TestBit(regno)) {
                  const int last = regno + HardRegnoNregs(regno, {r->mode, r->raw_mode}, mips_cfg);
                  for (int hr = regno; hr < last && hr < kFirstPseudoRegister; ++hr) {
                    record_one_conflict(hr);
                  }
                }
              }
            }
          }
        }
      }

      while (!regs_set.empty()) {
        const RtlExpr r = regs_set.back();
        regs_set.pop_back();
        const int regno = r.IsArgPointerReg() ? kArgPointerRegnum : r.RegNo();
        if (HasRegUnusedNoteForRegno(parsed->reg_notes, regno)) {
          mark_reg_death(r);
        }
      }
    }
  }
}

// Mirrors `global.c:513-523` (`eliminable_regset` masking) and
// `global.c:781-824` (`expand_preferences`).
void FinalizeEliminableAndExpandPreferences(const RtlControlFlowGraph& liveness_cfg,
                                            GlobalAllocModel* m) {
  const HardRegSet elim = EliminableRegSet();
  for (int a = 0; a < m->max_allocno; ++a) {
    m->base_hard_reg_conflicts[a].AndCompl(elim);
    m->base_hard_reg_copy_preferences[a].AndCompl(elim);
    m->base_hard_reg_preferences[a].AndCompl(elim);
  }

  for (const RtlBasicBlock& blk : liveness_cfg.rpo_blocks) {
    for (const std::string& insn_str : blk.instructions) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(insn_str);
      if (!parsed.ok()) {
        continue;
      }
      const RtlExpr* set = SingleSet(*parsed);
      if (set == nullptr || set->children.size() < 2 || set->children[0].op != RtlOp::kReg ||
          set->children[0].IsArgPointerReg()) {
        continue;
      }
      const int dst_regno = set->children[0].RegNo();
      if (dst_regno < kFirstPseudoRegister || dst_regno >= m->max_regno ||
          m->reg_allocno[dst_regno] < 0) {
        continue;
      }
      const int a1 = m->reg_allocno[dst_regno];
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
            !note.datum.IsArgPointerReg()) {
          const int dead_regno = note.datum.RegNo();
          if (dead_regno >= kFirstPseudoRegister && dead_regno < m->max_regno &&
              m->reg_allocno[dead_regno] >= 0) {
            const int a2 = m->reg_allocno[dead_regno];
            if (!m->ConflictP(a1, a2) && !m->ConflictP(a2, a1)) {
              if (set->children[1].op == RtlOp::kReg && !set->children[1].IsArgPointerReg() &&
                  set->children[1].RegNo() == dead_regno) {
                m->base_hard_reg_copy_preferences[a1].Ior(m->base_hard_reg_copy_preferences[a2]);
                m->base_hard_reg_copy_preferences[a2].Ior(m->base_hard_reg_copy_preferences[a1]);
              }
              m->base_hard_reg_preferences[a1].Ior(m->base_hard_reg_preferences[a2]);
              m->base_hard_reg_preferences[a2].Ior(m->base_hard_reg_preferences[a1]);
              m->base_hard_reg_full_preferences[a1].Ior(m->base_hard_reg_full_preferences[a2]);
              m->base_hard_reg_full_preferences[a2].Ior(m->base_hard_reg_full_preferences[a1]);
            }
          }
        }
      }
    }
  }
}

// Mirrors `global.c:580` (`reload(..., /*global=*/1, ...)`),
// `reload1.c:709` (`forbidden_regs = bad_spill_regs`), `reload1.c:1611`
// (class enum order: `GR_REGS` before `MD_REGS`), `reload1.c:3445`
// (`spill_hard_reg`), and `global.c:1203` (`retry_global_alloc`).
void SimulateGlobalReloadMdAndGrSpills(
    const GlobalAllocModel& m,
    absl::FunctionRef<void(int allocno, HardRegSet losers, bool alt_regs_p)> retry_find_reg,
    std::vector<int>* reg_renumber) {
  HardRegSet forbidden_regs = m.bad_spill_regs;
  int spilled_gr_count = 0;

  auto spill_one_hard_reg = [&](int spill_hr, const std::vector<HardRegSet>* md_needs_by_bb) {
    forbidden_regs.SetBit(spill_hr);
    for (int reg_i = kFirstPseudoRegister; reg_i < m.max_regno; ++reg_i) {
      const int cur_hr = (*reg_renumber)[reg_i];
      if (cur_hr < 0) {
        continue;
      }
      const int end_hr = cur_hr + HardRegnoNregs(cur_hr, m.pseudo_modes[reg_i], m.mips_cfg);
      if (cur_hr > spill_hr || spill_hr >= end_hr) {
        continue;
      }
      const int a = m.reg_allocno[reg_i];
      if (md_needs_by_bb != nullptr && a >= 0 &&
          static_cast<size_t>(a) < m.allocno_single_block.size()) {
        const int sb = m.allocno_single_block[a];
        if (sb >= 0 && sb < m.num_blocks && !(*md_needs_by_bb)[sb].TestBit(spill_hr)) {
          continue;
        }
      }
      (*reg_renumber)[reg_i] = -1;
      if (a >= 0) {
        retry_find_reg(a, forbidden_regs, false);
        if ((*reg_renumber)[reg_i] < 0 && m.alt_class[a] != MipsRegClass::kNoRegs) {
          retry_find_reg(a, forbidden_regs, true);
        }
      }
    }
  };

  for (int pass = 0; pass < 4; ++pass) {
    int max_gr_need = 0;
    for (const std::vector<int>& grp : m.non_move_read_pseudo_groups) {
      int insn_gr_need = 0;
      for (int p : grp) {
        if (p >= kFirstPseudoRegister && p < m.max_regno && (*reg_renumber)[p] >= kHiRegnum &&
            (*reg_renumber)[p] <= kHiloRegnum) {
          ++insn_gr_need;
        }
      }
      max_gr_need = std::max(max_gr_need, insn_gr_need);
    }

    std::vector<HardRegSet> block_needs_md = m.block_hard_md_needs;
    block_needs_md.resize(m.num_blocks);
    for (int b = 0; b < m.num_blocks && static_cast<size_t>(b) < m.block_md_pseudos.size(); ++b) {
      for (const BlockMdPseudoNeed& need : m.block_md_pseudos[b]) {
        HardRegSet got;
        if (need.regno >= 0 && need.regno < m.max_regno && (*reg_renumber)[need.regno] >= 0) {
          const int hr = (*reg_renumber)[need.regno];
          const int nregs = HardRegnoNregs(hr, m.pseudo_modes[need.regno], m.mips_cfg);
          for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
            got.SetBit(k);
          }
        }
        HardRegSet missing = need.required_md;
        missing.AndCompl(got);
        block_needs_md[b].Ior(missing);
      }
    }
    for (int a = 0; a < m.max_allocno; ++a) {
      const int p = m.allocno_reg[a];
      const int hr = (*reg_renumber)[p];
      if (hr >= kHiRegnum && hr <= kHiloRegnum &&
          static_cast<size_t>(a) < m.allocno_md_clobber_blocks.size()) {
        const int nregs = HardRegnoNregs(hr, m.pseudo_modes[p], m.mips_cfg);
        for (int b : m.allocno_md_clobber_blocks[a]) {
          if (b >= 0 && b < m.num_blocks) {
            for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
              block_needs_md[b].SetBit(k);
            }
          }
        }
      }
    }

    bool spilled_any = false;
    while (spilled_gr_count < max_gr_need) {
      std::array<int, 32> hr_uses{};
      for (int i = kFirstPseudoRegister; i < m.max_regno; ++i) {
        const int r = (*reg_renumber)[i];
        if (r >= 0 && r < 32) {
          const int lim = std::min(32, r + HardRegnoNregs(r, m.pseudo_modes[i], m.mips_cfg));
          const int refs = static_cast<size_t>(i) < m.reg_n_refs.size() ? m.reg_n_refs[i] : 0;
          for (int k = r; k < lim; ++k) {
            hr_uses[k] += refs;
          }
        }
      }
      int best_gr = -1;
      for (int r = 0; r < 32; ++r) {
        if (!forbidden_regs.TestBit(r) && hr_uses[r] == 0 && m.call_used_regs.TestBit(r)) {
          best_gr = r;
          break;
        }
      }
      if (best_gr < 0) {
        for (int r = 0; r < 32; ++r) {
          if (!forbidden_regs.TestBit(r) && hr_uses[r] == 0 && !m.call_used_regs.TestBit(r)) {
            best_gr = r;
            break;
          }
        }
      }
      if (best_gr < 0) {
        for (int r = 0; r < 32; ++r) {
          if (!forbidden_regs.TestBit(r) && (best_gr < 0 || hr_uses[r] < hr_uses[best_gr])) {
            best_gr = r;
          }
        }
      }
      if (best_gr < 0) {
        break;
      }
      ++spilled_gr_count;
      spilled_any = true;
      spill_one_hard_reg(best_gr, nullptr);
    }

    for (int md_hr : {kHiRegnum, kLoRegnum, kHiloRegnum}) {
      if (forbidden_regs.TestBit(md_hr)) {
        continue;
      }
      bool any_need = false;
      for (int b = 0; b < m.num_blocks; ++b) {
        if (block_needs_md[b].TestBit(md_hr)) {
          any_need = true;
          break;
        }
      }
      if (!any_need) {
        continue;
      }
      spilled_any = true;
      spill_one_hard_reg(md_hr, &block_needs_md);
    }
    if (!spilled_any) {
      break;
    }
  }
}

}  // namespace

SimResult SimulateGlobalAlloc(const GlobalAllocModel& m, const std::vector<int>& order,
                              bool simulate_reload_spills) {
  SimResult res;
  res.allocno_hr.assign(m.max_allocno, -1);
  if (m.max_allocno == 0) {
    return res;
  }

  std::vector<int> reg_renumber = m.local_reg_renumber;
  HardRegSet regs_used_so_far = m.base_regs_used_so_far;
  std::vector<int> local_reg_n_refs = m.base_local_reg_n_refs;
  const std::vector<int>& local_reg_live_length = m.base_local_reg_live_length;
  std::vector<HardRegSet> hard_reg_conflicts = m.base_hard_reg_conflicts;
  std::vector<HardRegSet> hard_reg_preferences = m.base_hard_reg_preferences;
  std::vector<HardRegSet> hard_reg_copy_preferences = m.base_hard_reg_copy_preferences;
  std::vector<HardRegSet> hard_reg_full_preferences = m.base_hard_reg_full_preferences;
  std::vector<HardRegSet> regs_someone_prefers(m.max_allocno);

  // `prune_preferences()` (`global.c:834-886`).
  for (int i = m.max_allocno - 1; i >= 0; --i) {
    const int allocno = order[i];
    HardRegSet temp = hard_reg_conflicts[allocno];
    if (m.allocno_calls_crossed[allocno] == 0) {
      temp.Ior(m.fixed_regs);
    } else {
      temp.Ior(m.call_used_regs);
    }
    temp.IorCompl(RegClassContents(m.pref_class[allocno]));

    hard_reg_preferences[allocno].AndCompl(temp);
    hard_reg_copy_preferences[allocno].AndCompl(temp);
    hard_reg_full_preferences[allocno].AndCompl(temp);
    regs_someone_prefers[allocno].Clear();

    for (int j = i + 1; j < m.max_allocno; ++j) {
      const int other = order[j];
      if (m.ConflictP(allocno, other)) {
        HardRegSet other_pref = hard_reg_full_preferences[other];
        if (m.allocno_size[other] <= m.allocno_size[allocno]) {
          other_pref.AndCompl(hard_reg_full_preferences[allocno]);
        }
        regs_someone_prefers[allocno].Ior(other_pref);
      }
    }
  }

  // `find_reg()` (`global.c:904-1195`), scanning hard registers in natural
  // MIPS order `0 .. FIRST_PSEUDO_REGISTER - 1` (`global.c:964-990, 1107-1118`,
  // `#ifndef REG_ALLOC_ORDER`).
  auto find_reg = [&](auto& self, int allocno, HardRegSet losers, bool alt_regs_p,
                      bool accept_call_clobbered, bool retrying) -> void {
    const MipsRegClass cls = alt_regs_p ? m.alt_class[allocno] : m.pref_class[allocno];
    const RegModeInfo& mode = m.pseudo_modes[m.allocno_reg[allocno]];

    HardRegSet used1 = (accept_call_clobbered || m.allocno_calls_crossed[allocno] == 0)
                           ? m.fixed_regs
                           : m.call_used_regs;
    used1.Ior(m.no_global_alloc_regs);
    used1.Ior(losers);
    used1.IorCompl(RegClassContents(cls));
    const HardRegSet used2 = used1;
    used1.Ior(hard_reg_conflicts[allocno]);

    HardRegSet used = used1;
    used.IorCompl(regs_used_so_far);
    used.Ior(regs_someone_prefers[allocno]);

    int best_reg = -1;
    int i = kFirstPseudoRegister;
    for (int pass = 0; pass <= 1 && i >= kFirstPseudoRegister; ++pass) {
      if (pass == 1) {
        used = used1;
      }
      for (i = 0; i < kFirstPseudoRegister; ++i) {
        const int regno = i;
        if (!used.TestBit(regno) && HardRegnoModeOk(regno, mode, m.mips_cfg)) {
          const int lim = regno + HardRegnoNregs(regno, mode, m.mips_cfg);
          int j = regno + 1;
          while (j < lim && !used.TestBit(j)) {
            ++j;
          }
          if (j == lim) {
            best_reg = regno;
            break;
          }
          i = j;
        }
      }
    }

    hard_reg_copy_preferences[allocno].AndCompl(used);
    bool took_copy_pref = false;
    if (!hard_reg_copy_preferences[allocno].IsEmpty() && best_reg >= 0) {
      for (int r = 0; r < kFirstPseudoRegister; ++r) {
        if (hard_reg_copy_preferences[allocno].TestBit(r) && HardRegnoModeOk(r, mode, m.mips_cfg) &&
            RegClassRelatedP(RegNoRegClass(r), RegNoRegClass(best_reg))) {
          const int lim = r + HardRegnoNregs(r, mode, m.mips_cfg);
          int j = r + 1;
          while (j < lim && !used.TestBit(j) &&
                 RegClassRelatedP(RegNoRegClass(j), RegNoRegClass(best_reg + (j - r)))) {
            ++j;
          }
          if (j == lim) {
            best_reg = r;
            took_copy_pref = true;
            break;
          }
        }
      }
    }

    if (!took_copy_pref) {
      hard_reg_preferences[allocno].AndCompl(used);
      if (!hard_reg_preferences[allocno].IsEmpty() && best_reg >= 0) {
        for (int r = 0; r < kFirstPseudoRegister; ++r) {
          if (hard_reg_preferences[allocno].TestBit(r) && HardRegnoModeOk(r, mode, m.mips_cfg) &&
              RegClassRelatedP(RegNoRegClass(r), RegNoRegClass(best_reg))) {
            const int lim = r + HardRegnoNregs(r, mode, m.mips_cfg);
            int j = r + 1;
            while (j < lim && !used.TestBit(j) &&
                   RegClassRelatedP(RegNoRegClass(j), RegNoRegClass(best_reg + (j - r)))) {
              ++j;
            }
            if (j == lim) {
              best_reg = r;
              break;
            }
          }
        }
      }
    }

    // `CALLER_SAVE_PROFITABLE` (`regs.h:165`, `global.c:1083-1095`).
    if (m.mips_cfg.caller_saves && best_reg < 0 && !accept_call_clobbered &&
        m.allocno_calls_crossed[allocno] != 0 &&
        (4 * m.allocno_calls_crossed[allocno] < m.allocno_n_refs[allocno])) {
      self(self, allocno, losers, alt_regs_p, true, retrying);
      if (reg_renumber[m.allocno_reg[allocno]] >= 0) {
        return;
      }
    }

    // Local-alloc eviction (`global.c:1102-1157`).
    if (best_reg < 0 && !retrying && m.allocno_size[allocno] == 1) {
      for (int regno = kFirstPseudoRegister - 1; regno >= 0; --regno) {
        if (local_reg_n_refs[regno] != 0 && !used2.TestBit(regno) &&
            HardRegnoModeOk(regno, mode, m.mips_cfg)) {
          const double tmp1 =
              static_cast<double>(local_reg_n_refs[regno]) / local_reg_live_length[regno];
          const double tmp2 =
              static_cast<double>(m.allocno_n_refs[allocno]) / m.allocno_live_length[allocno];
          if (tmp1 < tmp2) {
            for (int k = kFirstPseudoRegister; k < m.max_regno; ++k) {
              if (reg_renumber[k] >= 0) {
                const int r = reg_renumber[k];
                const int endregno = r + HardRegnoNregs(r, m.pseudo_modes[k], m.mips_cfg);
                if (regno >= r && regno < endregno) {
                  reg_renumber[k] = -1;
                }
              }
            }
            best_reg = regno;
            break;
          }
        }
      }
    }

    if (best_reg >= 0) {
      reg_renumber[m.allocno_reg[allocno]] = best_reg;
      HardRegSet this_reg;
      const int lim = best_reg + HardRegnoNregs(best_reg, mode, m.mips_cfg);
      for (int j = best_reg; j < lim; ++j) {
        this_reg.SetBit(j);
        regs_used_so_far.SetBit(j);
        local_reg_n_refs[j] = 0;
      }
      for (int j = 0; j < m.max_allocno; ++j) {
        if (m.ConflictP(allocno, j) || m.ConflictP(j, allocno)) {
          hard_reg_conflicts[j].Ior(this_reg);
        }
      }
    }
  };

  for (int i = 0; i < m.max_allocno; ++i) {
    const int allocno = order[i];
    if (m.allocno_live_length[allocno] >= 0) {
      find_reg(find_reg, allocno, {}, false, false, false);
      if (reg_renumber[m.allocno_reg[allocno]] >= 0) {
        continue;
      }
      if (m.alt_class[allocno] != MipsRegClass::kNoRegs) {
        find_reg(find_reg, allocno, {}, true, false, false);
      }
    }
  }

  // Pre-existing Stage 4 post-pass guard extracted verbatim from
  // `invert_global_alloc.cc` (zero `pre_state` peeking): skips the reload MD
  // spill approximation when the target `.09.greg` state explicitly retains an
  // MD hard register (`hi`/`lo`/`hilo`). Pure forward callers pass an empty
  // `greg_cfg` (`target_hr == -1`), so `any_target_md` is false.
  const bool any_target_md = std::any_of(m.target_hr.begin(), m.target_hr.end(), [](int hr) {
    return hr >= kHiRegnum && hr <= kHiloRegnum;
  });
  if (simulate_reload_spills && m.num_blocks > 0 && !any_target_md) {
    SimulateGlobalReloadMdAndGrSpills(
        m,
        [&](int a, HardRegSet losers, bool alt_regs_p) {
          find_reg(find_reg, a, losers, alt_regs_p, false, true);
        },
        &reg_renumber);
  }

  for (int a = 0; a < m.max_allocno; ++a) {
    res.allocno_hr[a] = reg_renumber[m.allocno_reg[a]];
    if (m.target_hr[a] >= 0) {
      if (res.allocno_hr[a] != m.target_hr[a]) {
        ++res.primary_mismatches;
      }
    } else if (res.allocno_hr[a] >= 0) {
      ++res.secondary_mismatches;
    }
  }
  for (int k = kFirstPseudoRegister; k < m.max_regno; ++k) {
    if (m.local_reg_renumber[k] >= 0 && reg_renumber[k] != m.local_reg_renumber[k]) {
      ++res.primary_mismatches;
    }
  }
  res.reg_renumber = std::move(reg_renumber);
  return res;
}

std::vector<int> SortAllocnoOrder(const GlobalAllocModel& m) {
  std::vector<int> order(m.max_allocno);
  for (int i = 0; i < m.max_allocno; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    const int pri_a =
        ComputeAllocnoPriority(m.allocno_n_refs[a], m.allocno_live_length[a], m.allocno_size[a]);
    const int pri_b =
        ComputeAllocnoPriority(m.allocno_n_refs[b], m.allocno_live_length[b], m.allocno_size[b]);
    if (pri_a != pri_b) {
      return pri_a > pri_b;
    }
    return a < b;
  });
  return order;
}

GlobalAllocModel BuildGlobalAllocModel(
    const RtlControlFlowGraph& out, const RtlControlFlowGraph& liveness_cfg,
    const RtlControlFlowGraph& greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    const HardRegSet& scratch_hard_regs) {
  GlobalAllocModel m;
  InitGlobalAllocnosAndClasses(out, liveness_cfg, greg_cfg, mips_cfg, pseudo_summaries,
                               pseudo_modes, fp_op_pseudos, gr_or_mem_op_pseudos, &m);
  if (m.max_allocno == 0) {
    return m;
  }
  ComputeRegsEverLiveAndBaseUseStats(out, liveness_cfg, mips_cfg, scratch_hard_regs, &m);
  RecordGlobalConflictsAndPreferences(out, liveness_cfg, mips_cfg, &m);
  FinalizeEliminableAndExpandPreferences(liveness_cfg, &m);
  return m;
}

}  // namespace rom_nom_nom::lifter
