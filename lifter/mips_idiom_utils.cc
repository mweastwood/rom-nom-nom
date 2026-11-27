#include "lifter/mips_idiom_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/functional/function_ref.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/rtl_cfg_utils.h"

namespace rom_nom_nom::lifter {
namespace {

bool IsIdiomControlTopExpr(const RtlExpr& expr) {
  return IsAnyJumpOrReturnTopExpr(expr) || expr.op == RtlOp::kAddrVec ||
         expr.op == RtlOp::kAddrDiffVec;
}

bool ExprHasVolatileOrAsm(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&found](const RtlExpr& n) {
    if (n.flags.volatil || n.op == RtlOp::kAsmInput || n.op == RtlOp::kAsmOperands ||
        n.op == RtlOp::kUnspecVolatile) {
      found = true;
    }
  });
  return found;
}

RtlPatternAndNotes MakeFoldedInsn(RtlExpr pattern, bool mark_newly_folded_sentinel) {
  RtlPatternAndNotes pn;
  pn.pattern = std::move(pattern);
  if (mark_newly_folded_sentinel) {
    pn.reg_notes.push_back(RtlRegNote{.kind = RtlMachineMode::kVoid, .datum = RtlExpr::MakeNil()});
  }
  return pn;
}

bool MatchGp32WordDest(const RtlExpr& dst, int expected_regno) {
  if (dst.op == RtlOp::kReg && dst.mode == RtlMachineMode::kSi && dst.RegNo() == expected_regno) {
    return true;
  }
  if (dst.op == RtlOp::kSubreg && dst.mode == RtlMachineMode::kSi && dst.children.size() == 1 &&
      dst.children[0].op == RtlOp::kReg && dst.children[0].mode == RtlMachineMode::kDi) {
    return dst.children[0].RegNo() + static_cast<int>(dst.int_val) == expected_regno;
  }
  return false;
}

bool ExprReadsExplicitDiReg(const RtlExpr& expr, int di_regno, bool in_dest = false) {
  if (expr.op == RtlOp::kSequence && expr.children.size() >= 2) {
    for (size_t sub = 0; sub < 2; ++sub) {
      if (!expr.children[sub].children.empty() &&
          ExprReadsExplicitDiReg(expr.children[sub].children[0], di_regno, false)) {
        return true;
      }
    }
    return false;
  }
  if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
    const bool dst_mem = (expr.children[0].op == RtlOp::kMem);
    if (dst_mem && ExprReadsExplicitDiReg(expr.children[0], di_regno, false)) {
      return true;
    }
    return ExprReadsExplicitDiReg(expr.children[1], di_regno, false);
  }
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : expr.children) {
      if (ExprReadsExplicitDiReg(elem, di_regno, false)) {
        return true;
      }
    }
    return false;
  }
  if (expr.op == RtlOp::kClobber || expr.op == RtlOp::kSubreg) {
    return false;
  }
  if (!in_dest && expr.op == RtlOp::kReg && expr.mode == RtlMachineMode::kDi &&
      expr.RegNo() == di_regno) {
    return true;
  }
  for (const RtlExpr& ch : expr.children) {
    if (ExprReadsExplicitDiReg(ch, di_regno, in_dest)) {
      return true;
    }
  }
  return false;
}

struct NoConflictBlockInsn {
  RtlPatternAndNotes parsed;
  MipsHardRegSet read_regs;
  MipsHardRegSet write_regs;
  bool is_call = false;
  bool is_jump_or_ret = false;
};

NoConflictBlockInsn BuildNoConflictBlockInsn(RtlPatternAndNotes parsed, bool gp64, bool fp64,
                                             const std::optional<RtlInsn>& orig_rinsn) {
  if (orig_rinsn.has_value() && orig_rinsn->has_explicit_call_fusage &&
      !parsed.has_explicit_call_fusage) {
    parsed.has_explicit_call_fusage = true;
    parsed.call_fusage = orig_rinsn->call_fusage;
  }
  NoConflictBlockInsn out{.parsed = std::move(parsed)};
  const RtlExpr& pat = out.parsed.pattern;
  out.is_jump_or_ret = IsIdiomControlTopExpr(pat);
  if (pat.op == RtlOp::kSequence && pat.children.size() >= 2 && !pat.children[0].children.empty() &&
      !pat.children[1].children.empty()) {
    CollectHardRegDefsAndUses(pat.children[0].children[0], gp64, fp64, &out.write_regs,
                              &out.read_regs);
    out.is_call =
        pat.children[0].op == RtlOp::kCallInsn || ContainsCallInExpr(pat.children[0].children[0]);
    if (out.is_call) {
      auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(pat.children[0]);
      if (sub0.ok() && sub0->has_explicit_call_fusage) {
        ApplyCallClobbersAndUses(*sub0, gp64, fp64, &out.write_regs, &out.read_regs);
      } else {
        ApplyCallClobbersAndUses(out.parsed, gp64, fp64, &out.write_regs, &out.read_regs);
      }
    }
    CollectHardRegDefsAndUses(pat.children[1].children[0], gp64, fp64, &out.write_regs,
                              &out.read_regs);
    return out;
  }
  CollectHardRegDefsAndUses(pat, gp64, fp64, &out.write_regs, &out.read_regs);
  out.is_call = ContainsCallInExpr(pat);
  if (out.is_call) {
    ApplyCallClobbersAndUses(out.parsed, gp64, fp64, &out.write_regs, &out.read_regs);
  }
  return out;
}

bool IsNoConflictPrologueSaveStore(const RtlExpr& pat) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kMem || dst.children.size() != 1 || dst.flags.volatil ||
      dst.flags.in_struct || src.op != RtlOp::kReg || src.flags.volatil || src.flags.in_struct) {
    return false;
  }
  const int regno = src.RegNo();
  const bool is_saved =
      (regno >= 16 && regno <= 23) || regno == 30 || regno == 31 || (regno >= 52 && regno <= 63);
  if (!is_saved) {
    return false;
  }
  const RtlExpr& addr = dst.children[0];
  if (addr.op == RtlOp::kReg) {
    return addr.RegNo() == 29 || addr.RegNo() == 13;
  }
  return addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
         addr.children[0].op == RtlOp::kReg &&
         (addr.children[0].RegNo() == 29 || addr.children[0].RegNo() == 13) &&
         addr.children[1].op == RtlOp::kConstInt && addr.children[1].int_val >= 0;
}

bool TryMatchExtendHighWordFill(const std::vector<NoConflictBlockInsn>& insns, size_t i,
                                int src_reg, std::optional<size_t> narrow_sll_idx, int dh,
                                const absl::btree_set<size_t>& used_indices, size_t* out_j,
                                std::optional<size_t>* out_hilo_sra_idx) {
  const int dl = dh + 1;
  for (size_t j = 0; j < insns.size(); ++j) {
    if (j == i || (narrow_sll_idx.has_value() && j <= *narrow_sll_idx) ||
        used_indices.contains(j)) {
      continue;
    }
    const RtlExpr& pj = insns[j].parsed.pattern;
    if (pj.op != RtlOp::kSet || pj.children.size() != 2 || !MatchGp32WordDest(pj.children[0], dh)) {
      continue;
    }
    const RtlExpr& hi_src = pj.children[1];
    const bool is_zero_hi =
        !narrow_sll_idx.has_value() && (hi_src.op == RtlOp::kConstInt && hi_src.int_val == 0);
    bool is_sign_hi =
        (hi_src.op == RtlOp::kAshiftrt && hi_src.mode == RtlMachineMode::kSi &&
         hi_src.children.size() == 2 && hi_src.children[0].op == RtlOp::kReg &&
         hi_src.children[0].mode == RtlMachineMode::kSi &&
         (hi_src.children[0].RegNo() == src_reg ||
          (!narrow_sll_idx.has_value() && i < j && hi_src.children[0].RegNo() == dl)) &&
         hi_src.children[1].op == RtlOp::kConstInt && hi_src.children[1].int_val == 31);
    std::optional<size_t> hilo_sra_idx;
    if (!is_zero_hi && !is_sign_hi && dh == 64 && hi_src.op == RtlOp::kReg &&
        hi_src.mode == RtlMachineMode::kSi) {
      const int tmp_reg = hi_src.RegNo();
      for (size_t rev = j; rev > 0; --rev) {
        const size_t cand_sra = rev - 1;
        if (cand_sra == i || (narrow_sll_idx.has_value() && cand_sra <= *narrow_sll_idx)) {
          continue;
        }
        const RtlExpr& ps = insns[cand_sra].parsed.pattern;
        if (ps.op == RtlOp::kSet && ps.children.size() == 2 && ps.children[0].op == RtlOp::kReg &&
            ps.children[0].mode == RtlMachineMode::kSi && ps.children[0].RegNo() == tmp_reg &&
            ps.children[1].op == RtlOp::kAshiftrt && ps.children[1].mode == RtlMachineMode::kSi &&
            ps.children[1].children.size() == 2 && ps.children[1].children[0].op == RtlOp::kReg &&
            ps.children[1].children[0].mode == RtlMachineMode::kSi &&
            ps.children[1].children[0].RegNo() == src_reg &&
            ps.children[1].children[1].op == RtlOp::kConstInt &&
            ps.children[1].children[1].int_val == 31 && (tmp_reg != src_reg || cand_sra >= i)) {
          hilo_sra_idx = cand_sra;
          is_sign_hi = true;
          break;
        }
        if (insns[cand_sra].write_regs.Test(tmp_reg) || insns[cand_sra].is_call ||
            insns[cand_sra].is_jump_or_ret) {
          break;
        }
      }
    }
    if (!is_zero_hi && !is_sign_hi) {
      continue;
    }
    size_t lo_pos = std::min(i, j);
    if (hilo_sra_idx.has_value()) {
      lo_pos = std::min(lo_pos, *hilo_sra_idx);
    }
    if (narrow_sll_idx.has_value()) {
      lo_pos = std::min(lo_pos, *narrow_sll_idx);
    }
    const size_t hi_pos = std::max(i, j);
    bool span_clean = true;
    for (size_t k = lo_pos; k < hi_pos; ++k) {
      if (k == i || k == j || (hilo_sra_idx.has_value() && k == *hilo_sra_idx) ||
          (narrow_sll_idx.has_value() && k == *narrow_sll_idx)) {
        continue;
      }
      const size_t src_live_end = hilo_sra_idx.has_value() ? std::max(i, *hilo_sra_idx) : hi_pos;
      if (insns[k].is_call || insns[k].is_jump_or_ret || insns[k].write_regs.Test(dh) ||
          insns[k].write_regs.Test(dl) || (k < src_live_end && insns[k].write_regs.Test(src_reg)) ||
          insns[k].read_regs.Test(dh) || insns[k].read_regs.Test(dl)) {
        span_clean = false;
        break;
      }
    }
    if (!span_clean) {
      continue;
    }
    *out_j = j;
    *out_hilo_sra_idx = hilo_sra_idx;
    return true;
  }
  return false;
}

