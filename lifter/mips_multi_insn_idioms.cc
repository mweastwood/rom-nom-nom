#include "lifter/mips_multi_insn_idioms.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_idiom_utils.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_cfg_utils.h"

namespace rom_nom_nom::lifter {
namespace {

bool IsReturnParallelTop(const RtlExpr& expr) {
  return expr.op == RtlOp::kParallel && !expr.children.empty() &&
         expr.children[0].op == RtlOp::kReturn;
}

const RtlExpr* GetSeqOrCondJumpIfThenElse(const RtlExpr& expr) {
  if (expr.op == RtlOp::kSequence && !expr.children.empty() &&
      expr.children[0].op == RtlOp::kJumpInsn && !expr.children[0].children.empty()) {
    return GetCondJumpIfThenElse(expr.children[0].children[0]);
  }
  return GetCondJumpIfThenElse(expr);
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

std::vector<MipsHardRegSet> ComputeBlockHardRegLiveAfter(
    const RtlControlFlowGraph& cfg, size_t b, const std::vector<RtlPatternAndNotes>& insns,
    bool gp64, bool fp64,
    const std::optional<RtlPatternAndNotes>* epilogue_delay_override = nullptr,
    bool compute_live_in_when_sideband_empty = false,
    bool include_unannotated_call_arg_uses = true) {
  MipsHardRegSet cur_live;
  const auto& blk = cfg.rpo_blocks[b];
  if (blk.successors.empty()) {
    cur_live.Set(29);
    cur_live.Set(31);
    if (cfg.sideband.frame_pointer_needed) {
      cur_live.Set(30);
    }
    if (epilogue_delay_override != nullptr && epilogue_delay_override->has_value()) {
      MipsHardRegSet ep_defs;
      MipsHardRegSet ep_uses;
      CollectHardRegDefsAndUses((*epilogue_delay_override)->pattern, gp64, fp64, &ep_defs,
                                &ep_uses);
      cur_live.AndNot(ep_defs);
      cur_live.Or(ep_uses);
    } else if (cfg.sideband.epilogue_delay_pattern.has_value() &&
               !cfg.sideband.epilogue_delay_pattern->empty()) {
      auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
      if (ep.ok()) {
        MipsHardRegSet ep_defs;
        MipsHardRegSet ep_uses;
        CollectHardRegDefsAndUses(ep->pattern, gp64, fp64, &ep_defs, &ep_uses);
        cur_live.AndNot(ep_defs);
        cur_live.Or(ep_uses);
      }
    }
  } else if (!cfg.sideband.basic_block_live_at_start.empty()) {
    for (size_t succ : blk.successors) {
      auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(succ));
      if (it != cfg.sideband.basic_block_live_at_start.end()) {
        for (int r : it->second) {
          cur_live.Set(r);
        }
      }
    }
  } else if (compute_live_in_when_sideband_empty) {
    const std::vector<std::vector<int>> computed_live_in = ComputePostReloadHardRegLiveIn(cfg);
    for (size_t succ : blk.successors) {
      if (succ < computed_live_in.size()) {
        for (int r : computed_live_in[succ]) {
          cur_live.Set(r);
        }
      }
    }
  }

  std::vector<MipsHardRegSet> live_after(insns.size());
  for (size_t idx = insns.size(); idx > 0; --idx) {
    const size_t i = idx - 1;
    live_after[i] = cur_live;
    const RtlExpr& pat = insns[i].pattern;
    if (pat.op == RtlOp::kNote) {
      continue;
    }
    if (pat.op == RtlOp::kSequence && pat.children.size() >= 2 &&
        !pat.children[0].children.empty() && !pat.children[1].children.empty()) {
      MipsHardRegSet head_defs;
      MipsHardRegSet head_uses;
      CollectHardRegDefsAndUses(pat.children[0].children[0], gp64, fp64, &head_defs, &head_uses);
      const bool is_call_seq =
          pat.children[0].op == RtlOp::kCallInsn || ContainsCallInExpr(pat.children[0].children[0]);
      if (is_call_seq) {
        auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(pat.children[0]);
        if (sub0.ok() && sub0->has_explicit_call_fusage) {
          ApplyCallClobbersAndUses(*sub0, gp64, fp64, &head_defs, &head_uses,
                                   include_unannotated_call_arg_uses);
        } else {
          ApplyCallClobbersAndUses(insns[i], gp64, fp64, &head_defs, &head_uses,
                                   include_unannotated_call_arg_uses);
        }
      }
      MipsHardRegSet slot_defs;
      MipsHardRegSet slot_uses;
      CollectHardRegDefsAndUses(pat.children[1].children[0], gp64, fp64, &slot_defs, &slot_uses);
      if (is_call_seq) {
        cur_live.AndNot(head_defs);
        cur_live.Or(head_uses);
        cur_live.AndNot(slot_defs);
        cur_live.Or(slot_uses);
      } else {
        cur_live.AndNot(slot_defs);
        cur_live.Or(slot_uses);
        cur_live.AndNot(head_defs);
        cur_live.Or(head_uses);
      }
      cur_live.Clear(0);
      continue;
    }
    MipsHardRegSet defs;
    MipsHardRegSet uses;
    CollectHardRegDefsAndUses(pat, gp64, fp64, &defs, &uses);
    if (ContainsCallInExpr(pat)) {
      ApplyCallClobbersAndUses(insns[i], gp64, fp64, &defs, &uses,
                               include_unannotated_call_arg_uses);
    }
    cur_live.AndNot(defs);
    cur_live.Or(uses);
    cur_live.Clear(0);
  }
  return live_after;
}

int PickDeadCallerSavedGpr(const MipsHardRegSet& live_after, const MipsHardRegSet& forbidden) {
  static constexpr int kCallerSavedGprs[] = {
      2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25,
  };
  for (int r : kCallerSavedGprs) {
    if (!live_after.Test(r) && !forbidden.Test(r)) {
      return r;
    }
  }
  return -1;
}

// Per `optabs.c:2485-2525` (`emit_no_conflict_block`) and `local-alloc.c:1317-1352`
// (`combine_regs`), when a live single-word GPR `src` later feeds an overlapping
// `extendsidi2` (`move dl, src; sra dh, src, 31` with `dh == src || dl == src`),
// `local-alloc` ties `src` and `(reg:DI dh)` into a single 2-word `DImode` qty
// spanning `src`'s entire lifetime, so both `dh` and `dl` are occupied while
// `src` is live and cannot be chosen as an unused `match_scratch` clobber.
void MarkForwardOverlapExtendsidi2PartnerRegsForbidden(absl::Span<const RtlPatternAndNotes> insns,
                                                       size_t start_idx,
                                                       const MipsHardRegSet& live_after_at_start,
                                                       MipsHardRegSet* forbidden) {
  if (forbidden == nullptr) {
    return;
  }
  MipsHardRegSet still_live = live_after_at_start;
  for (size_t k = start_idx + 1; k < insns.size(); ++k) {
    const RtlExpr& pat = insns[k].pattern;
    int dl = -1;
    int src = -1;
    if (pat.IsSet() && IsPlainSiGpr(pat.SetDest(), &dl) && IsPlainSiGpr(pat.SetSrc(), &src) &&
        still_live.Test(src) && (dl % 2) == 1 && IsValidGp32DiPair(dl - 1)) {
      const int dh = dl - 1;
      if (dh == src || dl == src) {
        for (size_t m = k + 1; m < insns.size(); ++m) {
          const RtlExpr& mpat = insns[m].pattern;
          if (ContainsCallInExpr(mpat) || mpat.op == RtlOp::kSequence) {
            break;
          }
          int m_dst = -1;
          int m_src = -1;
          if (mpat.IsSet() && IsPlainSiGpr(mpat.SetDest(), &m_dst) && m_dst == dh &&
              mpat.SetSrc().op == RtlOp::kAshiftrt && mpat.SetSrc().children.size() == 2 &&
              IsPlainSiGpr(mpat.SetSrc().children[0], &m_src) && m_src == src &&
              mpat.SetSrc().children[1].IsConstInt() && mpat.SetSrc().children[1].int_val == 31) {
            forbidden->Set(dh);
            forbidden->Set(dl);
            break;
          }
          MipsHardRegSet m_defs;
          MipsHardRegSet m_uses;
          CollectHardRegDefsAndUses(mpat, /*gp64=*/false, /*fp64=*/false, &m_defs, &m_uses);
          if (m_defs.Test(src) || m_defs.Test(dh) || m_defs.Test(dl)) {
            break;
          }
        }
      }
    }
    MipsHardRegSet defs;
    MipsHardRegSet uses;
    CollectHardRegDefsAndUses(pat, /*gp64=*/false, /*fp64=*/false, &defs, &uses);
    if (ContainsCallInExpr(pat)) {
      ApplyCallClobbersAndUses(insns[k], /*gp64=*/false, /*fp64=*/false, &defs, &uses);
    }
    still_live.AndNot(defs);
  }
}

RtlExpr MakeDiConstDouble(int64_t lo_word, int64_t hi_word) {
  RtlExpr cd;
  cd.op = RtlOp::kConstDouble;
  cd.mode = RtlMachineMode::kNone;
  cd.children = {RtlExpr::MakeConstInt(0)};
  cd.int_val = lo_word;
  cd.int_val_high = hi_word;
  return cd;
}

RtlPatternAndNotes MakeFoldedInsn(RtlExpr pattern, bool mark_newly_folded_sentinel) {
  RtlPatternAndNotes pn;
  pn.pattern = std::move(pattern);
  if (mark_newly_folded_sentinel) {
    pn.reg_notes.push_back(RtlRegNote{.kind = RtlMachineMode::kVoid, .datum = RtlExpr::MakeNil()});
  }
  return pn;
}

bool IsBarrierForFolding(const RtlExpr& pat) {
  return pat.op == RtlOp::kNote || pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber ||
         pat.op == RtlOp::kSequence || IsAnyJumpOrReturnTopExpr(pat) || ContainsCallInExpr(pat) ||
         ExprHasVolatileOrAsm(pat);
}

bool TryMatchAddrPlusFour(const RtlExpr& addr0, const RtlExpr& addr4, int* out_base_reg,
                          int64_t* out_offset) {
  if (out_base_reg != nullptr) {
    *out_base_reg = -1;
  }
  if (out_offset != nullptr) {
    *out_offset = 0;
  }
  int b0 = -1;
  if (IsPlainSiGpr(addr0, &b0)) {
    int b4 = -1;
    if (addr4.op == RtlOp::kPlus && addr4.mode == RtlMachineMode::kSi &&
        addr4.children.size() == 2 && IsPlainSiGpr(addr4.children[0], &b4) && b4 == b0 &&
        addr4.children[1].IsConstInt() && addr4.children[1].int_val == 4) {
      if (out_base_reg != nullptr) {
        *out_base_reg = b0;
      }
      return true;
    }
    return false;
  }
  if (addr0.op == RtlOp::kPlus && addr0.mode == RtlMachineMode::kSi && addr0.children.size() == 2 &&
      IsPlainSiGpr(addr0.children[0], &b0) && addr0.children[1].IsConstInt()) {
    const int64_t off0 = addr0.children[1].int_val;
    int b4 = -1;
    if (off0 + 4 == 0 && IsPlainSiGpr(addr4, &b4) && b4 == b0) {
      if (out_base_reg != nullptr) {
        *out_base_reg = b0;
      }
      if (out_offset != nullptr) {
        *out_offset = off0;
      }
      return true;
    }
    if (addr4.op == RtlOp::kPlus && addr4.mode == RtlMachineMode::kSi &&
        addr4.children.size() == 2 && IsPlainSiGpr(addr4.children[0], &b4) && b4 == b0 &&
        addr4.children[1].IsConstInt() && addr4.children[1].int_val == off0 + 4) {
      if (out_base_reg != nullptr) {
        *out_base_reg = b0;
      }
      if (out_offset != nullptr) {
        *out_offset = off0;
      }
      return true;
    }
    return false;
  }
  if (addr0.op == RtlOp::kSymbolRef && addr0.mode == RtlMachineMode::kSi) {
    return addr4.op == RtlOp::kConst && addr4.mode == RtlMachineMode::kSi &&
           addr4.children.size() == 1 && addr4.children[0].op == RtlOp::kPlus &&
           addr4.children[0].mode == RtlMachineMode::kSi &&
           addr4.children[0].children.size() == 2 &&
           addr4.children[0].children[0].op == RtlOp::kSymbolRef &&
           addr4.children[0].children[0].str_val == addr0.str_val &&
           addr4.children[0].children[1].IsConstInt() && addr4.children[0].children[1].int_val == 4;
  }
  if (addr0.op == RtlOp::kConst && addr0.mode == RtlMachineMode::kSi &&
      addr0.children.size() == 1 && addr0.children[0].op == RtlOp::kPlus &&
      addr0.children[0].mode == RtlMachineMode::kSi && addr0.children[0].children.size() == 2 &&
      addr0.children[0].children[0].op == RtlOp::kSymbolRef &&
      addr0.children[0].children[1].IsConstInt()) {
    const std::string& sym = addr0.children[0].children[0].str_val;
    const int64_t off0 = addr0.children[0].children[1].int_val;
    if (off0 + 4 == 0 && addr4.op == RtlOp::kSymbolRef && addr4.str_val == sym) {
      return true;
    }
    if (addr4.op == RtlOp::kConst && addr4.mode == RtlMachineMode::kSi &&
        addr4.children.size() == 1 && addr4.children[0].op == RtlOp::kPlus &&
        addr4.children[0].children.size() == 2 &&
        addr4.children[0].children[0].op == RtlOp::kSymbolRef &&
        addr4.children[0].children[0].str_val == sym &&
        addr4.children[0].children[1].IsConstInt() &&
        addr4.children[0].children[1].int_val == off0 + 4) {
      return true;
    }
  }
  return false;
}

bool MatchMovstrLoad(const RtlExpr& pat, int* out_reg, int* out_base, int64_t* out_off,
                     int* out_width) {
  if (!pat.IsSet() || !IsPlainSiGpr(pat.SetDest(), out_reg) || *out_reg < 1 || *out_reg > 25) {
    return false;
  }
  const RtlExpr& src = pat.SetSrc();
  if (src.IsMem() && src.mode == RtlMachineMode::kSi && !src.flags.volatil &&
      !src.flags.in_struct && src.children.size() == 1 &&
      ExtractBasePlusConstAddr(src.children[0], out_base, out_off)) {
    *out_width = 4;
    return *out_reg != *out_base;
  }
  if (src.op == RtlOp::kSignExtend && src.mode == RtlMachineMode::kSi && src.children.size() == 1 &&
      src.children[0].IsMem() && !src.children[0].flags.volatil &&
      !src.children[0].flags.in_struct && src.children[0].children.size() == 1 &&
      ExtractBasePlusConstAddr(src.children[0].children[0], out_base, out_off)) {
    if (src.children[0].mode == RtlMachineMode::kHi) {
      *out_width = 2;
      return *out_reg != *out_base;
    }
    if (src.children[0].mode == RtlMachineMode::kQi) {
      *out_width = 1;
      return *out_reg != *out_base;
    }
  }
  return false;
}

RtlExpr MakeMovstrsiAddr(int base_reg, int64_t base_off) {
  RtlExpr base = RtlExpr::MakeReg(RtlMachineMode::kSi, base_reg);
  if (base_reg == 30) {
    // Mirrors `config/mips/mips.h` (`REGISTER_NAMES`): hard frame pointer reg 30
    // prints as `$fp` in post-reload RTL (`print-rtl.c:218`).
    base.str_val = "$fp";
  }
  if (base_off == 0) {
    return base;
  }
  return RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi, std::move(base),
                             RtlExpr::MakeConstInt(base_off));
}

RtlExpr MakeMovstrsiParallel(int dst_base, int64_t dst_base_off, int src_base, int64_t src_base_off,
                             const int temps[4], int64_t total_bytes, int align, int split_kind) {
  return RtlExpr::MakeParallel({
      RtlExpr::MakeSet(
          RtlExpr::MakeMem(RtlMachineMode::kBlk, MakeMovstrsiAddr(dst_base, dst_base_off)),
          RtlExpr::MakeMem(RtlMachineMode::kBlk, MakeMovstrsiAddr(src_base, src_base_off))),
      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, temps[0])),
      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, temps[1])),
      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, temps[2])),
      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, temps[3])),
      RtlExpr::MakeUse(RtlExpr::MakeConstInt(total_bytes)),
      RtlExpr::MakeUse(RtlExpr::MakeConstInt(align)),
      RtlExpr::MakeUse(RtlExpr::MakeConstInt(split_kind)),
  });
}

