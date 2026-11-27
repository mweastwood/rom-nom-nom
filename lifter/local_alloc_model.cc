#include "lifter/local_alloc_model.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_reg_alloc_common.h"

namespace rom_nom_nom::lifter {
namespace {

bool HasRegNoteFor(const RtlPatternAndNotes& insn, RtlMachineMode kind, int regno) {
  for (const RtlRegNote& n : insn.reg_notes) {
    if (n.kind == kind && n.datum.op == RtlOp::kReg && !n.datum.IsArgPointerReg() &&
        n.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

bool HasRegNoteKind(const RtlPatternAndNotes& insn, RtlMachineMode kind) {
  for (const RtlRegNote& n : insn.reg_notes) {
    if (n.kind == kind) {
      return true;
    }
  }
  return false;
}

// Selects block-`b`-local pseudos eligible for `local-alloc.c:472-474`
// (`reg_basic_block[i] >= 0 && reg_n_deaths[i] == 1` and not
// `CLASS_LIKELY_SPILLED_P` single-reg MD class with alternate class),
// optionally ensuring `CALLER_SAVE_PROFITABLE` (`regs.h:165`,
// `local-alloc.c:2209`) for call-crossing targets assigned call-used regs.
std::vector<int> CollectEligibleBlockLocalPseudos(
    size_t b, int max_regno, const MipsTargetConfig& mips_cfg, const HardRegSet& call_used,
    const absl::btree_set<int>& live_at_any_block_start,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    bool bump_caller_saves_refs, RtlControlFlowGraph* dst_out) {
  std::vector<int> eligible_pseudos;
  for (int p = kFirstPseudoRegister; p < max_regno; ++p) {
    if (live_at_any_block_start.contains(p)) {
      continue;
    }
    const auto refs_it = dst_out->sideband.reg_n_refs.find(p);
    const auto deaths_it = dst_out->sideband.reg_n_deaths.find(p);
    const auto len_it = dst_out->sideband.reg_live_length.find(p);
    if (refs_it == dst_out->sideband.reg_n_refs.end() || refs_it->second <= 0 ||
        deaths_it == dst_out->sideband.reg_n_deaths.end() || deaths_it->second != 1 ||
        (len_it != dst_out->sideband.reg_live_length.end() && len_it->second < 0)) {
      continue;
    }
    const auto sum_it = pseudo_summaries.find(p);
    if (sum_it == pseudo_summaries.end() || sum_it->second.blocks_referenced.size() != 1 ||
        *sum_it->second.blocks_referenced.begin() != b || !sum_it->second.has_set_in_block ||
        sum_it->second.used_before_set || sum_it->second.alive_after_redef ||
        sum_it->second.note_deaths >= 2) {
      continue;
    }
    if (sum_it->second.def_count_in_block == 1 &&
        sum_it->second.single_reg_md_class != MipsRegClass::kNoRegs &&
        !sum_it->second.used_in_mem_address && sum_it->second.total_read_count <= 1) {
      continue;
    }
    eligible_pseudos.push_back(p);
  }

  if (bump_caller_saves_refs && mips_cfg.caller_saves) {
    for (int p : eligible_pseudos) {
      const auto ren_it = dst_out->sideband.reg_renumber.find(p);
      const auto cc_it = dst_out->sideband.reg_n_calls_crossed.find(p);
      if (ren_it != dst_out->sideband.reg_renumber.end() && ren_it->second >= 0 &&
          call_used.TestBit(ren_it->second) &&
          cc_it != dst_out->sideband.reg_n_calls_crossed.end() && cc_it->second > 0) {
        const int min_refs = 4 * cc_it->second + 1;
        int& cur_refs = dst_out->sideband.reg_n_refs[p];
        cur_refs = std::max(cur_refs, min_refs);
      }
    }
  }
  return eligible_pseudos;
}

// Populates `reg_qty`, `target_hr_by_pseudo`, `pref_class`, and `alt_class`
// for every block-local eligible pseudo (`config/mips/mips.h:2914-2929`,
// `regclass.c:960`, `local-alloc.c:472-474`).
void InitLocalPseudoClassesAndTargets(
    const RtlControlFlowGraph& dst_out, const MipsTargetConfig& mips_cfg,
    const std::vector<int>& eligible_pseudos,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    std::vector<int>* reg_qty, std::vector<MipsRegClass>* pref_class,
    std::vector<MipsRegClass>* alt_class, absl::btree_map<int, int>* target_hr_by_pseudo) {
  for (int p : eligible_pseudos) {
    (*reg_qty)[p] = -2;
    const auto ren_it = dst_out.sideband.reg_renumber.find(p);
    const int target_hr = (ren_it != dst_out.sideband.reg_renumber.end()) ? ren_it->second : -1;
    (*target_hr_by_pseudo)[p] = target_hr;
    const auto m_it = pseudo_modes.find(p);
    const RegModeInfo pmode =
        (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
    if (IsFpCcMode(pmode)) {
      (*pref_class)[p] = MipsRegClass::kStRegs;
    } else if (IsFloatOrComplexFloatMode(pmode) && !mips_cfg.soft_float) {
      // Pre-existing `regclass.c` stand-in extracted verbatim from
      // `invert_global_alloc.cc`: uses only the Stage 4 target post-pass
      // `reg_renumber` (`target_hr`, zero `pre_state` peeking) to disambiguate
      // `SF`/`DF` pseudos without FP-ALU uses between `GR_REGS` and `FP_REGS`.
      // Pure forward callers leave `dst_out.sideband.reg_renumber` empty
      // (`target_hr == -1`). Slated for replacement by a pure forward
      // `regclass.c:record_reg_classes` cost model in Sub-PR 2.
      if (!fp_op_pseudos.contains(p) &&
          ((target_hr >= 0 && target_hr < kFirstFprRegnum) || gr_or_mem_op_pseudos.contains(p))) {
        if (target_hr >= kFirstFprRegnum) {
          (*pref_class)[p] = MipsRegClass::kFpRegs;
          (*alt_class)[p] =
              mips_cfg.mem_move_cost_6 ? MipsRegClass::kAllRegs : MipsRegClass::kNoRegs;
        } else {
          (*pref_class)[p] = MipsRegClass::kGrRegs;
          (*alt_class)[p] = MipsRegClass::kAllRegs;
        }
      } else {
        (*pref_class)[p] = MipsRegClass::kFpRegs;
        (*alt_class)[p] = mips_cfg.mem_move_cost_6 ? MipsRegClass::kAllRegs : MipsRegClass::kNoRegs;
      }
    } else if (pseudo_summaries.at(p).total_def_count == 1 &&
               pseudo_summaries.at(p).single_reg_md_class != MipsRegClass::kNoRegs &&
               !pseudo_summaries.at(p).used_in_mem_address &&
               pseudo_summaries.at(p).total_read_count <= 1) {
      (*pref_class)[p] = pseudo_summaries.at(p).single_reg_md_class;
      (*alt_class)[p] = MipsRegClass::kGrRegs;
    } else if (!mips_cfg.gp_64bit && pseudo_summaries.at(p).total_def_count == 1 &&
               pseudo_summaries.at(p).defined_by_di_mult &&
               !pseudo_summaries.at(p).used_in_mem_address &&
               pseudo_summaries.at(p).total_read_count <= 1) {
      (*pref_class)[p] = MipsRegClass::kMdRegs;
      (*alt_class)[p] = MipsRegClass::kGrRegs;
    } else {
      (*pref_class)[p] = MipsRegClass::kGrRegs;
      (*alt_class)[p] = MipsRegClass::kNoRegs;
    }
  }
}

bool ParseBlockInsnsForLocalAlloc(const RtlBasicBlock& out_blk, const RtlBasicBlock& live_blk,
                                  std::vector<RtlPatternAndNotes>* parsed_insns,
                                  int* insn_count_out) {
  int insn_count = out_blk.has_label ? 1 : 0;
  parsed_insns->clear();
  parsed_insns->reserve(out_blk.instructions.size());
  for (size_t i = 0; i < out_blk.instructions.size(); ++i) {
    auto pn = ParsePatternAndNotes(out_blk.instructions[i]);
    if (!pn.ok()) {
      return false;
    }
    if (ExprContainsOp(pn->pattern, RtlOp::kCall) && i < live_blk.instructions.size()) {
      if (auto live_pn = ParsePatternAndNotes(live_blk.instructions[i]); live_pn.ok()) {
        pn->reg_notes = std::move(live_pn->reg_notes);
      }
    }
    if (pn->pattern.op != RtlOp::kNote) {
      ++insn_count;
    }
    parsed_insns->push_back(std::move(*pn));
  }
  *insn_count_out = insn_count;
  return true;
}

struct LocalAllocBuilderState {
  const RtlControlFlowGraph& dst_out;
  const MipsTargetConfig& mips_cfg;
  const absl::btree_map<int, RegModeInfo>& pseudo_modes;
  int max_regno = kFirstPseudoRegister;
  int units_per_word = 4;
  std::vector<int> reg_qty;
  std::vector<MipsRegClass> pref_class;
  std::vector<MipsRegClass> alt_class;
  HardRegSet regs_live;
  LocalBlockAllocModel model;
};

// Mirrors `local-alloc.c:2224-2241` (`mark_life`).
void MarkLocalHardRegLife(int regno, const RegModeInfo& mode, bool life,
                          LocalAllocBuilderState* st) {
  const int nregs = HardRegnoNregs(regno, mode, st->mips_cfg);
  for (int j = 0; j < nregs && regno + j < kFirstPseudoRegister; ++j) {
    if (life) {
      st->regs_live.SetBit(regno + j);
    } else {
      st->regs_live.ClearBit(regno + j);
    }
  }
}

// Mirrors `local-alloc.c:2243-2270` (`post_mark_life`).
void PostMarkLocalBaseLife(int regno, const RegModeInfo& mode, int birth, int death,
                           LocalAllocBuilderState* st) {
  const int nregs = HardRegnoNregs(regno, mode, st->mips_cfg);
  for (int ins = std::max(0, birth);
       ins < death && ins < static_cast<int>(st->model.base_regs_live_at.size()); ++ins) {
    for (int j = 0; j < nregs && regno + j < kFirstPseudoRegister; ++j) {
      st->model.base_regs_live_at[ins].SetBit(regno + j);
    }
  }
}

// Mirrors `local-alloc.c:279-302` (`alloc_qty`).
void AllocLocalPseudoQty(int regno, const RegModeInfo& mode, int birth,
                         LocalAllocBuilderState* st) {
  const int q = static_cast<int>(st->model.qtys.size());
  st->reg_qty[regno] = q;
  st->model.reg_offset[regno] = 0;
  LocalBlockQty qty;
  qty.qty_id = q;
  qty.first_reg = regno;
  qty.member_regs = {regno};
  qty.member_offsets = {0};
  qty.mode = mode;
  qty.size = PseudoRegnoSize(mode, st->mips_cfg);
  qty.birth = birth;
  qty.death = -1;
  if (auto cc_it = st->dst_out.sideband.reg_n_calls_crossed.find(regno);
      cc_it != st->dst_out.sideband.reg_n_calls_crossed.end()) {
    qty.calls_crossed = cc_it->second;
  }
  qty.min_class = st->pref_class[regno];
  qty.alt_class = st->alt_class[regno];
  qty.n_refs = st->dst_out.sideband.reg_n_refs.at(regno);
  st->model.qtys.push_back(std::move(qty));
}

// Mirrors `local-alloc.c:1976-2009` (`reg_is_born`).
void MarkLocalRegBorn(const RtlExpr& reg, int birth, int cur_insn_num, LocalAllocBuilderState* st) {
  int regno = -1;
  RegModeInfo rmode{reg.mode, reg.raw_mode};
  if (reg.op == RtlOp::kSubreg && !reg.children.empty() && reg.children[0].op == RtlOp::kReg) {
    // `rtlanal.c:note_stores` / `local-alloc.c:1958-1985`: `note_stores`
    // only passes `SUBREG` through for hard registers; for pseudos it
    // unwraps `SUBREG_REG`, so `SUBREG_WORD` must only offset hard regs.
    const int base_r =
        reg.children[0].IsArgPointerReg() ? kArgPointerRegnum : reg.children[0].RegNo();
    regno = (base_r < kFirstPseudoRegister) ? (base_r + static_cast<int>(reg.int_val)) : base_r;
  } else if (reg.op == RtlOp::kReg) {
    regno = reg.IsArgPointerReg() ? kArgPointerRegnum : reg.RegNo();
  } else {
    return;
  }
  if (regno < kFirstPseudoRegister) {
    MarkLocalHardRegLife(regno, rmode, true, st);
    if (birth < 2 * cur_insn_num) {
      PostMarkLocalBaseLife(regno, rmode, birth, 2 * cur_insn_num, st);
    }
  } else if (regno < st->max_regno) {
    if (st->reg_qty[regno] == -2) {
      const auto m_it = st->pseudo_modes.find(regno);
      const RegModeInfo pmode = (m_it != st->pseudo_modes.end()) ? m_it->second : rmode;
      AllocLocalPseudoQty(regno, pmode, birth, st);
    }
    if (st->reg_qty[regno] >= 0) {
      st->model.qtys[st->reg_qty[regno]].death = -1;
    }
  }
}

// Mirrors `local-alloc.c:2013-2065` (`wipe_dead_reg`).
void WipeLocalDeadReg(const RtlPatternAndNotes& pn, const RtlExpr* primary_set, int insn_number,
                      const RtlExpr& reg, bool output_p, LocalAllocBuilderState* st) {
  if (reg.op != RtlOp::kReg) {
    return;
  }
  const int regno = reg.IsArgPointerReg() ? kArgPointerRegnum : reg.RegNo();
  if (pn.pattern.op == RtlOp::kParallel && primary_set == nullptr) {
    for (int idx = static_cast<int>(pn.pattern.children.size()) - 1; idx >= 0; --idx) {
      const RtlExpr& sub = pn.pattern.children[idx];
      if (sub.op == RtlOp::kSet && !sub.children.empty() && sub.children[0].op != RtlOp::kReg &&
          reg != sub.children[0] && RegOverlapMentionedP(reg, sub.children[0], st->mips_cfg)) {
        output_p = true;
      }
    }
  }
  if (regno < kFirstPseudoRegister) {
    MarkLocalHardRegLife(regno, {reg.mode, reg.raw_mode}, false, st);
    if (output_p) {
      PostMarkLocalBaseLife(regno, {reg.mode, reg.raw_mode}, 2 * insn_number, 2 * insn_number + 1,
                            st);
    }
  } else if (regno < st->max_regno && st->reg_qty[regno] >= 0) {
    st->model.qtys[st->reg_qty[regno]].death = 2 * insn_number + (output_p ? 1 : 0);
  }
}

// Mirrors `local-alloc.c:1722-1875` (`combine_regs`) for pseudo-to-pseudo
// coalescing, returning `ureg` on merge or `-1` otherwise.
int TryCombineLocalPseudoPair(const RtlExpr& u_raw, const RtlExpr& s_raw,
                              const RtlPatternAndNotes& insn, bool already_dead,
                              LocalAllocBuilderState* st) {
  int offset = 0;
  const RtlExpr* u_expr = &u_raw;
  while (u_expr->op == RtlOp::kSubreg && !u_expr->children.empty()) {
    offset += static_cast<int>(u_expr->int_val);
    u_expr = &u_expr->children[0];
  }
  const RtlExpr* s_expr = &s_raw;
  while (s_expr->op == RtlOp::kSubreg && !s_expr->children.empty()) {
    offset -= static_cast<int>(s_expr->int_val);
    s_expr = &s_expr->children[0];
  }
  if (u_expr->op != RtlOp::kReg || s_expr->op != RtlOp::kReg) {
    return -1;
  }
  const int ureg = u_expr->IsArgPointerReg() ? kArgPointerRegnum : u_expr->RegNo();
  const int sreg = s_expr->IsArgPointerReg() ? kArgPointerRegnum : s_expr->RegNo();
  if (ureg < kFirstPseudoRegister || sreg < kFirstPseudoRegister || ureg >= st->max_regno ||
      sreg >= st->max_regno || st->reg_qty[ureg] < 0 || st->reg_qty[sreg] >= -1 || ureg == sreg) {
    return -1;
  }
  const RegModeInfo umode{u_expr->mode, u_expr->raw_mode};
  const RegModeInfo smode{s_expr->mode, s_expr->raw_mode};
  const int usize = PseudoRegnoSize(umode, st->mips_cfg);
  const int ssize = PseudoRegnoSize(smode, st->mips_cfg);
  if ((offset > 0 && usize + offset > ssize) || (offset < 0 && usize + offset < ssize) ||
      (ssize > usize && usize < st->model.qtys[st->reg_qty[ureg]].size) ||
      !ModesTieableForLocalAlloc(umode, smode)) {
    return -1;
  }
  if (!already_dead && HasRegNoteFor(insn, RtlMachineMode::kRegNoConflict, ureg)) {
    return -1;
  }
  if ((already_dead || HasRegNoteFor(insn, RtlMachineMode::kRegDead, ureg)) &&
      RegClassRelatedP(st->pref_class[sreg], st->model.qtys[st->reg_qty[ureg]].min_class)) {
    const int sqty = st->reg_qty[ureg];
    LocalBlockQty& sq = st->model.qtys[sqty];
    st->reg_qty[sreg] = sqty;
    st->model.reg_offset[sreg] = st->model.reg_offset[ureg] + offset;
    sq.member_regs.push_back(sreg);
    sq.member_offsets.push_back(st->model.reg_offset[sreg]);
    if (RegClassSubsetP(st->pref_class[sreg], sq.min_class)) {
      sq.min_class = st->pref_class[sreg];
    }
    if (RegClassSubsetP(st->alt_class[sreg], sq.alt_class)) {
      sq.alt_class = st->alt_class[sreg];
    }
    if (auto cc_it = st->dst_out.sideband.reg_n_calls_crossed.find(sreg);
        cc_it != st->dst_out.sideband.reg_n_calls_crossed.end()) {
      sq.calls_crossed += cc_it->second;
    }
    sq.n_refs += st->dst_out.sideband.reg_n_refs.at(sreg);
    if (usize < ssize) {
      for (size_t k = 0; k < sq.member_regs.size(); ++k) {
        st->model.reg_offset[sq.member_regs[k]] -= offset;
        sq.member_offsets[k] -= offset;
      }
      sq.size = ssize;
      sq.mode = smode;
    }
    return ureg;
  }
  return -1;
}

// Identifies `(clobber (reg:M hr))` scratch clobbers on MIPS `match_scratch`
// `PARALLEL`s (`local-alloc.c:306-389, 1396-1402`).
std::vector<std::pair<int, RegModeInfo>> CollectInsnMatchScratchClobbers(const RtlExpr& pat) {
  std::vector<std::pair<int, RegModeInfo>> insn_scratches;
  if (!IsMipsMatchScratchParallel(pat)) {
    return insn_scratches;
  }
  for (const RtlExpr& elem : pat.children) {
    if (elem.op == RtlOp::kClobber && elem.children.size() == 1 &&
        elem.children[0].op == RtlOp::kReg && !elem.children[0].IsArgPointerReg()) {
      const int hr = elem.children[0].RegNo();
      const bool is_fixed = (hr == 0 || hr == 1 || hr == 26 || hr == 27 || hr == 28 || hr == 29 ||
                             hr == 31 || hr == 67);
      if (hr >= 0 && hr < kFirstPseudoRegister && !is_fixed) {
        insn_scratches.push_back({hr, {elem.children[0].mode, elem.children[0].raw_mode}});
      }
    }
  }
  return insn_scratches;
}

// Mirrors `local-alloc.c:1211-1297` (`block_alloc` operand-0 tying &
// hard-register suggestions / pseudo-pseudo `combine_regs`), returning the
// combined dying source pseudo regno or `-1`.
int RecordInsnTiesAndCombines(const RtlPatternAndNotes& pn, int insn_number,
                              LocalAllocBuilderState* st) {
  // Per `local-alloc.c:1211-1297` (`block_alloc`), tying always targets
  // `recog_operand[0]` (`insn_extract`), not `single_set(insn)`. For multi-SET
  // `PARALLEL`s like `divmodsi4`/`udivmodsi4` (`config/mips/mips.md:1285-1369`),
  // operand 0 is the first `SET` (`div`/`udiv`), so a `REG_UNUSED` on operand 0
  // must NOT redirect `r0` to the second `SET` (`mod`/`umod`, operand 3).
  const RtlExpr* combine_set = nullptr;
  if (pn.pattern.op == RtlOp::kSet) {
    combine_set = &pn.pattern;
  } else if (pn.pattern.op == RtlOp::kParallel && !pn.pattern.children.empty() &&
             pn.pattern.children[0].op == RtlOp::kSet) {
    combine_set = &pn.pattern.children[0];
  }
  if (combine_set == nullptr || combine_set->children.size() != 2 ||
      (combine_set->children[0].op != RtlOp::kReg &&
       combine_set->children[0].op != RtlOp::kSubreg) ||
      ExprContainsOp(pn.pattern, RtlOp::kCall)) {
    return -1;
  }

  const RtlExpr& r0 = combine_set->children[0];
  const RtlExpr& rhs = combine_set->children[1];
  const bool earlyclobber_di =
      !st->mips_cfg.gp_64bit && r0.mode == RtlMachineMode::kDi &&
      (rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt || rhs.op == RtlOp::kLshiftrt) &&
      rhs.children.size() == 2 && !rhs.children[1].IsConstInt();
  // Neither `mulsi3_internal` (`config/mips/mips.md:848-853`, `"=l"`) nor
  // `mulsi3_r4000` (`config/mips/mips.md:860-866`, `"=d"`) has an earlyclobber
  // `&` on operand 0.
  if (earlyclobber_di) {
    return -1;
  }

  std::vector<const RtlExpr*> cand_r1s;
  auto collect_cand_r1 = [&](const auto& self, const RtlExpr& e) -> void {
    if (e.op == RtlOp::kReg ||
        (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg)) {
      cand_r1s.push_back(&e);
      return;
    }
    if (e.op == RtlOp::kMem) {
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch);
    }
  };
  collect_cand_r1(collect_cand_r1, rhs);

  for (const RtlExpr* r1_ptr : cand_r1s) {
    bool may_save_copy = (pn.pattern.op == RtlOp::kSet && &rhs == r1_ptr);
    int offset = 0;
    const RtlExpr* u_expr = r1_ptr;
    while (u_expr->op == RtlOp::kSubreg && !u_expr->children.empty()) {
      if (ModeByteSize({u_expr->children[0].mode, u_expr->children[0].raw_mode}) >
          st->units_per_word) {
        may_save_copy = false;
      }
      offset += static_cast<int>(u_expr->int_val);
      u_expr = &u_expr->children[0];
    }
    const RtlExpr* s_expr = &r0;
    while (s_expr->op == RtlOp::kSubreg && !s_expr->children.empty()) {
      if (ModeByteSize({s_expr->children[0].mode, s_expr->children[0].raw_mode}) >
          st->units_per_word) {
        may_save_copy = false;
      }
      offset -= static_cast<int>(s_expr->int_val);
      s_expr = &s_expr->children[0];
    }
    if (u_expr->op != RtlOp::kReg || s_expr->op != RtlOp::kReg) {
      continue;
    }
    const int ureg = u_expr->IsArgPointerReg() ? kArgPointerRegnum : u_expr->RegNo();
    const int sreg = s_expr->IsArgPointerReg() ? kArgPointerRegnum : s_expr->RegNo();
    const RegModeInfo umode{u_expr->mode, u_expr->raw_mode};
    const RegModeInfo smode{s_expr->mode, s_expr->raw_mode};
    const int usize = (ureg < kFirstPseudoRegister) ? HardRegnoNregs(ureg, umode, st->mips_cfg)
                                                    : PseudoRegnoSize(umode, st->mips_cfg);
    const int ssize = (sreg < kFirstPseudoRegister) ? HardRegnoNregs(sreg, smode, st->mips_cfg)
                                                    : PseudoRegnoSize(smode, st->mips_cfg);
    if ((ureg >= kFirstPseudoRegister && (ureg >= st->max_regno || st->reg_qty[ureg] < 0)) ||
        (offset > 0 && usize + offset > ssize) || (offset < 0 && usize + offset < ssize) ||
        (ssize > usize && ureg >= kFirstPseudoRegister &&
         usize < st->model.qtys[st->reg_qty[ureg]].size) ||
        (sreg >= kFirstPseudoRegister && (sreg >= st->max_regno || st->reg_qty[sreg] == -1)) ||
        ureg == sreg || (ureg < kFirstPseudoRegister && sreg < kFirstPseudoRegister) ||
        !ModesTieableForLocalAlloc(umode, smode)) {
      continue;
    }
    if (ureg < kFirstPseudoRegister) {
      if (st->reg_qty[sreg] == -2) {
        MarkLocalRegBorn(*s_expr, 2 * insn_number, insn_number, st);
      }
      if (st->reg_qty[sreg] >= 0) {
        LocalBlockQty& sq = st->model.qtys[st->reg_qty[sreg]];
        if (may_save_copy && !sq.phys_copy_sugg.TestBit(ureg)) {
          sq.phys_copy_sugg.SetBit(ureg);
          ++sq.num_copy_sugg;
        } else if (!may_save_copy && !sq.phys_sugg.TestBit(ureg)) {
          sq.phys_sugg.SetBit(ureg);
          ++sq.num_sugg;
        }
      }
      continue;
    }
    if (sreg < kFirstPseudoRegister) {
      LocalBlockQty& uq = st->model.qtys[st->reg_qty[ureg]];
      if (may_save_copy && !uq.phys_copy_sugg.TestBit(sreg)) {
        uq.phys_copy_sugg.SetBit(sreg);
        ++uq.num_copy_sugg;
      } else if (!may_save_copy && !uq.phys_sugg.TestBit(sreg)) {
        uq.phys_sugg.SetBit(sreg);
        ++uq.num_sugg;
      }
      continue;
    }
    const int comb = TryCombineLocalPseudoPair(*r1_ptr, r0, pn, /*already_dead=*/false, st);
    if (comb >= 0) {
      return comb;
    }
  }
  return -1;
}

// Mirrors `local-alloc.c:1317-1352, 2282-2312` (`block_alloc` + `no_conflict_p`):
// combines `(clobber (reg r0)) (REG_LIBCALL)` with its dying `REG_NO_CONFLICT`
// input `r1` from the matching `(set r0 r0) (REG_RETVAL) (REG_EQUAL ...)` insn.
int TryCombineNoConflictLibcall(size_t insn_idx, const std::vector<RtlPatternAndNotes>& parsed,
                                LocalAllocBuilderState* st) {
  const RtlPatternAndNotes& pn = parsed[insn_idx];
  if (pn.pattern.op != RtlOp::kClobber || pn.pattern.children.size() != 1 ||
      pn.pattern.children[0].op != RtlOp::kReg || pn.pattern.children[0].IsArgPointerReg() ||
      pn.pattern.children[0].RegNo() < kFirstPseudoRegister ||
      !HasRegNoteKind(pn, RtlMachineMode::kRegLibcall)) {
    return -1;
  }
  const RtlExpr& r0 = pn.pattern.children[0];
  const int r0_no = r0.RegNo();
  size_t rv_idx = parsed.size();
  const RtlExpr* eq_expr = nullptr;
  for (size_t k = insn_idx + 1; k < parsed.size(); ++k) {
    const RtlPatternAndNotes& kp = parsed[k];
    if (!HasRegNoteKind(kp, RtlMachineMode::kRegRetval)) {
      continue;
    }
    const RtlExpr* kset = SingleSet(kp);
    if (kset != nullptr && kset->children[0].op == RtlOp::kReg &&
        !kset->children[0].IsArgPointerReg() && kset->children[0].RegNo() == r0_no &&
        kset->children[1] == kset->children[0]) {
      for (const RtlRegNote& n : kp.reg_notes) {
        if (n.kind == RtlMachineMode::kRegEqual) {
          eq_expr = &n.datum;
          rv_idx = k;
          break;
        }
      }
    }
    break;
  }
  if (eq_expr == nullptr || rv_idx >= parsed.size()) {
    return -1;
  }

  auto check_no_conflict = [&](const RtlExpr& cand) -> bool {
    const RtlExpr* base = &cand;
    if (base->op == RtlOp::kSubreg && !base->children.empty()) {
      base = &base->children[0];
    }
    if (base->op != RtlOp::kReg || base->IsArgPointerReg() ||
        base->RegNo() < kFirstPseudoRegister) {
      return false;
    }
    const int r1_no = base->RegNo();
    bool saw_dead = false;
    for (size_t k = insn_idx + 1; k < rv_idx; ++k) {
      const RtlPatternAndNotes& kp = parsed[k];
      if (kp.pattern.op == RtlOp::kNote || kp.pattern.op == RtlOp::kCodeLabel ||
          kp.pattern.op == RtlOp::kBarrier) {
        continue;
      }
      if (HasRegNoteFor(kp, RtlMachineMode::kRegDead, r1_no)) {
        saw_dead = true;
      }
      if (kp.pattern.ContainsReg(r1_no) &&
          !HasRegNoteFor(kp, RtlMachineMode::kRegNoConflict, r1_no)) {
        return false;
      }
    }
    return saw_dead;
  };

  std::vector<const RtlExpr*> nc_cands;
  if (eq_expr->op == RtlOp::kReg) {
    nc_cands.push_back(eq_expr);
  } else if (!eq_expr->children.empty()) {
    if (eq_expr->children[0].op == RtlOp::kReg || eq_expr->children[0].op == RtlOp::kSubreg) {
      nc_cands.push_back(&eq_expr->children[0]);
    }
    if (eq_expr->children.size() >= 2 &&
        (eq_expr->op == RtlOp::kEq || eq_expr->op == RtlOp::kNe || eq_expr->op == RtlOp::kPlus ||
         eq_expr->op == RtlOp::kMult || eq_expr->op == RtlOp::kAnd || eq_expr->op == RtlOp::kIor ||
         eq_expr->op == RtlOp::kXor) &&
        (eq_expr->children[1].op == RtlOp::kReg || eq_expr->children[1].op == RtlOp::kSubreg)) {
      nc_cands.push_back(&eq_expr->children[1]);
    }
  }
  for (const RtlExpr* cand : nc_cands) {
    if (check_no_conflict(*cand)) {
      const int comb = TryCombineLocalPseudoPair(*cand, r0, pn, /*already_dead=*/true, st);
      if (comb >= 0) {
        return comb;
      }
    }
  }
  return -1;
}

// Mirrors `local-alloc.c:1369-1415`: processes `REG_DEAD` (`wipe_dead_reg`),
// `note_stores` (`reg_is_born`), `REG_UNUSED`, `alloc_qty_for_scratch`
// (`local-alloc.c:306-389`), and `regs_live_at` updates for one instruction.
void UpdateLocalLivenessAndScratchesForInsn(
    const RtlPatternAndNotes& pn, int insn_number, int combined_regno,
    int* no_conflict_combined_regno, const std::vector<std::pair<int, RegModeInfo>>& insn_scratches,
    LocalAllocBuilderState* st) {
  const RtlExpr* primary_set = SingleSet(pn);

  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
      const int r = note.datum.IsArgPointerReg() ? kArgPointerRegnum : note.datum.RegNo();
      if (r != combined_regno && (r != *no_conflict_combined_regno ||
                                  !HasRegNoteFor(pn, RtlMachineMode::kRegNoConflict, r))) {
        WipeLocalDeadReg(pn, primary_set, insn_number, note.datum, /*output_p=*/false, st);
      }
    }
  }

  std::vector<bool> scratch_store_consumed(insn_scratches.size(), false);
  NoteStores(pn.pattern, [&](const RtlExpr& dest, const RtlExpr& setter) {
    if (setter.op == RtlOp::kClobber && dest.op == RtlOp::kReg && !dest.IsArgPointerReg()) {
      for (size_t si = 0; si < insn_scratches.size(); ++si) {
        if (!scratch_store_consumed[si] && insn_scratches[si].first == dest.RegNo() &&
            insn_scratches[si].second.mode == dest.mode) {
          scratch_store_consumed[si] = true;
          return;
        }
      }
    }
    MarkLocalRegBorn(dest, 2 * insn_number - (setter.op == RtlOp::kClobber ? 1 : 0), insn_number,
                     st);
  });

  std::vector<bool> scratch_note_consumed(insn_scratches.size(), false);
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegUnused && note.datum.op == RtlOp::kReg) {
      bool was_scratch = false;
      if (!note.datum.IsArgPointerReg()) {
        for (size_t si = 0; si < insn_scratches.size(); ++si) {
          if (!scratch_note_consumed[si] && insn_scratches[si].first == note.datum.RegNo() &&
              insn_scratches[si].second.mode == note.datum.mode) {
            scratch_note_consumed[si] = true;
            was_scratch = true;
            break;
          }
        }
      }
      if (!was_scratch) {
        WipeLocalDeadReg(pn, primary_set, insn_number, note.datum, /*output_p=*/true, st);
      }
    }
  }