struct NoConflictExtendMatch {
  int src_reg = -1;
  int dh = -1;
  size_t lo_idx = 0;
  size_t hi_idx = 0;
  size_t lo_pos = 0;
  std::optional<size_t> di_consumer_idx;
};

// Helper 1 of `RestoreNoConflictExtendClobbersInBlock`: identifies a
// `!TARGET_64BIT` `emit_no_conflict_block` (`optabs.c:2429-2526`) 32-to-64-bit
// extension anchored at `lo_idx` (`i`) whose `DImode` result is consumed in the
// same block and does not already have an explicit `(clobber (reg:DI dh))`.
std::optional<NoConflictExtendMatch> FindNoConflictExtendMatch(
    const RtlControlFlowGraph& cfg, size_t b, const std::vector<NoConflictBlockInsn>& insns,
    size_t i, bool gp64, bool fp64, const absl::btree_set<size_t>& used_indices,
    std::optional<TailDelaySlotDiConsumerFn> tail_di_consumer_check) {
  const RtlExpr& pi = insns[i].parsed.pattern;
  if (pi.op != RtlOp::kSet || pi.children.size() != 2) {
    return std::nullopt;
  }
  int src_reg = -1;
  std::optional<size_t> narrow_sll_idx;
  if (pi.children[1].op == RtlOp::kReg && pi.children[1].mode == RtlMachineMode::kSi) {
    src_reg = pi.children[1].RegNo();
  } else if (pi.children[1].op == RtlOp::kAshiftrt && pi.children[1].mode == RtlMachineMode::kSi &&
             pi.children[1].children.size() == 2 && pi.children[1].children[0].op == RtlOp::kReg &&
             pi.children[1].children[0].mode == RtlMachineMode::kSi &&
             pi.children[1].children[1].op == RtlOp::kConstInt &&
             (pi.children[1].children[1].int_val == 16 ||
              pi.children[1].children[1].int_val == 24)) {
    const int cand_src = pi.children[1].children[0].RegNo();
    const int64_t sh = pi.children[1].children[1].int_val;
    for (size_t rev = i; rev > 0; --rev) {
      const size_t k = rev - 1;
      if (used_indices.contains(k) || insns[k].is_call || insns[k].is_jump_or_ret) {
        break;
      }
      if (insns[k].write_regs.Test(cand_src)) {
        const RtlExpr& pk = insns[k].parsed.pattern;
        if (pk.op == RtlOp::kSet && pk.children.size() == 2 && pk.children[0].op == RtlOp::kReg &&
            pk.children[0].mode == RtlMachineMode::kSi && pk.children[0].RegNo() == cand_src &&
            pk.children[1].op == RtlOp::kAshift && pk.children[1].mode == RtlMachineMode::kSi &&
            pk.children[1].children.size() == 2 && pk.children[1].children[0].op == RtlOp::kReg &&
            pk.children[1].children[0].mode == RtlMachineMode::kSi &&
            pk.children[1].children[1].op == RtlOp::kConstInt &&
            pk.children[1].children[1].int_val == sh) {
          src_reg = cand_src;
          narrow_sll_idx = k;
        }
        break;
      }
    }
  }
  if (src_reg < 1 || src_reg > 25) {
    return std::nullopt;
  }

  static constexpr int kCandidateDiRegs[] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 64};
  for (int dh : kCandidateDiRegs) {
    const int dl = dh + 1;
    if (src_reg == dl || !MatchGp32WordDest(pi.children[0], dl)) {
      continue;
    }
    size_t j = 0;
    std::optional<size_t> hilo_sra_idx;
    if (!TryMatchExtendHighWordFill(insns, i, src_reg, narrow_sll_idx, dh, used_indices, &j,
                                    &hilo_sra_idx)) {
      continue;
    }
    size_t lo_pos = std::min(i, j);
    if (hilo_sra_idx.has_value()) {
      lo_pos = std::min(lo_pos, *hilo_sra_idx);
    }
    if (narrow_sll_idx.has_value()) {
      lo_pos = std::min(lo_pos, *narrow_sll_idx);
    }
    const size_t hi_pos = std::max(i, j);

    bool di_consumed = false;
    std::optional<size_t> di_consumer_idx;
    for (size_t k = hi_pos + 1; k < insns.size(); ++k) {
      if (ExprReadsExplicitDiReg(insns[k].parsed.pattern, dh)) {
        di_consumed = true;
        di_consumer_idx = k;
        break;
      }
      if (insns[k].write_regs.Test(dh) || insns[k].write_regs.Test(dl) || insns[k].is_call) {
        break;
      }
    }
    if (!di_consumed && tail_di_consumer_check.has_value()) {
      std::vector<RtlPatternAndNotes> tail_insns;
      tail_insns.reserve(insns.size() - (hi_pos + 1));
      for (size_t k = hi_pos + 1; k < insns.size(); ++k) {
        tail_insns.push_back(insns[k].parsed);
      }
      if ((*tail_di_consumer_check)(cfg, b, tail_insns, dh, gp64, fp64)) {
        di_consumed = true;
        di_consumer_idx = insns.size() - 1;
      }
    }
    if (!di_consumed) {
      continue;
    }

    bool already_has_clobber = false;
    for (size_t rev = lo_pos; rev > 0; --rev) {
      const RtlExpr& prev_pat = insns[rev - 1].parsed.pattern;
      if (prev_pat.op == RtlOp::kClobber && prev_pat.children.size() == 1 &&
          prev_pat.children[0].op == RtlOp::kReg &&
          prev_pat.children[0].mode == RtlMachineMode::kDi && prev_pat.children[0].RegNo() == dh) {
        already_has_clobber = true;
        break;
      }
      // `sched.c:2201-2234` + `config/mips/mips.h:1203-1210`
      // (`CALL_USED_REGISTERS`): `sched2` can hoist a callee-saved
      // `(clobber (reg:DI s_even))` (`$s0-$s7`, even pairs `16, 18, 20, 22`)
      // across a `CALL_INSN` because calls do not clobber callee-saved regs.
      const bool call_clobbers_pair = insns[rev - 1].is_call && (dh < 16 || dh > 22);
      if (insns[rev - 1].write_regs.Test(dh) || insns[rev - 1].write_regs.Test(dl) ||
          insns[rev - 1].read_regs.Test(dh) || insns[rev - 1].read_regs.Test(dl) ||
          call_clobbers_pair) {
        break;
      }
    }
    if (already_has_clobber) {
      return std::nullopt;
    }
    return NoConflictExtendMatch{
        .src_reg = src_reg,
        .dh = dh,
        .lo_idx = i,
        .hi_idx = j,
        .lo_pos = lo_pos,
        .di_consumer_idx = di_consumer_idx,
    };
  }
  return std::nullopt;
}