bool IsUnfilledUncondJumpOrReturn(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn || IsReturnParallelTop(pat)) {
    return true;
  }
  return pat.IsSet() && pat.SetDest().op == RtlOp::kPc && pat.SetSrc().op == RtlOp::kLabelRef;
}

// In `!TARGET_64BIT`, `convert_move` (`expr.c:936-984`) expands signed
// `QImode`/`HImode` -> `DImode` extension via `extendqisi2`/`extendhisi2`
// (`sll tmp, src, 24|16`) followed by two `SImode` arithmetic right shifts
// (`sra dL, tmp, 24|16` and `sra dH, tmp, 31`), leaving `tmp` as a 32-bit
// `SImode` temporary rather than a `DImode` register pair `(tmp, tmp + 1)`.
bool IsNarrowSignExtendShiftTemp(const std::vector<RtlPatternAndNotes>& insns, size_t idx, int s0,
                                 int64_t k_lo, int dst_hi, const MipsHardRegSet& live_after_pair) {
  if ((k_lo != 16 && k_lo != 24) || (s0 != dst_hi && live_after_pair.Test(s0))) {
    return false;
  }
  for (size_t rev = idx; rev > 0; --rev) {
    const RtlExpr& prev = insns[rev - 1].pattern;
    if (prev.op == RtlOp::kNote) {
      continue;
    }
    if (IsAnyJumpOrReturnTopExpr(prev) || ContainsCallInExpr(prev) || prev.op == RtlOp::kSequence) {
      return false;
    }
    MipsHardRegSet defs;
    MipsHardRegSet uses;
    CollectHardRegDefsAndUses(prev, /*gp64=*/false, /*fp64=*/false, &defs, &uses);
    if (defs.Test(s0)) {
      if (defs.Test(s0 + 1) || !prev.IsSet()) {
        return false;
      }
      int dst = -1;
      int shift_src = -1;
      return IsPlainSiGpr(prev.SetDest(), &dst) && dst == s0 &&
             prev.SetSrc().op == RtlOp::kAshift && prev.SetSrc().children.size() == 2 &&
             IsPlainSiGpr(prev.SetSrc().children[0], &shift_src) &&
             prev.SetSrc().children[1].IsConstInt() && prev.SetSrc().children[1].int_val == k_lo;
    }
  }
  return false;
}

}  // namespace

void CollectCompareDiamondDiPairs(const std::vector<std::vector<RtlPatternAndNotes>>& blocks,
                                  absl::flat_hash_set<int>* out_di_even_regs) {
  auto block_has_pair_cmp = [](const std::vector<RtlPatternAndNotes>& insns, int r_a, int r_b) {
    bool found = false;
    for (const auto& pn : insns) {
      pn.pattern.Visit([&](const RtlExpr& n) {
        if (n.IsComparison() && n.children.size() == 2) {
          int x = -1;
          int y = -1;
          if (IsPlainSiGpr(n.children[0], &x) && IsPlainSiGpr(n.children[1], &y) &&
              ((x == r_a && y == r_b) || (x == r_b && y == r_a))) {
            found = true;
          }
        }
      });
    }
    return found;
  };
  for (size_t b = 0; b + 1 < blocks.size(); ++b) {
    if (blocks[b].empty()) {
      continue;
    }
    const RtlExpr* ite = GetSeqOrCondJumpIfThenElse(blocks[b].back().pattern);
    if (ite == nullptr ||
        (ite->children[0].op != RtlOp::kNe && ite->children[0].op != RtlOp::kEq) ||
        ite->children[0].children.size() != 2) {
      continue;
    }
    int p_hi = -1;
    int q_hi = -1;
    if (!IsPlainSiGpr(ite->children[0].children[0], &p_hi) ||
        !IsPlainSiGpr(ite->children[0].children[1], &q_hi) || p_hi == q_hi ||
        !IsValidGp32DiPair(p_hi) || !IsValidGp32DiPair(q_hi)) {
      continue;
    }
    // Require ordered 64-bit compare diamond (`lt`/`ltu` on `(p_hi, q_hi)` in `b - 1`
    // and `ltu` on `(p_hi + 1, q_hi + 1)` in `b` or `b + 1`) so plain 32-bit equality
    // chains are never mistaken for `DImode`.
    const bool prev_has_hi_cmp = b > 0 && block_has_pair_cmp(blocks[b - 1], p_hi, q_hi);
    const bool lo_cmp_present = block_has_pair_cmp(blocks[b + 1], p_hi + 1, q_hi + 1) ||
                                block_has_pair_cmp(blocks[b], p_hi + 1, q_hi + 1);
    if (prev_has_hi_cmp && lo_cmp_present) {
      out_di_even_regs->insert(p_hi);
      out_di_even_regs->insert(q_hi);
    }
  }
}

void CollectExistingDiEvenRegs(const std::vector<std::vector<RtlPatternAndNotes>>& blocks,
                               absl::flat_hash_set<int>* out_di_even_regs, bool* out_has_any_di) {
  for (const auto& blk : blocks) {
    for (const auto& pn : blk) {
      pn.pattern.Visit([&](const RtlExpr& n) {
        if (n.mode == RtlMachineMode::kDi) {
          *out_has_any_di = true;
          if (n.IsReg() && IsValidGp32DiPair(n.RegNo())) {
            out_di_even_regs->insert(n.RegNo());
          }
        }
      });
    }
  }
}