  for (const auto& [scr_hr, scr_mode] : insn_scratches) {
    LocalBlockQty sq;
    sq.qty_id = static_cast<int>(st->model.qtys.size());
    sq.first_reg = -1;
    sq.mode = scr_mode;
    sq.size = ModeByteSize(scr_mode);
    sq.birth = 2 * insn_number - 1;
    sq.death = 2 * insn_number + 1;
    sq.min_class = RegNoRegClass(scr_hr);
    sq.alt_class = MipsRegClass::kNoRegs;
    sq.n_refs = 1;
    st->model.qtys.push_back(std::move(sq));
  }

  if (HasRegNoteKind(pn, RtlMachineMode::kRegRetval)) {
    *no_conflict_combined_regno = -1;
  }

  st->model.base_regs_live_at[2 * insn_number].Ior(st->regs_live);
  st->model.base_regs_live_at[2 * insn_number + 1].Ior(st->regs_live);
}

}  // namespace

int ComputeLocalQtyPriority(int n_refs, int span_len, int qty_size) {
  if (span_len <= 0) {
    span_len = 1;
  }
  return static_cast<int>(
      ((static_cast<double>(FloorLog2(n_refs) * n_refs * qty_size)) / span_len) * 10000.0);
}

bool ModesTieableForLocalAlloc(const RegModeInfo& m1, const RegModeInfo& m2) {
  return IsFloatOrComplexFloatMode(m1) == IsFloatOrComplexFloatMode(m2);
}