// Helper 2 of `RestoreNoConflictExtendClobbersInBlock`: computes the exact
// block insertion position for `(clobber (reg:DI match.dh))` accounting for
// `sched2` (`sched.c:2384-2430`) load-stall hoisting, prologue saves,
// consumer-fed loads, and callee-saved cross-call FP stall slots.
size_t ComputeClobberInsertPos(const std::vector<NoConflictBlockInsn>& insns,
                               const NoConflictExtendMatch& match, bool runs_sched2) {
  const int dh = match.dh;
  const int dl = dh + 1;
  const int src_reg = match.src_reg;
  const size_t lo_pos = match.lo_pos;
  size_t insert_pos = lo_pos;

  const bool is_zero_extend_hi_after_lo =
      runs_sched2 && match.hi_idx > match.lo_idx && insns[match.hi_idx].parsed.pattern.IsSet() &&
      insns[match.hi_idx].parsed.pattern.SetSrc().IsConstInt() &&
      insns[match.hi_idx].parsed.pattern.SetSrc().int_val == 0;
  for (size_t rev = lo_pos; rev > 0; --rev) {
    const size_t cand = rev - 1;
    const RtlExpr& cpat = insns[cand].parsed.pattern;
    if (insns[cand].is_call || insns[cand].is_jump_or_ret || insns[cand].read_regs.Test(dh) ||
        insns[cand].read_regs.Test(dl) || insns[cand].write_regs.Test(dh) ||
        insns[cand].write_regs.Test(dl)) {
      break;
    }
    if (insns[cand].write_regs.Test(src_reg)) {
      // `optabs.c:2485-2525` + `sched.c:2384-2430`: when a zero-extend `lo`
      // consumer immediately follows an ALU producer of `src_reg` stalled on an
      // earlier load within 2 insns, `sched2` schedules the zero-cost `clobber`
      // into the load-delay slot ahead of that producer.
      if (is_zero_extend_hi_after_lo && cand == lo_pos - 1 && cand > 0) {
        bool stalled_on_recent_load = false;
        const size_t lo_win = (cand >= 2) ? (cand - 2) : 0;
        for (size_t m = lo_win; m < cand; ++m) {
          const RtlExpr& mpat = insns[m].parsed.pattern;
          if (mpat.IsSet() && mpat.SetDest().IsHardReg() && mpat.SetSrc().IsMem() &&
              insns[cand].read_regs.Test(mpat.SetDest().RegNo())) {
            stalled_on_recent_load = true;
            break;
          }
        }
        if (stalled_on_recent_load) {
          insert_pos = cand;
        }
      }
      break;
    }
    if (IsNoConflictPrologueSaveStore(cpat)) {
      if (runs_sched2) {
        insert_pos = cand;
      }
      continue;
    }
    if (match.di_consumer_idx.has_value() && cpat.op == RtlOp::kSet && cpat.children.size() == 2 &&
        cpat.children[0].op == RtlOp::kReg && cpat.children[1].op == RtlOp::kMem &&
        !cpat.children[1].flags.volatil) {
      const int load_reg = cpat.children[0].RegNo();
      const bool load_is_pair = (cpat.children[0].mode == RtlMachineMode::kDi ||
                                 cpat.children[0].mode == RtlMachineMode::kDf);
      bool fed_to_di_consumer = insns[*match.di_consumer_idx].read_regs.Test(load_reg);
      if (fed_to_di_consumer) {
        for (size_t m = cand + 1; m < *match.di_consumer_idx; ++m) {
          if (insns[m].read_regs.Test(load_reg) || insns[m].write_regs.Test(load_reg) ||
              (load_is_pair &&
               (insns[m].read_regs.Test(load_reg + 1) || insns[m].write_regs.Test(load_reg + 1)))) {
            fed_to_di_consumer = false;
            break;
          }
        }
      }
      bool overlap_src_def_precedes_loads = false;
      if (!fed_to_di_consumer && runs_sched2 && (dh == src_reg || dl == src_reg)) {
        for (size_t prev_k = cand; prev_k > 0; --prev_k) {
          const auto& pk = insns[prev_k - 1];
          if (pk.is_call || pk.is_jump_or_ret) {
            break;
          }
          if (pk.write_regs.Test(src_reg)) {
            if (pk.parsed.pattern.IsSet() && pk.parsed.pattern.SetDest().IsHardReg() &&
                pk.parsed.pattern.SetDest().RegNo() == src_reg &&
                pk.parsed.pattern.SetDest().mode != RtlMachineMode::kDi) {
              overlap_src_def_precedes_loads = true;
            }
            break;
          }
          if (pk.read_regs.Test(dh) || pk.read_regs.Test(dl) || pk.write_regs.Test(dh) ||
              pk.write_regs.Test(dl)) {
            break;
          }
          const RtlExpr& pkpat = pk.parsed.pattern;
          const bool is_save_or_load = IsNoConflictPrologueSaveStore(pkpat) ||
                                       (pkpat.IsSet() && pkpat.SetDest().IsHardReg() &&
                                        pkpat.SetSrc().IsMem() && !pkpat.SetSrc().flags.volatil);
          if (!is_save_or_load) {
            break;
          }
        }
      }
      if (fed_to_di_consumer || overlap_src_def_precedes_loads) {
        insert_pos = cand;
        continue;
      }
    }
    break;
  }

  // `optabs.c:2485-2525` + `sched.c:1920-2020, 2384-2430`: `sched1` may hoist
  // a callee-saved `(clobber (reg:DI dh))` across a `CALL_INSN`, and `sched2`
  // may schedule it into a 1-cycle unfilled `const_double -> FP arith` stall.
  if (runs_sched2 && insert_pos == lo_pos && dh >= 16 && dh <= 22 && (dh % 2) == 0) {
    bool crossed_call = false;
    for (size_t rev = lo_pos; rev > 0; --rev) {
      const size_t u = rev - 1;
      if (insns[u].is_jump_or_ret || insns[u].read_regs.Test(dh) || insns[u].read_regs.Test(dl) ||
          insns[u].write_regs.Test(dh) || insns[u].write_regs.Test(dl)) {
        break;
      }
      if (insns[u].is_call) {
        if (crossed_call) {
          break;
        }
        crossed_call = true;
        continue;
      }
      if (!crossed_call || u == 0 || u + 1 >= lo_pos) {
        continue;
      }
      auto is_fp_const_load = [](const RtlExpr& p) {
        return p.IsSet() && p.SetDest().IsHardReg() && p.SetDest().RegNo() >= 32 &&
               p.SetDest().RegNo() < 64 && p.SetSrc().op == RtlOp::kConstDouble;
      };
      auto is_fp_arith_using = [&](size_t idx, int fp_reg) {
        const RtlExpr& p = insns[idx].parsed.pattern;
        return p.IsSet() && p.SetDest().IsHardReg() && p.SetDest().RegNo() >= 32 &&
               p.SetDest().RegNo() < 64 &&
               (p.SetSrc().op == RtlOp::kPlus || p.SetSrc().op == RtlOp::kMinus ||
                p.SetSrc().op == RtlOp::kMult || p.SetSrc().op == RtlOp::kDiv) &&
               insns[idx].read_regs.Test(fp_reg);
      };
      auto is_gpr_reg_move = [&](size_t idx) {
        const RtlExpr& p = insns[idx].parsed.pattern;
        return !insns[idx].is_call && !insns[idx].is_jump_or_ret && p.IsSet() &&
               p.SetDest().IsHardReg() && p.SetDest().RegNo() >= 1 && p.SetDest().RegNo() < 32 &&
               p.SetSrc().IsHardReg() && p.SetSrc().RegNo() >= 1 && p.SetSrc().RegNo() < 32;
      };
      auto gpr_indep_of = [&](size_t gpr_idx, size_t a_idx, size_t b_idx) {
        const int gd = insns[gpr_idx].parsed.pattern.SetDest().RegNo();
        const int gs = insns[gpr_idx].parsed.pattern.SetSrc().RegNo();
        return !insns[a_idx].read_regs.Test(gd) && !insns[a_idx].write_regs.Test(gd) &&
               !insns[a_idx].write_regs.Test(gs) && !insns[b_idx].read_regs.Test(gd) &&
               !insns[b_idx].write_regs.Test(gd) && !insns[b_idx].write_regs.Test(gs);
      };
      const RtlExpr& p_u = insns[u].parsed.pattern;
      const RtlExpr& p_prev = insns[u - 1].parsed.pattern;
      const bool order_move_then_xfer = is_fp_const_load(p_u) &&
                                        is_fp_arith_using(u + 1, p_u.SetDest().RegNo()) &&
                                        is_gpr_reg_move(u - 1) && gpr_indep_of(u - 1, u, u + 1);
      const bool order_xfer_then_move = is_fp_const_load(p_prev) &&
                                        is_fp_arith_using(u + 1, p_prev.SetDest().RegNo()) &&
                                        is_gpr_reg_move(u) && gpr_indep_of(u, u - 1, u + 1);
      if (order_move_then_xfer || order_xfer_then_move) {
        insert_pos = u + 1;
        break;
      }
    }
  }
  return insert_pos;
}