bool TryFoldGp32DiMultiInsnIdiomsInBlock(const RtlControlFlowGraph& cfg, size_t b,
                                         bool mark_newly_folded_sentinel,
                                         std::vector<RtlPatternAndNotes>* insns,
                                         bool compute_live_in_when_sideband_empty) {
  bool changed = false;
  auto recompute_live_after = [&]() {
    return ComputeBlockHardRegLiveAfter(cfg, b, *insns, /*gp64=*/false, /*fp64=*/false,
                                        /*epilogue_delay_override=*/nullptr,
                                        compute_live_in_when_sideband_empty);
  };
  // Per `mips.md` (`adddi3_internal_1`, `subdi3_internal`, `ashldi3_internal3`,
  // `ashrdi3_internal3`, `lshrdi3_internal3`, `adddi3_internal_2`), the carry/
  // shift temp `tmp` is a `match_scratch` already overwritten by the assembly
  // sequence and never live into an unannotated `CALL_INSN` unless explicitly
  // referenced in `call_fusage`.
  auto recompute_live_after_scratch = [&]() {
    return ComputeBlockHardRegLiveAfter(cfg, b, *insns, /*gp64=*/false, /*fp64=*/false,
                                        /*epilogue_delay_override=*/nullptr,
                                        compute_live_in_when_sideband_empty,
                                        /*include_unannotated_call_arg_uses=*/false);
  };
  std::vector<MipsHardRegSet> live_after = recompute_live_after();
  std::vector<MipsHardRegSet> live_after_scratch = recompute_live_after_scratch();
  auto refresh_liveness = [&]() {
    live_after = recompute_live_after();
    live_after_scratch = recompute_live_after_scratch();
  };

  for (size_t i = 0; i < insns->size(); ++i) {
    // 1. Widening 32x32->64 multiply (`mulsidi3_internal` / `umulsidi3_internal`),
    // supporting both `[mfhi; mflo]` and `[mflo; mfhi]`.
    const RtlExpr& pat_i = (*insns)[i].pattern;
    if (pat_i.op == RtlOp::kParallel && pat_i.children.size() == 3 && pat_i.children[0].IsSet() &&
        pat_i.children[0].SetDest().IsReg() &&
        pat_i.children[0].SetDest().mode == RtlMachineMode::kSi &&
        pat_i.children[0].SetDest().RegNo() == 64 && pat_i.children[1].op == RtlOp::kClobber &&
        !pat_i.children[1].children.empty() && pat_i.children[1].children[0].IsReg() &&
        pat_i.children[1].children[0].RegNo() == 65 && pat_i.children[2].op == RtlOp::kClobber &&
        !pat_i.children[2].children.empty() && pat_i.children[2].children[0].IsReg() &&
        pat_i.children[2].children[0].RegNo() == 66) {
      const RtlExpr& trunc = pat_i.children[0].SetSrc();
      if (trunc.op == RtlOp::kTruncate && trunc.children.size() == 1 &&
          trunc.children[0].op == RtlOp::kLshiftrt && trunc.children[0].children.size() == 2 &&
          trunc.children[0].children[0].op == RtlOp::kMult &&
          trunc.children[0].children[0].mode == RtlMachineMode::kDi &&
          trunc.children[0].children[1].IsConstInt() &&
          trunc.children[0].children[1].int_val == 32) {
        for (size_t j = i + 1; j + 1 < insns->size(); ++j) {
          const RtlExpr& p0 = (*insns)[j].pattern;
          const RtlExpr& p1 = (*insns)[j + 1].pattern;
          int r_hi = -1;
          int r_lo = -1;
          bool matched_mf = false;
          RtlFlags dst_flags;
          if (p0.IsSet() && p1.IsSet() && IsPlainSiGpr(p0.SetDest(), &r_hi) &&
              IsPlainSiGpr(p1.SetDest(), &r_lo) && p0.SetSrc().IsReg() && p1.SetSrc().IsReg()) {
            if (p0.SetSrc().RegNo() == 64 && p1.SetSrc().RegNo() == 65 && IsValidGp32DiPair(r_hi) &&
                r_lo == r_hi + 1) {
              matched_mf = true;
              dst_flags = p0.SetDest().flags;
            } else if (p0.SetSrc().RegNo() == 65 && p1.SetSrc().RegNo() == 64 &&
                       IsValidGp32DiPair(r_lo) && r_hi == r_lo + 1) {
              std::swap(r_hi, r_lo);
              matched_mf = true;
              dst_flags = p1.SetDest().flags;
            }
          }
          if (matched_mf && !live_after[j + 1].Test(64) && !live_after[j + 1].Test(65)) {
            RtlExpr mult_di = trunc.children[0].children[0];
            (*insns)[i] = MakeFoldedInsn(
                RtlExpr::MakeParallel({
                    RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, 64), std::move(mult_di)),
                    RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, 66)),
                }),
                mark_newly_folded_sentinel);
            (*insns)[j] = MakeFoldedInsn(
                RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, r_hi, dst_flags),
                                 RtlExpr::MakeReg(RtlMachineMode::kDi, 64)),
                mark_newly_folded_sentinel);
            insns->erase(insns->begin() + static_cast<ptrdiff_t>(j + 1));
            live_after = recompute_live_after();
            live_after_scratch = recompute_live_after_scratch();
            changed = true;
            break;
          }
          if (IsBarrierForFolding(p0) || p0.ContainsReg(64) || p0.ContainsReg(65)) {
            break;
          }
        }
      }
    }

    // 2a. 4-insn `adddi3_internal_1` (`addu dL, xL, yL; sltu tmp, dL, yL; addu dH, xH, yH;
    // addu dH, dH, tmp`).
    if (i + 3 < insns->size()) {
      const RtlExpr& p0 = (*insns)[i].pattern;
      const RtlExpr& p1 = (*insns)[i + 1].pattern;
      const RtlExpr& p2 = (*insns)[i + 2].pattern;
      const RtlExpr& p3 = (*insns)[i + 3].pattern;
      int d_lo = -1, x_lo = -1, y_lo = -1;
      int tmp = -1, cmp_lo = -1, carry_rhs = -1;
      int d_hi = -1, x_hi = -1, y_hi = -1;
      int d_hi2 = -1, sum_lhs = -1, sum_rhs = -1;
      if (p0.IsSet() && IsPlainSiGpr(p0.SetDest(), &d_lo) && p0.SetSrc().op == RtlOp::kPlus &&
          p0.SetSrc().children.size() == 2 && IsPlainSiGpr(p0.SetSrc().children[0], &x_lo) &&
          IsPlainSiGpr(p0.SetSrc().children[1], &y_lo) && p1.IsSet() &&
          IsPlainSiGpr(p1.SetDest(), &tmp) && p1.SetSrc().op == RtlOp::kLtu &&
          p1.SetSrc().children.size() == 2 && IsPlainSiGpr(p1.SetSrc().children[0], &cmp_lo) &&
          IsPlainSiGpr(p1.SetSrc().children[1], &carry_rhs) && p2.IsSet() &&
          IsPlainSiGpr(p2.SetDest(), &d_hi) && p2.SetSrc().op == RtlOp::kPlus &&
          p2.SetSrc().children.size() == 2 && IsPlainSiGpr(p2.SetSrc().children[0], &x_hi) &&
          IsPlainSiGpr(p2.SetSrc().children[1], &y_hi) && p3.IsSet() &&
          IsPlainSiGpr(p3.SetDest(), &d_hi2) && p3.SetSrc().op == RtlOp::kPlus &&
          p3.SetSrc().children.size() == 2 && IsPlainSiGpr(p3.SetSrc().children[0], &sum_lhs) &&
          IsPlainSiGpr(p3.SetSrc().children[1], &sum_rhs)) {
        const bool sum_matches =
            (sum_lhs == d_hi && sum_rhs == tmp) || (sum_lhs == tmp && sum_rhs == d_hi);
        int a_hi = -1;
        int b_hi = -1;
        if ((x_lo == x_hi + 1 && y_lo == y_hi + 1) || (x_lo == y_hi + 1 && y_lo == x_hi + 1)) {
          a_hi = x_hi;
          b_hi = y_hi;
        }
        if (a_hi != -1 && carry_rhs == a_hi + 1 && carry_rhs != b_hi + 1 && d_hi != b_hi) {
          std::swap(a_hi, b_hi);
        }
        if (IsValidGp32DiPair(d_hi) && d_lo == d_hi + 1 && d_hi2 == d_hi && cmp_lo == d_lo &&
            sum_matches && a_hi != -1 && IsValidGp32DiPair(a_hi) && IsValidGp32DiPair(b_hi) &&
            carry_rhs == b_hi + 1 && tmp != d_hi && tmp != d_lo &&
            !live_after_scratch[i + 3].Test(tmp)) {
          const RtlFlags dst_flags = p3.SetDest().flags;
          (*insns)[i] = MakeFoldedInsn(
              RtlExpr::MakeParallel({
                  RtlExpr::MakeSet(
                      RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                      RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kDi,
                                          RtlExpr::MakeReg(RtlMachineMode::kDi, a_hi),
                                          RtlExpr::MakeReg(RtlMachineMode::kDi, b_hi))),
                  RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, tmp)),
              }),
              mark_newly_folded_sentinel);
          insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1),
                       insns->begin() + static_cast<ptrdiff_t>(i + 4));
          refresh_liveness();
          changed = true;
          continue;
        }
      }

      // 2b. Self-double `adddi3_internal_1` (`srl tmp, dL, 31; sll dH, dH, 1; sll dL, dL, 1;
      // addu dH, dH, tmp`).
      if (p0.IsSet() && IsPlainSiGpr(p0.SetDest(), &tmp) && p0.SetSrc().op == RtlOp::kLshiftrt &&
          p0.SetSrc().children.size() == 2 && IsPlainSiGpr(p0.SetSrc().children[0], &d_lo) &&
          p0.SetSrc().children[1].IsConstInt() && p0.SetSrc().children[1].int_val == 31 &&
          p1.IsSet() && IsPlainSiGpr(p1.SetDest(), &d_hi) && p1.SetSrc().op == RtlOp::kAshift &&
          p1.SetSrc().children.size() == 2 && IsPlainSiGpr(p1.SetSrc().children[0], &x_hi) &&
          p1.SetSrc().children[1].IsConstInt() && p1.SetSrc().children[1].int_val == 1 &&
          p2.IsSet() && IsPlainSiGpr(p2.SetDest(), &y_lo) && p2.SetSrc().op == RtlOp::kAshift &&
          p2.SetSrc().children.size() == 2 && IsPlainSiGpr(p2.SetSrc().children[0], &x_lo) &&
          p2.SetSrc().children[1].IsConstInt() && p2.SetSrc().children[1].int_val == 1 &&
          p3.IsSet() && IsPlainSiGpr(p3.SetDest(), &d_hi2) && p3.SetSrc().op == RtlOp::kPlus &&
          p3.SetSrc().children.size() == 2 && IsPlainSiGpr(p3.SetSrc().children[0], &sum_lhs) &&
          IsPlainSiGpr(p3.SetSrc().children[1], &sum_rhs) && IsValidGp32DiPair(d_hi) &&
          d_lo == d_hi + 1 && x_hi == d_hi && y_lo == d_lo && x_lo == d_lo && d_hi2 == d_hi &&
          ((sum_lhs == d_hi && sum_rhs == tmp) || (sum_lhs == tmp && sum_rhs == d_hi)) &&
          tmp != d_hi && tmp != d_lo && !live_after_scratch[i + 3].Test(tmp)) {
        const RtlFlags dst_flags = p3.SetDest().flags;
        (*insns)[i] = MakeFoldedInsn(
            RtlExpr::MakeParallel({
                RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                 RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kDi,
                                                     RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi),
                                                     RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi))),
                RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, tmp)),
            }),
            mark_newly_folded_sentinel);
        insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1),
                     insns->begin() + static_cast<ptrdiff_t>(i + 4));
        refresh_liveness();
        changed = true;
        continue;
      }

      // 3. 4-insn `subdi3_internal` (`sltu tmp, aL, bL; subu dL, aL, bL; subu dH, aH, bH;
      // subu dH, dH, tmp`).
      if (p0.IsSet() && IsPlainSiGpr(p0.SetDest(), &tmp) && p0.SetSrc().op == RtlOp::kLtu &&
          p0.SetSrc().children.size() == 2 && IsPlainSiGpr(p0.SetSrc().children[0], &x_lo) &&
          IsPlainSiGpr(p0.SetSrc().children[1], &y_lo) && p1.IsSet() &&
          IsPlainSiGpr(p1.SetDest(), &d_lo) && p1.SetSrc().op == RtlOp::kMinus &&
          p1.SetSrc().children.size() == 2 && IsPlainSiGpr(p1.SetSrc().children[0], &cmp_lo) &&
          IsPlainSiGpr(p1.SetSrc().children[1], &carry_rhs) && p2.IsSet() &&
          IsPlainSiGpr(p2.SetDest(), &d_hi) && p2.SetSrc().op == RtlOp::kMinus &&
          p2.SetSrc().children.size() == 2 && IsPlainSiGpr(p2.SetSrc().children[0], &x_hi) &&
          IsPlainSiGpr(p2.SetSrc().children[1], &y_hi) && p3.IsSet() &&
          IsPlainSiGpr(p3.SetDest(), &d_hi2) && p3.SetSrc().op == RtlOp::kMinus &&
          p3.SetSrc().children.size() == 2 && IsPlainSiGpr(p3.SetSrc().children[0], &sum_lhs) &&
          IsPlainSiGpr(p3.SetSrc().children[1], &sum_rhs) && IsValidGp32DiPair(d_hi) &&
          d_lo == d_hi + 1 && IsValidGp32DiPair(x_hi) && x_lo == x_hi + 1 &&
          IsValidGp32DiPair(y_hi) && y_lo == y_hi + 1 && cmp_lo == x_lo && carry_rhs == y_lo &&
          d_hi2 == d_hi && sum_lhs == d_hi && sum_rhs == tmp && tmp != d_hi && tmp != d_lo &&
          !live_after_scratch[i + 3].Test(tmp)) {
        const RtlFlags dst_flags = p3.SetDest().flags;
        (*insns)[i] = MakeFoldedInsn(
            RtlExpr::MakeParallel({
                RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                 RtlExpr::MakeBinary(RtlOp::kMinus, RtlMachineMode::kDi,
                                                     RtlExpr::MakeReg(RtlMachineMode::kDi, x_hi),
                                                     RtlExpr::MakeReg(RtlMachineMode::kDi, y_hi))),
                RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, tmp)),
            }),
            mark_newly_folded_sentinel);
        insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1),
                     insns->begin() + static_cast<ptrdiff_t>(i + 4));
        refresh_liveness();
        changed = true;
        continue;
      }

      // 4a. 4-insn `ashldi3_internal3` (`1 <= k <= 31`: `sll dH, sH, k; srl tmp, sL, 32-k;
      // or dH, dH, tmp; sll dL, sL, k`).
      int s_hi = -1, s_lo = -1, s_lo2 = -1, d_hi_or = -1, or_lhs = -1, or_rhs = -1;
      if (p0.IsSet() && IsPlainSiGpr(p0.SetDest(), &d_hi) && p0.SetSrc().op == RtlOp::kAshift &&
          p0.SetSrc().children.size() == 2 && IsPlainSiGpr(p0.SetSrc().children[0], &s_hi) &&
          p0.SetSrc().children[1].IsConstInt() && p1.IsSet() && IsPlainSiGpr(p1.SetDest(), &tmp) &&
          p1.SetSrc().op == RtlOp::kLshiftrt && p1.SetSrc().children.size() == 2 &&
          IsPlainSiGpr(p1.SetSrc().children[0], &s_lo) && p1.SetSrc().children[1].IsConstInt() &&
          p2.IsSet() && IsPlainSiGpr(p2.SetDest(), &d_hi_or) && p2.SetSrc().op == RtlOp::kIor &&
          p2.SetSrc().children.size() == 2 && IsPlainSiGpr(p2.SetSrc().children[0], &or_lhs) &&
          IsPlainSiGpr(p2.SetSrc().children[1], &or_rhs) && p3.IsSet() &&
          IsPlainSiGpr(p3.SetDest(), &d_lo) && p3.SetSrc().op == RtlOp::kAshift &&
          p3.SetSrc().children.size() == 2 && IsPlainSiGpr(p3.SetSrc().children[0], &s_lo2) &&
          p3.SetSrc().children[1].IsConstInt()) {
        const int64_t k = p0.SetSrc().children[1].int_val;
        if (k >= 1 && k <= 31 && p1.SetSrc().children[1].int_val == 32 - k &&
            p3.SetSrc().children[1].int_val == k && IsValidGp32DiPair(d_hi) && d_lo == d_hi + 1 &&
            IsValidGp32DiPair(s_hi) && s_lo == s_hi + 1 && s_lo2 == s_lo && d_hi_or == d_hi &&
            ((or_lhs == d_hi && or_rhs == tmp) || (or_lhs == tmp && or_rhs == d_hi)) &&
            tmp != d_hi && tmp != d_lo && !live_after_scratch[i + 3].Test(tmp)) {
          const RtlFlags dst_flags = p2.SetDest().flags;
          (*insns)[i] = MakeFoldedInsn(
              RtlExpr::MakeParallel({
                  RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                   RtlExpr::MakeBinary(RtlOp::kAshift, RtlMachineMode::kDi,
                                                       RtlExpr::MakeReg(RtlMachineMode::kDi, s_hi),
                                                       RtlExpr::MakeConstInt(k))),
                  RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, tmp)),
              }),
              mark_newly_folded_sentinel);
          insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1),
                       insns->begin() + static_cast<ptrdiff_t>(i + 4));
          refresh_liveness();
          changed = true;
          continue;
        }
      }

      // 5a. 4-insn `ashrdi3_internal3` / `lshrdi3_internal3` (`1 <= k <= 31`: `srl dL, sL, k;
      // sll tmp, sH, 32-k; or dL, dL, tmp; sra|srl dH, sH, k`).
      int d_lo_or = -1, s_hi2 = -1;
      if (p0.IsSet() && IsPlainSiGpr(p0.SetDest(), &d_lo) && p0.SetSrc().op == RtlOp::kLshiftrt &&
          p0.SetSrc().children.size() == 2 && IsPlainSiGpr(p0.SetSrc().children[0], &s_lo) &&
          p0.SetSrc().children[1].IsConstInt() && p1.IsSet() && IsPlainSiGpr(p1.SetDest(), &tmp) &&
          p1.SetSrc().op == RtlOp::kAshift && p1.SetSrc().children.size() == 2 &&
          IsPlainSiGpr(p1.SetSrc().children[0], &s_hi) && p1.SetSrc().children[1].IsConstInt() &&
          p2.IsSet() && IsPlainSiGpr(p2.SetDest(), &d_lo_or) && p2.SetSrc().op == RtlOp::kIor &&
          p2.SetSrc().children.size() == 2 && IsPlainSiGpr(p2.SetSrc().children[0], &or_lhs) &&
          IsPlainSiGpr(p2.SetSrc().children[1], &or_rhs) && p3.IsSet() &&
          IsPlainSiGpr(p3.SetDest(), &d_hi) &&
          (p3.SetSrc().op == RtlOp::kAshiftrt || p3.SetSrc().op == RtlOp::kLshiftrt) &&
          p3.SetSrc().children.size() == 2 && IsPlainSiGpr(p3.SetSrc().children[0], &s_hi2) &&
          p3.SetSrc().children[1].IsConstInt()) {
        const int64_t k = p0.SetSrc().children[1].int_val;
        const RtlOp shr_op = p3.SetSrc().op;
        if (k >= 1 && k <= 31 && p1.SetSrc().children[1].int_val == 32 - k &&
            p3.SetSrc().children[1].int_val == k && IsValidGp32DiPair(d_hi) && d_lo == d_hi + 1 &&
            IsValidGp32DiPair(s_hi) && s_lo == s_hi + 1 && s_hi2 == s_hi && d_lo_or == d_lo &&
            ((or_lhs == d_lo && or_rhs == tmp) || (or_lhs == tmp && or_rhs == d_lo)) &&
            tmp != d_hi && tmp != d_lo && !live_after_scratch[i + 3].Test(tmp)) {
          const RtlFlags dst_flags = p3.SetDest().flags;
          (*insns)[i] = MakeFoldedInsn(
              RtlExpr::MakeParallel({
                  RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                   RtlExpr::MakeBinary(shr_op, RtlMachineMode::kDi,
                                                       RtlExpr::MakeReg(RtlMachineMode::kDi, s_hi),
                                                       RtlExpr::MakeConstInt(k))),
                  RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, tmp)),
              }),
              mark_newly_folded_sentinel);
          insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1),
                       insns->begin() + static_cast<ptrdiff_t>(i + 4));
          refresh_liveness();
          changed = true;
          continue;
        }
      }
    }

    // 2c. 3-insn `adddi3_internal_2` (`config/mips/mips.md:482-494`,
    // `1 <= C <= 32767`: `addiu dL, aL, C; sltiu tmp, dL, C; addu dH, aH, tmp`).
    // For `C == 1`, `sltiu tmp, dL, 1` is decoded as `(eq:SI dL (const_int 0))`.
    if (i + 2 < insns->size()) {
      const RtlExpr& p0 = (*insns)[i].pattern;
      const RtlExpr& p1 = (*insns)[i + 1].pattern;
      const RtlExpr& p2 = (*insns)[i + 2].pattern;
      int d_lo = -1, a_lo = -1, tmp = -1, cmp_lo = -1, d_hi = -1, a_hi = -1, rhs = -1;
      if (p0.IsSet() && IsPlainSiGpr(p0.SetDest(), &d_lo) && p0.SetSrc().op == RtlOp::kPlus &&
          p0.SetSrc().children.size() == 2 && IsPlainSiGpr(p0.SetSrc().children[0], &a_lo) &&
          p0.SetSrc().children[1].IsConstInt() && p1.IsSet() && IsPlainSiGpr(p1.SetDest(), &tmp) &&
          (p1.SetSrc().op == RtlOp::kLtu || p1.SetSrc().op == RtlOp::kEq) &&
          p1.SetSrc().children.size() == 2 && IsPlainSiGpr(p1.SetSrc().children[0], &cmp_lo) &&
          p1.SetSrc().children[1].IsConstInt() && p2.IsSet() && IsPlainSiGpr(p2.SetDest(), &d_hi) &&
          p2.SetSrc().op == RtlOp::kPlus && p2.SetSrc().children.size() == 2 &&
          IsPlainSiGpr(p2.SetSrc().children[0], &a_hi) &&
          IsPlainSiGpr(p2.SetSrc().children[1], &rhs)) {
        const int64_t c_val = p0.SetSrc().children[1].int_val;
        const bool carry_matches =
            (p1.SetSrc().op == RtlOp::kLtu && p1.SetSrc().children[1].int_val == c_val) ||
            (c_val == 1 && p1.SetSrc().op == RtlOp::kEq && p1.SetSrc().children[1].int_val == 0);
        if (c_val > 0 && c_val < 32768 && carry_matches && IsValidGp32DiPair(d_hi) &&
            d_lo == d_hi + 1 && IsValidGp32DiPair(a_hi) && a_lo == a_hi + 1 && cmp_lo == d_lo &&
            rhs == tmp && tmp != d_hi && tmp != d_lo && !live_after_scratch[i + 2].Test(tmp)) {
          const RtlFlags dst_flags = p2.SetDest().flags;
          (*insns)[i] = MakeFoldedInsn(
              RtlExpr::MakeParallel({
                  RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                   RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kDi,
                                                       RtlExpr::MakeReg(RtlMachineMode::kDi, a_hi),
                                                       RtlExpr::MakeConstInt(c_val))),
                  RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, tmp)),
              }),
              mark_newly_folded_sentinel);
          insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1),
                       insns->begin() + static_cast<ptrdiff_t>(i + 3));
          refresh_liveness();
          changed = true;
          continue;
        }
      }
    }

    // 4b & 5b. 2-insn `ashldi3_internal2` (`sll dH, sL, k_lo; move dL, $0`, `1 <= k_lo <= 31`),
    // `lshrdi3_internal2` (`srl dL, sH, k_lo; move dH, $0`, `1 <= k_lo <= 31`), and
    // `ashrdi3_internal2` (`sra dL, sH, k_lo; sra dH, sH, 31`, `0 <= k_lo <= 30`;
    // `k_lo == 0` is `sra dL, sH, 0`, distinct from `extendsidi2` `move dL, s`).
    if (i + 1 < insns->size()) {
      const RtlExpr& p0 = (*insns)[i].pattern;
      const RtlExpr& p1 = (*insns)[i + 1].pattern;
      int r0 = -1, s0 = -1, r1 = -1, s1 = -1;
      if (p0.IsSet() && p1.IsSet() && IsPlainSiGpr(p0.SetDest(), &r0) &&
          IsPlainSiGpr(p1.SetDest(), &r1)) {
        if (p0.SetSrc().op == RtlOp::kAshift && p0.SetSrc().children.size() == 2 &&
            IsPlainSiGpr(p0.SetSrc().children[0], &s0) && p0.SetSrc().children[1].IsConstInt() &&
            p1.SetSrc().IsConstInt() && p1.SetSrc().int_val == 0) {
          const int64_t k_lo = p0.SetSrc().children[1].int_val;
          const int s_hi = s0 - 1;
          if (k_lo >= 1 && k_lo <= 31 && IsValidGp32DiPair(r0) && r1 == r0 + 1 &&
              IsValidGp32DiPair(s_hi)) {
            MipsHardRegSet forbidden;
            forbidden.Set(r0);
            forbidden.Set(r1);
            forbidden.Set(s_hi);
            forbidden.Set(s0);
            MarkForwardOverlapExtendsidi2PartnerRegsForbidden(*insns, i + 1, live_after[i + 1],
                                                              &forbidden);
            const int scratch = PickDeadCallerSavedGpr(live_after[i + 1], forbidden);
            if (scratch >= 2) {
              const RtlFlags dst_flags = p0.SetDest().flags;
              (*insns)[i] = MakeFoldedInsn(
                  RtlExpr::MakeParallel({
                      RtlExpr::MakeSet(
                          RtlExpr::MakeReg(RtlMachineMode::kDi, r0, dst_flags),
                          RtlExpr::MakeBinary(RtlOp::kAshift, RtlMachineMode::kDi,
                                              RtlExpr::MakeReg(RtlMachineMode::kDi, s_hi),
                                              RtlExpr::MakeConstInt(32 + k_lo))),
                      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, scratch)),
                  }),
                  mark_newly_folded_sentinel);
              insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1));
              live_after = recompute_live_after();
              live_after_scratch = recompute_live_after_scratch();
              changed = true;
              continue;
            }
          }
        }
        if (p0.SetSrc().op == RtlOp::kLshiftrt && p0.SetSrc().children.size() == 2 &&
            IsPlainSiGpr(p0.SetSrc().children[0], &s0) && p0.SetSrc().children[1].IsConstInt() &&
            p1.SetSrc().IsConstInt() && p1.SetSrc().int_val == 0) {
          const int64_t k_lo = p0.SetSrc().children[1].int_val;
          if (k_lo >= 1 && k_lo <= 31 && IsValidGp32DiPair(r1) && r0 == r1 + 1 &&
              IsValidGp32DiPair(s0)) {
            MipsHardRegSet forbidden;
            forbidden.Set(r1);
            forbidden.Set(r0);
            forbidden.Set(s0);
            forbidden.Set(s0 + 1);
            MarkForwardOverlapExtendsidi2PartnerRegsForbidden(*insns, i + 1, live_after[i + 1],
                                                              &forbidden);
            const int scratch = PickDeadCallerSavedGpr(live_after[i + 1], forbidden);
            if (scratch >= 2) {
              const RtlFlags dst_flags = p1.SetDest().flags;
              (*insns)[i] = MakeFoldedInsn(
                  RtlExpr::MakeParallel({
                      RtlExpr::MakeSet(
                          RtlExpr::MakeReg(RtlMachineMode::kDi, r1, dst_flags),
                          RtlExpr::MakeBinary(RtlOp::kLshiftrt, RtlMachineMode::kDi,
                                              RtlExpr::MakeReg(RtlMachineMode::kDi, s0),
                                              RtlExpr::MakeConstInt(32 + k_lo))),
                      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, scratch)),
                  }),
                  mark_newly_folded_sentinel);
              insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1));
              live_after = recompute_live_after();
              live_after_scratch = recompute_live_after_scratch();
              changed = true;
              continue;
            }
          }
        }
        if (p0.SetSrc().op == RtlOp::kAshiftrt && p0.SetSrc().children.size() == 2 &&
            IsPlainSiGpr(p0.SetSrc().children[0], &s0) && p0.SetSrc().children[1].IsConstInt() &&
            p1.SetSrc().op == RtlOp::kAshiftrt && p1.SetSrc().children.size() == 2 &&
            IsPlainSiGpr(p1.SetSrc().children[0], &s1) && p1.SetSrc().children[1].IsConstInt() &&
            p1.SetSrc().children[1].int_val == 31) {
          const int64_t k_lo = p0.SetSrc().children[1].int_val;
          // `config/mips/mips.md:3983-3996, 4015-4028` (`ashrdi3_internal2` and
          // its post-reload big-endian `define_split`): shift counts in `[32, 62]`
          // emit `sra d_lo, s_hi, (amt & 31); sra d_hi, s_hi, 31`, so `k_lo == 0`
          // uniquely identifies `amt == 32` (GCC never emits a 32-bit `sra r, s, 0`).
          // Partial-word liveness (`flow.c:2529-2553`) preserves `s0 + 1` live
          // via a single-word `(REG_DEAD (reg:SI s0))` when `s0 + 1` is still read.
          const bool k_lo_ok = (k_lo >= 0 && k_lo <= 30);
          if (k_lo_ok && IsValidGp32DiPair(r1) && r0 == r1 + 1 && IsValidGp32DiPair(s0) &&
              s1 == s0 &&
              !IsNarrowSignExtendShiftTemp(*insns, i, s0, k_lo, r1, live_after[i + 1])) {
            MipsHardRegSet forbidden;
            forbidden.Set(r1);
            forbidden.Set(r0);
            forbidden.Set(s0);
            forbidden.Set(s0 + 1);
            MarkForwardOverlapExtendsidi2PartnerRegsForbidden(*insns, i + 1, live_after[i + 1],
                                                              &forbidden);
            const int scratch = PickDeadCallerSavedGpr(live_after[i + 1], forbidden);
            if (scratch >= 2) {
              const RtlFlags dst_flags = p1.SetDest().flags;
              (*insns)[i] = MakeFoldedInsn(
                  RtlExpr::MakeParallel({
                      RtlExpr::MakeSet(
                          RtlExpr::MakeReg(RtlMachineMode::kDi, r1, dst_flags),
                          RtlExpr::MakeBinary(RtlOp::kAshiftrt, RtlMachineMode::kDi,
                                              RtlExpr::MakeReg(RtlMachineMode::kDi, s0),
                                              RtlExpr::MakeConstInt(32 + k_lo))),
                      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, scratch)),
                  }),
                  mark_newly_folded_sentinel);
              insns->erase(insns->begin() + static_cast<ptrdiff_t>(i + 1));
              live_after = recompute_live_after();
              live_after_scratch = recompute_live_after_scratch();
              changed = true;
              continue;
            }
          }
        }
      }
    }
  }
  return changed;
}