int SimulateLocalBlockAlloc(const LocalBlockAllocModel& m,
                            const std::vector<int>* custom_pass2_order,
                            absl::btree_map<int, int>* sim_hr_out) {
  const int next_qty = static_cast<int>(m.qtys.size());
  std::vector<HardRegSet> regs_live_at = m.base_regs_live_at;
  std::vector<int> qty_phys_reg(next_qty, -1);
  std::vector<int> num_copy_sugg(next_qty, 0);
  for (int q = 0; q < next_qty; ++q) {
    num_copy_sugg[q] = m.qtys[q].num_copy_sugg;
  }

  auto post_mark_life = [&](int regno, const RegModeInfo& mode, int birth, int death) {
    const int nregs = HardRegnoNregs(regno, mode, m.mips_cfg);
    const int hi_idx = std::min(death, static_cast<int>(regs_live_at.size()));
    for (int ins = std::max(0, birth); ins < hi_idx; ++ins) {
      for (int k = 0; k < nregs && regno + k < kFirstPseudoRegister; ++k) {
        regs_live_at[ins].SetBit(regno + k);
      }
    }
  };

  // Mirrors `local-alloc.c:2073-2217` (`find_free_reg`), scanning hard
  // registers in natural MIPS order `0 .. FIRST_PSEUDO_REGISTER - 1`
  // (`local-alloc.c:2158-2182`, `#ifndef REG_ALLOC_ORDER`).
  auto find_free_reg = [&](auto& self, MipsRegClass cls, const RegModeInfo& mode, int q,
                           bool accept_call_clobbered, bool just_try_suggested, int born_idx,
                           int dead_idx) -> int {
    if (born_idx < 0 || born_idx > dead_idx) {
      return -1;
    }
    HardRegSet used =
        (accept_call_clobbered || m.qtys[q].calls_crossed == 0) ? m.fixed_regs : m.call_used_regs;
    const int hi_idx = std::min(dead_idx, static_cast<int>(regs_live_at.size()));
    for (int ins = born_idx; ins < hi_idx; ++ins) {
      used.Ior(regs_live_at[ins]);
    }
    used.IorCompl(RegClassContents(cls));
    used.SetBit(kArgPointerRegnum);
    used.SetBit(kFramePointerRegnum);

    HardRegSet first_used = used;
    if (just_try_suggested) {
      if (num_copy_sugg[q] != 0) {
        first_used.IorCompl(m.qtys[q].phys_copy_sugg);
      } else {
        first_used.IorCompl(m.qtys[q].phys_sugg);
      }
    }

    const HardRegSet all_regs = RegClassContents(MipsRegClass::kAllRegs);
    const bool all_excluded =
        (all_regs.lo & ~first_used.lo) == 0 && ((all_regs.hi & 0xfULL) & ~first_used.hi) == 0;
    if (!all_excluded) {
      for (int i = 0; i < kFirstPseudoRegister; ++i) {
        const int regno = i;
        if (!first_used.TestBit(regno) && HardRegnoModeOk(regno, mode, m.mips_cfg)) {
          const int size1 = HardRegnoNregs(regno, mode, m.mips_cfg);
          int j = 1;
          while (j < size1 && regno + j < kFirstPseudoRegister && !used.TestBit(regno + j)) {
            ++j;
          }
          if (j == size1) {
            post_mark_life(regno, mode, born_idx, dead_idx);
            return regno;
          }
          i += j;
        }
      }
    }

    if (just_try_suggested && num_copy_sugg[q] != 0 && m.qtys[q].num_sugg != 0) {
      num_copy_sugg[q] = 0;
      return self(self, cls, mode, q, accept_call_clobbered, true, born_idx, dead_idx);
    }
    if (!accept_call_clobbered && m.mips_cfg.caller_saves && !just_try_suggested &&
        m.qtys[q].calls_crossed != 0 && (4 * m.qtys[q].calls_crossed < m.qtys[q].n_refs)) {
      return self(self, cls, mode, q, true, false, born_idx, dead_idx);
    }
    return -1;
  };

  auto qty_pri = [&](int q) {
    return ComputeLocalQtyPriority(m.qtys[q].n_refs, m.qtys[q].death - m.qtys[q].birth,
                                   m.qtys[q].size);
  };

  std::vector<int> custom_rank;
  if (custom_pass2_order != nullptr) {
    custom_rank.assign(next_qty, next_qty);
    for (int pos = 0; pos < static_cast<int>(custom_pass2_order->size()); ++pos) {
      const int q = (*custom_pass2_order)[pos];
      if (q >= 0 && q < next_qty) {
        custom_rank[q] = pos;
      }
    }
  }

  // Pass 1: suggested-register pass (`local-alloc.c:1435-1479, 1633-1659`).
  auto qty_sugg_cmp = [&](int q1, int q2) {
    const int s1 =
        (num_copy_sugg[q1] != 0) ? num_copy_sugg[q1] : m.qtys[q1].num_sugg * kFirstPseudoRegister;
    const int s2 =
        (num_copy_sugg[q2] != 0) ? num_copy_sugg[q2] : m.qtys[q2].num_sugg * kFirstPseudoRegister;
    if (s1 != s2) {
      return s1 - s2;
    }
    if (!custom_rank.empty()) {
      return custom_rank[q1] - custom_rank[q2];
    }
    return qty_pri(q2) - qty_pri(q1);
  };

  std::vector<int> sugg_order(next_qty);
  for (int i = 0; i < next_qty; ++i) {
    sugg_order[i] = i;
  }
  switch (next_qty) {
    case 3:
      if (qty_sugg_cmp(0, 1) > 0) {
        std::swap(sugg_order[0], sugg_order[1]);
      }
      if (qty_sugg_cmp(1, 2) > 0) {
        std::swap(sugg_order[2], sugg_order[1]);
      }
      [[fallthrough]];
    case 2:
      if (qty_sugg_cmp(0, 1) > 0) {
        std::swap(sugg_order[0], sugg_order[1]);
      }
      break;
    case 1:
    case 0:
      break;
    default:
      std::sort(sugg_order.begin(), sugg_order.end(), [&](int q1, int q2) {
        const int cmp = qty_sugg_cmp(q1, q2);
        return (cmp != 0) ? (cmp < 0) : (q1 < q2);
      });
      break;
  }

  for (int q : sugg_order) {
    if (m.qtys[q].num_sugg != 0 || num_copy_sugg[q] != 0) {
      qty_phys_reg[q] = find_free_reg(find_free_reg, m.qtys[q].min_class, m.qtys[q].mode, q, false,
                                      true, m.qtys[q].birth, m.qtys[q].death);
    }
  }

  // Pass 2: priority-sorted allocation pass (`local-alloc.c:1483-1540, 1579-1595`).
  auto qty_cmp = [&](int q1, int q2) {
    if (!custom_rank.empty()) {
      return custom_rank[q1] - custom_rank[q2];
    }
    const int p1 = qty_pri(q1);
    const int p2 = qty_pri(q2);
    return p2 - p1;
  };

  std::vector<int> pass2_order(next_qty);
  for (int i = 0; i < next_qty; ++i) {
    pass2_order[i] = i;
  }
  switch (next_qty) {
    case 3:
      if (qty_cmp(0, 1) > 0) {
        std::swap(pass2_order[0], pass2_order[1]);
      }
      if (qty_cmp(1, 2) > 0) {
        std::swap(pass2_order[2], pass2_order[1]);
      }
      [[fallthrough]];
    case 2:
      if (qty_cmp(0, 1) > 0) {
        std::swap(pass2_order[0], pass2_order[1]);
      }
      break;
    case 1:
    case 0:
      break;
    default:
      std::sort(pass2_order.begin(), pass2_order.end(), [&](int q1, int q2) {
        const int cmp = qty_cmp(q1, q2);
        return (cmp != 0) ? (cmp < 0) : (q1 < q2);
      });
      break;
  }

  for (int q : pass2_order) {
    if (qty_phys_reg[q] < 0) {
      qty_phys_reg[q] = find_free_reg(find_free_reg, m.qtys[q].min_class, m.qtys[q].mode, q, false,
                                      false, m.qtys[q].birth, m.qtys[q].death);
      if (qty_phys_reg[q] < 0 && m.qtys[q].alt_class != MipsRegClass::kNoRegs) {
        qty_phys_reg[q] = find_free_reg(find_free_reg, m.qtys[q].alt_class, m.qtys[q].mode, q,
                                        false, false, m.qtys[q].birth, m.qtys[q].death);
      }
    }
  }

  absl::btree_map<int, int> sim_hr;
  for (int p : m.eligible_pseudos) {
    sim_hr[p] = -1;
  }
  for (int q = 0; q < next_qty; ++q) {
    if (qty_phys_reg[q] >= 0) {
      for (size_t k = 0; k < m.qtys[q].member_regs.size(); ++k) {
        sim_hr[m.qtys[q].member_regs[k]] = qty_phys_reg[q] + m.qtys[q].member_offsets[k];
      }
    }
  }
  int mismatches = 0;
  for (int p : m.eligible_pseudos) {
    const int target = m.target_hr_by_pseudo.at(p);
    if (sim_hr[p] != target) {
      ++mismatches;
    }
  }
  if (sim_hr_out != nullptr) {
    *sim_hr_out = std::move(sim_hr);
  }
  return mismatches;
}