// Helper 3 of `RestoreNoConflictExtendClobbersInBlock`: restores
// `alter_subreg`-collapsed `(subreg:SI (reg:DI dh) 1/0)` destinations
// (`final.c:1928-1946, 2095-2107`), `REG_NO_CONFLICT` (`optabs.c:2495-2502`,
// `local-alloc.c:1317-1352`), and `REG_DEAD` (`sched.c:2971`) notes in `blk`.
void ApplyExtendSubregAndNoConflictNotes(const RtlControlFlowGraph& cfg, bool gp64, bool fp64,
                                         bool runs_sched2, const NoConflictExtendMatch& match,
                                         size_t matched_insert_pos,
                                         std::vector<NoConflictBlockInsn>* insns_ptr,
                                         RtlBasicBlock* blk) {
  auto& insns = *insns_ptr;
  const int matched_dh = match.dh;
  const int src_reg = match.src_reg;
  const size_t i = match.lo_idx;
  const size_t matched_hi_idx = match.hi_idx;

  bool inter_src_read = false;
  bool dst_overlaps_src_with_prior_read = false;
  if (matched_dh >= 2 && matched_dh <= 24 && match.di_consumer_idx.has_value()) {
    const size_t first_ext = std::min(i, matched_hi_idx);
    const size_t second_ext = std::max(i, matched_hi_idx);
    for (size_t k = first_ext + 1; k < *match.di_consumer_idx; ++k) {
      if (k != second_ext && insns[k].read_regs.Test(src_reg)) {
        inter_src_read = true;
        break;
      }
    }
    if ((matched_dh == src_reg || matched_dh + 1 == src_reg) && matched_insert_pos > 0) {
      for (size_t k = 0; k < matched_insert_pos; ++k) {
        if (insns[k].read_regs.Test(src_reg)) {
          dst_overlaps_src_with_prior_read = true;
          break;
        }
      }
      const bool is_overlap_sign_extend_hi_after_lo =
          matched_hi_idx > i && insns[i].parsed.pattern.IsSet() &&
          insns[i].parsed.pattern.SetSrc().IsHardReg() &&
          insns[i].parsed.pattern.SetSrc().RegNo() == src_reg &&
          insns[matched_hi_idx].parsed.pattern.IsSet() &&
          insns[matched_hi_idx].parsed.pattern.SetSrc().op == RtlOp::kAshiftrt &&
          insns[matched_hi_idx].parsed.pattern.SetSrc().children.size() == 2 &&
          insns[matched_hi_idx].parsed.pattern.SetSrc().children[1].IsConstInt() &&
          insns[matched_hi_idx].parsed.pattern.SetSrc().children[1].int_val == 31;
      if (!dst_overlaps_src_with_prior_read && is_overlap_sign_extend_hi_after_lo) {
        // `local-alloc.c:1317-1352, 1722-1838` (`combine_regs`): tying a dying
        // `SImode` input pseudo `r1` (`src_reg`) onto a `DImode` destination
        // `r0` (`matched_dh`) requires `r0` to be block-local.
        bool di_live_past_block_or_return = false;
        bool di_killed_in_block = false;
        for (size_t m = matched_hi_idx + 1; m < insns.size(); ++m) {
          const RtlExpr& p = insns[m].parsed.pattern;
          if (p.op == RtlOp::kUse && !p.children.empty() && p.children[0].IsHardReg()) {
            const int ur = p.children[0].RegNo();
            if (ur == matched_dh || ur == matched_dh + 1) {
              di_live_past_block_or_return = true;
              break;
            }
          }
          if (insns[m].is_call && matched_dh < 16) {
            di_killed_in_block = true;
            break;
          }
          if (!insns[m].read_regs.Test(matched_dh) && !insns[m].read_regs.Test(matched_dh + 1) &&
              insns[m].write_regs.Test(matched_dh) && insns[m].write_regs.Test(matched_dh + 1)) {
            di_killed_in_block = true;
            break;
          }
          if (!insns[m].write_regs.Test(matched_dh) && !insns[m].write_regs.Test(matched_dh + 1)) {
            for (const RtlRegNote& rn : insns[m].parsed.reg_notes) {
              if (rn.kind == RtlMachineMode::kRegDead && rn.datum.IsHardReg() &&
                  rn.datum.mode == RtlMachineMode::kDi && rn.datum.RegNo() == matched_dh) {
                di_killed_in_block = true;
                break;
              }
            }
            if (di_killed_in_block) {
              break;
            }
          }
        }
        if (!di_live_past_block_or_return && !di_killed_in_block) {
          if (blk->successors.empty()) {
            if (matched_dh == 2) {
              di_live_past_block_or_return = true;
            }
          } else {
            const std::vector<MipsHardRegSet> live_in = ComputeHardRegLiveIn(cfg, gp64, fp64);
            for (size_t succ : blk->successors) {
              if (succ < live_in.size() &&
                  (live_in[succ].Test(matched_dh) || live_in[succ].Test(matched_dh + 1))) {
                di_live_past_block_or_return = true;
                break;
              }
            }
          }
        }
        if (!di_live_past_block_or_return) {
          for (size_t k = matched_insert_pos; k > 0; --k) {
            const auto& prev = insns[k - 1];
            if (!prev.write_regs.Test(src_reg)) {
              continue;
            }
            if (!prev.is_call && prev.parsed.pattern.IsSet() &&
                prev.parsed.pattern.SetDest().IsHardReg() &&
                prev.parsed.pattern.SetDest().RegNo() == src_reg &&
                prev.parsed.pattern.SetDest().mode != RtlMachineMode::kDi) {
              dst_overlaps_src_with_prior_read = true;
            }
            break;
          }
        }
      }
    }
  }

  bool src_defined_before_lo = false;
  for (size_t k = 0; k < i; ++k) {
    if (insns[k].write_regs.Test(src_reg)) {
      src_defined_before_lo = true;
      break;
    }
  }
  // `optabs.c:2485-2525`, `final.c:1928-1946`, `sched.c:1659-1792` (`sched_analyze_1`).
  const bool is_zero_extend_hi_after_lo =
      runs_sched2 && src_defined_before_lo && matched_dh != src_reg && matched_dh + 1 != src_reg &&
      matched_hi_idx > i && insns[matched_hi_idx].parsed.pattern.IsSet() &&
      insns[matched_hi_idx].parsed.pattern.SetSrc().IsConstInt() &&
      insns[matched_hi_idx].parsed.pattern.SetSrc().int_val == 0;
  // `optabs.c:2485-2510`, `final.c:1941-1946, 2095-2120`, `local-alloc.c:700, 1004-1019`
  // (`optimize_reg_copy_1`): restore `(subreg:SI (reg:DI dh) 1/0)` on disjoint
  // `extendsidi2` so `optimize_reg_copy_1` does not mistake `lo = src` for a
  // plain `REG` copy.
  const bool is_sign_extend_hi_after_lo =
      src_defined_before_lo && matched_dh != src_reg && matched_dh + 1 != src_reg &&
      matched_hi_idx > i && insns[i].parsed.pattern.IsSet() &&
      insns[i].parsed.pattern.SetSrc().IsHardReg() &&
      insns[i].parsed.pattern.SetSrc().RegNo() == src_reg &&
      insns[matched_hi_idx].parsed.pattern.IsSet() &&
      insns[matched_hi_idx].parsed.pattern.SetSrc().op == RtlOp::kAshiftrt &&
      insns[matched_hi_idx].parsed.pattern.SetSrc().children.size() == 2 &&
      insns[matched_hi_idx].parsed.pattern.SetSrc().children[1].IsConstInt() &&
      insns[matched_hi_idx].parsed.pattern.SetSrc().children[1].int_val == 31;
  const bool wrap_di_subregs = inter_src_read || dst_overlaps_src_with_prior_read ||
                               is_zero_extend_hi_after_lo || is_sign_extend_hi_after_lo;
  auto append_note_if_missing = [&](size_t idx, RtlMachineMode kind, int regno) {
    RtlPatternAndNotes& pn = insns[idx].parsed;
    const bool exists =
        std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
          return n.kind == kind && n.datum.IsReg() && n.datum.RegNo() == regno;
        });
    if (!exists) {
      pn.reg_notes.push_back(RtlRegNote{.kind = kind,
                                        .list_op = RtlOp::kExprList,
                                        .datum = RtlExpr::MakeReg(RtlMachineMode::kSi, regno)});
    }
  };
  auto sync_insn = [&](size_t idx) {
    std::string updated = insns[idx].parsed.ToGccPrintRtlString();
    if (blk->rtl_insns.size() == blk->instructions.size()) {
      blk->rtl_insns[idx].canonical_pattern = updated;
    }
    blk->instructions[idx] = std::move(updated);
  };
  if ((matched_dh == 64 || wrap_di_subregs) &&
      insns[i].parsed.pattern.children[0].op == RtlOp::kReg) {
    insns[i].parsed.pattern.children[0] = RtlExpr::MakeSubreg(
        RtlMachineMode::kSi, RtlExpr::MakeReg(RtlMachineMode::kDi, matched_dh), 1);
    if (dst_overlaps_src_with_prior_read) {
      append_note_if_missing(i, RtlMachineMode::kRegNoConflict, src_reg);
    }
    sync_insn(i);
  }
  if (wrap_di_subregs && insns[matched_hi_idx].parsed.pattern.children[0].op == RtlOp::kReg) {
    insns[matched_hi_idx].parsed.pattern.children[0] = RtlExpr::MakeSubreg(
        RtlMachineMode::kSi, RtlExpr::MakeReg(RtlMachineMode::kDi, matched_dh), 0);
    if (dst_overlaps_src_with_prior_read) {
      append_note_if_missing(matched_hi_idx, RtlMachineMode::kRegNoConflict, src_reg);
    }
    sync_insn(matched_hi_idx);
  }
  if (dst_overlaps_src_with_prior_read) {
    const size_t last_ext = std::max(i, matched_hi_idx);
    // `sched.c:2971-3018` (`attach_deaths_insn`): `REG_DEAD` prepends ahead of
    // `REG_NO_CONFLICT`.
    RtlPatternAndNotes& last_pn = insns[last_ext].parsed;
    const bool has_dead =
        std::any_of(last_pn.reg_notes.begin(), last_pn.reg_notes.end(), [&](const RtlRegNote& n) {
          return n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                 n.datum.RegNo() == src_reg;
        });
    if (!has_dead) {
      last_pn.reg_notes.insert(last_pn.reg_notes.begin(),
                               RtlRegNote{.kind = RtlMachineMode::kRegDead,
                                          .list_op = RtlOp::kExprList,
                                          .datum = RtlExpr::MakeReg(RtlMachineMode::kSi, src_reg)});
    }
    sync_insn(last_ext);
  }
  if (match.di_consumer_idx.has_value()) {
    RtlPatternAndNotes& cons_pn = insns[*match.di_consumer_idx].parsed;
    const RtlExpr* cons_set = nullptr;
    if (cons_pn.pattern.IsSet()) {
      cons_set = &cons_pn.pattern;
    } else if (cons_pn.pattern.IsParallel()) {
      for (const RtlExpr& elem : cons_pn.pattern.children) {
        if (elem.IsSet()) {
          cons_set = (cons_set == nullptr) ? &elem : nullptr;
          if (cons_set == nullptr) {
            break;
          }
        }
      }
    }
    const bool has_retval_or_dead =
        std::any_of(cons_pn.reg_notes.begin(), cons_pn.reg_notes.end(), [&](const RtlRegNote& n) {
          return n.kind == RtlMachineMode::kRegRetval ||
                 (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                  n.datum.RegNo() == matched_dh);
        });
    if (cons_set != nullptr && !has_retval_or_dead && cons_set->SetDest().IsHardReg() &&
        cons_set->SetDest().mode == RtlMachineMode::kDi &&
        cons_set->SetDest().RegNo() == matched_dh && !cons_set->SetDest().flags.integrated) {
      auto first_preorder_pos = [](const RtlExpr& root, int regno) {
        int idx = 0;
        int found = -1;
        root.Visit([&](const RtlExpr& n) {
          ++idx;
          if (found < 0 && n.IsReg() && !n.IsArgPointerReg() && n.RegNo() == regno) {
            found = idx;
          }
        });
        return (found < 0) ? idx : found;
      };
      const int my_ord = first_preorder_pos(cons_set->SetSrc(), matched_dh);
      auto ins_it = std::find_if(
          cons_pn.reg_notes.begin(), cons_pn.reg_notes.end(), [&](const RtlRegNote& ex) {
            if (ex.kind == RtlMachineMode::kRegEquiv) {
              return false;
            }
            if (ex.kind != RtlMachineMode::kRegDead || !ex.datum.IsReg() ||
                ex.datum.IsArgPointerReg()) {
              return true;
            }
            return first_preorder_pos(cons_set->SetSrc(), ex.datum.RegNo()) > my_ord;
          });
      cons_pn.reg_notes.insert(ins_it,
                               RtlRegNote{
                                   .kind = RtlMachineMode::kRegDead,
                                   .list_op = RtlOp::kExprList,
                                   .datum = RtlExpr::MakeReg(RtlMachineMode::kDi, matched_dh),
                               });
      sync_insn(*match.di_consumer_idx);
    }
  }
}