bool TryFoldGp32DiWitnessedIdioms(const RtlControlFlowGraph& cfg, int frame_size,
                                  const absl::flat_hash_set<int>& compare_di_pairs,
                                  const absl::flat_hash_set<int>& existing_di_regs,
                                  const MipsWitnessedDiFoldOptions& options,
                                  std::vector<std::vector<RtlPatternAndNotes>>* blocks) {
  bool changed = false;
  for (size_t b = 0; b < blocks->size(); ++b) {
    auto& insns = (*blocks)[b];
    const bool is_exit_blk = cfg.rpo_blocks[b].successors.empty();
    MipsHardRegSet defined_so_far;

    for (size_t i = 0; i < insns.size(); ++i) {
      // 1. Contiguous 2-insn `anddi3`, `iordi3`, `xordi3`, `movdi` load, and `movdi` store.
      if (i + 1 < insns.size()) {
        const RtlExpr& p0 = insns[i].pattern;
        const RtlExpr& p1 = insns[i + 1].pattern;

        if (p0.IsSet() && p1.IsSet()) {
          int d_hi = -1, d_lo = -1, a_hi = -1, b_hi = -1, a_lo = -1, b_lo = -1;
          const RtlOp op0 = p0.SetSrc().op;
          if ((op0 == RtlOp::kAnd || op0 == RtlOp::kIor || op0 == RtlOp::kXor) &&
              p1.SetSrc().op == op0 && p0.SetSrc().children.size() == 2 &&
              p1.SetSrc().children.size() == 2 && IsPlainSiGpr(p0.SetDest(), &d_hi) &&
              IsPlainSiGpr(p1.SetDest(), &d_lo) && IsValidGp32DiPair(d_hi) && d_lo == d_hi + 1 &&
              IsPlainSiGpr(p0.SetSrc().children[0], &a_hi) &&
              IsPlainSiGpr(p0.SetSrc().children[1], &b_hi) &&
              IsPlainSiGpr(p1.SetSrc().children[0], &a_lo) &&
              IsPlainSiGpr(p1.SetSrc().children[1], &b_lo)) {
            if (a_lo == b_hi + 1 && b_lo == a_hi + 1) {
              std::swap(a_lo, b_lo);
            }
            if (IsValidGp32DiPair(a_hi) && a_lo == a_hi + 1 && IsValidGp32DiPair(b_hi) &&
                b_lo == b_hi + 1) {
              const RtlFlags dst_flags = p0.SetDest().flags;
              insns[i] = MakeFoldedInsn(
                  RtlExpr::MakeSet(
                      RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                      RtlExpr::MakeBinary(op0, RtlMachineMode::kDi,
                                          RtlExpr::MakeReg(RtlMachineMode::kDi, a_hi),
                                          RtlExpr::MakeReg(RtlMachineMode::kDi, b_hi))),
                  options.mark_newly_folded_sentinel);
              insns.erase(insns.begin() + static_cast<ptrdiff_t>(i + 1));
              defined_so_far.Set(d_hi);
              defined_so_far.Set(d_lo);
              changed = true;
              continue;
            }
          }
          if (op0 == RtlOp::kNot && p1.SetSrc().op == RtlOp::kNot &&
              p0.SetSrc().children.size() == 1 && p1.SetSrc().children.size() == 1 &&
              IsPlainSiGpr(p0.SetDest(), &d_hi) && IsPlainSiGpr(p1.SetDest(), &d_lo) &&
              IsValidGp32DiPair(d_hi) && d_lo == d_hi + 1 &&
              IsPlainSiGpr(p0.SetSrc().children[0], &a_hi) &&
              IsPlainSiGpr(p1.SetSrc().children[0], &a_lo) && IsValidGp32DiPair(a_hi) &&
              a_lo == a_hi + 1) {
            const RtlFlags dst_flags = p0.SetDest().flags;
            insns[i] = MakeFoldedInsn(
                RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                 RtlExpr::MakeUnary(RtlOp::kNot, RtlMachineMode::kDi,
                                                    RtlExpr::MakeReg(RtlMachineMode::kDi, a_hi))),
                options.mark_newly_folded_sentinel);
            insns.erase(insns.begin() + static_cast<ptrdiff_t>(i + 1));
            defined_so_far.Set(d_hi);
            defined_so_far.Set(d_lo);
            changed = true;
            continue;
          }

          // `movdi` load (`[dH <- mem(addr0), dL <- mem(addr4)]` when `dH != base`, or
          // `[dL <- mem(addr4), dH <- mem(addr0)]` when `dH == base`).
          int r0 = -1, r1 = -1;
          if (IsPlainSiGpr(p0.SetDest(), &r0) && IsPlainSiGpr(p1.SetDest(), &r1) &&
              p0.SetSrc().IsMem() && p0.SetSrc().mode == RtlMachineMode::kSi &&
              !p0.SetSrc().flags.volatil && !p0.SetSrc().flags.in_struct &&
              p0.SetSrc().children.size() == 1 && p1.SetSrc().IsMem() &&
              p1.SetSrc().mode == RtlMachineMode::kSi && !p1.SetSrc().flags.volatil &&
              !p1.SetSrc().flags.in_struct && p1.SetSrc().children.size() == 1) {
            int base_reg = -1;
            int64_t off0 = 0;
            int fold_d_hi = -1;
            RtlExpr fold_addr0;
            RtlFlags dst_flags;
            if (IsValidGp32DiPair(r0) && r1 == r0 + 1 &&
                TryMatchAddrPlusFour(p0.SetSrc().children[0], p1.SetSrc().children[0], &base_reg,
                                     &off0) &&
                r0 != base_reg) {
              fold_d_hi = r0;
              fold_addr0 = p0.SetSrc().children[0];
              dst_flags = p0.SetDest().flags;
            } else if (IsValidGp32DiPair(r1) && r0 == r1 + 1 &&
                       TryMatchAddrPlusFour(p1.SetSrc().children[0], p0.SetSrc().children[0],
                                            &base_reg, &off0) &&
                       r1 == base_reg) {
              fold_d_hi = r1;
              fold_addr0 = p1.SetSrc().children[0];
              dst_flags = p1.SetDest().flags;
            }
            const bool is_epilogue_callee_restore = is_exit_blk && base_reg == 29 &&
                                                    IsSavedGprOrRaRegno(fold_d_hi) && off0 >= 0 &&
                                                    off0 + 4 < frame_size;
            const bool aligned_frame_off = (base_reg != 29 && base_reg != 30) || (off0 % 8) == 0;
            if (fold_d_hi != -1 && !is_epilogue_callee_restore && aligned_frame_off) {
              insns[i] = MakeFoldedInsn(
                  RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, fold_d_hi, dst_flags),
                                   RtlExpr::MakeMem(RtlMachineMode::kDi, std::move(fold_addr0))),
                  options.mark_newly_folded_sentinel);
              insns.erase(insns.begin() + static_cast<ptrdiff_t>(i + 1));
              defined_so_far.Set(fold_d_hi);
              defined_so_far.Set(fold_d_hi + 1);
              changed = true;
              continue;
            }
          }

          // `movdi` store (`[mem(addr0) <- sH, mem(addr4) <- sL]`).
          int s0 = -1, s1 = -1;
          if (p0.SetDest().IsMem() && p0.SetDest().mode == RtlMachineMode::kSi &&
              !p0.SetDest().flags.volatil && !p0.SetDest().flags.in_struct &&
              p0.SetDest().children.size() == 1 && p1.SetDest().IsMem() &&
              p1.SetDest().mode == RtlMachineMode::kSi && !p1.SetDest().flags.volatil &&
              !p1.SetDest().flags.in_struct && p1.SetDest().children.size() == 1 &&
              IsPlainSiGpr(p0.SetSrc(), &s0) && IsPlainSiGpr(p1.SetSrc(), &s1) &&
              IsValidGp32DiPair(s0) && s1 == s0 + 1) {
            int base_reg = -1;
            int64_t off0 = 0;
            if (TryMatchAddrPlusFour(p0.SetDest().children[0], p1.SetDest().children[0], &base_reg,
                                     &off0)) {
              const bool is_prologue_callee_save =
                  b == 0 && base_reg == 29 && IsSavedGprOrRaRegno(s0) && off0 >= 0 &&
                  off0 + 4 < frame_size && !defined_so_far.Test(s0) && !defined_so_far.Test(s1);
              const bool is_prologue_variadic_save =
                  b == 0 && frame_size > 0 && base_reg == 29 && !defined_so_far.Test(29) &&
                  (s0 == 4 || s0 == 6) && off0 == (s0 - 4) * 4 && !defined_so_far.Test(s0) &&
                  !defined_so_far.Test(s1);
              const bool aligned_frame_off = (base_reg != 29 && base_reg != 30) || (off0 % 8) == 0;
              bool separate_si_frame_slots = false;
              if (aligned_frame_off && (base_reg == 29 || base_reg == 30)) {
                bool saw_di_slot = false;
                bool saw_si_read_lo = false;
                bool saw_si_read_hi = false;
                auto extract_base_disp = [&](const RtlExpr& addr, int64_t* out_disp) -> bool {
                  if (addr.op == RtlOp::kReg && addr.RegNo() == base_reg) {
                    *out_disp = 0;
                    return true;
                  }
                  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                      addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == base_reg &&
                      addr.children[1].IsConstInt()) {
                    *out_disp = addr.children[1].int_val;
                    return true;
                  }
                  return false;
                };
                for (size_t qb = 0; qb < blocks->size() && !saw_di_slot; ++qb) {
                  const bool q_is_exit_blk = cfg.rpo_blocks[qb].successors.empty();
                  for (size_t qi = 0; qi < (*blocks)[qb].size(); ++qi) {
                    if (qb == b && (qi == i || qi == i + 1)) {
                      continue;
                    }
                    const RtlExpr& qpat = (*blocks)[qb][qi].pattern;
                    if (qi + 1 < (*blocks)[qb].size() && (qb != b || qi + 1 != i)) {
                      const RtlExpr& qnext = (*blocks)[qb][qi + 1].pattern;
                      int qr0 = -1, qr1 = -1, qbase = -1;
                      int64_t qload_off = 0;
                      if (qpat.IsSet() && qnext.IsSet() && IsPlainSiGpr(qpat.SetDest(), &qr0) &&
                          IsPlainSiGpr(qnext.SetDest(), &qr1) && qpat.SetSrc().IsMem() &&
                          qpat.SetSrc().mode == RtlMachineMode::kSi &&
                          !qpat.SetSrc().flags.volatil && !qpat.SetSrc().flags.in_struct &&
                          qpat.SetSrc().children.size() == 1 && qnext.SetSrc().IsMem() &&
                          qnext.SetSrc().mode == RtlMachineMode::kSi &&
                          !qnext.SetSrc().flags.volatil && !qnext.SetSrc().flags.in_struct &&
                          qnext.SetSrc().children.size() == 1) {
                        int fold_qr = -1;
                        if (IsValidGp32DiPair(qr0) && qr1 == qr0 + 1 &&
                            TryMatchAddrPlusFour(qpat.SetSrc().children[0],
                                                 qnext.SetSrc().children[0], &qbase, &qload_off) &&
                            qr0 != qbase) {
                          fold_qr = qr0;
                        } else if (IsValidGp32DiPair(qr1) && qr0 == qr1 + 1 &&
                                   TryMatchAddrPlusFour(qnext.SetSrc().children[0],
                                                        qpat.SetSrc().children[0], &qbase,
                                                        &qload_off) &&
                                   qr1 == qbase) {
                          fold_qr = qr1;
                        }
                        const bool is_epilogue_restore =
                            q_is_exit_blk && qbase == 29 && IsSavedGprOrRaRegno(fold_qr) &&
                            qload_off >= 0 && qload_off + 4 < frame_size;
                        if (fold_qr != -1 && !is_epilogue_restore && qbase == base_reg &&
                            qload_off == off0) {
                          saw_di_slot = true;
                          break;
                        }
                      }
                    }
                    qpat.Visit([&](const RtlExpr& n) {
                      if (n.op != RtlOp::kMem || n.children.empty()) {
                        return;
                      }
                      int64_t qoff = 0;
                      if (!extract_base_disp(n.children[0], &qoff)) {
                        return;
                      }
                      if (n.mode == RtlMachineMode::kDi && qoff == off0) {
                        saw_di_slot = true;
                        return;
                      }
                      const bool is_lhs = qpat.IsSet() && &qpat.SetDest() == &n;
                      if (!is_lhs && n.mode == RtlMachineMode::kSi) {
                        if (qoff == off0) {
                          saw_si_read_lo = true;
                        } else if (qoff == off0 + 4) {
                          saw_si_read_hi = true;
                        }
                      }
                    });
                  }
                }
                separate_si_frame_slots = !saw_di_slot && saw_si_read_lo && saw_si_read_hi;
              }
              if (!is_prologue_callee_save && !is_prologue_variadic_save && aligned_frame_off &&
                  !separate_si_frame_slots) {
                insns[i] = MakeFoldedInsn(
                    RtlExpr::MakeSet(
                        RtlExpr::MakeMem(RtlMachineMode::kDi, p0.SetDest().children[0]),
                        RtlExpr::MakeReg(RtlMachineMode::kDi, s0)),
                    options.mark_newly_folded_sentinel);
                insns.erase(insns.begin() + static_cast<ptrdiff_t>(i + 1));
                changed = true;
                continue;
              }
            }
          }
        }
      }

      // 2. `movdi` reg-reg / HILO move (`[dH <- sH, dL <- sL]` or `[dL <- sL, dH <- sH]`
      // including `[mfhi; mflo]` and `[mflo; mfhi]`).
      auto is_si_gpr_or_hilo = [](const RtlExpr& expr, int* out_reg) -> bool {
        if (expr.op != RtlOp::kReg || expr.mode != RtlMachineMode::kSi) {
          return false;
        }
        const int r = expr.RegNo();
        if ((r < 1 || r > 31) && r != 64 && r != 65) {
          return false;
        }
        *out_reg = r;
        return true;
      };
      const RtlExpr& pi = insns[i].pattern;
      int di0 = -1, si0 = -1;
      if (pi.IsSet() && is_si_gpr_or_hilo(pi.SetDest(), &di0) &&
          is_si_gpr_or_hilo(pi.SetSrc(), &si0)) {
        int d_hi = -1, s_hi = -1;
        bool first_is_hi = true;
        if ((IsValidGp32DiPair(di0) || di0 == 64) && (IsValidGp32DiPair(si0) || si0 == 64) &&
            di0 != si0) {
          d_hi = di0;
          s_hi = si0;
        } else if (si0 == 65 && IsValidGp32DiPair(di0 - 1)) {
          d_hi = di0 - 1;
          s_hi = 64;
          first_is_hi = false;
        } else if (IsValidGp32DiPair(di0 - 1) && IsValidGp32DiPair(si0 - 1) && di0 - 1 == si0) {
          d_hi = di0 - 1;
          s_hi = si0 - 1;
          first_is_hi = false;
        }
        bool allow_pair = (d_hi != -1);
        if (allow_pair && (d_hi == 64 || s_hi == 64)) {
          const int gpr_hi = (s_hi == 64) ? d_hi : s_hi;
          allow_pair = existing_di_regs.contains(gpr_hi) || compare_di_pairs.contains(gpr_hi);
          if (allow_pair && s_hi == 64) {
            for (size_t rev = i; rev > 0; --rev) {
              const RtlExpr& prev = insns[rev - 1].pattern;
              if (!prev.ContainsReg(64) && !prev.ContainsReg(65)) {
                continue;
              }
              bool is_divmod = false;
              prev.Visit([&](const RtlExpr& node) {
                if (node.op == RtlOp::kDiv || node.op == RtlOp::kUdiv || node.op == RtlOp::kMod ||
                    node.op == RtlOp::kUmod) {
                  is_divmod = true;
                }
              });
              if (is_divmod) {
                allow_pair = false;
              }
              break;
            }
          }
        }
        if (allow_pair) {
          const int d_lo = d_hi + 1;
          const int s_lo = s_hi + 1;
          const int want_dj = first_is_hi ? d_lo : d_hi;
          const int want_sj = first_is_hi ? s_lo : s_hi;
          const size_t max_j =
              options.allow_noncontiguous_reg_move ? insns.size() : std::min(insns.size(), i + 2);
          bool folded_reg_move = false;
          for (size_t j = i + 1; j < max_j; ++j) {
            const RtlExpr& pj = insns[j].pattern;
            int dj = -1, sj = -1;
            if (pj.IsSet() && is_si_gpr_or_hilo(pj.SetDest(), &dj) &&
                is_si_gpr_or_hilo(pj.SetSrc(), &sj) && dj == want_dj && sj == want_sj) {
              const RtlFlags dst_flags = first_is_hi ? pi.SetDest().flags : pj.SetDest().flags;
              insns[i] = MakeFoldedInsn(
                  RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                   RtlExpr::MakeReg(RtlMachineMode::kDi, s_hi)),
                  options.mark_newly_folded_sentinel);
              insns.erase(insns.begin() + static_cast<ptrdiff_t>(j));
              defined_so_far.Set(d_hi);
              defined_so_far.Set(d_lo);
              changed = true;
              folded_reg_move = true;
              break;
            }
            if (IsBarrierForFolding(pj) || pj.ContainsReg(d_hi) || pj.ContainsReg(d_lo) ||
                pj.ContainsReg(s_hi) || pj.ContainsReg(s_lo)) {
              break;
            }
          }
          if (folded_reg_move) {
            continue;
          }
        }
      }

      MipsHardRegSet defs_i;
      MipsHardRegSet uses_i;
      CollectHardRegDefsAndUses(insns[i].pattern, /*gp64=*/false, /*fp64=*/false, &defs_i, &uses_i);
      defined_so_far.Or(defs_i);
    }
  }

  // 3. Collect all even GPRs now known to hold `DImode` values, and fold contiguous
  // 64-bit constant loads into `(dH, dL = dH + 1)`, where each 32-bit half is
  // either a 1-insn `const_int`/`ori $r, $0, C` or a 2-insn GAS-expanded
  // `lui $r, HI; ori $r, $r, LO` (`config/mips/mips.c:1368-1402`, `mips_move_2words`).
  absl::flat_hash_set<int> di_even_regs = compare_di_pairs;
  bool dummy_any_di = false;
  CollectExistingDiEvenRegs(*blocks, &di_even_regs, &dummy_any_di);

  auto parse_si_const_half = [](const std::vector<RtlPatternAndNotes>& blk_insns, size_t pos,
                                int* out_reg, int64_t* out_val, size_t* out_len,
                                bool* out_was_split) -> bool {
    if (pos >= blk_insns.size()) {
      return false;
    }
    const RtlExpr& p0 = blk_insns[pos].pattern;
    int r0 = -1;
    if (!p0.IsSet() || !IsPlainSiGpr(p0.SetDest(), &r0)) {
      return false;
    }
    if (pos + 1 < blk_insns.size() && p0.SetSrc().IsConstInt()) {
      const int64_t hi = p0.SetSrc().int_val;
      const RtlExpr& p1 = blk_insns[pos + 1].pattern;
      int r1 = -1;
      if (hi != 0 && (hi & 0xffff) == 0 && p1.IsSet() && IsPlainSiGpr(p1.SetDest(), &r1) &&
          r1 == r0 && p1.SetSrc().op == RtlOp::kIor && p1.SetSrc().mode == RtlMachineMode::kSi &&
          p1.SetSrc().children.size() == 2 && p1.SetSrc().children[0].IsReg() &&
          p1.SetSrc().children[0].mode == RtlMachineMode::kSi &&
          p1.SetSrc().children[0].RegNo() == r0 && p1.SetSrc().children[1].IsConstInt() &&
          p1.SetSrc().children[1].int_val > 0 && p1.SetSrc().children[1].int_val <= 0xffff) {
        const uint32_t combined =
            static_cast<uint32_t>(hi) | static_cast<uint32_t>(p1.SetSrc().children[1].int_val);
        *out_reg = r0;
        *out_val = static_cast<int32_t>(combined);
        *out_len = 2;
        *out_was_split = true;
        return true;
      }
    }
    if (p0.SetSrc().IsConstInt()) {
      *out_reg = r0;
      *out_val = p0.SetSrc().int_val;
      *out_len = 1;
      *out_was_split = false;
      return true;
    }
    if (p0.SetSrc().op == RtlOp::kIor && p0.SetSrc().mode == RtlMachineMode::kSi &&
        p0.SetSrc().children.size() == 2 && p0.SetSrc().children[0].IsReg() &&
        !p0.SetSrc().children[0].IsArgPointerReg() && p0.SetSrc().children[0].RegNo() == 0 &&
        p0.SetSrc().children[1].IsConstInt() && p0.SetSrc().children[1].int_val >= 0 &&
        p0.SetSrc().children[1].int_val <= 0xffff) {
      *out_reg = r0;
      *out_val = p0.SetSrc().children[1].int_val;
      *out_len = 1;
      *out_was_split = false;
      return true;
    }
    return false;
  };

  for (size_t b = 0; b < blocks->size(); ++b) {
    auto& insns = (*blocks)[b];
    for (size_t i = 0; i + 1 < insns.size(); ++i) {
      int d_hi = -1, d_lo = -1;
      int64_t hi_val = 0, lo_val = 0;
      size_t len0 = 0, len1 = 0;
      bool split0 = false, split1 = false;
      if (parse_si_const_half(insns, i, &d_hi, &hi_val, &len0, &split0) &&
          IsValidGp32DiPair(d_hi) && di_even_regs.contains(d_hi) &&
          parse_si_const_half(insns, i + len0, &d_lo, &lo_val, &len1, &split1) &&
          d_lo == d_hi + 1) {
        const size_t total_len = len0 + len1;
        // Guard against folding two unrelated 32-bit constants in `$2`/`$3` if the
        // very next instruction reads `d_hi` or `d_lo` in `SImode` outside a 64-bit compare.
        if (i + total_len < insns.size() && !compare_di_pairs.contains(d_hi)) {
          bool next_reads_si = false;
          insns[i + total_len].pattern.Visit([&](const RtlExpr& n) {
            if (n.IsReg() && n.mode == RtlMachineMode::kSi &&
                (n.RegNo() == d_hi || n.RegNo() == d_lo)) {
              next_reads_si = true;
            }
          });
          if (next_reads_si) {
            continue;
          }
        }
        const RtlFlags dst_flags = insns[i].pattern.SetDest().flags;
        RtlExpr src_const = (hi_val == 0 && lo_val == 0)
                                ? RtlExpr::MakeConstInt(0)
                                : (((split0 || split1) &&
                                    ((hi_val == 0 && lo_val > 0) || (hi_val == -1 && lo_val < 0)))
                                       ? RtlExpr::MakeConstInt(lo_val)
                                       : MakeDiConstDouble(lo_val, hi_val));
        insns[i] =
            MakeFoldedInsn(RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, d_hi, dst_flags),
                                            std::move(src_const)),
                           options.mark_newly_folded_sentinel);
        insns.erase(insns.begin() + static_cast<ptrdiff_t>(i + 1),
                    insns.begin() + static_cast<ptrdiff_t>(i + total_len));
        changed = true;
      }
    }

    // Upgrade `(use (reg/i:SI 2))` to `(use (reg/i:DI 2))` if `$2` is defined as `DImode`
    // in this exit block without being overwritten in `SImode`.
    bool reg2_defined_di_in_blk = false;
    bool reg2_defined_si_after_di = false;
    for (const auto& pn : insns) {
      auto check_set_reg2 = [&](const RtlExpr& elem) {
        if (elem.IsSet() && elem.SetDest().IsReg() && elem.SetDest().RegNo() == 2) {
          if (elem.SetDest().mode == RtlMachineMode::kDi) {
            reg2_defined_di_in_blk = true;
            reg2_defined_si_after_di = false;
          } else {
            reg2_defined_si_after_di = true;
          }
        }
      };
      auto inspect_pat = [&](const RtlExpr& pat) {
        if (pat.IsParallel()) {
          for (const RtlExpr& ch : pat.children) {
            check_set_reg2(ch);
          }
        } else {
          check_set_reg2(pat);
        }
      };
      if (pn.pattern.op == RtlOp::kSequence && pn.pattern.children.size() >= 2 &&
          !pn.pattern.children[1].children.empty()) {
        inspect_pat(pn.pattern.children[1].children[0]);
      } else {
        inspect_pat(pn.pattern);
      }
    }
    if (reg2_defined_di_in_blk && !reg2_defined_si_after_di) {
      for (auto& pn : insns) {
        if (pn.pattern.op == RtlOp::kUse && pn.pattern.children.size() == 1 &&
            pn.pattern.children[0].IsReg() && pn.pattern.children[0].RegNo() == 2 &&
            pn.pattern.children[0].mode == RtlMachineMode::kSi) {
          pn.pattern.children[0].mode = RtlMachineMode::kDi;
          changed = true;
        }
      }
    }
  }
  return changed;
}