std::optional<LocalBlockAllocModel> BuildSingleBlockLocalAllocModel(
    size_t b, RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::btree_set<int>& live_at_any_block_start,
    const absl::btree_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::btree_map<int, RegModeInfo>& pseudo_modes,
    const absl::btree_set<int>& fp_op_pseudos, const absl::btree_set<int>& gr_or_mem_op_pseudos,
    bool bump_caller_saves_refs) {
  const int max_regno = std::max(dst_out->sideband.max_regno, kFirstPseudoRegister);
  const HardRegSet call_used = CallUsedRegSet(mips_cfg);
  std::vector<int> eligible_pseudos =
      CollectEligibleBlockLocalPseudos(b, max_regno, mips_cfg, call_used, live_at_any_block_start,
                                       pseudo_summaries, bump_caller_saves_refs, dst_out);
  if (eligible_pseudos.empty()) {
    return std::nullopt;
  }

  const RtlBasicBlock& out_blk = dst_out->rpo_blocks[b];
  const RtlBasicBlock& live_blk = liveness_cfg.rpo_blocks[b];
  std::vector<RtlPatternAndNotes> parsed_insns;
  int insn_count = 0;
  if (!ParseBlockInsnsForLocalAlloc(out_blk, live_blk, &parsed_insns, &insn_count)) {
    return std::nullopt;
  }

  LocalAllocBuilderState st{
      .dst_out = *dst_out,
      .mips_cfg = mips_cfg,
      .pseudo_modes = pseudo_modes,
      .max_regno = max_regno,
      .units_per_word = mips_cfg.gp_64bit ? 8 : 4,
      .reg_qty = std::vector<int>(max_regno, -1),
      .pref_class = std::vector<MipsRegClass>(max_regno, MipsRegClass::kGrRegs),
      .alt_class = std::vector<MipsRegClass>(max_regno, MipsRegClass::kNoRegs),
  };
  st.model.mips_cfg = mips_cfg;
  st.model.fixed_regs = FixedRegSet(mips_cfg);
  st.model.call_used_regs = call_used;
  st.model.eligible_pseudos = std::move(eligible_pseudos);
  st.model.reg_offset.assign(max_regno, 0);
  st.model.base_regs_live_at.assign(static_cast<size_t>(2 * insn_count + 2), {});

  InitLocalPseudoClassesAndTargets(*dst_out, mips_cfg, st.model.eligible_pseudos, pseudo_summaries,
                                   pseudo_modes, fp_op_pseudos, gr_or_mem_op_pseudos, &st.reg_qty,
                                   &st.pref_class, &st.alt_class, &st.model.target_hr_by_pseudo);

  if (auto live_it = dst_out->sideband.basic_block_live_at_start.find(static_cast<int>(b));
      live_it != dst_out->sideband.basic_block_live_at_start.end()) {
    for (int r : live_it->second) {
      if (r >= 0 && r < kFirstPseudoRegister) {
        st.regs_live.SetBit(r);
      }
    }
  }

  int insn_number = 0;
  if (out_blk.has_label) {
    insn_number = 1;
    st.model.base_regs_live_at[2].Ior(st.regs_live);
    st.model.base_regs_live_at[3].Ior(st.regs_live);
  }

  int no_conflict_combined_regno = -1;
  for (size_t i = 0; i < parsed_insns.size(); ++i) {
    const RtlPatternAndNotes& pn = parsed_insns[i];
    if (pn.pattern.op == RtlOp::kNote) {
      st.model.base_regs_live_at[2 * insn_number].Ior(st.regs_live);
      st.model.base_regs_live_at[2 * insn_number + 1].Ior(st.regs_live);
      continue;
    }
    ++insn_number;
    if (pn.pattern.op == RtlOp::kCodeLabel || pn.pattern.op == RtlOp::kBarrier ||
        pn.pattern.op == RtlOp::kAddrVec) {
      st.model.base_regs_live_at[2 * insn_number].Ior(st.regs_live);
      st.model.base_regs_live_at[2 * insn_number + 1].Ior(st.regs_live);
      continue;
    }

    const std::vector<std::pair<int, RegModeInfo>> insn_scratches =
        CollectInsnMatchScratchClobbers(pn.pattern);

    int combined_regno = RecordInsnTiesAndCombines(pn, insn_number, &st);
    const int nc_comb = TryCombineNoConflictLibcall(i, parsed_insns, &st);
    if (nc_comb >= 0) {
      combined_regno = nc_comb;
      no_conflict_combined_regno = nc_comb;
    }

    UpdateLocalLivenessAndScratchesForInsn(pn, insn_number, combined_regno,
                                           &no_conflict_combined_regno, insn_scratches, &st);
  }

  for (const LocalBlockQty& q : st.model.qtys) {
    if (q.birth < 0 || q.death < q.birth) {
      return std::nullopt;
    }
  }
  return st.model;
}

}  // namespace rom_nom_nom::lifter