// Helper 4 of `RestoreNoConflictExtendClobbersInBlock`: splices sorted
// `(clobber (reg:DI dh)) (insn_list:REG_LIBCALL (nil))` markers
// (`optabs.c:2487-2523`, skipped in `final.c:1548-1550`) into `blk`.
void SpliceClobberInsnsIntoBlock(std::vector<std::pair<size_t, int>> insertions,
                                 RtlBasicBlock* blk) {
  if (insertions.empty()) {
    return;
  }
  std::sort(insertions.begin(), insertions.end());
  const bool had_synced_rtl = (blk->rtl_insns.size() == blk->instructions.size());
  std::vector<std::string> new_instructions;
  std::vector<RtlInsn> new_rtl_insns;
  new_instructions.reserve(blk->instructions.size() + insertions.size());
  if (had_synced_rtl) {
    new_rtl_insns.reserve(blk->rtl_insns.size() + insertions.size());
  }
  size_t ins_cursor = 0;
  for (size_t idx = 0; idx < blk->instructions.size(); ++idx) {
    while (ins_cursor < insertions.size() && insertions[ins_cursor].first == idx) {
      RtlPatternAndNotes clob_pn;
      clob_pn.pattern = RtlExpr::MakeClobber(
          RtlExpr::MakeReg(RtlMachineMode::kDi, insertions[ins_cursor].second));
      clob_pn.reg_notes.push_back(RtlRegNote{
          .kind = RtlMachineMode::kRegLibcall,
          .list_op = RtlOp::kInsnList,
          .datum = RtlExpr::MakeNil(),
      });
      std::string clob_str = clob_pn.ToGccPrintRtlString();
      if (had_synced_rtl) {
        RtlInsn clob_rinsn;
        clob_rinsn.canonical_pattern = clob_str;
        new_rtl_insns.push_back(std::move(clob_rinsn));
      }
      new_instructions.push_back(std::move(clob_str));
      ++ins_cursor;
    }
    new_instructions.push_back(std::move(blk->instructions[idx]));
    if (had_synced_rtl) {
      new_rtl_insns.push_back(std::move(blk->rtl_insns[idx]));
    }
  }
  blk->instructions = std::move(new_instructions);
  if (had_synced_rtl) {
    blk->rtl_insns = std::move(new_rtl_insns);
  } else {
    blk->SyncRtlInsnsFromInstructions();
  }
}

void RestoreNoConflictExtendClobbersInBlock(
    const RtlControlFlowGraph& cfg, size_t b, bool gp64, bool fp64, bool runs_sched2,
    std::optional<TailDelaySlotDiConsumerFn> tail_di_consumer_check, RtlBasicBlock* blk) {
  if (blk->instructions.size() < 2) {
    return;
  }
  std::vector<NoConflictBlockInsn> insns;
  insns.reserve(blk->instructions.size());
  for (size_t i = 0; i < blk->instructions.size(); ++i) {
    auto parsed = ParsePatternAndNotes(blk->instructions[i]);
    if (!parsed.ok()) {
      return;
    }
    std::optional<RtlInsn> orig;
    if (blk->rtl_insns.size() == blk->instructions.size()) {
      orig = blk->rtl_insns[i];
    }
    insns.push_back(BuildNoConflictBlockInsn(std::move(*parsed), gp64, fp64, orig));
  }

  std::vector<std::pair<size_t, int>> insertions;
  absl::btree_set<size_t> used_indices;
  for (size_t i = 0; i < insns.size(); ++i) {
    if (used_indices.contains(i)) {
      continue;
    }
    std::optional<NoConflictExtendMatch> match = FindNoConflictExtendMatch(
        cfg, b, insns, i, gp64, fp64, used_indices, tail_di_consumer_check);
    if (!match.has_value()) {
      continue;
    }
    const size_t insert_pos = ComputeClobberInsertPos(insns, *match, runs_sched2);
    ApplyExtendSubregAndNoConflictNotes(cfg, gp64, fp64, runs_sched2, *match, insert_pos, &insns,
                                        blk);
    used_indices.insert(match->lo_idx);
    used_indices.insert(match->hi_idx);
    insertions.push_back({insert_pos, match->dh});
  }
  SpliceClobberInsnsIntoBlock(std::move(insertions), blk);
}

// Restores `flow.c:1907, 2537-2550` and `sched.c:2855-2920` `REG_DEAD` /
// `REG_UNUSED` notes on the two cross-multiply instructions of a 32-bit
// `muldi3` sequence after wrapping their `SUBREG` operands.
void UpdateMuldi3CrossMulNotes(size_t first_mul, size_t last_mul, int a_hi, int a_lo, int b_hi,
                               int b_lo, bool wrap_a, bool wrap_b, bool runs_sched2,
                               std::vector<NoConflictBlockInsn>* insns_ptr) {
  auto& insns = *insns_ptr;
  bool stripped_first_a = false;
  bool stripped_first_b = false;

  // Drop premature single-word `REG_DEAD` on `first_mul` and restore canonical
  // forward `flow.c:1907` (`mark_set_regs`) `REG_UNUSED` scratch order.
  auto& first_notes = insns[first_mul].parsed.reg_notes;
  first_notes.erase(std::remove_if(first_notes.begin(), first_notes.end(),
                                   [&](const RtlRegNote& rn) {
                                     if (rn.kind != RtlMachineMode::kRegDead ||
                                         !rn.datum.IsHardReg() ||
                                         rn.datum.mode != RtlMachineMode::kSi) {
                                       return false;
                                     }
                                     const int nr = rn.datum.RegNo();
                                     if (wrap_a && (nr == a_hi || nr == a_lo)) {
                                       stripped_first_a = true;
                                       return true;
                                     }
                                     if (wrap_b && (nr == b_hi || nr == b_lo)) {
                                       stripped_first_b = true;
                                       return true;
                                     }
                                     return false;
                                   }),
                    first_notes.end());
  const RtlExpr& first_dst = insns[first_mul].parsed.pattern.children[0].SetDest();
  if ((stripped_first_a || stripped_first_b) && first_dst.IsHardReg() && !first_dst.flags.Any() &&
      first_dst.RegNo() >= 3 && first_dst.RegNo() <= 25) {
    std::vector<int> clobber_order;
    for (size_t ci = 1; ci < insns[first_mul].parsed.pattern.children.size(); ++ci) {
      const RtlExpr& ch = insns[first_mul].parsed.pattern.children[ci];
      if (ch.op == RtlOp::kClobber && ch.children.size() == 1 && ch.children[0].IsHardReg()) {
        clobber_order.push_back(ch.children[0].RegNo());
      }
    }
    if (clobber_order.size() >= 2) {
      std::vector<size_t> note_slots;
      std::vector<RtlRegNote> ordered_notes;
      for (size_t ni = 0; ni < first_notes.size(); ++ni) {
        if (first_notes[ni].kind == RtlMachineMode::kRegUnused &&
            first_notes[ni].datum.IsHardReg() &&
            std::find(clobber_order.begin(), clobber_order.end(), first_notes[ni].datum.RegNo()) !=
                clobber_order.end()) {
          note_slots.push_back(ni);
        }
      }
      if (note_slots.size() == clobber_order.size()) {
        for (int cr : clobber_order) {
          for (size_t ni : note_slots) {
            if (first_notes[ni].datum.RegNo() == cr) {
              ordered_notes.push_back(first_notes[ni]);
              break;
            }
          }
        }
        if (ordered_notes.size() == note_slots.size()) {
          for (size_t si = 0; si < note_slots.size(); ++si) {
            first_notes[note_slots[si]] = std::move(ordered_notes[si]);
          }
        }
      }
    }
  }

  auto has_intervening_reg_dead = [&](int target_reg) {
    for (size_t k = first_mul + 1; k < last_mul; ++k) {
      for (const RtlRegNote& krn : insns[k].parsed.reg_notes) {
        if (krn.kind == RtlMachineMode::kRegDead && krn.datum.IsHardReg() &&
            krn.datum.RegNo() == target_reg) {
          return true;
        }
      }
    }
    return false;
  };
  for (RtlRegNote& rn : insns[last_mul].parsed.reg_notes) {
    if (rn.kind != RtlMachineMode::kRegDead || !rn.datum.IsHardReg() ||
        rn.datum.mode != RtlMachineMode::kSi) {
      continue;
    }
    const int nr = rn.datum.RegNo();
    if (wrap_a && (nr == a_lo || nr == a_hi) &&
        (stripped_first_a ||
         (insns[last_mul].write_regs.Test(a_hi) && !insns[last_mul].write_regs.Test(a_lo) &&
          (!runs_sched2 || !has_intervening_reg_dead(a_hi))))) {
      rn.datum = RtlExpr::MakeReg(RtlMachineMode::kDi, a_hi);
    } else if (wrap_b && (nr == b_lo || nr == b_hi) &&
               (stripped_first_b ||
                (insns[last_mul].write_regs.Test(b_hi) && !insns[last_mul].write_regs.Test(b_lo) &&
                 (!runs_sched2 || !has_intervening_reg_dead(b_hi))))) {
      rn.datum = RtlExpr::MakeReg(RtlMachineMode::kDi, b_hi);
    }
  }
}