bool TryFoldMovstrsiBlockMovesInBlock(const RtlControlFlowGraph& cfg, size_t b,
                                      const MipsMovstrsiFoldOptions& options,
                                      std::vector<RtlPatternAndNotes>* insns,
                                      std::optional<RtlPatternAndNotes>* epilogue_delay_pn) {
  bool changed = false;
  auto recompute_live_after = [&]() {
    return ComputeBlockHardRegLiveAfter(cfg, b, *insns, options.gp64, options.fp64,
                                        epilogue_delay_pn,
                                        options.compute_live_in_when_sideband_empty);
  };
  std::vector<MipsHardRegSet> live_after = recompute_live_after();

  for (size_t pos = 0; pos < insns->size(); ++pos) {
    bool folded_here = false;
    for (size_t n = 4; n >= 2; --n) {
      if (pos + 2 * n <= insns->size()) {
        int src_base = -1;
        int64_t src_base_off = 0;
        int dst_base = -1;
        int64_t dst_base_off = 0;
        int temps[4] = {-1, -1, -1, -1};
        int widths[4] = {0, 0, 0, 0};
        int64_t expected_off = 0;
        bool ok = true;
        MipsHardRegSet temp_set;

        for (size_t k = 0; k < n; ++k) {
          int lk_reg = -1, lk_base = -1, lk_w = 0;
          int64_t lk_off = -1;
          if (!MatchMovstrLoad((*insns)[pos + k].pattern, &lk_reg, &lk_base, &lk_off, &lk_w)) {
            ok = false;
            break;
          }
          if (k == 0) {
            src_base = lk_base;
            src_base_off = lk_off;
          } else if (lk_base != src_base) {
            ok = false;
            break;
          }
          const int64_t rel_off = lk_off - src_base_off;
          if (rel_off != expected_off || (lk_off % lk_w) != 0 || temp_set.Test(lk_reg)) {
            ok = false;
            break;
          }
          temp_set.Set(lk_reg);
          temps[k] = lk_reg;
          widths[k] = lk_w;
          expected_off += lk_w;
        }
        if (ok) {
          const int64_t total_bytes = expected_off;
          const int align = widths[0];
          int64_t rem = total_bytes;
          for (size_t k = 0; k < n; ++k) {
            if (k > 0 && widths[k] > widths[k - 1]) {
              ok = false;
              break;
            }
            if (k > 0 && widths[k] < widths[k - 1] && rem >= widths[k - 1]) {
              ok = false;
              break;
            }
            rem -= widths[k];
          }
          int64_t store_off = 0;
          for (size_t k = 0; ok && k < n; ++k) {
            int sk_reg = -1, sk_base = -1, sk_w = 0;
            int64_t sk_off = -1;
            if (!MatchMovstrStore((*insns)[pos + n + k].pattern, &sk_reg, &sk_base, &sk_off,
                                  &sk_w)) {
              ok = false;
              break;
            }
            if (k == 0) {
              dst_base = sk_base;
              dst_base_off = sk_off;
            } else if (sk_base != dst_base) {
              ok = false;
              break;
            }
            const int64_t rel_store_off = sk_off - dst_base_off;
            if (sk_reg != temps[k] || sk_w != widths[k] || rel_store_off != store_off ||
                (sk_off % sk_w) != 0) {
              ok = false;
              break;
            }
            store_off += sk_w;
          }
          if (ok && options.has_di_witness && n == 2 && widths[0] == 4 && widths[1] == 4 &&
              IsValidGp32DiPair(temps[0]) && temps[1] == temps[0] + 1) {
            ok = false;
          }
          if (ok && options.skip_unfilled_dbr_jump_tail && src_base_off == 0 && dst_base_off == 0) {
            size_t next_idx = pos + 2 * n;
            while (next_idx < insns->size() && ((*insns)[next_idx].pattern.op == RtlOp::kUse ||
                                                (*insns)[next_idx].pattern.op == RtlOp::kClobber)) {
              ++next_idx;
            }
            if ((next_idx < insns->size() &&
                 IsUnfilledUncondJumpOrReturn((*insns)[next_idx].pattern)) ||
                (next_idx == insns->size() && epilogue_delay_pn != nullptr &&
                 !epilogue_delay_pn->has_value() && cfg.rpo_blocks[b].successors.empty())) {
              ok = false;
            }
          }
          const bool disjoint_mem = (src_base != dst_base) ||
                                    (dst_base_off + total_bytes <= src_base_off) ||
                                    (src_base_off + total_bytes <= dst_base_off);
          if (ok && disjoint_mem && !temp_set.Test(dst_base) && !temp_set.Test(src_base)) {
            const MipsHardRegSet& live_end = live_after[pos + 2 * n - 1];
            for (size_t k = 0; k < n; ++k) {
              if (live_end.Test(temps[k])) {
                ok = false;
                break;
              }
            }
            bool is_first_nonnote = true;
            for (size_t prev = 0; prev < pos; ++prev) {
              if ((*insns)[prev].pattern.op != RtlOp::kNote) {
                is_first_nonnote = false;
                break;
              }
            }
            if (is_first_nonnote && !live_end.Test(src_base) && src_base >= 2 && src_base <= 25) {
              MipsHardRegSet la_forbidden = temp_set;
              for (size_t k = n; ok && k < 4; ++k) {
                const int extra = PickDeadCallerSavedGpr(live_end, la_forbidden);
                if (extra < 2) {
                  ok = false;
                  break;
                }
                temps[k] = extra;
                la_forbidden.Set(extra);
              }
              if (ok) {
                MipsHardRegSet rl_forbidden = temp_set;
                rl_forbidden.Set(src_base);
                rl_forbidden.Set(dst_base);
                for (size_t k = n; k < 4; ++k) {
                  if (temps[k] != src_base && temps[k] != dst_base) {
                    rl_forbidden.Set(temps[k]);
                  }
                }
                for (size_t k = n; k < 4; ++k) {
                  if (temps[k] == src_base || temps[k] == dst_base) {
                    const int repl = PickDeadCallerSavedGpr(live_end, rl_forbidden);
                    if (repl < 2) {
                      ok = false;
                      break;
                    }
                    temps[k] = repl;
                    rl_forbidden.Set(repl);
                  }
                }
              }
            } else {
              MipsHardRegSet forbidden = temp_set;
              forbidden.Set(src_base);
              forbidden.Set(dst_base);
              for (size_t k = n; ok && k < 4; ++k) {
                const int extra = PickDeadCallerSavedGpr(live_end, forbidden);
                if (extra < 2) {
                  ok = false;
                  break;
                }
                temps[k] = extra;
                forbidden.Set(extra);
              }
            }
            if (ok) {
              (*insns)[pos] =
                  MakeFoldedInsn(MakeMovstrsiParallel(dst_base, dst_base_off, src_base,
                                                      src_base_off, temps, total_bytes, align, 0),
                                 options.mark_newly_folded_sentinel);
              insns->erase(insns->begin() + static_cast<ptrdiff_t>(pos + 1),
                           insns->begin() + static_cast<ptrdiff_t>(pos + 2 * n));
              live_after = recompute_live_after();
              changed = true;
              folded_here = true;
              break;
            }
          }
        }
      }

      // Check `reorg.c` `define_split` (`mips.md:3609`): `2 * n - 1` (`n` loads + `n - 1` stores)
      // immediately preceding a `SEQUENCE` (or exit `epilogue_delay_pattern`) whose delay slot is
      // the matching `n`-th store (`movstrsi_internal2` + `movstrsi_internal3`).
      if (!folded_here && options.allow_delay_slot_split_movstrsi &&
          pos + 2 * n - 1 <= insns->size()) {
        size_t tail_idx = pos + 2 * n - 1;
        while (tail_idx < insns->size() && ((*insns)[tail_idx].pattern.op == RtlOp::kUse ||
                                            (*insns)[tail_idx].pattern.op == RtlOp::kClobber)) {
          ++tail_idx;
        }
        const RtlExpr* slot_store_pat = nullptr;
        bool slot_in_sequence = false;
        bool slot_in_epilogue_delay = false;
        if (tail_idx < insns->size() && (*insns)[tail_idx].pattern.op == RtlOp::kSequence &&
            (*insns)[tail_idx].pattern.children.size() >= 2 &&
            !(*insns)[tail_idx].pattern.children[1].children.empty()) {
          slot_store_pat = &(*insns)[tail_idx].pattern.children[1].children[0];
          slot_in_sequence = true;
        } else if (tail_idx == insns->size() && epilogue_delay_pn != nullptr &&
                   epilogue_delay_pn->has_value()) {
          slot_store_pat = &(*epilogue_delay_pn)->pattern;
          slot_in_epilogue_delay = true;
        }
        if (slot_store_pat != nullptr) {
          int src_base = -1;
          int dst_base = -1;
          int temps[4] = {-1, -1, -1, -1};
          int widths[4] = {0, 0, 0, 0};
          int64_t expected_off = 0;
          bool ok = true;
          MipsHardRegSet temp_set;
          for (size_t k = 0; k < n; ++k) {
            int lk_reg = -1, lk_base = -1, lk_w = 0;
            int64_t lk_off = -1;
            if (!MatchMovstrLoad((*insns)[pos + k].pattern, &lk_reg, &lk_base, &lk_off, &lk_w)) {
              ok = false;
              break;
            }
            if (k == 0) {
              src_base = lk_base;
            } else if (lk_base != src_base) {
              ok = false;
              break;
            }
            if (lk_off != expected_off || (lk_off % lk_w) != 0 || temp_set.Test(lk_reg)) {
              ok = false;
              break;
            }
            temp_set.Set(lk_reg);
            temps[k] = lk_reg;
            widths[k] = lk_w;
            expected_off += lk_w;
          }
          if (ok) {
            const int64_t total_bytes = expected_off;
            const int align = widths[0];
            int64_t rem = total_bytes;
            for (size_t k = 0; k < n; ++k) {
              if (k > 0 && widths[k] > widths[k - 1]) {
                ok = false;
                break;
              }
              if (k > 0 && widths[k] < widths[k - 1] && rem >= widths[k - 1]) {
                ok = false;
                break;
              }
              rem -= widths[k];
            }
            int64_t store_off = 0;
            for (size_t k = 0; ok && k < n; ++k) {
              const RtlExpr& st_pat = (k + 1 < n) ? (*insns)[pos + n + k].pattern : *slot_store_pat;
              int sk_reg = -1, sk_base = -1, sk_w = 0;
              int64_t sk_off = -1;
              if (!MatchMovstrStore(st_pat, &sk_reg, &sk_base, &sk_off, &sk_w)) {
                ok = false;
                break;
              }
              if (k == 0) {
                dst_base = sk_base;
              } else if (sk_base != dst_base) {
                ok = false;
                break;
              }
              if (sk_reg != temps[k] || sk_w != widths[k] || sk_off != store_off) {
                ok = false;
                break;
              }
              store_off += sk_w;
            }
            if (ok && src_base != dst_base && !temp_set.Test(dst_base)) {
              MipsHardRegSet live_constraint =
                  slot_in_sequence ? live_after[tail_idx] : live_after[pos + 2 * n - 2];
              if (slot_in_epilogue_delay) {
                live_constraint = MipsHardRegSet{};
                live_constraint.Set(29);
                live_constraint.Set(31);
                if (cfg.sideband.frame_pointer_needed) {
                  live_constraint.Set(30);
                }
                for (size_t u = pos + 2 * n - 1; u < tail_idx; ++u) {
                  MipsHardRegSet u_defs, u_uses;
                  CollectHardRegDefsAndUses((*insns)[u].pattern, options.gp64, options.fp64,
                                            &u_defs, &u_uses);
                  live_constraint.Or(u_uses);
                }
              } else if (slot_in_sequence) {
                MipsHardRegSet head_defs, head_uses;
                CollectHardRegDefsAndUses((*insns)[tail_idx].pattern.children[0].children[0],
                                          options.gp64, options.fp64, &head_defs, &head_uses);
                if ((*insns)[tail_idx].pattern.children[0].op == RtlOp::kCallInsn ||
                    ContainsCallInExpr((*insns)[tail_idx].pattern.children[0].children[0])) {
                  auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(
                      (*insns)[tail_idx].pattern.children[0]);
                  if (sub0.ok() && sub0->has_explicit_call_fusage) {
                    ApplyCallClobbersAndUses(*sub0, options.gp64, options.fp64, &head_defs,
                                             &head_uses);
                  } else {
                    ApplyCallClobbersAndUses((*insns)[tail_idx], options.gp64, options.fp64,
                                             &head_defs, &head_uses);
                  }
                }
                live_constraint.Or(head_uses);
                for (size_t u = pos + 2 * n - 1; u < tail_idx; ++u) {
                  MipsHardRegSet u_defs, u_uses;
                  CollectHardRegDefsAndUses((*insns)[u].pattern, options.gp64, options.fp64,
                                            &u_defs, &u_uses);
                  live_constraint.Or(u_uses);
                }
              }
              for (size_t k = 0; k < n; ++k) {
                const bool dead_at_end = slot_in_sequence ? !live_after[tail_idx].Test(temps[k])
                                                          : !live_constraint.Test(temps[k]);
                if (!dead_at_end) {
                  ok = false;
                  break;
                }
              }
              MipsHardRegSet forbidden = temp_set;
              forbidden.Set(src_base);
              forbidden.Set(dst_base);
              for (size_t k = n; ok && k < 4; ++k) {
                const int extra = PickDeadCallerSavedGpr(live_constraint, forbidden);
                if (extra < 2) {
                  ok = false;
                  break;
                }
                temps[k] = extra;
                forbidden.Set(extra);
              }
              if (ok) {
                RtlExpr part1 =
                    MakeMovstrsiParallel(dst_base, 0, src_base, 0, temps, total_bytes, align, 1);
                RtlExpr part2 =
                    MakeMovstrsiParallel(dst_base, 0, src_base, 0, temps, total_bytes, align, 2);
                if (slot_in_sequence) {
                  (*insns)[tail_idx].pattern.children[1].children[0] = std::move(part2);
                } else if (slot_in_epilogue_delay) {
                  (*epilogue_delay_pn)->pattern = std::move(part2);
                }
                (*insns)[pos] =
                    MakeFoldedInsn(std::move(part1), options.mark_newly_folded_sentinel);
                insns->erase(insns->begin() + static_cast<ptrdiff_t>(pos + 1),
                             insns->begin() + static_cast<ptrdiff_t>(pos + 2 * n - 1));
                live_after = recompute_live_after();
                changed = true;
                folded_here = true;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (TryCoalesceMultiBatchMovstrsiInBlock(insns, options.gp64,
                                           options.mark_newly_folded_sentinel)) {
    live_after = recompute_live_after();
    changed = true;
  }

  // Sanitize any padded earlyclobber `"=&d"` scratch operands on
  // `movstrsi_internal` (`kind == 0`, e.g. `align == 1` moves disassembled by
  // `InvertFinalScanInsn` where trailing clobber slots were padded with
  // `src_base`) so all 4 clobber registers are distinct GPRs non-overlapping
  // `src_base` and `dst_base` (`local-alloc.c:1553-1557`), without perturbing
  // distinct clobber registers already present in `.12.dbr` snapshots.
  for (size_t pos = 0; pos < insns->size(); ++pos) {
    ParsedMovstrsiInsn info;
    if (!ParseMovstrsiInsn((*insns)[pos].pattern, &info) || info.kind != 0) {
      continue;
    }
    MipsHardRegSet forbidden;
    forbidden.Set(info.src_base);
    forbidden.Set(info.dst_base);
    std::vector<size_t> slots_to_fix;
    for (size_t k = 0; k < 4; ++k) {
      const int hr = info.clobbers[k];
      if (hr < 2 || hr >= 32 || forbidden.Test(hr)) {
        slots_to_fix.push_back(k);
      } else {
        forbidden.Set(hr);
      }
    }
    if (slots_to_fix.empty()) {
      continue;
    }
    RtlExpr updated = (*insns)[pos].pattern;
    bool all_fixed = true;
    for (size_t k : slots_to_fix) {
      const int extra = PickDeadCallerSavedGpr(live_after[pos], forbidden);
      if (extra < 2) {
        all_fixed = false;
        break;
      }
      updated.children[1 + k] = RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, extra));
      forbidden.Set(extra);
    }
    if (all_fixed) {
      (*insns)[pos] = MakeFoldedInsn(std::move(updated), options.mark_newly_folded_sentinel);
      live_after = recompute_live_after();
      changed = true;
    }
  }

  return changed;
}

bool TryFoldGp32DfInGprPairMoves(const RtlControlFlowGraph& cfg, int frame_size,
                                 const absl::flat_hash_set<int>& compare_di_pairs,
                                 const absl::flat_hash_set<int>& existing_di_regs,
                                 bool mark_newly_folded_sentinel,
                                 std::vector<std::vector<RtlPatternAndNotes>>* blocks) {
  absl::flat_hash_set<int> df_gpr_pairs;
  for (const auto& blk : *blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      pn.pattern.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kReg && n.mode == RtlMachineMode::kDf) {
          const int r = n.RegNo();
          if (IsValidGp32DiPair(r) && !existing_di_regs.contains(r) &&
              !compare_di_pairs.contains(r)) {
            df_gpr_pairs.insert(r);
          }
        }
      });
    }
  }
  if (df_gpr_pairs.empty()) {
    return false;
  }

  bool changed = false;
  for (size_t b = 0; b < blocks->size(); ++b) {
    auto& insns = (*blocks)[b];
    const bool is_exit_blk = cfg.rpo_blocks[b].successors.empty();
    MipsHardRegSet defined_so_far;
    for (size_t i = 0; i < insns.size(); ++i) {
      if (i + 1 < insns.size() && insns[i].pattern.IsSet() && insns[i + 1].pattern.IsSet()) {
        const RtlExpr& p0 = insns[i].pattern;
        const RtlExpr& p1 = insns[i + 1].pattern;
        int r0 = -1;
        int r1 = -1;
        if (IsPlainSiGpr(p0.SetDest(), &r0) && IsPlainSiGpr(p1.SetDest(), &r1) &&
            p0.SetSrc().IsMem() && p0.SetSrc().mode == RtlMachineMode::kSi &&
            !p0.SetSrc().flags.volatil && !p0.SetSrc().flags.in_struct &&
            p0.SetSrc().children.size() == 1 && p1.SetSrc().IsMem() &&
            p1.SetSrc().mode == RtlMachineMode::kSi && !p1.SetSrc().flags.volatil &&
            !p1.SetSrc().flags.in_struct && p1.SetSrc().children.size() == 1) {
          int base_reg = -1;
          int64_t off0 = 0;
          int fold_d_hi = -1;
          RtlExpr fold_addr0;
          RtlFlags dst_flags;
          if (df_gpr_pairs.contains(r0) && r1 == r0 + 1 &&
              TryMatchAddrPlusFour(p0.SetSrc().children[0], p1.SetSrc().children[0], &base_reg,
                                   &off0) &&
              r0 != base_reg) {
            fold_d_hi = r0;
            fold_addr0 = p0.SetSrc().children[0];
            dst_flags = p0.SetDest().flags;
          } else if (df_gpr_pairs.contains(r1) && r0 == r1 + 1 &&
                     TryMatchAddrPlusFour(p1.SetSrc().children[0], p0.SetSrc().children[0],
                                          &base_reg, &off0) &&
                     r1 == base_reg) {
            fold_d_hi = r1;
            fold_addr0 = p1.SetSrc().children[0];
            dst_flags = p1.SetDest().flags;
          }
          const bool is_epilogue_callee_restore = is_exit_blk && base_reg == 29 &&
                                                  IsSavedGprOrRaRegno(fold_d_hi) && off0 >= 0 &&
                                                  off0 + 4 < frame_size;
          if (fold_d_hi != -1 && !is_epilogue_callee_restore) {
            insns[i] = MakeFoldedInsn(
                RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDf, fold_d_hi, dst_flags),
                                 RtlExpr::MakeMem(RtlMachineMode::kDf, std::move(fold_addr0))),
                mark_newly_folded_sentinel);
            insns.erase(insns.begin() + static_cast<ptrdiff_t>(i + 1));
            defined_so_far.Set(fold_d_hi);
            defined_so_far.Set(fold_d_hi + 1);
            changed = true;
            continue;
          }
        }
        int s0 = -1;
        int s1 = -1;
        if (p0.SetDest().IsMem() && p0.SetDest().mode == RtlMachineMode::kSi &&
            !p0.SetDest().flags.volatil && !p0.SetDest().flags.in_struct &&
            p0.SetDest().children.size() == 1 && p1.SetDest().IsMem() &&
            p1.SetDest().mode == RtlMachineMode::kSi && !p1.SetDest().flags.volatil &&
            !p1.SetDest().flags.in_struct && p1.SetDest().children.size() == 1 &&
            IsPlainSiGpr(p0.SetSrc(), &s0) && IsPlainSiGpr(p1.SetSrc(), &s1) &&
            df_gpr_pairs.contains(s0) && s1 == s0 + 1) {
          int base_reg = -1;
          int64_t off0 = 0;
          if (TryMatchAddrPlusFour(p0.SetDest().children[0], p1.SetDest().children[0], &base_reg,
                                   &off0)) {
            const bool is_prologue_callee_save =
                b == 0 && base_reg == 29 && IsSavedGprOrRaRegno(s0) && off0 >= 0 &&
                off0 + 4 < frame_size && !defined_so_far.Test(s0) && !defined_so_far.Test(s1);
            // `mips.c:4998-5020` (`mips_expand_prologue`): variadic GP arg home
            // stores (`$a0..$a3` -> `0..12($sp)`) are emitted as separate `SImode`
            // stores before `$sp` adjustment, never as a folded `DFmode` store.
            const bool is_prologue_variadic_arg_home =
                b == 0 && base_reg == 29 && (s0 == 4 || s0 == 6) && off0 == (s0 - 4) * 4 &&
                !defined_so_far.Test(29) && !defined_so_far.Test(s0) && !defined_so_far.Test(s1);
            if (!is_prologue_callee_save && !is_prologue_variadic_arg_home) {
              insns[i] = MakeFoldedInsn(
                  RtlExpr::MakeSet(RtlExpr::MakeMem(RtlMachineMode::kDf, p0.SetDest().children[0]),
                                   RtlExpr::MakeReg(RtlMachineMode::kDf, s0)),
                  mark_newly_folded_sentinel);
              insns.erase(insns.begin() + static_cast<ptrdiff_t>(i + 1));
              changed = true;
              continue;
            }
          }
        }
      }
      MipsHardRegSet defs;
      MipsHardRegSet uses;
      CollectHardRegDefsAndUses(insns[i].pattern, /*gp64=*/false, /*fp64=*/false, &defs, &uses);
      defined_so_far.Or(defs);
    }
  }
  return changed;
}