// Per `optabs.c:expand_binop` (`muldi3`), `final.c:1928-1946, 2095-2107`
// (`alter_subreg`), `config/mips/mips.md:860-878` (`mulsi3_r4000`), and
// `sched.c:1797-2005` (`sched_analyze_2`): restores `alter_subreg`-collapsed
// `(subreg:SI (reg:DI A/B) 0/1)` operands on 32-bit `muldi3` 3-multiply
// sequences.
void RestoreMuldi3SubregOperandsInBlock(bool gp64, bool fp64, bool runs_sched2,
                                        RtlBasicBlock* blk) {
  if (gp64 || blk->instructions.size() < 3) {
    return;
  }
  const size_t n = blk->instructions.size();
  std::vector<NoConflictBlockInsn> insns;
  insns.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    auto parsed = ParsePatternAndNotes(blk->instructions[i]);
    if (!parsed.ok()) {
      return;
    }
    std::optional<RtlInsn> orig;
    if (blk->rtl_insns.size() == n) {
      orig = blk->rtl_insns[i];
    }
    insns.push_back(BuildNoConflictBlockInsn(std::move(*parsed), gp64, fp64, orig));
  }

  auto extract_si_hard_word = [](const RtlExpr& e) -> int {
    if (e.IsHardReg() && e.mode == RtlMachineMode::kSi) {
      return e.RegNo();
    }
    if (e.op == RtlOp::kSubreg && e.mode == RtlMachineMode::kSi && e.children.size() == 1 &&
        e.children[0].IsHardReg() && e.children[0].mode == RtlMachineMode::kDi &&
        (e.int_val == 0 || e.int_val == 1)) {
      return e.children[0].RegNo() + static_cast<int>(e.int_val);
    }
    return -1;
  };
  auto match_umulsidi3 = [&](const RtlExpr& pat, int* r0, int* r1) -> bool {
    if (pat.op != RtlOp::kParallel || pat.children.empty() || !pat.children[0].IsSet()) {
      return false;
    }
    const RtlExpr& s = pat.children[0];
    if (!s.SetDest().IsHardReg() || s.SetDest().mode != RtlMachineMode::kDi ||
        s.SetDest().RegNo() != 64 || s.SetSrc().op != RtlOp::kMult ||
        s.SetSrc().mode != RtlMachineMode::kDi || s.SetSrc().children.size() != 2) {
      return false;
    }
    const RtlExpr& c0 = s.SetSrc().children[0];
    const RtlExpr& c1 = s.SetSrc().children[1];
    if (c0.op != RtlOp::kZeroExtend || c0.children.size() != 1 || c1.op != RtlOp::kZeroExtend ||
        c1.children.size() != 1) {
      return false;
    }
    const int w0 = extract_si_hard_word(c0.children[0]);
    const int w1 = extract_si_hard_word(c1.children[0]);
    if (w0 < 0 || w1 < 0) {
      return false;
    }
    *r0 = w0;
    *r1 = w1;
    return true;
  };
  auto match_mulsi3 = [&](const RtlExpr& pat, int* r0, int* r1) -> bool {
    if (pat.op != RtlOp::kParallel || pat.children.empty() || !pat.children[0].IsSet()) {
      return false;
    }
    const RtlExpr& s = pat.children[0];
    if (!s.SetDest().IsHardReg() || s.SetDest().mode != RtlMachineMode::kSi ||
        (s.SetDest().RegNo() != 65 && (s.SetDest().RegNo() < 2 || s.SetDest().RegNo() > 25)) ||
        s.SetSrc().op != RtlOp::kMult || s.SetSrc().mode != RtlMachineMode::kSi ||
        s.SetSrc().children.size() != 2) {
      return false;
    }
    const int w0 = extract_si_hard_word(s.SetSrc().children[0]);
    const int w1 = extract_si_hard_word(s.SetSrc().children[1]);
    if (w0 < 0 || w1 < 0) {
      return false;
    }
    *r0 = w0;
    *r1 = w1;
    return true;
  };

  bool any_changed = false;
  for (size_t i = 0; i < n; ++i) {
    int a_lo = -1;
    int b_lo = -1;
    if (!match_umulsidi3(insns[i].parsed.pattern, &a_lo, &b_lo)) {
      continue;
    }
    if (a_lo < 3 || a_lo > 23 || (a_lo % 2) != 1 || b_lo < 3 || b_lo > 23 || (b_lo % 2) != 1 ||
        a_lo == b_lo) {
      continue;
    }
    const int a_hi = a_lo - 1;
    const int b_hi = b_lo - 1;
    std::optional<size_t> cross_ab;
    std::optional<size_t> cross_ba;
    for (size_t k = i + 1; k < n; ++k) {
      if (insns[k].is_call || insns[k].is_jump_or_ret) {
        break;
      }
      int m0 = -1;
      int m1 = -1;
      if (match_mulsi3(insns[k].parsed.pattern, &m0, &m1)) {
        if (!cross_ab.has_value() && ((m0 == a_lo && m1 == b_hi) || (m0 == b_hi && m1 == a_lo))) {
          cross_ab = k;
        } else if (!cross_ba.has_value() &&
                   ((m0 == b_lo && m1 == a_hi) || (m0 == a_hi && m1 == b_lo))) {
          cross_ba = k;
        }
        if (cross_ab.has_value() && cross_ba.has_value()) {
          break;
        }
      }
      if (insns[k].write_regs.Test(a_hi) || insns[k].write_regs.Test(a_lo) ||
          insns[k].write_regs.Test(b_hi) || insns[k].write_regs.Test(b_lo)) {
        break;
      }
    }
    if (!cross_ab.has_value() || !cross_ba.has_value()) {
      continue;
    }
    const size_t last_mul = std::max(*cross_ab, *cross_ba);
    bool wrap_a = false;
    bool wrap_b = false;
    if ((insns[last_mul].write_regs.Test(a_hi) && !insns[last_mul].write_regs.Test(a_lo)) ||
        (insns[last_mul].write_regs.Test(b_hi) && !insns[last_mul].write_regs.Test(b_lo))) {
      wrap_a = true;
      wrap_b = true;
    }
    if (runs_sched2) {
      for (size_t k = last_mul + 1; k < n; ++k) {
        if (insns[k].is_call || insns[k].is_jump_or_ret || insns[k].read_regs.Test(65)) {
          break;
        }
        if (insns[k].write_regs.Test(a_hi) && !insns[k].write_regs.Test(a_lo)) {
          wrap_a = true;
        }
        if (insns[k].write_regs.Test(b_hi) && !insns[k].write_regs.Test(b_lo)) {
          wrap_b = true;
        }
      }
    }
    if (!wrap_a && !wrap_b) {
      continue;
    }
    auto wrap_operand = [&](RtlExpr* expr) {
      if (!expr->IsHardReg() || expr->mode != RtlMachineMode::kSi) {
        return;
      }
      const int r = expr->RegNo();
      if (wrap_a && r == a_hi) {
        *expr = RtlExpr::MakeSubreg(RtlMachineMode::kSi,
                                    RtlExpr::MakeReg(RtlMachineMode::kDi, a_hi), 0);
      } else if (wrap_a && r == a_lo) {
        *expr = RtlExpr::MakeSubreg(RtlMachineMode::kSi,
                                    RtlExpr::MakeReg(RtlMachineMode::kDi, a_hi), 1);
      } else if (wrap_b && r == b_hi) {
        *expr = RtlExpr::MakeSubreg(RtlMachineMode::kSi,
                                    RtlExpr::MakeReg(RtlMachineMode::kDi, b_hi), 0);
      } else if (wrap_b && r == b_lo) {
        *expr = RtlExpr::MakeSubreg(RtlMachineMode::kSi,
                                    RtlExpr::MakeReg(RtlMachineMode::kDi, b_hi), 1);
      }
    };
    auto update_insn = [&](size_t idx) {
      std::string updated = insns[idx].parsed.ToGccPrintRtlString();
      if (updated == blk->instructions[idx]) {
        return;
      }
      if (blk->rtl_insns.size() == blk->instructions.size()) {
        blk->rtl_insns[idx].canonical_pattern = updated;
      }
      blk->instructions[idx] = std::move(updated);
      any_changed = true;
    };

    RtlExpr& mult0 = insns[i].parsed.pattern.children[0].children[1];
    wrap_operand(&mult0.children[0].children[0]);
    wrap_operand(&mult0.children[1].children[0]);
    update_insn(i);

    const size_t first_mul = (*cross_ab == last_mul) ? *cross_ba : *cross_ab;
    for (size_t idx : {first_mul, last_mul}) {
      RtlExpr& multk = insns[idx].parsed.pattern.children[0].children[1];
      wrap_operand(&multk.children[0]);
      wrap_operand(&multk.children[1]);
    }
    UpdateMuldi3CrossMulNotes(first_mul, last_mul, a_hi, a_lo, b_hi, b_lo, wrap_a, wrap_b,
                              runs_sched2, &insns);
    update_insn(first_mul);
    update_insn(last_mul);
  }
  if (any_changed && blk->rtl_insns.size() != blk->instructions.size()) {
    blk->SyncRtlInsnsFromInstructions();
  }
}

void RestoreNoConflictExtendClobbersImpl(
    RtlControlFlowGraph* cfg, bool gp64, bool fp64, bool runs_sched2,
    std::optional<TailDelaySlotDiConsumerFn> tail_di_consumer_check) {
  if (cfg == nullptr || gp64) {
    return;
  }
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RestoreNoConflictExtendClobbersInBlock(*cfg, b, gp64, fp64, runs_sched2, tail_di_consumer_check,
                                           &cfg->rpo_blocks[b]);
    RestoreMuldi3SubregOperandsInBlock(gp64, fp64, runs_sched2, &cfg->rpo_blocks[b]);
  }
}

}  // namespace

bool IsSavedGprOrRaRegno(int r) {
  return (r >= 16 && r <= 23) || r == 30 || r == 31;
}

bool IsValidGp32DiPair(int reg_hi) {
  return reg_hi >= 2 && reg_hi <= 24 && (reg_hi % 2) == 0;
}

bool IsPlainSiGpr(const RtlExpr& expr, int* out_reg) {
  if (expr.op != RtlOp::kReg || expr.mode != RtlMachineMode::kSi) {
    return false;
  }
  const int r = expr.RegNo();
  if (r < 1 || r > 31) {
    return false;
  }
  if (out_reg != nullptr) {
    *out_reg = r;
  }
  return true;
}

int DetectStackFrameSize(const RtlControlFlowGraph& cfg) {
  if (cfg.rpo_blocks.empty()) {
    return 0;
  }
  for (const std::string& s : cfg.rpo_blocks[0].instructions) {
    auto p = ParsePatternAndNotes(s);
    if (!p.ok() || !p->pattern.IsSet() || !p->pattern.SetDest().IsReg() ||
        p->pattern.SetDest().RegNo() != 29) {
      continue;
    }
    const RtlExpr& src = p->pattern.SetSrc();
    if (src.op == RtlOp::kPlus && src.children.size() == 2 && src.children[0].IsReg() &&
        src.children[0].RegNo() == 29 && src.children[1].IsConstInt() &&
        src.children[1].int_val < 0) {
      return static_cast<int>(-src.children[1].int_val);
    }
    if (src.op == RtlOp::kMinus && src.children.size() == 2 && src.children[0].IsReg() &&
        src.children[0].RegNo() == 29 && src.children[1].IsConstInt() &&
        src.children[1].int_val > 0) {
      return static_cast<int>(src.children[1].int_val);
    }
  }
  return 0;
}

int ModeAccessByteSize(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQi:
      return 1;
    case RtlMachineMode::kHi:
      return 2;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
      return 4;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      return 8;
    default:
      return 0;
  }
}

int HardRegModeSpan(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  // Mirrors `HARD_REGNO_NREGS` (`config/mips/mips.h:1255-1258`).
  int bytes = ModeAccessByteSize(mode);
  if (bytes == 0) {
    if (mode == RtlMachineMode::kTi || mode == RtlMachineMode::kTf) {
      bytes = 16;
    } else {
      bytes = 4;
    }
  }
  const bool is_fp = (regno >= 32 && regno < 64);
  const int unit_bytes = is_fp ? (fp64 ? 8 : 4) : (gp64 ? 8 : 4);
  return std::max(1, (bytes + unit_bytes - 1) / unit_bytes);
}

bool ExtractBasePlusConstAddr(const RtlExpr& addr, int* out_base, int64_t* out_off) {
  int base = -1;
  if (IsPlainSiGpr(addr, &base)) {
    *out_base = base;
    *out_off = 0;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.mode == RtlMachineMode::kSi && addr.children.size() == 2 &&
      IsPlainSiGpr(addr.children[0], &base) && addr.children[1].IsConstInt()) {
    *out_base = base;
    *out_off = addr.children[1].int_val;
    return true;
  }
  return false;
}

bool ExtractMovstrsiAddrsAndBytes(const RtlExpr& pat, int* out_dst_reg, int64_t* out_dst_off,
                                  int* out_src_reg, int64_t* out_src_off, int* out_bytes) {
  if (pat.op != RtlOp::kParallel || pat.children.size() != 8) {
    return false;
  }
  if (!pat.children[0].IsSet() || !pat.children[0].SetDest().IsMem() ||
      pat.children[0].SetDest().mode != RtlMachineMode::kBlk ||
      pat.children[0].SetDest().children.size() != 1 ||
      !ExtractBasePlusConstAddr(pat.children[0].SetDest().children[0], out_dst_reg, out_dst_off) ||
      !pat.children[0].SetSrc().IsMem() || pat.children[0].SetSrc().mode != RtlMachineMode::kBlk ||
      pat.children[0].SetSrc().children.size() != 1 ||
      !ExtractBasePlusConstAddr(pat.children[0].SetSrc().children[0], out_src_reg, out_src_off)) {
    return false;
  }
  if (pat.children[5].op != RtlOp::kUse || pat.children[5].children.size() != 1 ||
      !pat.children[5].children[0].IsConstInt() || pat.children[5].children[0].int_val <= 0) {
    return false;
  }
  *out_bytes = static_cast<int>(pat.children[5].children[0].int_val);
  return true;
}

bool MatchMovstrStore(const RtlExpr& pat, int* out_reg, int* out_base, int64_t* out_off,
                      int* out_width) {
  if (!pat.IsSet() || !pat.SetDest().IsMem() || pat.SetDest().flags.volatil ||
      pat.SetDest().flags.in_struct || pat.SetDest().children.size() != 1) {
    return false;
  }
  const RtlMachineMode mem_mode = pat.SetDest().mode;
  const RtlExpr& src = pat.SetSrc();
  int r = -1;
  if (src.IsReg() && src.mode == mem_mode) {
    r = src.RegNo();
  } else if (src.IsSubreg() && src.mode == mem_mode && src.int_val == 0 &&
             src.children.size() == 1 && src.children[0].IsReg() &&
             (mem_mode == RtlMachineMode::kQi || mem_mode == RtlMachineMode::kHi) &&
             src.children[0].mode == RtlMachineMode::kSi) {
    r = src.children[0].RegNo();
  } else {
    return false;
  }
  if (r < 1 || r > 25) {
    return false;
  }
  if (!ExtractBasePlusConstAddr(pat.SetDest().children[0], out_base, out_off) || r == *out_base) {
    return false;
  }
  *out_reg = r;
  if (mem_mode == RtlMachineMode::kSi) {
    *out_width = 4;
    return true;
  }
  if (mem_mode == RtlMachineMode::kHi) {
    *out_width = 2;
    return true;
  }
  if (mem_mode == RtlMachineMode::kQi) {
    *out_width = 1;
    return true;
  }
  return false;
}

bool IsMovstrsiSplitPart(const RtlExpr& pat, int64_t expected_split_kind) {
  if (pat.op != RtlOp::kParallel || pat.children.size() != 8) {
    return false;
  }
  if (!pat.children[0].IsSet() || !pat.children[0].SetDest().IsMem() ||
      pat.children[0].SetDest().mode != RtlMachineMode::kBlk || !pat.children[0].SetSrc().IsMem() ||
      pat.children[0].SetSrc().mode != RtlMachineMode::kBlk) {
    return false;
  }
  for (size_t k = 1; k <= 4; ++k) {
    if (pat.children[k].op != RtlOp::kClobber) {
      return false;
    }
  }
  for (size_t k = 5; k <= 7; ++k) {
    if (pat.children[k].op != RtlOp::kUse || pat.children[k].children.size() != 1 ||
        !pat.children[k].children[0].IsConstInt()) {
      return false;
    }
  }
  return pat.children[7].children[0].int_val == expected_split_kind;
}

void CollectHardRegReads(const RtlExpr& expr, bool gp64, bool fp64, MipsHardRegSet* uses) {
  if (expr.IsReg()) {
    const int r = expr.RegNo();
    if (r >= 0 && r < 68) {
      const int span = HardRegModeSpan(r, expr.mode, gp64, fp64);
      for (int k = 0; k < span; ++k) {
        uses->Set(r + k);
      }
    }
    return;
  }
  if (expr.IsSubreg() && !expr.children.empty() && expr.children[0].IsReg()) {
    const int base = expr.children[0].RegNo();
    if (base >= 0 && base < 68) {
      const int r = base + static_cast<int>(expr.int_val);
      const int span = HardRegModeSpan(r, expr.mode, gp64, fp64);
      for (int k = 0; k < span; ++k) {
        uses->Set(r + k);
      }
    }
    return;
  }
  for (const RtlExpr& c : expr.children) {
    CollectHardRegReads(c, gp64, fp64, uses);
  }
}

void CollectHardRegDefsAndUses(const RtlExpr& pat, bool gp64, bool fp64, MipsHardRegSet* defs,
                               MipsHardRegSet* uses) {
  if (pat.op == RtlOp::kSequence) {
    for (const RtlExpr& sub : pat.children) {
      if (!sub.children.empty()) {
        CollectHardRegDefsAndUses(sub.children[0], gp64, fp64, defs, uses);
      }
    }
    return;
  }
  auto handle_dest = [&](const RtlExpr& dst) {
    if (dst.IsReg()) {
      const int r = dst.RegNo();
      if (r >= 0 && r < 68) {
        const int span = HardRegModeSpan(r, dst.mode, gp64, fp64);
        for (int k = 0; k < span; ++k) {
          defs->Set(r + k);
        }
      }
    } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg()) {
      const int base = dst.children[0].RegNo();
      if (base >= 0 && base < 68) {
        const int r = base + static_cast<int>(dst.int_val);
        const int span = HardRegModeSpan(r, dst.mode, gp64, fp64);
        for (int k = 0; k < span; ++k) {
          defs->Set(r + k);
        }
      }
    } else if (dst.IsMem() && !dst.children.empty()) {
      CollectHardRegReads(dst.children[0], gp64, fp64, uses);
    } else {
      CollectHardRegReads(dst, gp64, fp64, uses);
    }
  };
  auto visit_stmt = [&](const RtlExpr& stmt) {
    if (stmt.IsSet()) {
      if (stmt.SetDest().op != RtlOp::kPc) {
        handle_dest(stmt.SetDest());
      }
      CollectHardRegReads(stmt.SetSrc(), gp64, fp64, uses);
    } else if (stmt.op == RtlOp::kClobber && !stmt.children.empty()) {
      handle_dest(stmt.children[0]);
    } else {
      CollectHardRegReads(stmt, gp64, fp64, uses);
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      visit_stmt(elem);
    }
  } else {
    visit_stmt(pat);
  }
}

void ApplyCallClobbersAndUses(const RtlPatternAndNotes& call_pn, bool gp64, bool fp64,
                              MipsHardRegSet* defs, MipsHardRegSet* uses,
                              bool include_call_arg_uses) {
  // `config/mips/mips.h:1203-1210` (`CALL_USED_REGISTERS`).
  static constexpr int kCallClobbered[] = {
      1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 24, 25, 31, 32, 33, 34,
      35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 65, 66, 67,
  };
  for (int r : kCallClobbered) {
    defs->Set(r);
  }
  if (!include_call_arg_uses) {
    return;
  }
  if (call_pn.has_explicit_call_fusage) {
    for (const RtlExpr& fu : call_pn.call_fusage) {
      CollectHardRegReads(fu, gp64, fp64, uses);
    }
    return;
  }
  for (int r : {4, 5, 6, 7, 44, 45, 46, 47}) {
    uses->Set(r);
  }
}