namespace {

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

bool IsDiRegConsumedAfterRecombiningTailDelaySlot(const RtlControlFlowGraph& cfg, size_t b,
                                                  absl::Span<const RtlPatternAndNotes> tail_insns,
                                                  int dh, bool gp64, bool fp64) {
  const RtlBasicBlock& blk = cfg.rpo_blocks[b];
  const bool has_epilogue_delay = blk.successors.empty() &&
                                  cfg.sideband.epilogue_delay_pattern.has_value() &&
                                  !cfg.sideband.epilogue_delay_pattern->empty();
  bool has_seq = false;
  for (const RtlPatternAndNotes& pn : tail_insns) {
    if (pn.pattern.op == RtlOp::kSequence) {
      has_seq = true;
      break;
    }
  }
  if (!has_seq && !has_epilogue_delay) {
    return false;
  }
  RtlControlFlowGraph scratch;
  scratch.rpo_blocks.resize(1);
  std::vector<std::vector<RtlPatternAndNotes>> scratch_blocks(1);
  std::vector<RtlPatternAndNotes>& sinsns = scratch_blocks[0];
  auto insert_before_trailing_uses = [&sinsns](RtlPatternAndNotes pn) {
    size_t ins_pos = sinsns.size();
    while (ins_pos > 0 && (sinsns[ins_pos - 1].pattern.op == RtlOp::kUse ||
                           sinsns[ins_pos - 1].pattern.op == RtlOp::kClobber)) {
      --ins_pos;
    }
    sinsns.insert(sinsns.begin() + static_cast<ptrdiff_t>(ins_pos), std::move(pn));
  };
  bool pushed_epilogue_delay = false;
  for (size_t k = 0; k < tail_insns.size(); ++k) {
    const RtlExpr& pat = tail_insns[k].pattern;
    if (pat.op == RtlOp::kSequence && pat.children.size() >= 2) {
      auto head_pn = RtlPatternAndNotes::FromSequenceSubInsn(pat.children[0]);
      auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(pat.children[1]);
      if (!head_pn.ok() || !slot_pn.ok()) {
        return false;
      }
      const bool is_call =
          pat.children[0].op == RtlOp::kCallInsn || ContainsCallInExpr(head_pn->pattern);
      if (is_call) {
        sinsns.push_back(std::move(*slot_pn));
        sinsns.push_back(std::move(*head_pn));
      } else {
        insert_before_trailing_uses(std::move(*slot_pn));
        sinsns.push_back(std::move(*head_pn));
      }
      continue;
    }
    if (has_epilogue_delay && !pushed_epilogue_delay &&
        (pat.op == RtlOp::kReturn || IsReturnParallelTop(pat) ||
         (k + 1 == tail_insns.size() && pat.op == RtlOp::kUse))) {
      auto ep_pn = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
      if (ep_pn.ok()) {
        insert_before_trailing_uses(std::move(*ep_pn));
        pushed_epilogue_delay = true;
      }
    }
    sinsns.push_back(tail_insns[k]);
  }
  if (has_epilogue_delay && !pushed_epilogue_delay) {
    auto ep_pn = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
    if (ep_pn.ok()) {
      insert_before_trailing_uses(std::move(*ep_pn));
    }
  }
  auto sync_scratch_block = [&]() {
    scratch.rpo_blocks[0].instructions.clear();
    scratch.rpo_blocks[0].instructions.reserve(sinsns.size());
    for (const RtlPatternAndNotes& pn : sinsns) {
      scratch.rpo_blocks[0].instructions.push_back(pn.ToGccPrintRtlString());
    }
    scratch.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  };
  sync_scratch_block();

  const int frame_size = DetectStackFrameSize(cfg);
  absl::flat_hash_set<int> compare_di_pairs;
  absl::flat_hash_set<int> existing_di_regs = {dh};
  bool dummy_has_di = true;
  CollectExistingDiEvenRegs(scratch_blocks, &existing_di_regs, &dummy_has_di);
  for (int iter = 0; iter < 4; ++iter) {
    bool changed = TryFoldGp32DiMultiInsnIdiomsInBlock(
        scratch, /*b=*/0, /*mark_newly_folded_sentinel=*/false, &sinsns,
        /*compute_live_in_when_sideband_empty=*/false);
    if (changed) {
      sync_scratch_block();
      CollectExistingDiEvenRegs(scratch_blocks, &existing_di_regs, &dummy_has_di);
    }
    MipsWitnessedDiFoldOptions wit_opts;
    wit_opts.allow_noncontiguous_reg_move = true;
    wit_opts.mark_newly_folded_sentinel = false;
    if (TryFoldGp32DiWitnessedIdioms(scratch, frame_size, compare_di_pairs, existing_di_regs,
                                     wit_opts, &scratch_blocks)) {
      changed = true;
      sync_scratch_block();
    }
    if (!changed) {
      break;
    }
    CollectExistingDiEvenRegs(scratch_blocks, &existing_di_regs, &dummy_has_di);
  }

  const int dl = dh + 1;
  for (const RtlPatternAndNotes& pn : sinsns) {
    if (ExprReadsExplicitDiReg(pn.pattern, dh)) {
      return true;
    }
    MipsHardRegSet defs;
    MipsHardRegSet uses;
    CollectHardRegDefsAndUses(pn.pattern, gp64, fp64, &defs, &uses);
    if (defs.Test(dh) || defs.Test(dl) || ContainsCallInExpr(pn.pattern)) {
      break;
    }
  }
  return false;
}

}  // namespace

void RestoreStage0DbrNoConflictExtendClobbers(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                              bool runs_sched2) {
  RestoreNoConflictExtendClobbersWithTailCheck(cfg, gp64, fp64, runs_sched2,
                                               IsDiRegConsumedAfterRecombiningTailDelaySlot);
}

}  // namespace rom_nom_nom::lifter