std::vector<MipsHardRegSet> ComputeHardRegLiveIn(const RtlControlFlowGraph& cfg, bool gp64,
                                                 bool fp64) {
  const size_t n = cfg.rpo_blocks.size();
  std::vector<MipsHardRegSet> live_in(n);
  if (n == 0) {
    return live_in;
  }
  std::vector<std::vector<RtlPatternAndNotes>> parsed_blocks(n);
  MipsHardRegSet integrated_ret_regs;
  for (size_t b = 0; b < n; ++b) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[b];
    parsed_blocks[b].reserve(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      if (blk.rtl_insns.size() == blk.instructions.size() &&
          blk.rtl_insns[i].has_explicit_call_fusage && !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = blk.rtl_insns[i].call_fusage;
      }
      parsed->pattern.Visit([&](const RtlExpr& node) {
        if (node.IsHardReg() && node.flags.integrated && node.RegNo() >= 0 && node.RegNo() < 68) {
          const int span = HardRegModeSpan(node.RegNo(), node.mode, gp64, fp64);
          for (int k = 0; k < span; ++k) {
            integrated_ret_regs.Set(node.RegNo() + k);
          }
        }
      });
      parsed_blocks[b].push_back(std::move(*parsed));
    }
  }

  MipsHardRegSet exit_seed;
  exit_seed.Set(29);
  exit_seed.Set(31);
  if (cfg.sideband.frame_pointer_needed) {
    exit_seed.Set(30);
  }
  exit_seed.Or(integrated_ret_regs);
  if (cfg.sideband.epilogue_delay_pattern.has_value() &&
      !cfg.sideband.epilogue_delay_pattern->empty()) {
    auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
    if (ep.ok()) {
      MipsHardRegSet ep_defs;
      MipsHardRegSet ep_uses;
      CollectHardRegDefsAndUses(ep->pattern, gp64, fp64, &ep_defs, &ep_uses);
      exit_seed.AndNot(ep_defs);
      exit_seed.Or(ep_uses);
    }
  }
  exit_seed.Clear(0);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = n; b-- > 0;) {
      const RtlBasicBlock& blk = cfg.rpo_blocks[b];
      MipsHardRegSet cur;
      if (blk.successors.empty()) {
        cur = exit_seed;
      } else {
        for (size_t succ : blk.successors) {
          if (succ < n) {
            cur.Or(live_in[succ]);
          }
        }
        for (size_t succ : blk.addr_vec_targets) {
          if (succ < n) {
            cur.Or(live_in[succ]);
          }
        }
      }
      for (size_t i = parsed_blocks[b].size(); i-- > 0;) {
        const RtlPatternAndNotes& pn = parsed_blocks[b][i];
        const RtlExpr& pat = pn.pattern;
        if (pat.op == RtlOp::kNote) {
          continue;
        }
        if (pat.op == RtlOp::kSequence && pat.children.size() >= 2 &&
            !pat.children[0].children.empty() && !pat.children[1].children.empty()) {
          MipsHardRegSet head_defs;
          MipsHardRegSet head_uses;
          CollectHardRegDefsAndUses(pat.children[0].children[0], gp64, fp64, &head_defs,
                                    &head_uses);
          const bool is_call_seq = pat.children[0].op == RtlOp::kCallInsn ||
                                   ContainsCallInExpr(pat.children[0].children[0]);
          if (is_call_seq) {
            auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(pat.children[0]);
            if (sub0.ok() && sub0->has_explicit_call_fusage) {
              ApplyCallClobbersAndUses(*sub0, gp64, fp64, &head_defs, &head_uses);
            } else {
              ApplyCallClobbersAndUses(pn, gp64, fp64, &head_defs, &head_uses);
            }
          }
          MipsHardRegSet slot_defs;
          MipsHardRegSet slot_uses;
          CollectHardRegDefsAndUses(pat.children[1].children[0], gp64, fp64, &slot_defs,
                                    &slot_uses);
          if (is_call_seq) {
            cur.AndNot(head_defs);
            cur.Or(head_uses);
            cur.AndNot(slot_defs);
            cur.Or(slot_uses);
          } else {
            cur.AndNot(slot_defs);
            cur.Or(slot_uses);
            cur.AndNot(head_defs);
            cur.Or(head_uses);
          }
          cur.Clear(0);
          continue;
        }
        MipsHardRegSet defs;
        MipsHardRegSet uses;
        CollectHardRegDefsAndUses(pat, gp64, fp64, &defs, &uses);
        if (ContainsCallInExpr(pat)) {
          ApplyCallClobbersAndUses(pn, gp64, fp64, &defs, &uses);
        }
        cur.AndNot(defs);
        cur.Or(uses);
        cur.Clear(0);
      }
      if (cur != live_in[b]) {
        live_in[b] = cur;
        changed = true;
      }
    }
  }
  return live_in;
}

bool ParseMovstrsiInsn(const RtlExpr& pat, ParsedMovstrsiInsn* out) {
  if (pat.op != RtlOp::kParallel || pat.children.size() != 8 || !pat.children[0].IsSet()) {
    return false;
  }
  const RtlExpr& dst_mem = pat.children[0].SetDest();
  const RtlExpr& src_mem = pat.children[0].SetSrc();
  if (!dst_mem.IsMem() || dst_mem.mode != RtlMachineMode::kBlk || dst_mem.children.size() != 1 ||
      !src_mem.IsMem() || src_mem.mode != RtlMachineMode::kBlk || src_mem.children.size() != 1) {
    return false;
  }
  ParsedMovstrsiInsn info;
  if (!ExtractBasePlusConstAddr(dst_mem.children[0], &info.dst_base, &info.dst_off) ||
      !ExtractBasePlusConstAddr(src_mem.children[0], &info.src_base, &info.src_off)) {
    return false;
  }
  for (size_t k = 0; k < 4; ++k) {
    const RtlExpr& clob = pat.children[1 + k];
    if (clob.op != RtlOp::kClobber || clob.children.size() != 1 || !clob.children[0].IsReg()) {
      return false;
    }
    info.clobbers[k] = clob.children[0].RegNo();
  }
  for (size_t k = 5; k <= 7; ++k) {
    if (pat.children[k].op != RtlOp::kUse || pat.children[k].children.size() != 1 ||
        !pat.children[k].children[0].IsConstInt()) {
      return false;
    }
  }
  info.bytes = pat.children[5].children[0].int_val;
  info.align = pat.children[6].children[0].int_val;
  info.kind = pat.children[7].children[0].int_val;
  *out = info;
  return true;
}

bool TryCoalesceMultiBatchMovstrsiInBlock(std::vector<RtlPatternAndNotes>* insns, bool gp64,
                                          bool mark_newly_folded_sentinel) {
  bool changed = false;
  // `config/mips/mips.c:2217, 2361`: `MAX_MOVE_BYTES = 4 * UNITS_PER_WORD`.
  const int64_t max_move_bytes = 4 * (gp64 ? 8 : 4);
  size_t i = 0;
  while (i + 1 < insns->size()) {
    ParsedMovstrsiInsn a;
    ParsedMovstrsiInsn b;
    if (ParseMovstrsiInsn((*insns)[i].pattern, &a) &&
        ParseMovstrsiInsn((*insns)[i + 1].pattern, &b) && a.kind == 0 && b.kind == 0 &&
        a.align == b.align && a.bytes == max_move_bytes && b.bytes > 0 &&
        a.bytes + b.bytes <= 2 * max_move_bytes && a.dst_base == b.dst_base &&
        a.src_base == b.src_base && b.dst_off == a.dst_off + a.bytes &&
        b.src_off == a.src_off + a.bytes) {
      RtlExpr merged = (*insns)[i].pattern;
      merged.children[5] = RtlExpr::MakeUse(RtlExpr::MakeConstInt(a.bytes + b.bytes));
      (*insns)[i] = MakeFoldedInsn(std::move(merged), mark_newly_folded_sentinel);
      insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1));
      changed = true;
      continue;
    }
    ++i;
  }
  return changed;
}

bool TryRecombineAdjacentSplitMovstrsiInBlock(std::vector<RtlPatternAndNotes>* insns,
                                              bool mark_newly_folded_sentinel, bool gp64) {
  bool changed = false;
  for (size_t i = 0; i + 1 < insns->size(); ++i) {
    const RtlExpr& p0 = (*insns)[i].pattern;
    if (!IsMovstrsiSplitPart(p0, 1)) {
      continue;
    }
    ParsedMovstrsiInsn first;
    const bool parsed_first = ParseMovstrsiInsn(p0, &first);
    for (size_t j = i + 1; j < insns->size(); ++j) {
      const RtlExpr& pj = (*insns)[j].pattern;
      bool matched = false;
      if (IsMovstrsiSplitPart(pj, 2)) {
        bool same_operands = true;
        for (size_t k = 0; k < 7; ++k) {
          if (p0.children[k] != pj.children[k]) {
            same_operands = false;
            break;
          }
        }
        if (same_operands) {
          matched = true;
        } else if (parsed_first) {
          ParsedMovstrsiInsn second;
          if (ParseMovstrsiInsn(pj, &second) && second.dst_base == first.dst_base &&
              second.align == first.align) {
            if (second.dst_off == first.dst_off && second.bytes == first.bytes) {
              matched = true;
            } else if (first.align == 1 && first.bytes >= 4 && (first.bytes % 4) == 0 &&
                       (first.bytes / 4) <= 4 && second.bytes == 4 &&
                       second.dst_off == first.dst_off + first.bytes - 4 &&
                       second.clobbers[0] == first.clobbers[(first.bytes / 4) - 1]) {
              matched = true;
            }
          }
        }
      } else if (parsed_first && first.align == 1 && (first.bytes % 4) != 0) {
        const int64_t used_slots = (first.bytes / 4) + (first.bytes % 4);
        int sk_reg = -1;
        int sk_base = -1;
        int64_t sk_off = -1;
        int sk_w = 0;
        if (used_slots >= 1 && used_slots <= 4 &&
            MatchMovstrStore(pj, &sk_reg, &sk_base, &sk_off, &sk_w) && sk_w == 1 &&
            sk_base == first.dst_base && sk_off == first.dst_off + first.bytes - 1 &&
            sk_reg == first.clobbers[used_slots - 1]) {
          matched = true;
        }
      }

      if (matched) {
        RtlExpr merged = p0;
        merged.children[7].children[0].int_val = 0;
        (*insns)[i] = MakeFoldedInsn(std::move(merged), mark_newly_folded_sentinel);
        insns->erase(insns->begin() + static_cast<ptrdiff_t>(j));
        changed = true;
        break;
      }

      if (ExprHasMem(pj) || pj.op == RtlOp::kSequence || IsIdiomControlTopExpr(pj) ||
          ContainsCallInExpr(pj) || ExprHasVolatileOrAsm(pj)) {
        break;
      }
      if (parsed_first) {
        MipsHardRegSet defs_j;
        MipsHardRegSet uses_j;
        CollectHardRegDefsAndUses(pj, gp64, /*fp64=*/false, &defs_j, &uses_j);
        if (defs_j.Test(first.dst_base) || defs_j.Test(first.src_base)) {
          break;
        }
      } else {
        break;
      }
    }
  }
  if (TryCoalesceMultiBatchMovstrsiInBlock(insns, gp64, mark_newly_folded_sentinel)) {
    changed = true;
  }
  return changed;
}

void RestoreNoConflictExtendClobbers(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                     bool runs_sched2) {
  RestoreNoConflictExtendClobbersImpl(cfg, gp64, fp64, runs_sched2, std::nullopt);
}

void RestoreNoConflictExtendClobbersWithTailCheck(
    RtlControlFlowGraph* cfg, bool gp64, bool fp64, bool runs_sched2,
    TailDelaySlotDiConsumerFn tail_di_consumer_check) {
  RestoreNoConflictExtendClobbersImpl(cfg, gp64, fp64, runs_sched2, tail_di_consumer_check);
}

}  // namespace rom_nom_nom::lifter
